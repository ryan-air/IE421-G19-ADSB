#!/usr/bin/env python3
"""
ADS-B Data Streamer for dump1090 - OPTIMIZED VERSION
Streams real ADS-B aircraft data from dump1090 SBS-1 BaseStation format to Supabase
Uses batch inserts and CSV COPY for 100-1000x performance improvement
"""

import socket
import sys
import os
import time
import logging
from datetime import datetime, timezone
from typing import Dict, Optional, Set, List
from dataclasses import dataclass, astuple
import psycopg2
from psycopg2 import pool
from psycopg2.extras import execute_values
import argparse
from io import StringIO

# Load environment variables from .env file if present
try:
    from dotenv import load_dotenv
    load_dotenv()
except ImportError:
    pass  # dotenv not installed, will use system env vars only

# Configure logging
logging.basicConfig(
    level=logging.INFO,
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
            hex_code = parts[4].strip().upper()

            # Validate hex code (must be exactly 6 characters, alphanumeric)
            if not hex_code or len(hex_code) != 6 or not hex_code.isalnum():
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
            # dump1090 sends timestamps in system local time, we need to convert to UTC
            timestamp = datetime.now(timezone.utc)
            if len(parts) > 9 and parts[8] and parts[9]:
                try:
                    date_str = parts[8].strip()
                    time_str = parts[9].strip()
                    timestamp_str = f"{date_str} {time_str}"
                    # Parse as naive datetime (no timezone)
                    local_time = datetime.strptime(timestamp_str, "%Y/%m/%d %H:%M:%S.%f")
                    # Convert from local time to UTC by getting the current UTC offset
                    # This automatically handles DST changes
                    utc_offset = datetime.now().astimezone().utcoffset()
                    timestamp = local_time - utc_offset
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
    OPTIMIZED: Uses batch inserts and CSV COPY for massive performance improvement
    """

    def __init__(self, db_pool: psycopg2.pool.ThreadedConnectionPool,
                 batch_size: int = 100,
                 flush_interval_seconds: float = 1.0,
                 use_csv_copy: bool = True):
        self.db_pool = db_pool
        self.batch_size = batch_size
        self.flush_interval_seconds = flush_interval_seconds
        self.use_csv_copy = use_csv_copy

        # Buffers for batch processing
        self.aircraft_buffer: Dict[str, AircraftPosition] = {}  # hex -> latest position
        self.session_buffer: Dict[str, AircraftPosition] = {}   # hex -> latest position

        self.last_flush_time = time.time()
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

    def add_to_buffer(self, position: AircraftPosition) -> bool:
        """
        Add aircraft position to buffer for batch processing
        Returns True if added, False if rate-limited
        """
        # Check if we should update (rate limiting)
        if not self.should_update(position.hex, position.timestamp):
            return False

        # Add to buffers (overwrites if aircraft already in buffer with newer data)
        self.aircraft_buffer[position.hex] = position
        self.session_buffer[position.hex] = position

        # Update last update timestamp
        self.last_updates[position.hex] = position.timestamp

        # Auto-flush if buffer is full
        if len(self.aircraft_buffer) >= self.batch_size:
            self.flush_buffers()

        return True

    def flush_buffers(self) -> int:
        """
        Flush buffered aircraft data to database using batch operations
        Returns number of records inserted
        """
        if not self.aircraft_buffer:
            return 0

        conn = None
        total_inserted = 0

        try:
            conn = self.db_pool.getconn()
            cursor = conn.cursor()

            # Convert buffers to lists
            aircraft_list = list(self.aircraft_buffer.values())
            session_list = list(self.session_buffer.values())

            if self.use_csv_copy:
                # METHOD 1: CSV COPY - Fastest method (100-1000x faster than individual inserts)
                total_inserted += self._bulk_insert_csv(cursor, aircraft_list, session_list)
            else:
                # METHOD 2: Batch INSERT with execute_values (10-100x faster than individual inserts)
                total_inserted += self._bulk_insert_execute_values(cursor, aircraft_list, session_list)

            conn.commit()

            # Clear buffers
            self.aircraft_buffer.clear()
            self.session_buffer.clear()
            self.last_flush_time = time.time()

            return total_inserted

        except Exception as e:
            if conn:
                conn.rollback()
            logger.error(f"Database error during bulk insert: {e}")
            return 0

        finally:
            if conn:
                self.db_pool.putconn(conn)

    def _bulk_insert_csv(self, cursor, aircraft_list: List[AircraftPosition],
                         session_list: List[AircraftPosition]) -> int:
        """
        Bulk insert using PostgreSQL COPY command with CSV
        This is the FASTEST method - 100-1000x faster than individual inserts
        """
        total_inserted = 0

        # Prepare CSV data for aircraft_real table
        aircraft_csv = StringIO()
        for pos in aircraft_list:
            # Format: hex, flight, lat, lon, altitude, speed, track, vertical_rate, squawk, receiver_id, seen_at
            aircraft_csv.write(f"{pos.hex}\t"
                             f"{pos.flight or ''}\t"
                             f"{pos.lat if pos.lat is not None else ''}\t"
                             f"{pos.lon if pos.lon is not None else ''}\t"
                             f"{pos.altitude if pos.altitude is not None else ''}\t"
                             f"{pos.speed if pos.speed is not None else ''}\t"
                             f"{pos.track if pos.track is not None else ''}\t"
                             f"{pos.vertical_rate if pos.vertical_rate is not None else ''}\t"
                             f"{pos.squawk or ''}\t"
                             f"{pos.receiver_id}\t"
                             f"{pos.timestamp.isoformat()}\n")

        aircraft_csv.seek(0)

        # Use a temporary table to handle UPSERT with COPY
        # PostgreSQL's COPY doesn't support ON CONFLICT, so we use a temp table
        cursor.execute("""
            CREATE TEMP TABLE temp_aircraft_real (LIKE aircraft_real INCLUDING DEFAULTS)
            ON COMMIT DROP
        """)

        # COPY data into temp table
        cursor.copy_from(
            aircraft_csv,
            'temp_aircraft_real',
            columns=('hex', 'flight', 'lat', 'lon', 'altitude', 'speed', 'track',
                    'vertical_rate', 'squawk', 'receiver_id', 'seen_at'),
            null=''
        )
        total_inserted += len(aircraft_list)

        # UPSERT from temp table to main table
        cursor.execute("""
            INSERT INTO aircraft_real (hex, flight, lat, lon, altitude, speed, track,
                                      vertical_rate, squawk, receiver_id, seen_at)
            SELECT hex, flight, lat, lon, altitude, speed, track,
                   vertical_rate, squawk, receiver_id, seen_at
            FROM temp_aircraft_real
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
        """)

        # Prepare CSV data for aircraft_sessions_real table
        session_csv = StringIO()
        for pos in session_list:
            # Format: hex, flight, receiver_id, first_seen, last_seen
            session_csv.write(f"{pos.hex}\t"
                            f"{pos.flight or ''}\t"
                            f"{pos.receiver_id}\t"
                            f"{pos.timestamp.isoformat()}\t"
                            f"{pos.timestamp.isoformat()}\n")

        session_csv.seek(0)

        # Same temp table approach for sessions
        cursor.execute("""
            CREATE TEMP TABLE temp_aircraft_sessions_real (LIKE aircraft_sessions_real INCLUDING DEFAULTS)
            ON COMMIT DROP
        """)

        cursor.copy_from(
            session_csv,
            'temp_aircraft_sessions_real',
            columns=('hex', 'flight', 'receiver_id', 'first_seen', 'last_seen'),
            null=''
        )
        total_inserted += len(session_list)

        cursor.execute("""
            INSERT INTO aircraft_sessions_real (hex, flight, receiver_id, first_seen, last_seen)
            SELECT hex, flight, receiver_id, first_seen, last_seen
            FROM temp_aircraft_sessions_real
            ON CONFLICT (hex)
            DO UPDATE SET
                flight = COALESCE(EXCLUDED.flight, aircraft_sessions_real.flight),
                receiver_id = EXCLUDED.receiver_id,
                last_seen = EXCLUDED.last_seen
        """)

        # Prepare CSV data for aircraft_position_history table (for Kalman filter)
        # Only insert positions with valid lat/lon/altitude for tracking
        history_csv = StringIO()
        history_count = 0
        for pos in aircraft_list:
            # Only record history if we have valid position data
            if pos.lat is not None and pos.lon is not None and pos.altitude is not None:
                # Format: hex, lat, lon, altitude, speed, track, vertical_rate, timestamp
                history_csv.write(f"{pos.hex}\t"
                                f"{pos.lat}\t"
                                f"{pos.lon}\t"
                                f"{pos.altitude}\t"
                                f"{pos.speed if pos.speed is not None else ''}\t"
                                f"{pos.track if pos.track is not None else ''}\t"
                                f"{pos.vertical_rate if pos.vertical_rate is not None else ''}\t"
                                f"{pos.timestamp.isoformat()}\n")
                history_count += 1

        # Insert position history (no temp table needed - simple insert, no conflicts)
        if history_count > 0:
            history_csv.seek(0)
            cursor.copy_from(
                history_csv,
                'aircraft_position_history',
                columns=('hex', 'lat', 'lon', 'altitude', 'speed', 'track', 'vertical_rate', 'timestamp'),
                null=''
            )
            total_inserted += history_count

        return total_inserted

    def _bulk_insert_execute_values(self, cursor, aircraft_list: List[AircraftPosition],
                                    session_list: List[AircraftPosition]) -> int:
        """
        Bulk insert using execute_values (batch INSERT)
        10-100x faster than individual inserts, but slower than CSV COPY
        """
        total_inserted = 0

        # Bulk insert aircraft data
        if aircraft_list:
            aircraft_values = [
                (pos.hex, pos.flight, pos.lat, pos.lon, pos.altitude, pos.speed,
                 pos.track, pos.vertical_rate, pos.squawk, pos.receiver_id, pos.timestamp)
                for pos in aircraft_list
            ]

            execute_values(
                cursor,
                """
                INSERT INTO aircraft_real (hex, flight, lat, lon, altitude, speed, track,
                                          vertical_rate, squawk, receiver_id, seen_at)
                VALUES %s
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
                """,
                aircraft_values
            )
            total_inserted += len(aircraft_values)

        # Bulk insert session data
        if session_list:
            session_values = [
                (pos.hex, pos.flight, pos.receiver_id, pos.timestamp, pos.timestamp)
                for pos in session_list
            ]

            execute_values(
                cursor,
                """
                INSERT INTO aircraft_sessions_real (hex, flight, receiver_id, first_seen, last_seen)
                VALUES %s
                ON CONFLICT (hex)
                DO UPDATE SET
                    flight = COALESCE(EXCLUDED.flight, aircraft_sessions_real.flight),
                    receiver_id = EXCLUDED.receiver_id,
                    last_seen = EXCLUDED.last_seen
                """,
                session_values
            )
            total_inserted += len(session_values)

        # Bulk insert position history (for Kalman filter)
        # Only insert positions with valid lat/lon/altitude
        history_values = [
            (pos.hex, pos.lat, pos.lon, pos.altitude, pos.speed, pos.track, pos.vertical_rate, pos.timestamp)
            for pos in aircraft_list
            if pos.lat is not None and pos.lon is not None and pos.altitude is not None
        ]

        if history_values:
            execute_values(
                cursor,
                """
                INSERT INTO aircraft_position_history (hex, lat, lon, altitude, speed, track, vertical_rate, timestamp)
                VALUES %s
                """,
                history_values
            )
            total_inserted += len(history_values)

        return total_inserted

    def periodic_flush(self):
        """Flush buffers if enough time has passed"""
        current_time = time.time()
        if current_time - self.last_flush_time >= self.flush_interval_seconds:
            if self.aircraft_buffer:
                count = self.flush_buffers()
                if count > 0:
                    logger.debug(f"Auto-flushed {count} records after {self.flush_interval_seconds}s")

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
        """Remove old position history to prevent database bloat

        Args:
            hours_to_keep: Hours of history to keep (0 = keep all history forever)
        """
        # If hours_to_keep is 0, don't delete anything (keep all history)
        if hours_to_keep == 0:
            logger.debug("History cleanup disabled (hours_to_keep=0)")
            return

        conn = None
        try:
            conn = self.db_pool.getconn()
            cursor = conn.cursor()

            # Clean up old aircraft_history_real if it exists
            cursor.execute("""
                SELECT EXISTS (
                    SELECT FROM information_schema.tables
                    WHERE table_name = 'aircraft_history_real'
                )
            """)
            if cursor.fetchone()[0]:
                cursor.execute("""
                    DELETE FROM aircraft_history_real
                    WHERE recorded_at < NOW() - INTERVAL %s
                """, (f'{hours_to_keep} hours',))

            # Clean up aircraft_position_history
            # Use same retention period as aircraft_history_real
            cursor.execute("""
                DELETE FROM aircraft_position_history
                WHERE timestamp < NOW() - INTERVAL %s
            """, (f'{hours_to_keep} hours',))

            deleted_count = cursor.rowcount
            conn.commit()

            if deleted_count > 0:
                logger.info(f"Cleaned up {deleted_count} old position history records (older than {hours_to_keep}h)")

        except Exception as e:
            if conn:
                conn.rollback()
            logger.error(f"Error during history cleanup: {e}")

        finally:
            if conn:
                self.db_pool.putconn(conn)

    def cleanup_old_history_legacy(self, hours_to_keep: int = 2):
        """DEPRECATED: Old method - kept for backwards compatibility"""
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
    """Main ADS-B data streamer - OPTIMIZED VERSION"""

    def __init__(self, host: str, port: int, receiver_id: str, db_url: str,
                 cleanup_interval_minutes: int = 5,
                 history_cleanup_hours: int = 2,
                 batch_size: int = 100,
                 flush_interval_seconds: float = 1.0,
                 use_csv_copy: bool = True):
        self.host = host
        self.port = port
        self.receiver_id = receiver_id
        self.db_url = db_url
        self.running = False

        # Cleanup configuration
        self.cleanup_interval_minutes = cleanup_interval_minutes
        self.history_cleanup_hours = history_cleanup_hours

        # Performance configuration
        self.batch_size = batch_size
        self.flush_interval_seconds = flush_interval_seconds
        self.use_csv_copy = use_csv_copy

        # Initialize database connection pool
        self.db_pool = psycopg2.pool.ThreadedConnectionPool(
            minconn=1,
            maxconn=5,
            dsn=db_url
        )

        self.data_manager = AircraftDataManager(
            self.db_pool,
            batch_size=batch_size,
            flush_interval_seconds=flush_interval_seconds,
            use_csv_copy=use_csv_copy
        )
        self.parser = SBS1Parser()

        # Statistics
        self.stats = {
            'messages_received': 0,
            'messages_parsed': 0,
            'buffered_updates': 0,
            'db_flushes': 0,
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
            buffer_size = len(self.data_manager.aircraft_buffer)

            logger.info(
                f"Stats: {self.stats['messages_received']} msgs "
                f"({msgs_per_sec:.1f}/s), "
                f"{self.stats['messages_parsed']} parsed, "
                f"{self.stats['buffered_updates']} buffered, "
                f"{self.stats['db_flushes']} DB flushes, "
                f"buffer={buffer_size}, "
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
            self.data_manager.cleanup_old_aircraft(timeout_minutes=self.cleanup_interval_minutes)
            self.last_cleanup_time = current_time

        # Cleanup old position history every 2 minutes (for Kalman filter)
        # This runs regardless of history_cleanup_hours setting
        if current_time - self.last_history_cleanup_time >= 120:
            self.data_manager.cleanup_old_history(hours_to_keep=self.history_cleanup_hours)
            self.last_history_cleanup_time = current_time

    def run(self):
        """Main streaming loop"""
        self.running = True
        buffer = ""

        logger.info("Starting ADS-B data streamer (OPTIMIZED VERSION)")
        logger.info(f"Receiver ID: {self.receiver_id}")
        logger.info(f"Batch size: {self.batch_size}, Flush interval: {self.flush_interval_seconds}s")
        logger.info(f"Using {'CSV COPY' if self.use_csv_copy else 'execute_values'} for bulk inserts")
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

                                # Add to buffer instead of immediate DB insert
                                if self.data_manager.add_to_buffer(position):
                                    self.stats['buffered_updates'] += 1

                        # Periodic flush
                        self.data_manager.periodic_flush()

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

                # Flush any remaining data before reconnecting
                if self.data_manager.aircraft_buffer:
                    count = self.data_manager.flush_buffers()
                    self.stats['db_flushes'] += 1
                    logger.info(f"Flushed {count} records before reconnect")

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

        # Cleanup - flush remaining data
        logger.info("Shutting down...")
        if self.data_manager.aircraft_buffer:
            count = self.data_manager.flush_buffers()
            logger.info(f"Final flush: {count} records")

        if hasattr(self, 'db_pool'):
            self.db_pool.closeall()
        logger.info("Stopped")


def main():
    parser = argparse.ArgumentParser(
        description='Stream ADS-B data from dump1090 to Supabase (OPTIMIZED)',
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

    parser.add_argument(
        '--cleanup-interval',
        type=int,
        default=5,
        help='Minutes of inactivity before removing aircraft from tracking (default: 5)'
    )

    parser.add_argument(
        '--history-cleanup',
        type=int,
        default=0,
        help='Hours to keep position history (0 = disable history, default: 0)'
    )

    parser.add_argument(
        '--batch-size',
        type=int,
        default=100,
        help='Number of records to batch before flushing to DB (default: 100)'
    )

    parser.add_argument(
        '--flush-interval',
        type=float,
        default=1.0,
        help='Seconds between auto-flushes (default: 1.0)'
    )

    parser.add_argument(
        '--use-execute-values',
        action='store_true',
        help='Use execute_values instead of CSV COPY (slower but more compatible)'
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
        db_url=args.db_url,
        cleanup_interval_minutes=args.cleanup_interval,
        history_cleanup_hours=args.history_cleanup,
        batch_size=args.batch_size,
        flush_interval_seconds=args.flush_interval,
        use_csv_copy=not args.use_execute_values
    )

    logger.info(f"Configuration: cleanup={args.cleanup_interval}min, history={args.history_cleanup}h, "
                f"batch={args.batch_size}, flush={args.flush_interval}s")
    streamer.run()


if __name__ == '__main__':
    main()
