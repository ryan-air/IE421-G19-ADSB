#!/usr/bin/env python3
"""
Test script to validate dump1090 and database connections
Run this before starting the main streamer to verify setup
"""

import socket
import sys
import os
import argparse
from datetime import datetime

try:
    import psycopg2
    from dotenv import load_dotenv
except ImportError:
    print("Error: Required packages not installed")
    print("Run: pip install psycopg2-binary python-dotenv")
    sys.exit(1)


def test_dump1090(host: str, port: int, timeout: int = 5) -> bool:
    """Test connection to dump1090"""
    print(f"\n[1/3] Testing dump1090 connection at {host}:{port}...")

    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.settimeout(timeout)
        sock.connect((host, port))

        print(f"✓ Successfully connected to dump1090")

        # Try to read some data
        print("  Attempting to read data (waiting up to 5 seconds)...")
        data = sock.recv(4096).decode('utf-8', errors='ignore')

        if data:
            lines = data.strip().split('\n')
            print(f"✓ Received {len(lines)} message(s)")

            # Show first message as example
            if lines:
                print(f"  Sample message: {lines[0][:80]}...")

            return True
        else:
            print("✗ Connected but no data received")
            print("  Make sure dump1090 is running and receiving aircraft signals")
            return False

    except socket.timeout:
        print("✗ Connection timeout")
        print(f"  Could not connect to {host}:{port} within {timeout} seconds")
        return False
    except ConnectionRefusedError:
        print("✗ Connection refused")
        print(f"  dump1090 is not listening on {host}:{port}")
        print("  Make sure dump1090 is running")
        return False
    except Exception as e:
        print(f"✗ Error: {e}")
        return False
    finally:
        try:
            sock.close()
        except:
            pass


def test_database(db_url: str) -> bool:
    """Test database connection"""
    print(f"\n[2/3] Testing database connection...")

    if not db_url:
        print("✗ DATABASE_URL not set")
        print("  Set it in .env file or as environment variable")
        return False

    # Mask password in output
    masked_url = db_url
    if '@' in db_url and ':' in db_url:
        parts = db_url.split('@')
        if len(parts) == 2:
            creds = parts[0].split(':')
            if len(creds) >= 2:
                masked_url = f"{creds[0]}:****@{parts[1]}"

    print(f"  URL: {masked_url}")

    try:
        conn = psycopg2.connect(db_url)
        cursor = conn.cursor()

        # Test basic query
        cursor.execute("SELECT NOW(), version()")
        timestamp, version = cursor.fetchone()

        print(f"✓ Successfully connected to database")
        print(f"  Server time: {timestamp}")
        print(f"  PostgreSQL: {version.split(',')[0]}")

        cursor.close()
        conn.close()

        return True

    except psycopg2.OperationalError as e:
        print(f"✗ Connection failed: {e}")
        print("  Check your DATABASE_URL and network connection")
        return False
    except Exception as e:
        print(f"✗ Error: {e}")
        return False


def test_database_tables(db_url: str) -> bool:
    """Test if required tables exist"""
    print(f"\n[3/3] Testing database schema...")

    required_tables = [
        'aircraft_real',
        'aircraft_history_real',
        'aircraft_sessions_real'
    ]

    try:
        conn = psycopg2.connect(db_url)
        cursor = conn.cursor()

        # Check for tables
        cursor.execute("""
            SELECT table_name
            FROM information_schema.tables
            WHERE table_schema = 'public'
            AND table_name IN %s
        """, (tuple(required_tables),))

        existing_tables = [row[0] for row in cursor.fetchall()]

        print(f"  Checking for required tables...")

        all_exist = True
        for table in required_tables:
            if table in existing_tables:
                print(f"  ✓ {table}")
            else:
                print(f"  ✗ {table} (missing)")
                all_exist = False

        if not all_exist:
            print("\n  Missing tables detected!")
            print("  Run the schema.sql script in Supabase SQL editor:")
            print("  1. Open Supabase dashboard → SQL Editor")
            print("  2. Copy contents of schema.sql")
            print("  3. Execute the query")

        cursor.close()
        conn.close()

        return all_exist

    except Exception as e:
        print(f"✗ Error checking tables: {e}")
        return False


def main():
    parser = argparse.ArgumentParser(
        description='Test ADS-B streamer connections'
    )

    parser.add_argument(
        '--host',
        default='localhost',
        help='dump1090 host (default: localhost)'
    )

    parser.add_argument(
        '--port',
        type=int,
        default=30003,
        help='dump1090 port (default: 30003)'
    )

    parser.add_argument(
        '--db-url',
        default=os.environ.get('DATABASE_URL'),
        help='Database URL (default: DATABASE_URL env var)'
    )

    parser.add_argument(
        '--skip-dump1090',
        action='store_true',
        help='Skip dump1090 test (test database only)'
    )

    args = parser.parse_args()

    # Try to load .env file
    if os.path.exists('.env'):
        load_dotenv()
        print("Loaded configuration from .env file")
        # Re-read DATABASE_URL after loading .env
        if not args.db_url:
            args.db_url = os.environ.get('DATABASE_URL')

    print("=" * 60)
    print("ADS-B Streamer Connection Test")
    print("=" * 60)

    results = {}

    # Test dump1090
    if not args.skip_dump1090:
        results['dump1090'] = test_dump1090(args.host, args.port)
    else:
        print("\n[1/3] Skipping dump1090 test")
        results['dump1090'] = None

    # Test database connection
    results['database'] = test_database(args.db_url)

    # Test database schema
    if results['database']:
        results['schema'] = test_database_tables(args.db_url)
    else:
        results['schema'] = False

    # Summary
    print("\n" + "=" * 60)
    print("Summary")
    print("=" * 60)

    if results['dump1090'] is not None:
        status = "✓ PASS" if results['dump1090'] else "✗ FAIL"
        print(f"dump1090:        {status}")

    status = "✓ PASS" if results['database'] else "✗ FAIL"
    print(f"Database:        {status}")

    status = "✓ PASS" if results['schema'] else "✗ FAIL"
    print(f"Schema:          {status}")

    # Overall result
    print()
    if all(v for v in results.values() if v is not None):
        print("✓ All tests passed! Ready to run the streamer.")
        print("\nStart the streamer with:")
        print("  python3 adsb_streamer.py")
        print("  or")
        print("  ./run.sh")
        return 0
    else:
        print("✗ Some tests failed. Please fix the issues above.")
        return 1


if __name__ == '__main__':
    sys.exit(main())
