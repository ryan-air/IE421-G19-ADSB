#!/usr/bin/env python3
"""
ADS-B Data Streamer for dump1090
Streams real ADS-B aircraft data from dump1090 SBS-1 BaseStation format to Supabase
Supports multiple receivers and handles duplicate data gracefully
"""

import socket
import sys
import os
import time
import logging
from datetime import datetime, timezone
from typing import Dict, Optional, Set
from dataclasses import dataclass
import psycopg2
from psycopg2 import pool
from psycopg2.extras import execute_values
import argparse

# Load environment variables from .env file if present
try:
    from dotenv import load_dotenv
    load_dotenv()
except ImportError:
    pass  # dotenv not installed, will use system env vars only

# Configure logging
logging.basicConfig(
    level=logging.DEBUG,  # Temporarily set to DEBUG to diagnose trail insertion issue
    format='%(asctime)s - %(levelname)s - %(message)s',
    datefmt='%Y-%m-%d %H:%M:%S'
)
logger = logging.getLogger(__name__)


@dataclass
class AircraftPosition:
    """Represents a single ADS-B position update"""
    hex: str
    flight: Optional[str]
    lat: Optional[float]
    lon: Optional[float]
    altitude: Optional[int]
    speed: Optional[int]
    track: Optional[int]
    vertical_rate: Optional[int]
    squawk: Optional[str]
    timestamp: datetime
    receiver_id: str


class SBS1Parser:
    """
    Parser for SBS-1 BaseStation format from dump1090

    Format: MSG,transmission_type,session_id,aircraft_id,hex,flight_id,
            date_gen,time_gen,date_log,time_log,callsign,altitude,speed,
            track,lat,lon,vertical_rate,squawk,alert,emergency,spi,is_on_ground

    Example: MSG,3,111,11111,A12345,111111,2025/12/08,19:40:05.123,
             2025/12/08,19:40:05.123,,35000,450,180,40.1164,-88.2434,0,,,,,
    """

    @staticmethod
    def parse_line(line: str, receiver_id: str) -> Optional[AircraftPosition]:
        """Parse a single SBS-1 format line"""
        try:
            parts = line.strip().split(',')

            # We need at least the basic fields
            if len(parts) < 22 or parts[0] != 'MSG':
                return None

            transmission_type = parts[1]
            hex_code = parts[4].strip()

            # Skip if no hex code (essential identifier)
            if not hex_code:
                return None

            # Parse optional fields with safe conversion
            def safe_str(val: str) -> Optional[str]:
                val = val.strip()
                return val if val else None

            def safe_float(val: str) -> Optional[float]:
                val = val.strip()
                try:
                    return float(val) if val else None
                except ValueError:
                    return None

            def safe_int(val: str) -> Optional[int]:
                val = val.strip()
                try:
                    return int(float(val)) if val else None
                except ValueError:
                    return None

            # Extract fields based on position
            flight_raw = safe_str(parts[10]) if len(parts) > 10 else None
            # Truncate flight to 10 characters (database limit)
            # Filter out obviously invalid callsigns (single digits, etc.)
            if flight_raw and len(flight_raw.strip()) >= 2:
                flight = flight_raw[:10]
            else:
                flight = None
            altitude = safe_int(parts[11]) if len(parts) > 11 else None
            speed = safe_int(parts[12]) if len(parts) > 12 else None
            track = safe_int(parts[13]) if len(parts) > 13 else None
            lat = safe_float(parts[14]) if len(parts) > 14 else None
            lon = safe_float(parts[15]) if len(parts) > 15 else None
            vertical_rate = safe_int(parts[16]) if len(parts) > 16 else None
            squawk_raw = safe_str(parts[17]) if len(parts) > 17 else None
            # Truncate squawk to 4 characters (database limit)
            squawk = squawk_raw[:4] if squawk_raw else None

            # Parse timestamp (use logged time if available, otherwise generated time)
            timestamp = datetime.now(timezone.utc)
            if len(parts) > 9 and parts[8] and parts[9]:
                try:
                    date_str = parts[8].strip()
                    time_str = parts[9].strip()
                    timestamp_str = f"{date_str} {time_str}"
                    timestamp = datetime.strptime(timestamp_str, "%Y/%m/%d %H:%M:%S.%f")
                    timestamp = timestamp.replace(tzinfo=timezone.utc)
                except (ValueError, IndexError):
                    pass

            return AircraftPosition(
                hex=hex_code,
                flight=flight,
                lat=lat,
                lon=lon,
                altitude=altitude,
                speed=speed,
                track=track,
                vertical_rate=vertical_rate,
                squawk=squawk,
                timestamp=timestamp,
                receiver_id=receiver_id
            )

        except Exception as e:
            logger.error(f"Error parsing line: {e} - Line: {line[:100]}")
            return None


class AircraftDataManager:
    """
    Manages aircraft data updates to Supabase
    Handles deduplication and multiple receiver updates
    """

    def __init__(self, db_pool: psycopg2.pool.ThreadedConnectionPool):
        self.db_pool = db_pool
        self.last_updates: Dict[str, datetime] = {}  # hex -> last update time
        self.update_interval_seconds = 1  # Minimum seconds between DB updates per aircraft

    def should_update(self, hex_code: str, timestamp: datetime) -> bool:
        """
        Check if enough time has passed since last update for this aircraft
        This prevents flooding DB with duplicate data from multiple receivers
        """
        if hex_code not in self.last_updates:
            return True

        time_diff = (timestamp - self.last_updates[hex_code]).total_seconds()
        return time_diff >= self.update_interval_seconds

    def update_aircraft(self, position: AircraftPosition) -> bool:
        """
        Update aircraft data in database
        Returns True if successful, False otherwise
        """
        # Check if we should update (rate limiting)
        if not self.should_update(position.hex, position.timestamp):
            return False

        conn = None
        try:
            conn = self.db_pool.getconn()
            cursor = conn.cursor()

            # Update main real aircraft table with UPSERT
            cursor.execute("""
                INSERT INTO aircraft_real (
                    hex, flight, lat, lon, altitude, speed, track,
                    vertical_rate, squawk, receiver_id, seen_at
                )
                VALUES (%s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s)
                ON CONFLICT (hex)
                DO UPDATE SET
                    flight = COALESCE(EXCLUDED.flight, aircraft_real.flight),
                    lat = COALESCE(EXCLUDED.lat, aircraft_real.lat),
                    lon = COALESCE(EXCLUDED.lon, aircraft_real.lon),
                    altitude = COALESCE(EXCLUDED.altitude, aircraft_real.altitude),
                    speed = COALESCE(EXCLUDED.speed, aircraft_real.speed),
                    track = COALESCE(EXCLUDED.track, aircraft_real.track),
                    vertical_rate = COALESCE(EXCLUDED.vertical_rate, aircraft_real.vertical_rate),
                    squawk = COALESCE(EXCLUDED.squawk, aircraft_real.squawk),
                    receiver_id = EXCLUDED.receiver_id,
                    seen_at = EXCLUDED.seen_at
            """, (
                position.hex,
                position.flight,
                position.lat,
                position.lon,
                position.altitude,
                position.speed,
                position.track,
                position.vertical_rate,
                position.squawk,
                position.receiver_id,
                position.timestamp
            ))

            # Insert into history table if we have position data
            if position.lat is not None and position.lon is not None:
                try:
                    logger.debug(f"Inserting trail for {position.hex}: lat={position.lat}, lon={position.lon}")
                    cursor.execute("""
                        INSERT INTO aircraft_history_real (
                            hex, lat, lon, altitude, speed, track,
                            receiver_id, recorded_at
                        )
                        VALUES (%s, %s, %s, %s, %s, %s, %s, %s)
                    """, (
                        position.hex,
                        position.lat,
                        position.lon,
                        position.altitude,
                        position.speed,
                        position.track,
                        position.receiver_id,
                        position.timestamp
                    ))
                    logger.debug(f"Trail inserted successfully for {position.hex}")
                except Exception as trail_error:
                    logger.error(f"Failed to insert trail for {position.hex}: {trail_error}")
                    raise  # Re-raise to trigger rollback
            else:
                logger.debug(f"Skipping trail for {position.hex}: lat={position.lat}, lon={position.lon}")

            # Update or create session
            cursor.execute("""
                INSERT INTO aircraft_sessions_real (
                    hex, flight, receiver_id, first_seen, last_seen
                )
                VALUES (%s, %s, %s, %s, %s)
                ON CONFLICT (hex)
                DO UPDATE SET
                    flight = COALESCE(EXCLUDED.flight, aircraft_sessions_real.flight),
                    receiver_id = EXCLUDED.receiver_id,
                    last_seen = EXCLUDED.last_seen
            """, (
                position.hex,
                position.flight,
                position.receiver_id,
                position.timestamp,
                position.timestamp
            ))

            conn.commit()

            # Update last update timestamp
            self.last_updates[position.hex] = position.timestamp

            return True

        except Exception as e:
            if conn:
                conn.rollback()
            logger.error(f"Database error updating aircraft {position.hex}: {e}")
            return False

        finally:
            if conn:
                self.db_pool.putconn(conn)

    def cleanup_old_aircraft(self, timeout_minutes: int = 5):
        """Remove aircraft that haven't been seen recently"""
        conn = None
        try:
            conn = self.db_pool.getconn()
            cursor = conn.cursor()

            # Delete aircraft not seen in timeout_minutes
            cursor.execute("""
                DELETE FROM aircraft_real
                WHERE seen_at < NOW() - INTERVAL %s
            """, (f'{timeout_minutes} minutes',))

            deleted_count = cursor.rowcount

            # Mark sessions as ended
            cursor.execute("""
                UPDATE aircraft_sessions_real
                SET ended_at = NOW()
                WHERE hex IN (
                    SELECT hex FROM aircraft_sessions_real
                    WHERE last_seen < NOW() - INTERVAL %s
                    AND ended_at IS NULL
                )
            """, (f'{timeout_minutes} minutes',))

            conn.commit()

            if deleted_count > 0:
                logger.info(f"Cleaned up {deleted_count} old aircraft (not seen in {timeout_minutes}min)")

        except Exception as e:
            if conn:
                conn.rollback()
            logger.error(f"Error during cleanup: {e}")

        finally:
            if conn:
                self.db_pool.putconn(conn)

    def cleanup_old_history(self, hours_to_keep: int = 2):
        """Remove old position history to prevent database bloat"""
        conn = None
        try:
            conn = self.db_pool.getconn()
            cursor = conn.cursor()

            cursor.execute("""
                DELETE FROM aircraft_history_real
                WHERE recorded_at < NOW() - INTERVAL %s
            """, (f'{hours_to_keep} hours',))

            deleted_count = cursor.rowcount
            conn.commit()

            if deleted_count > 0:
                logger.info(f"Cleaned up {deleted_count} old history records (older than {hours_to_keep}h)")

        except Exception as e:
            if conn:
                conn.rollback()
            logger.error(f"Error during history cleanup: {e}")

        finally:
            if conn:
                self.db_pool.putconn(conn)


class ADSBStreamer:
    """Main ADS-B data streamer"""

    def __init__(self, host: str, port: int, receiver_id: str, db_url: str):
        self.host = host
        self.port = port
        self.receiver_id = receiver_id
        self.db_url = db_url
        self.running = False

        # Initialize database connection pool
        self.db_pool = psycopg2.pool.ThreadedConnectionPool(
            minconn=1,
            maxconn=5,
            dsn=db_url
        )

        self.data_manager = AircraftDataManager(self.db_pool)
        self.parser = SBS1Parser()

        # Statistics
        self.stats = {
            'messages_received': 0,
            'messages_parsed': 0,
            'db_updates': 0,
            'errors': 0
        }

        self.last_stats_time = time.time()
        self.last_cleanup_time = time.time()
        self.last_history_cleanup_time = time.time()

    def connect_to_dump1090(self) -> socket.socket:
        """Establish connection to dump1090"""
        logger.info(f"Connecting to dump1090 at {self.host}:{self.port} (receiver: {self.receiver_id})")

        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.settimeout(10)
        sock.connect((self.host, self.port))

        logger.info("Successfully connected to dump1090")
        return sock

    def print_stats(self):
        """Print periodic statistics"""
        current_time = time.time()
        elapsed = current_time - self.last_stats_time

        if elapsed >= 30:  # Print stats every 30 seconds
            msgs_per_sec = self.stats['messages_received'] / elapsed if elapsed > 0 else 0

            logger.info(
                f"Stats: {self.stats['messages_received']} msgs "
                f"({msgs_per_sec:.1f}/s), "
                f"{self.stats['messages_parsed']} parsed, "
                f"{self.stats['db_updates']} DB updates, "
                f"{self.stats['errors']} errors"
            )

            # Reset stats
            self.stats = {k: 0 for k in self.stats}
            self.last_stats_time = current_time

    def periodic_cleanup(self):
        """Run periodic cleanup tasks"""
        current_time = time.time()

        # Cleanup old aircraft every 1 minute
        if current_time - self.last_cleanup_time >= 60:
            self.data_manager.cleanup_old_aircraft(timeout_minutes=5)
            self.last_cleanup_time = current_time

        # Cleanup old history every 5 minutes
        if current_time - self.last_history_cleanup_time >= 300:
            self.data_manager.cleanup_old_history(hours_to_keep=2)
            self.last_history_cleanup_time = current_time

    def run(self):
        """Main streaming loop"""
        self.running = True
        buffer = ""

        logger.info("Starting ADS-B data streamer")
        logger.info(f"Receiver ID: {self.receiver_id}")
        logger.info("Press Ctrl+C to stop")

        while self.running:
            try:
                # Connect to dump1090
                sock = self.connect_to_dump1090()

                # Stream data
                while self.running:
                    try:
                        # Receive data
                        data = sock.recv(4096).decode('utf-8', errors='ignore')

                        if not data:
                            logger.warning("Connection closed by dump1090")
                            break

                        # Add to buffer and process complete lines
                        buffer += data
                        while '\n' in buffer:
                            line, buffer = buffer.split('\n', 1)

                            self.stats['messages_received'] += 1

                            # Parse line
                            position = self.parser.parse_line(line, self.receiver_id)

                            if position:
                                self.stats['messages_parsed'] += 1

                                # Update database
                                if self.data_manager.update_aircraft(position):
                                    self.stats['db_updates'] += 1

                        # Print stats periodically
                        self.print_stats()

                        # Run periodic cleanup
                        self.periodic_cleanup()

                    except socket.timeout:
                        continue
                    except Exception as e:
                        logger.error(f"Error processing data: {e}")
                        self.stats['errors'] += 1
                        time.sleep(1)

                sock.close()

                # Reconnect after a delay
                if self.running:
                    logger.info("Reconnecting in 5 seconds...")
                    time.sleep(5)

            except KeyboardInterrupt:
                logger.info("Received interrupt signal")
                self.running = False
                break
            except Exception as e:
                logger.error(f"Connection error: {e}")
                self.stats['errors'] += 1
                if self.running:
                    logger.info("Retrying in 10 seconds...")
                    time.sleep(10)

        # Cleanup
        logger.info("Shutting down...")
        if hasattr(self, 'db_pool'):
            self.db_pool.closeall()
        logger.info("Stopped")


def main():
    parser = argparse.ArgumentParser(
        description='Stream ADS-B data from dump1090 to Supabase',
        formatter_class=argparse.ArgumentDefaultsHelpFormatter
    )

    parser.add_argument(
        '--host',
        default='localhost',
        help='dump1090 host address'
    )

    parser.add_argument(
        '--port',
        type=int,
        default=30003,
        help='dump1090 SBS-1 port (default: 30003)'
    )

    parser.add_argument(
        '--receiver-id',
        default=socket.gethostname(),
        help='Unique identifier for this receiver (default: hostname)'
    )

    parser.add_argument(
        '--db-url',
        default=os.environ.get('DATABASE_URL'),
        help='PostgreSQL/Supabase connection URL (default: DATABASE_URL env var)'
    )

    args = parser.parse_args()

    # Validate database URL
    if not args.db_url:
        logger.error("Database URL not provided. Set DATABASE_URL environment variable or use --db-url")
        sys.exit(1)

    # Create and run streamer
    streamer = ADSBStreamer(
        host=args.host,
        port=args.port,
        receiver_id=args.receiver_id,
        db_url=args.db_url
    )

    streamer.run()


if __name__ == '__main__':
    main()
