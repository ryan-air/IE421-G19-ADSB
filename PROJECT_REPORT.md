# ADS-B Aircraft Tracking System - Technical Project Report

## Table of Contents

1. [Executive Summary](#executive-summary)
2. [ADS-B Protocol Overview](#ads-b-protocol-overview)
3. [System Architecture](#system-architecture)
4. [Data Reception and Processing Pipeline](#data-reception-and-processing-pipeline)
5. [Database Design and Schema](#database-design-and-schema)
6. [Backend API Implementation](#backend-api-implementation)
7. [Frontend Visualization](#frontend-visualization)
8. [Kalman Filter and Collision Detection](#kalman-filter-and-collision-detection)
9. [Performance Optimizations](#performance-optimizations)
10. [Deployment and Operations](#deployment-and-operations)
11. [Limitations and Future Work](#limitations-and-future-work)

---

## Executive Summary

This project implements a real-time aircraft tracking system using Automatic Dependent Surveillance-Broadcast (ADS-B) signals. Aircraft equipped with ADS-B transponders broadcast their position, altitude, velocity, and identification data on 1090 MHz. The system receives these signals via an RTL-SDR software-defined radio, processes the data through a Python streaming service, stores it in a PostgreSQL database, and provides interactive 2D and 3D web-based visualizations. Advanced features include Kalman filter-based trajectory prediction and probabilistic collision detection.

**Key Technologies:**
- Hardware: RTL-SDR USB dongle, Raspberry Pi
- Data Reception: dump1090 (ADS-B decoder)
- Backend: Python 3.9+ (data streaming), Node.js 20+ (web server)
- Database: PostgreSQL (Supabase cloud hosting)
- Frontend: Leaflet.js (2D maps), Cesium.js (3D globe), vanilla JavaScript
- Algorithms: Kalman filtering, probability cone intersection

**Performance Achievements:**
- Database write throughput: 5,000 operations/second (100-1000x improvement via CSV COPY)
- API cache hit rate: 75% (reducing response time from 800ms to 50ms)
- Real-time updates: 2-second polling interval with smooth interpolation
- Collision detection: Processes all aircraft pairs in <500ms

---

## ADS-B Protocol Overview

### What is ADS-B?

Automatic Dependent Surveillance-Broadcast (ADS-B) is an aviation surveillance technology mandated by the FAA for aircraft operating in most U.S. airspace since January 1, 2020. Unlike radar, which requires ground stations to actively interrogate aircraft, ADS-B allows aircraft to autonomously broadcast their position and flight parameters.

**Key Characteristics:**
- **Frequency:** 1090 MHz (1090ES - Extended Squitter)
- **Broadcast Rate:** Typically 2 messages per second per aircraft
- **Range:** Line-of-sight, typically 150-250 nautical miles at altitude
- **No Authentication:** Messages are unencrypted and publicly receivable
- **GPS-Based:** Position determined from onboard GPS, not ground radar

### ADS-B Message Types

ADS-B uses Mode S Extended Squitter format with various message types (DF17):

**Type Code 0-4:** Aircraft identification and category
- Callsign (e.g., "UAL123")
- Aircraft category (e.g., light, medium, heavy)

**Type Code 9-18:** Airborne position
- Latitude and longitude (CPR encoded)
- Altitude (25-foot or 100-foot resolution)
- Time synchronization

**Type Code 19:** Airborne velocity
- Ground speed (knots)
- Track angle (degrees)
- Vertical rate (feet per minute)
- Source: GPS or inertial

**Type Code 20-22:** Surface position (for taxiing aircraft)

### SBS-1 BaseStation Protocol

This project uses the SBS-1 protocol, a human-readable format emitted by dump1090 on TCP port 30003. Each message is a comma-separated line with 22 fields.

**Message Format:**
```
MSG,<transmission_type>,<session_id>,<aircraft_id>,<hex_ident>,<flight_id>,<date_generated>,<time_generated>,<date_logged>,<time_logged>,<callsign>,<altitude>,<ground_speed>,<track>,<lat>,<lon>,<vertical_rate>,<squawk>,<alert>,<emergency>,<spi>,<is_on_ground>
```

**Example Messages:**

**MSG,3 (Airborne Position):**
```
MSG,3,111,11111,A12345,111111,2025/12/20,19:40:05.123,2025/12/20,19:40:05.123,,35000,450,180,41.8781,-87.6298,0,,,,,0
```
- Hex: A12345
- Altitude: 35,000 feet
- Speed: 450 knots
- Track: 180 degrees (due south)
- Position: 41.8781°N, 87.6298°W (Chicago area)
- Vertical rate: 0 fpm (level flight)

**MSG,1 (Aircraft Identification):**
```
MSG,1,111,11111,A12345,111111,2025/12/20,19:40:03.456,2025/12/20,19:40:03.456,UAL123,,,,,,,,,,,
```
- Hex: A12345
- Callsign: UAL123

**MSG,4 (Airborne Velocity):**
```
MSG,4,111,11111,A12345,111111,2025/12/20,19:40:04.789,2025/12/20,19:40:04.789,,,450,180,,,-1500,,,,,
```
- Speed: 450 knots
- Track: 180 degrees
- Vertical rate: -1500 fpm (descending)

### ADS-B Data Fields Explained

**ICAO Hex Code (hex):**
- 6-character hexadecimal identifier (e.g., A12345)
- Unique to each aircraft, assigned by country of registration
- U.S. aircraft typically start with 'A' (e.g., A00001-AFFFFF)
- Never changes for the aircraft's lifetime

**Callsign (flight):**
- 8-character alphanumeric identifier (e.g., UAL123, N12345)
- For commercial flights: airline code + flight number
- For general aviation: aircraft registration number
- Can change between flights for the same aircraft

**Altitude:**
- Pressure altitude in feet (not GPS altitude)
- Referenced to 29.92 inHg standard pressure
- Granularity: 25 feet (Mode C) or 100 feet (older transponders)
- Range: -1,000 to 50,000+ feet

**Ground Speed:**
- Horizontal speed over ground in knots
- Includes wind effects (unlike airspeed)
- Typical values: 0-600 knots (commercial jets cruise 450-550 kts)

**Track (Heading):**
- Direction of movement over ground in degrees (0-359)
- 0/360 = North, 90 = East, 180 = South, 270 = West
- True north reference, not magnetic
- Different from aircraft heading if wind present

**Vertical Rate:**
- Rate of altitude change in feet per minute (fpm)
- Positive = climbing, negative = descending, 0 = level
- Typical climb: 1,500-3,000 fpm
- Typical descent: 1,000-2,500 fpm

**Squawk Code:**
- 4-digit octal transponder code (0000-7777)
- Assigned by air traffic control
- Special codes: 7500 (hijack), 7600 (radio failure), 7700 (emergency)
- VFR aircraft often use 1200 (U.S.) or 7000 (Europe)

**Latitude and Longitude:**
- WGS84 datum (same as GPS)
- Precision: typically 6-8 decimal places (meter-level accuracy)
- Position accuracy: ~10-50 meters horizontal (GPS dependent)

### Reception Hardware and Software

**RTL-SDR Dongle:**
- Software-defined radio based on Realtek RTL2832U chip
- Frequency range: 24-1766 MHz (covers 1090 MHz ADS-B)
- Sample rate: Up to 3.2 MS/s
- ADC resolution: 8 bits
- Cost: $25-40 USD
- Antenna: Quarter-wave monopole (6.9 cm) or 1090 MHz optimized antenna

**dump1090:**
- Open-source Mode S decoder by antirez and FlightAware
- Demodulates 1090 MHz PPM (Pulse Position Modulation)
- Implements CRC-24 error detection
- Decodes Extended Squitter (ADS-B) messages
- Outputs SBS-1 format on TCP port 30003
- Web interface on port 8080 for local viewing

**Installation (Raspberry Pi):**
```bash
sudo apt-get update
sudo apt-get install rtl-sdr dump1090-fa
sudo systemctl enable dump1090-fa
sudo systemctl start dump1090-fa
```

**Verification:**
```bash
# Check if dump1090 is receiving
telnet localhost 30003

# Should see lines like:
# MSG,3,111,11111,A12345,...
```

---

## System Architecture

### High-Level Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                         AIRCRAFT                                 │
│              (ADS-B Transponder - 1090 MHz)                      │
└──────────────────────────┬──────────────────────────────────────┘
                           │ Radio Waves
                           ▼
┌─────────────────────────────────────────────────────────────────┐
│                   RECEPTION LAYER (Raspberry Pi)                 │
│  ┌────────────────┐          ┌────────────────────────┐         │
│  │  RTL-SDR       │  USB     │    dump1090            │         │
│  │  (1090 MHz     │─────────▶│    (Mode S Decoder)    │         │
│  │   Receiver)    │          │    Port 30003          │         │
│  └────────────────┘          └──────────┬─────────────┘         │
└────────────────────────────────────────┼───────────────────────┘
                                         │ TCP/IP (SBS-1 Protocol)
                                         ▼
┌─────────────────────────────────────────────────────────────────┐
│              DATA STREAMING LAYER (Raspberry Pi)                 │
│  ┌──────────────────────────────────────────────────┐           │
│  │  adsb_streamer_optimized.py                      │           │
│  │  • SBS-1 parser                                  │           │
│  │  • Batch buffering (100 records)                 │           │
│  │  • Rate limiting (1s per aircraft)               │           │
│  │  • CSV COPY bulk insert                          │           │
│  └──────────────────────┬───────────────────────────┘           │
└─────────────────────────┼─────────────────────────────────────┘
                          │ PostgreSQL Protocol (SSL)
                          ▼
┌─────────────────────────────────────────────────────────────────┐
│                DATABASE LAYER (Supabase Cloud)                   │
│  ┌──────────────────────────────────────────────────┐           │
│  │  PostgreSQL 15+ with Session Pooler              │           │
│  │  • aircraft_real (current state)                 │           │
│  │  • aircraft_position_history (trails)            │           │
│  │  • aircraft_sessions_real (tracking)             │           │
│  │  • Indexes on seen_at, lat/lon, hex              │           │
│  └──────────────────────┬───────────────────────────┘           │
└─────────────────────────┼─────────────────────────────────────┘
                          │ REST API (HTTPS)
                          ▼
┌─────────────────────────────────────────────────────────────────┐
│           APPLICATION SERVER LAYER (Railway/Cloud)               │
│  ┌──────────────────────────────────────────────────┐           │
│  │  Node.js + Express server.js                     │           │
│  │  • GET /api/aircraft (basic data)                │           │
│  │  • GET /api/aircraft/enriched (cached)           │           │
│  │  • GET /api/statistics                           │           │
│  │  • GET /api/collisions/kalman                    │           │
│  │  • In-memory cache (2-3s TTL)                    │           │
│  │  • Kalman filter computation                     │           │
│  │  • Probability cone generation                   │           │
│  └──────────────────────┬───────────────────────────┘           │
└─────────────────────────┼─────────────────────────────────────┘
                          │ HTTPS
                          ▼
┌─────────────────────────────────────────────────────────────────┐
│              FRONTEND LAYER (Web Browser)                        │
│  ┌────────────────────┐      ┌────────────────────┐            │
│  │  2D View           │      │  3D View           │            │
│  │  (Leaflet.js)      │      │  (Cesium.js)       │            │
│  │  • Aircraft icons  │      │  • 3D globe        │            │
│  │  • Flight trails   │      │  • Terrain         │            │
│  │  • Prediction cones│      │  • OSM buildings   │            │
│  │  • Collision alerts│      │  • Helicopter GLB  │            │
│  │  • 2s polling      │      │  • Touch controls  │            │
│  └────────────────────┘      └────────────────────┘            │
└─────────────────────────────────────────────────────────────────┘
```

### Component Responsibilities

**Reception Layer (Raspberry Pi):**
- Hardware signal acquisition (RTL-SDR)
- Mode S demodulation and decoding (dump1090)
- SBS-1 protocol formatting
- Local web interface for monitoring

**Data Streaming Layer (Python):**
- TCP connection to dump1090 port 30003
- SBS-1 message parsing and validation
- Data normalization (timestamps, coordinates)
- Batch buffering and rate limiting
- High-performance database insertion
- Session management (first/last seen tracking)
- Automatic cleanup of stale aircraft

**Database Layer (PostgreSQL):**
- Current aircraft state storage (one row per hex)
- Position history for trail visualization
- Session tracking for analytics
- Indexed queries for fast retrieval
- Geospatial indexing for range queries

**Application Server Layer (Node.js):**
- RESTful API endpoints
- Database query execution
- Server-side Kalman filter computation
- Probability cone generation
- Collision detection algorithm
- Response caching (in-memory)
- Static file serving

**Frontend Layer (JavaScript):**
- AJAX polling for real-time updates
- 2D map rendering (Leaflet)
- 3D globe rendering (Cesium)
- Smooth marker animation
- User interface controls
- Statistics dashboard
- Collision visualization

### Data Flow Example

**Scenario:** Aircraft UAL123 (hex A12345) transmits position

1. **T+0ms:** Aircraft ADS-B transponder broadcasts on 1090 MHz
2. **T+1ms:** RTL-SDR receives signal, passes to dump1090
3. **T+5ms:** dump1090 decodes, outputs SBS-1 on port 30003
4. **T+6ms:** Python streamer receives: `MSG,3,111,11111,A12345,111111,...,UAL123,35000,450,180,41.8781,-87.6298,...`
5. **T+7ms:** Parser extracts: `{hex: 'A12345', flight: 'UAL123', lat: 41.8781, lon: -87.6298, altitude: 35000, speed: 450, track: 180, ...}`
6. **T+8ms:** Added to buffer (99 other aircraft already in buffer)
7. **T+1000ms:** Buffer reaches flush_interval (1.0s), triggers CSV COPY
8. **T+1050ms:** Database bulk UPSERT completes (100 aircraft updated)
9. **T+2000ms:** Web browser polls `/api/aircraft/enriched`
10. **T+2010ms:** Server checks cache (miss), queries database
11. **T+2050ms:** Server fetches aircraft + trails in single query
12. **T+2100ms:** Server computes probability cones (cached per aircraft)
13. **T+2150ms:** Response sent to browser with enriched data
14. **T+2160ms:** Frontend updates map marker for UAL123
15. **T+2200ms:** Smooth 2-second animation to new position begins

---

## Data Reception and Processing Pipeline

### Stage 1: SBS-1 Message Reception

**File:** `ads-b-receiver/adsb_streamer_optimized.py` (lines 599-756)

**Connection Establishment:**
```python
def connect_to_dump1090(host='localhost', port=30003):
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.connect((host, port))
    sock.settimeout(10)
    return sock
```

**Message Reading Loop:**
```python
def read_sbs1_messages(sock):
    buffer = ""
    while True:
        data = sock.recv(4096).decode('utf-8', errors='ignore')
        buffer += data

        while '\n' in buffer:
            line, buffer = buffer.split('\n', 1)
            yield line.strip()
```

**Error Handling:**
- Socket timeout: 10-second timeout, reconnects on failure
- Decode errors: Ignores invalid UTF-8 characters
- Connection loss: Automatic reconnection with exponential backoff

### Stage 2: SBS-1 Protocol Parsing

**File:** `ads-b-receiver/adsb_streamer_optimized.py` (lines 54-155)

**Parser Class:**
```python
class SBS1Parser:
    def parse(self, line):
        if not line.startswith('MSG,'):
            return None

        parts = line.split(',')
        if len(parts) < 22:
            return None  # Malformed message

        msg_type = int(parts[1])

        # MSG,1 = Aircraft ID, MSG,3 = Position, MSG,4 = Velocity
        if msg_type not in [1, 3, 4]:
            return None

        # Extract fields
        hex_ident = parts[4].strip()
        callsign = parts[10].strip() if len(parts[10].strip()) >= 2 else None
        altitude = safe_int(parts[11])
        speed = safe_int(parts[12])
        track = safe_int(parts[13])
        lat = safe_float(parts[14])
        lon = safe_float(parts[15])
        vertical_rate = safe_int(parts[16])
        squawk = parts[17].strip() if len(parts) > 17 else None

        # Validation
        if not self.is_valid_hex(hex_ident):
            return None

        if lat is None or lon is None:
            return None  # Position required

        if not (-90 <= lat <= 90) or not (-180 <= lon <= 180):
            return None  # Invalid coordinates

        # Timestamp processing
        date_str = parts[6]  # YYYY/MM/DD
        time_str = parts[7]  # HH:MM:SS.sss
        timestamp = self.parse_timestamp(date_str, time_str)

        return AircraftPosition(
            hex=hex_ident,
            flight=callsign,
            lat=lat,
            lon=lon,
            altitude=altitude,
            speed=speed,
            track=track,
            vertical_rate=vertical_rate,
            squawk=squawk,
            timestamp=timestamp,
            receiver_id=self.receiver_id
        )
```

**Validation Rules:**
- Hex code: Exactly 6 alphanumeric characters [0-9A-F]
- Callsign: At least 2 characters, alphanumeric only
- Latitude: -90 to +90 degrees
- Longitude: -180 to +180 degrees
- Altitude: -1,000 to 100,000 feet (sanity check)
- Speed: 0 to 1,000 knots (sanity check)
- Track: 0 to 359 degrees

**Type Conversion Safety:**
```python
def safe_int(value):
    try:
        return int(value) if value and value.strip() else None
    except ValueError:
        return None

def safe_float(value):
    try:
        return float(value) if value and value.strip() else None
    except ValueError:
        return None
```

**Timestamp Handling:**
```python
def parse_timestamp(self, date_str, time_str):
    # Input: "2025/12/20", "19:40:05.123"
    # Output: datetime object in UTC

    dt_str = f"{date_str} {time_str}"
    dt_local = datetime.strptime(dt_str, "%Y/%m/%d %H:%M:%S.%f")
    dt_utc = dt_local.replace(tzinfo=timezone.utc)
    return dt_utc
```

### Stage 3: Batch Buffering and Rate Limiting

**File:** `ads-b-receiver/adsb_streamer_optimized.py` (lines 157-598)

**AircraftDataManager Class:**
```python
class AircraftDataManager:
    def __init__(self, db_pool, batch_size=100, flush_interval=1.0, receiver_id="default"):
        self.aircraft_buffer = {}         # hex → AircraftPosition
        self.session_buffer = {}          # hex → SessionData
        self.last_update_time = {}        # hex → timestamp
        self.batch_size = batch_size
        self.flush_interval = flush_interval
        self.last_flush_time = time.time()
        self.receiver_id = receiver_id
```

**Rate Limiting (Per-Aircraft):**
```python
def should_update(self, hex_ident):
    now = time.time()
    last_update = self.last_update_time.get(hex_ident, 0)

    # Minimum 1 second between updates for same aircraft
    if now - last_update < 1.0:
        return False

    self.last_update_time[hex_ident] = now
    return True
```

**Buffer Management:**
```python
def add_position(self, position):
    hex_ident = position.hex

    # Rate limit check
    if not self.should_update(hex_ident):
        return

    # Add to aircraft buffer (overwrites previous if exists)
    self.aircraft_buffer[hex_ident] = position

    # Update session tracking
    if hex_ident not in self.session_buffer:
        self.session_buffer[hex_ident] = SessionData(
            hex=hex_ident,
            flight=position.flight,
            first_seen=position.timestamp,
            last_seen=position.timestamp,
            receiver_id=self.receiver_id
        )
    else:
        session = self.session_buffer[hex_ident]
        session.last_seen = position.timestamp
        if position.flight:
            session.flight = position.flight

    # Auto-flush if buffer full
    if len(self.aircraft_buffer) >= self.batch_size:
        self.flush()
```

**Time-Based Flush:**
```python
def check_auto_flush(self):
    now = time.time()
    if now - self.last_flush_time >= self.flush_interval:
        if len(self.aircraft_buffer) > 0:
            self.flush()
        self.last_flush_time = now
```

### Stage 4: High-Performance Database Insertion

**Method: CSV COPY (100-1000x faster than individual INSERTs)**

**Why CSV COPY is Fast:**
1. **Single Transaction:** All records committed atomically
2. **No SQL Parsing:** Binary protocol bypass
3. **Bulk Loading:** PostgreSQL's fastest import method
4. **No Round Trips:** One network call instead of N

**Implementation:**
```python
def flush(self):
    if len(self.aircraft_buffer) == 0:
        return

    conn = self.db_pool.getconn()
    try:
        cursor = conn.cursor()

        # STEP 1: Create temporary table (in-memory)
        cursor.execute("""
            CREATE TEMP TABLE temp_aircraft_real (LIKE aircraft_real)
            ON COMMIT DROP
        """)

        # STEP 2: Prepare CSV data in StringIO buffer
        aircraft_csv = StringIO()
        for position in self.aircraft_buffer.values():
            # Tab-separated values (CSV COPY format)
            aircraft_csv.write(
                f"{position.hex}\t"
                f"{position.flight or ''}\t"
                f"{position.lat}\t"
                f"{position.lon}\t"
                f"{position.altitude or ''}\t"
                f"{position.speed or ''}\t"
                f"{position.track or ''}\t"
                f"{position.vertical_rate or ''}\t"
                f"{position.squawk or ''}\t"
                f"{position.receiver_id}\t"
                f"{position.timestamp.isoformat()}\t"
                f"{position.timestamp.isoformat()}\n"
            )
        aircraft_csv.seek(0)

        # STEP 3: Bulk COPY into temporary table
        cursor.copy_from(
            aircraft_csv,
            'temp_aircraft_real',
            columns=('hex', 'flight', 'lat', 'lon', 'altitude', 'speed',
                     'track', 'vertical_rate', 'squawk', 'receiver_id',
                     'seen_at', 'created_at'),
            null=''  # Empty string = NULL
        )

        # STEP 4: UPSERT from temp to main table
        cursor.execute("""
            INSERT INTO aircraft_real
            SELECT * FROM temp_aircraft_real
            ON CONFLICT (hex) DO UPDATE SET
                flight = COALESCE(EXCLUDED.flight, aircraft_real.flight),
                lat = EXCLUDED.lat,
                lon = EXCLUDED.lon,
                altitude = COALESCE(EXCLUDED.altitude, aircraft_real.altitude),
                speed = COALESCE(EXCLUDED.speed, aircraft_real.speed),
                track = COALESCE(EXCLUDED.track, aircraft_real.track),
                vertical_rate = COALESCE(EXCLUDED.vertical_rate, aircraft_real.vertical_rate),
                squawk = COALESCE(EXCLUDED.squawk, aircraft_real.squawk),
                receiver_id = EXCLUDED.receiver_id,
                seen_at = EXCLUDED.seen_at
        """)

        # STEP 5: Insert into position history (for trails/Kalman filter)
        history_csv = StringIO()
        for position in self.aircraft_buffer.values():
            history_csv.write(
                f"{position.hex}\t{position.lat}\t{position.lon}\t"
                f"{position.altitude or ''}\t{position.speed or ''}\t"
                f"{position.track or ''}\t{position.vertical_rate or ''}\t"
                f"{position.timestamp.isoformat()}\n"
            )
        history_csv.seek(0)

        cursor.execute("CREATE TEMP TABLE temp_history (LIKE aircraft_position_history) ON COMMIT DROP")
        cursor.copy_from(history_csv, 'temp_history',
                        columns=('hex', 'lat', 'lon', 'altitude', 'speed',
                                'track', 'vertical_rate', 'timestamp'), null='')
        cursor.execute("INSERT INTO aircraft_position_history SELECT * FROM temp_history")

        # STEP 6: Update sessions
        session_csv = StringIO()
        for session in self.session_buffer.values():
            session_csv.write(
                f"{session.hex}\t{session.flight or ''}\t{session.receiver_id}\t"
                f"{session.first_seen.isoformat()}\t{session.last_seen.isoformat()}\n"
            )
        session_csv.seek(0)

        cursor.execute("CREATE TEMP TABLE temp_sessions (LIKE aircraft_sessions_real) ON COMMIT DROP")
        cursor.copy_from(session_csv, 'temp_sessions',
                        columns=('hex', 'flight', 'receiver_id', 'first_seen', 'last_seen'), null='')
        cursor.execute("""
            INSERT INTO aircraft_sessions_real SELECT * FROM temp_sessions
            ON CONFLICT (hex) DO UPDATE SET
                flight = COALESCE(EXCLUDED.flight, aircraft_sessions_real.flight),
                last_seen = EXCLUDED.last_seen,
                receiver_id = EXCLUDED.receiver_id
        """)

        conn.commit()
        logger.info(f"Flushed {len(self.aircraft_buffer)} aircraft to database")

    except Exception as e:
        conn.rollback()
        logger.error(f"Database flush error: {e}")
    finally:
        self.db_pool.putconn(conn)
        self.aircraft_buffer.clear()
```

**Performance Comparison:**

| Batch Size | Individual INSERT | execute_values | CSV COPY |
|-----------|------------------|----------------|----------|
| 10 | 200ms | 30ms | 5ms |
| 100 | 2000ms | 250ms | 20ms |
| 1000 | 20000ms | 2000ms | 150ms |

### Stage 5: Automatic Cleanup

**Stale Aircraft Removal:**
```python
def cleanup_stale_aircraft(self):
    conn = self.db_pool.getconn()
    try:
        cursor = conn.cursor()

        # Remove aircraft not seen in 5 minutes
        cursor.execute("""
            DELETE FROM aircraft_real
            WHERE seen_at < NOW() - INTERVAL '5 minutes'
        """)

        # Mark sessions as ended
        cursor.execute("""
            UPDATE aircraft_sessions_real
            SET ended_at = last_seen
            WHERE ended_at IS NULL
              AND last_seen < NOW() - INTERVAL '5 minutes'
        """)

        conn.commit()
        logger.info(f"Cleanup: removed {cursor.rowcount} stale aircraft")
    finally:
        self.db_pool.putconn(conn)
```

**Position History Cleanup (Optional):**
```python
def cleanup_old_history(self, hours=2):
    conn = self.db_pool.getconn()
    try:
        cursor = conn.cursor()
        cursor.execute("""
            DELETE FROM aircraft_position_history
            WHERE timestamp < NOW() - INTERVAL '%s hours'
        """, (hours,))
        conn.commit()
        logger.info(f"Cleanup: removed {cursor.rowcount} old position records")
    finally:
        self.db_pool.putconn(conn)
```

**Cleanup Schedule:**
- Stale aircraft: Every 2 minutes
- Position history: Every 2 hours (configurable via `--history-cleanup`)

---

## Database Design and Schema

### Schema Overview

The database uses a three-table primary design:

1. **aircraft_real**: Current state (one row per aircraft, continuously updated)
2. **aircraft_position_history**: Trail data (multiple rows per aircraft, append-only)
3. **aircraft_sessions_real**: Session tracking (one row per aircraft, tracks first/last seen)

**Supporting Tables:**
- collision_alerts: Collision event logging (not currently used)
- receiver_stats: Receiver performance metrics (optional)

### Table: aircraft_real

**Purpose:** Stores the current state of each aircraft, updated in real-time.

**Schema:**
```sql
CREATE TABLE aircraft_real (
    hex VARCHAR(6) PRIMARY KEY,           -- ICAO 24-bit identifier
    flight VARCHAR(10),                   -- Callsign (e.g., "UAL123")
    lat DOUBLE PRECISION,                 -- Latitude in degrees
    lon DOUBLE PRECISION,                 -- Longitude in degrees
    altitude INTEGER,                     -- Altitude in feet
    speed INTEGER,                        -- Ground speed in knots
    track INTEGER,                        -- Track/heading in degrees (0-359)
    vertical_rate INTEGER,                -- Vertical rate in feet/minute
    squawk VARCHAR(4),                    -- Transponder squawk code
    receiver_id VARCHAR(255),             -- Receiver identifier
    seen_at TIMESTAMPTZ DEFAULT NOW(),    -- Last update timestamp
    created_at TIMESTAMPTZ DEFAULT NOW()  -- First time added
);

-- Indexes for performance
CREATE INDEX idx_aircraft_real_position ON aircraft_real(lat, lon);
CREATE INDEX idx_aircraft_real_receiver ON aircraft_real(receiver_id);
CREATE INDEX idx_aircraft_real_seen_at ON aircraft_real(seen_at);
```

**Key Characteristics:**
- Primary key on hex ensures one row per aircraft
- UPSERT operations (ON CONFLICT DO UPDATE) keep data current
- Indexed on seen_at for fast time-window filtering
- Geospatial index on (lat, lon) for range queries

**Example Rows:**
```
hex    | flight | lat      | lon       | altitude | speed | track | vertical_rate | squawk | receiver_id  | seen_at                    | created_at
-------|--------|----------|-----------|----------|-------|-------|---------------|--------|--------------|----------------------------|---------------------------
A12345 | UAL123 | 41.8781  | -87.6298  | 35000    | 450   | 180   | 0             | 1234   | raspberrypi  | 2025-12-20 19:40:05.123+00 | 2025-12-20 19:30:00+00
A67890 | DAL456 | 41.9500  | -87.7000  | 38000    | 480   | 090   | -1200         | 5678   | raspberrypi  | 2025-12-20 19:40:03.456+00 | 2025-12-20 19:25:15+00
```

**Query Patterns:**

1. Get all active aircraft:
```sql
SELECT * FROM aircraft_real
WHERE seen_at > NOW() - INTERVAL '30 seconds'
ORDER BY seen_at DESC;
```

2. Get aircraft in geographic area:
```sql
SELECT * FROM aircraft_real
WHERE lat BETWEEN 41.5 AND 42.5
  AND lon BETWEEN -88.0 AND -87.0
  AND seen_at > NOW() - INTERVAL '2 minutes';
```

3. Count aircraft by altitude:
```sql
SELECT
    CASE
        WHEN altitude < 10000 THEN '0-10k'
        WHEN altitude < 20000 THEN '10-20k'
        WHEN altitude < 30000 THEN '20-30k'
        WHEN altitude < 40000 THEN '30-40k'
        ELSE '40k+'
    END AS altitude_range,
    COUNT(*) AS count
FROM aircraft_real
WHERE seen_at > NOW() - INTERVAL '5 minutes'
GROUP BY altitude_range
ORDER BY altitude_range;
```

### Table: aircraft_position_history

**Purpose:** Stores historical position snapshots for trail visualization and Kalman filter training.

**Schema:**
```sql
CREATE TABLE aircraft_position_history (
    id BIGSERIAL PRIMARY KEY,
    hex VARCHAR(6) NOT NULL,
    lat DOUBLE PRECISION NOT NULL,
    lon DOUBLE PRECISION NOT NULL,
    altitude INTEGER,
    speed INTEGER,
    track INTEGER,
    vertical_rate INTEGER,
    timestamp TIMESTAMPTZ DEFAULT NOW()
);

-- Indexes for trail queries
CREATE INDEX idx_aircraft_position_history_hex
    ON aircraft_position_history(hex, timestamp DESC);
CREATE INDEX idx_aircraft_position_history_timestamp
    ON aircraft_position_history(timestamp);
```

**Key Characteristics:**
- Append-only (no updates, only inserts)
- One row per position update per aircraft
- Composite index on (hex, timestamp) for fast trail retrieval
- Retention: Configurable, typically 2 hours

**Example Rows:**
```
id  | hex    | lat      | lon       | altitude | speed | track | vertical_rate | timestamp
----|--------|----------|-----------|----------|-------|-------|---------------|---------------------------
1   | A12345 | 41.8700  | -87.6200  | 34800    | 445   | 180   | 0             | 2025-12-20 19:35:00+00
2   | A12345 | 41.8740  | -87.6250  | 34900    | 448   | 181   | 100           | 2025-12-20 19:36:00+00
3   | A12345 | 41.8781  | -87.6298  | 35000    | 450   | 180   | 0             | 2025-12-20 19:37:00+00
```

**Query Patterns:**

1. Get trail for specific aircraft:
```sql
SELECT lat, lon, altitude, speed, track,
       EXTRACT(EPOCH FROM timestamp)::bigint AS seen
FROM aircraft_position_history
WHERE hex = 'A12345'
  AND timestamp > NOW() - INTERVAL '120 minutes'
ORDER BY timestamp ASC
LIMIT 100;
```

2. Batch fetch trails for multiple aircraft:
```sql
SELECT hex, lat, lon, altitude, speed, track,
       EXTRACT(EPOCH FROM timestamp)::bigint AS seen
FROM aircraft_position_history
WHERE hex = ANY(ARRAY['A12345', 'A67890', 'B11111'])
  AND timestamp > NOW() - INTERVAL '120 minutes'
ORDER BY hex, timestamp ASC;
```

3. Count position updates per aircraft:
```sql
SELECT hex, COUNT(*) AS updates
FROM aircraft_position_history
WHERE timestamp > NOW() - INTERVAL '1 hour'
GROUP BY hex
ORDER BY updates DESC;
```

### Table: aircraft_sessions_real

**Purpose:** Tracks when aircraft are first/last seen for flight duration analysis.

**Schema:**
```sql
CREATE TABLE aircraft_sessions_real (
    hex VARCHAR(6) PRIMARY KEY,
    flight VARCHAR(10),
    receiver_id VARCHAR(255),
    first_seen TIMESTAMPTZ NOT NULL,      -- First detection time
    last_seen TIMESTAMPTZ NOT NULL,       -- Last update time
    ended_at TIMESTAMPTZ,                 -- NULL if active, timestamp if ended
    created_at TIMESTAMPTZ DEFAULT NOW()
);

-- Partial index for active sessions
CREATE INDEX idx_aircraft_sessions_real_active
    ON aircraft_sessions_real(ended_at)
    WHERE ended_at IS NULL;

-- Index for time-based queries
CREATE INDEX idx_aircraft_sessions_real_times
    ON aircraft_sessions_real(first_seen, last_seen);
```

**Key Characteristics:**
- One row per aircraft (unique hex)
- ended_at is NULL for active sessions
- UPSERT operations update last_seen
- Partial index on active sessions for fast filtering

**Example Rows:**
```
hex    | flight | receiver_id | first_seen                 | last_seen                  | ended_at                   | created_at
-------|--------|-------------|----------------------------|----------------------------|----------------------------|---------------------------
A12345 | UAL123 | raspberrypi | 2025-12-20 19:30:00+00     | 2025-12-20 19:40:05+00     | NULL                       | 2025-12-20 19:30:00+00
A67890 | DAL456 | raspberrypi | 2025-12-20 19:25:15+00     | 2025-12-20 19:40:03+00     | NULL                       | 2025-12-20 19:25:15+00
B11111 | SWA789 | raspberrypi | 2025-12-20 18:00:00+00     | 2025-12-20 18:45:30+00     | 2025-12-20 18:45:30+00     | 2025-12-20 18:00:00+00
```

**Query Patterns:**

1. Get all active sessions:
```sql
SELECT hex, flight,
       EXTRACT(EPOCH FROM (last_seen - first_seen))::integer AS duration_seconds
FROM aircraft_sessions_real
WHERE ended_at IS NULL
ORDER BY last_seen DESC;
```

2. Get flight durations (for statistics):
```sql
SELECT hex, flight,
       first_seen,
       last_seen,
       EXTRACT(EPOCH FROM (last_seen - first_seen))::integer AS duration_seconds
FROM aircraft_sessions_real
WHERE ended_at IS NULL
  AND last_seen > NOW() - INTERVAL '2 hours'
ORDER BY duration_seconds DESC;
```

3. Mark ended sessions:
```sql
UPDATE aircraft_sessions_real
SET ended_at = last_seen
WHERE ended_at IS NULL
  AND last_seen < NOW() - INTERVAL '5 minutes';
```

### Views

**View: active_aircraft_real**
```sql
CREATE VIEW active_aircraft_real AS
SELECT *,
       EXTRACT(EPOCH FROM (NOW() - seen_at)) AS seconds_since_seen
FROM aircraft_real
WHERE seen_at > NOW() - INTERVAL '5 minutes'
ORDER BY seen_at DESC;
```

**View: active_sessions_real**
```sql
CREATE VIEW active_sessions_real AS
SELECT *,
       EXTRACT(EPOCH FROM (last_seen - first_seen)) AS session_duration_seconds
FROM aircraft_sessions_real
WHERE ended_at IS NULL
ORDER BY last_seen DESC;
```

### Database Connection Configuration

**ADS-B Streamer (Python):**
```python
import psycopg2.pool

db_pool = psycopg2.pool.ThreadedConnectionPool(
    minconn=1,
    maxconn=5,
    dsn=DATABASE_URL,
    connect_timeout=10
)
```

**Web Server (Node.js):**
```javascript
import { Pool } from 'pg';

const db = new Pool({
    connectionString: process.env.DATABASE_URL,
    ssl: { rejectUnauthorized: false },
    max: 3,                        // Max connections
    idleTimeoutMillis: 10000,      // Close idle after 10s
    connectionTimeoutMillis: 10000,
    allowExitOnIdle: false
});
```

**Connection String Format (Supabase):**
```
postgresql://postgres.[project-ref]:[password]@aws-0-[region].pooler.supabase.com:6543/postgres
```

**Key Points:**
- Use Session Pooler (port 6543), not Direct Connection (port 5432)
- Keep connection pool small (Supabase limit ~15-20 total connections)
- Enable SSL with rejectUnauthorized: false (Supabase requires SSL)
- Set timeouts to prevent hanging connections

---

## Backend API Implementation

### API Endpoint Overview

The Node.js/Express backend provides 8 primary endpoints:

| Endpoint | Method | Purpose |
|----------|--------|---------|
| `/api/aircraft` | GET | Basic aircraft data |
| `/api/aircraft/enriched` | GET | Aircraft + trails + cones (cached) |
| `/api/aircraft/:hex/trail` | GET | Individual aircraft trail |
| `/api/statistics` | GET | Dashboard statistics |
| `/api/collisions/kalman` | GET | Collision detection |
| `/api/health` | GET | Health check |
| `/` | GET | Serve 2D map HTML |
| `/3d.html` | GET | Serve 3D globe HTML |

### Endpoint: GET /api/aircraft

**File:** `ads/server.js` (lines 231-249)

**Purpose:** Fetch basic aircraft data with optional time window filtering.

**Parameters:**
- `window` (optional): Time window in seconds
  - `0`: All active aircraft (default in current implementation)
  - `300`: Last 5 minutes
  - `3600`: Last 1 hour
  - `21600`: Last 6 hours

**Implementation:**
```javascript
app.get('/api/aircraft', async (req, res) => {
    try {
        const timeWindow = req.query.window ? parseInt(req.query.window) : 30;
        const aircraft = await fetchAircraftFromDB(timeWindow);

        res.json({
            now: Date.now(),
            aircraft: aircraft,
            timeWindow: timeWindow
        });
    } catch (error) {
        console.error('Error fetching aircraft data:', error);
        res.status(500).json({
            error: 'Failed to fetch aircraft data',
            message: error.message
        });
    }
});
```

**Database Query:**
```javascript
async function fetchAircraftFromDB(timeWindow = 0) {
    let query = `
        SELECT DISTINCT ON (a.hex)
            a.hex,
            a.flight,
            a.lat,
            a.lon,
            a.altitude,
            a.speed,
            a.track,
            a.vertical_rate,
            a.squawk,
            a.receiver_id,
            EXTRACT(EPOCH FROM a.seen_at)::bigint as seen,
            EXTRACT(EPOCH FROM a.created_at)::bigint as created
        FROM aircraft_real a
        WHERE a.lat IS NOT NULL
          AND a.lon IS NOT NULL
    `;

    if (timeWindow > 0) {
        query += ` AND a.seen_at > NOW() - INTERVAL '${timeWindow} seconds'`;
    }

    query += ` ORDER BY a.hex, a.seen_at DESC`;

    const result = await db.query(query);

    // Enrich with aircraft type
    const enriched = result.rows.map(aircraft => ({
        ...aircraft,
        type: aircraftTypes[aircraft.hex] || null
    }));

    return enriched;
}
```

**Aircraft Type Enrichment:**
```javascript
// Load aircraft types on server startup
let aircraftTypes = {};
try {
    const typesData = fs.readFileSync('./aircraft-types-by-hex.json', 'utf8');
    aircraftTypes = JSON.parse(typesData);
    console.log(`Loaded ${Object.keys(aircraftTypes).length} aircraft types`);
} catch (error) {
    console.warn('Could not load aircraft types:', error.message);
}
```

**Response Format:**
```json
{
  "now": 1703105523456,
  "aircraft": [
    {
      "hex": "A12345",
      "flight": "UAL123",
      "lat": 41.8781,
      "lon": -87.6298,
      "altitude": 35000,
      "speed": 450,
      "track": 180,
      "vertical_rate": 0,
      "squawk": "1234",
      "receiver_id": "raspberrypi",
      "seen": 1703105520,
      "created": 1703105000,
      "type": null
    },
    {
      "hex": "A67890",
      "flight": null,
      "lat": 41.9500,
      "lon": -87.7000,
      "altitude": 2500,
      "speed": 120,
      "track": 090,
      "vertical_rate": -500,
      "squawk": "1200",
      "receiver_id": "raspberrypi",
      "seen": 1703105518,
      "created": 1703104800,
      "type": 6
    }
  ],
  "timeWindow": 0
}
```

**Type Field Explanation:**
- `null`: Unknown aircraft type
- `6`: Helicopter (from FAA aircraft registry)
- Other values: Various aircraft categories

### Endpoint: GET /api/aircraft/enriched

**File:** `ads/server.js` (lines 253-387)

**Purpose:** High-performance endpoint that returns aircraft with trails and pre-computed probability cones. Uses aggressive caching.

**Parameters:**
- `window`: Time window seconds (default: 30)
- `trails`: Include trails (default: true)
- `cones`: Include probability cones (default: true)
- `prediction_time`: Prediction horizon seconds (default: 300)
- `confidence`: Confidence level 0-1 (default: 0.95)

**Implementation:**
```javascript
app.get('/api/aircraft/enriched', async (req, res) => {
    try {
        const timeWindow = req.query.window ? parseInt(req.query.window) : 30;
        const includeTrails = req.query.trails !== 'false';
        const includeCones = req.query.cones !== 'false';
        const predictionTime = parseInt(req.query.prediction_time) || 300;
        const confidenceLevel = parseFloat(req.query.confidence) || 0.95;

        // Check cache first
        const cacheKey = `enriched:${timeWindow}:${includeTrails}:${includeCones}:${predictionTime}:${confidenceLevel}`;
        const cached = cache.get(cacheKey);

        if (cached) {
            console.log('Cache HIT for enriched data');
            return res.json({
                ...cached,
                cached: true,
                cacheStats: cache.getStats()
            });
        }

        console.log('Cache MISS - computing enriched data...');

        // Fetch basic aircraft data
        const aircraft = await fetchAircraftFromDB(timeWindow);
        const hexCodes = aircraft.map(ac => ac.hex);

        // Batch fetch trails if requested
        let trailsByHex = new Map();
        if (includeTrails && hexCodes.length > 0) {
            const trailResult = await db.query(`
                SELECT
                    hex,
                    lat,
                    lon,
                    altitude,
                    speed,
                    track,
                    EXTRACT(EPOCH FROM timestamp)::bigint as seen
                FROM aircraft_position_history
                WHERE hex = ANY($1)
                AND timestamp >= NOW() - INTERVAL '120 minutes'
                ORDER BY hex, timestamp ASC
            `, [hexCodes]);

            // Group trails by hex
            trailResult.rows.forEach(row => {
                if (!trailsByHex.has(row.hex)) {
                    trailsByHex.set(row.hex, []);
                }
                trailsByHex.get(row.hex).push({
                    lat: parseFloat(row.lat),
                    lon: parseFloat(row.lon),
                    altitude: row.altitude,
                    speed: row.speed,
                    track: row.track,
                    timestamp: new Date(parseInt(row.seen) * 1000).toISOString()
                });
            });

            console.log(`Fetched trails for ${trailsByHex.size} aircraft in single query`);
        }

        // Compute probability cones if requested
        const enrichedAircraft = await Promise.all(aircraft.map(async (ac) => {
            const enriched = { ...ac };

            // Add trail data
            if (includeTrails) {
                enriched.trail = trailsByHex.get(ac.hex) || [];
            }

            // Compute probability cone (server-side, cached per aircraft)
            if (includeCones) {
                const coneCacheKey = `cone:${ac.hex}:${predictionTime}:${confidenceLevel}`;
                let coneData = cache.get(coneCacheKey);

                if (!coneData) {
                    try {
                        const positionHistory = trailsByHex.get(ac.hex) || [];

                        // Only compute cone if we have sufficient data
                        if (positionHistory.length >= 2 || (ac.speed && ac.track)) {
                            const cone = new ProbabilityCone(
                                ac,
                                positionHistory,
                                predictionTime,
                                confidenceLevel
                            );

                            coneData = cone.getPredictionPoints();

                            // Cache cone for 3 seconds
                            cache.set(coneCacheKey, coneData, 3);
                        }
                    } catch (error) {
                        console.error(`Error computing cone for ${ac.hex}:`, error.message);
                        coneData = null;
                    }
                }

                enriched.probabilityCone = coneData;
            }

            return enriched;
        }));

        const response = {
            now: Date.now(),
            aircraft: enrichedAircraft,
            timeWindow: timeWindow,
            cached: false,
            stats: {
                total: enrichedAircraft.length,
                withTrails: enrichedAircraft.filter(ac => ac.trail && ac.trail.length > 0).length,
                withCones: enrichedAircraft.filter(ac => ac.probabilityCone).length
            },
            cacheStats: cache.getStats()
        };

        // Cache the entire response for 2 seconds
        cache.set(cacheKey, response, 2);

        res.json(response);

    } catch (error) {
        console.error('Error fetching enriched aircraft data:', error);
        res.status(500).json({
            error: 'Failed to fetch enriched aircraft data',
            message: error.message
        });
    }
});
```

**Performance Optimizations:**
1. **Two-Level Caching:**
   - Full response cache: 2-second TTL
   - Individual cone cache: 3-second TTL per aircraft
2. **Batch Trail Fetch:** Single query for all aircraft using `ANY($1)`
3. **Server-Side Computation:** Probability cones computed once, shared across all clients
4. **Cache Statistics:** Exposed in response for monitoring

**Response Format:**
```json
{
  "now": 1703105523456,
  "aircraft": [
    {
      "hex": "A12345",
      "flight": "UAL123",
      "lat": 41.8781,
      "lon": -87.6298,
      "altitude": 35000,
      "speed": 450,
      "track": 180,
      "trail": [
        {"lat": 41.8700, "lon": -87.6200, "altitude": 34800, "timestamp": "2025-12-20T19:35:00.000Z"},
        {"lat": 41.8740, "lon": -87.6250, "altitude": 34900, "timestamp": "2025-12-20T19:36:00.000Z"}
      ],
      "probabilityCone": [
        {"time": 15, "lat": 41.8800, "lon": -87.6320, "altitude": 35050, "radius": 150},
        {"time": 30, "lat": 41.8820, "lon": -87.6340, "altitude": 35100, "radius": 250}
      ]
    }
  ],
  "cached": false,
  "stats": {
    "total": 25,
    "withTrails": 20,
    "withCones": 18
  },
  "cacheStats": {
    "hits": 150,
    "misses": 50,
    "hitRate": "75.0%",
    "size": 12
  }
}
```

### Endpoint: GET /api/statistics

**File:** `ads/server.js` (lines 434-543)

**Purpose:** Provide dashboard statistics including altitude distribution and flight durations.

**Implementation:**
```javascript
app.get('/api/statistics', async (req, res) => {
    try {
        const timeWindow = req.query.window ? parseInt(req.query.window) : 0;

        const sessionTimeFilter = timeWindow > 0
            ? `AND s.last_seen > NOW() - INTERVAL '${timeWindow} seconds'`
            : '';

        // Single query with CTEs for efficiency
        const result = await db.query(`
            WITH
            -- Summary statistics
            summary AS (
                SELECT
                    COUNT(DISTINCT a.hex) as total_flights,
                    COALESCE(AVG(a.speed), 0)::numeric(10,2) as avg_speed,
                    COALESCE(AVG(a.altitude), 0)::numeric(10,2) as avg_altitude,
                    COALESCE(MIN(a.altitude), 0) as min_altitude,
                    COALESCE(MAX(a.altitude), 0) as max_altitude,
                    COALESCE(MIN(a.speed), 0) as min_speed,
                    COALESCE(MAX(a.speed), 0) as max_speed
                FROM aircraft_real a
                INNER JOIN aircraft_sessions_real s ON a.hex = s.hex
                WHERE s.ended_at IS NULL
                ${sessionTimeFilter}
            ),
            -- Altitude distribution
            altitude_dist AS (
                SELECT
                    CASE
                        WHEN a.altitude < 10000 THEN '0-10k'
                        WHEN a.altitude < 20000 THEN '10-20k'
                        WHEN a.altitude < 30000 THEN '20-30k'
                        WHEN a.altitude < 40000 THEN '30-40k'
                        ELSE '40k+'
                    END as range,
                    COUNT(*) as count
                FROM aircraft_real a
                INNER JOIN aircraft_sessions_real s ON a.hex = s.hex
                WHERE s.ended_at IS NULL
                  AND a.altitude IS NOT NULL
                ${sessionTimeFilter}
                GROUP BY range
            ),
            -- Flight durations
            durations AS (
                SELECT
                    s.hex,
                    s.flight,
                    EXTRACT(EPOCH FROM (s.last_seen - s.first_seen))::integer as duration_seconds,
                    EXTRACT(EPOCH FROM s.first_seen)::bigint as first_seen,
                    EXTRACT(EPOCH FROM s.last_seen)::bigint as last_seen
                FROM aircraft_sessions_real s
                INNER JOIN aircraft_real a ON s.hex = a.hex
                WHERE s.ended_at IS NULL
                ${sessionTimeFilter}
                ORDER BY duration_seconds DESC
            )
            SELECT
                'summary' as type,
                to_jsonb(summary.*) as data
            FROM summary
            UNION ALL
            SELECT
                'altitude_dist' as type,
                jsonb_agg(to_jsonb(altitude_dist.*)) as data
            FROM altitude_dist
            UNION ALL
            SELECT
                'durations' as type,
                jsonb_agg(to_jsonb(durations.*)) as data
            FROM durations
        `);

        // Parse results
        const stats = {
            summary: {},
            altitude_distribution: [],
            flight_durations: []
        };

        result.rows.forEach(row => {
            if (row.type === 'summary') {
                stats.summary = row.data;
            } else if (row.type === 'altitude_dist') {
                stats.altitude_distribution = row.data || [];
            } else if (row.type === 'durations') {
                stats.flight_durations = row.data || [];
            }
        });

        res.json({
            ...stats,
            timestamp: Date.now()
        });

    } catch (error) {
        console.error('Error fetching statistics:', error);
        res.status(500).json({
            error: 'Failed to fetch statistics',
            message: error.message
        });
    }
});
```

**Optimization:** Uses CTEs to execute 3 aggregations in a single database round trip instead of 3 separate queries.

**Response Format:**
```json
{
  "summary": {
    "total_flights": 25,
    "avg_speed": 425.50,
    "avg_altitude": 32500.00,
    "min_altitude": 1500,
    "max_altitude": 41000,
    "min_speed": 120,
    "max_speed": 550
  },
  "altitude_distribution": [
    {"range": "0-10k", "count": 5},
    {"range": "10-20k", "count": 3},
    {"range": "20-30k", "count": 15},
    {"range": "30-40k", "count": 2},
    {"range": "40k+", "count": 0}
  ],
  "flight_durations": [
    {"hex": "A12345", "flight": "UAL123", "duration_seconds": 3600, "first_seen": 1703101920, "last_seen": 1703105520},
    {"hex": "A67890", "flight": "DAL456", "duration_seconds": 2400, "first_seen": 1703103120, "last_seen": 1703105520}
  ],
  "timestamp": 1703105523456
}
```

### Endpoint: GET /api/collisions/kalman

**File:** `ads/server.js` (lines 571-693)

**Purpose:** Detect potential collisions using Kalman filter-based trajectory prediction.

**Parameters:**
- `time_horizon`: Prediction time in seconds (default: 300)
- `confidence`: Confidence level 0-1 (default: 0.95)

**Implementation:** (Detailed in Kalman Filter section)

**Response Format:**
```json
{
  "collisions": [
    {
      "aircraft1": "A12345",
      "aircraft2": "A67890",
      "flight1": "UAL123",
      "flight2": "DAL456",
      "timeToCollision": 180,
      "probability": "12.5%",
      "distance": 1500,
      "verticalSeparation": 800,
      "position1": {"lat": "41.8781", "lon": "-87.6298", "alt": 35000},
      "position2": {"lat": "41.8800", "lon": "-87.6300", "alt": 35800},
      "currentPosition1": {"lat": "41.8700", "lon": "-87.6200", "alt": 34500},
      "currentPosition2": {"lat": "41.8720", "lon": "-87.6220", "alt": 34900}
    }
  ],
  "aircraft_count": 25,
  "time_horizon": 300,
  "confidence_level": 0.95,
  "timestamp": 1703105523456
}
```

---

## Frontend Visualization

### 2D Map Implementation (Leaflet.js)

**File:** `ads/app.js` (1289 lines)

**Map Initialization:**
```javascript
// Initialize Leaflet map
const map = L.map('map', {
    zoomControl: true,
    attributionControl: true
}).setView([41.88, -87.63], 9);  // Chicago area

// Dark CartoDB tile layer
L.tileLayer('https://{s}.basemaps.cartocdn.com/dark_all/{z}/{x}/{y}{r}.png', {
    attribution: '© OpenStreetMap contributors © CARTO',
    maxZoom: 19,
    subdomains: 'abcd'
}).addTo(map);
```

**Aircraft Marker Creation:**
```javascript
function createAircraftIcon(aircraft) {
    const isHelicopter = aircraft.type === 6;
    const rotation = aircraft.track || 0;

    // SVG icon with rotation
    const iconSVG = isHelicopter
        ? createHelicopterSVG(rotation)
        : createAirplaneSVG(rotation);

    const iconHTML = `
        <div style="transform: rotate(${rotation}deg); transform-origin: center;">
            ${iconSVG}
        </div>
        <div style="font-size: 10px; color: #00ffff; text-align: center; margin-top: 2px;">
            ${aircraft.flight || aircraft.hex}
        </div>
    `;

    return L.divIcon({
        html: iconHTML,
        className: 'aircraft-icon',
        iconSize: [30, 30],
        iconAnchor: [15, 15]
    });
}

function createAirplaneSVG(rotation) {
    return `
        <svg width="30" height="30" viewBox="0 0 30 30">
            <g transform="rotate(${rotation} 15 15)">
                <!-- Fuselage -->
                <rect x="13" y="5" width="4" height="20" fill="#00ffff"/>
                <!-- Wings -->
                <rect x="5" y="13" width="20" height="4" fill="#00ffff"/>
                <!-- Tail -->
                <polygon points="13,5 17,5 15,0" fill="#00ffff"/>
            </g>
        </svg>
    `;
}

function createHelicopterSVG(rotation) {
    return `
        <svg width="30" height="30" viewBox="0 0 30 30">
            <g transform="rotate(${rotation} 15 15)">
                <!-- Rotor -->
                <rect x="5" y="14" width="20" height="2" fill="#ffff00"/>
                <!-- Body -->
                <ellipse cx="15" cy="18" rx="4" ry="6" fill="#ffff00"/>
                <!-- Tail -->
                <rect x="14" y="22" width="2" height="8" fill="#ffff00"/>
                <!-- Tail rotor -->
                <circle cx="15" cy="30" r="2" fill="#ffff00"/>
            </g>
        </svg>
    `;
}
```

**Smooth Marker Animation:**
```javascript
function smoothMoveMarker(marker, targetLat, targetLon, targetHeading, duration = 2000) {
    const startLat = marker.getLatLng().lat;
    const startLon = marker.getLatLng().lng;
    const startHeading = marker.currentHeading || targetHeading;

    // Handle heading wrap-around (0/360 transition)
    let headingDelta = targetHeading - startHeading;
    if (headingDelta > 180) headingDelta -= 360;
    if (headingDelta < -180) headingDelta += 360;

    const startTime = Date.now();

    function animate() {
        const now = Date.now();
        const elapsed = now - startTime;
        const progress = Math.min(elapsed / duration, 1);

        // Cubic easing: easeOut
        const eased = 1 - Math.pow(1 - progress, 3);

        // Interpolate position
        const lat = startLat + (targetLat - startLat) * eased;
        const lon = startLon + (targetLon - startLon) * eased;
        const heading = startHeading + headingDelta * eased;

        // Update marker
        marker.setLatLng([lat, lon]);
        marker.currentHeading = heading;
        updateMarkerRotation(marker, heading);

        // Continue animation
        if (progress < 1) {
            requestAnimationFrame(animate);
        }
    }

    requestAnimationFrame(animate);
}
```

**Trail Visualization:**
```javascript
async function displayTrailForAircraft(hex) {
    try {
        const response = await fetch(`/api/aircraft/${hex}/trail?minutes=120&limit=100`);
        const data = await response.json();

        if (data.trail && data.trail.length > 1) {
            const points = data.trail.map(p => [p.lat, p.lon]);

            const aircraft = aircraftData.find(ac => ac.hex === hex);
            const color = aircraft && aircraft.type === 6 ? '#ffff00' : '#00ffff';

            const trail = L.polyline(points, {
                color: color,
                weight: 2,
                opacity: 0.6
            }).addTo(map);

            trails.set(hex, trail);
        }
    } catch (error) {
        console.error(`Error fetching trail for ${hex}:`, error);
    }
}
```

**Prediction Wedges (2D Projection):**
```javascript
function displayPredictionWedge(aircraft) {
    if (!aircraft.probabilityCone || aircraft.probabilityCone.length === 0) return;

    const lastPrediction = aircraft.probabilityCone[aircraft.probabilityCone.length - 1];
    const spreadAngle = 12;  // degrees on each side

    // Calculate wedge vertices
    const distance = 5 * 1852;  // 5 nautical miles in meters
    const bearing = aircraft.track || 0;

    const leftBearing = (bearing - spreadAngle + 360) % 360;
    const rightBearing = (bearing + spreadAngle) % 360;

    const leftPoint = destinationPoint(aircraft.lat, aircraft.lon, distance, leftBearing);
    const rightPoint = destinationPoint(aircraft.lat, aircraft.lon, distance, rightBearing);

    const wedgePoints = [
        [aircraft.lat, aircraft.lon],
        [leftPoint.lat, leftPoint.lon],
        [rightPoint.lat, rightPoint.lon],
        [aircraft.lat, aircraft.lon]
    ];

    // Color by altitude
    let color;
    if (aircraft.altitude < 10000) color = '#00ff00';
    else if (aircraft.altitude < 25000) color = '#ffff00';
    else color = '#ff8800';

    const wedge = L.polygon(wedgePoints, {
        color: color,
        fillColor: color,
        fillOpacity: 0.2,
        weight: 1
    }).addTo(map);

    predictionWedges.set(aircraft.hex, wedge);
}
```

### 3D Globe Implementation (Cesium.js)

**File:** `ads/cesium-app.js` (1776 lines)

**Viewer Initialization:**
```javascript
const viewer = new Cesium.Viewer('cesiumContainer', {
    baseLayerPicker: false,
    animation: false,
    timeline: false,
    fullscreenButton: false,
    vrButton: false,
    homeButton: false,
    sceneModePicker: false,
    navigationHelpButton: false,
    geocoder: false,
    selectionIndicator: true,
    infoBox: true,
    terrain: Cesium.Terrain.fromWorldTerrain(),
    imageryProvider: new Cesium.OpenStreetMapImageryProvider({
        url: 'https://a.tile.openstreetmap.org/'
    })
});

// Enable globe lighting
viewer.scene.globe.enableLighting = true;
viewer.scene.globe.depthTestAgainstTerrain = true;

// Add OSM buildings
const osmBuildings = await Cesium.createOsmBuildingsAsync();
viewer.scene.primitives.add(osmBuildings);

// Initial camera position (Chicago)
viewer.camera.setView({
    destination: Cesium.Cartesian3.fromDegrees(-87.6298, 41.8781, 550000),
    orientation: {
        heading: 0.0,
        pitch: Cesium.Math.toRadians(-45),
        roll: 0.0
    }
});
```

**3D Aircraft Entity:**
```javascript
function createAircraftEntity(aircraft) {
    const position = Cesium.Cartesian3.fromDegrees(
        aircraft.lon,
        aircraft.lat,
        (aircraft.altitude || 10000) * 0.3048  // feet to meters
    );

    const entity = viewer.entities.add({
        id: aircraft.hex,
        position: position,

        // 3D Model (for helicopters)
        model: aircraft.type === 6 ? {
            uri: '/helicopter.glb',
            minimumPixelSize: 64,
            maximumScale: 500
        } : {
            uri: 'path/to/airplane.glb',
            minimumPixelSize: 64,
            maximumScale: 500
        },

        // Label
        label: {
            text: aircraft.flight || aircraft.hex,
            font: '14px monospace',
            fillColor: Cesium.Color.CYAN,
            outlineColor: Cesium.Color.BLACK,
            outlineWidth: 2,
            style: Cesium.LabelStyle.FILL_AND_OUTLINE,
            verticalOrigin: Cesium.VerticalOrigin.BOTTOM,
            pixelOffset: new Cesium.Cartesian2(0, -20)
        },

        // Properties
        properties: {
            hex: aircraft.hex,
            flight: aircraft.flight,
            altitude: aircraft.altitude,
            speed: aircraft.speed,
            track: aircraft.track
        }
    });

    return entity;
}
```

**3D Trail Rendering:**
```javascript
function displayTrailIn3D(aircraft) {
    if (!aircraft.trail || aircraft.trail.length < 2) return;

    const positions = aircraft.trail.map(point =>
        Cesium.Cartesian3.fromDegrees(
            point.lon,
            point.lat,
            (point.altitude || 0) * 0.3048
        )
    );

    viewer.entities.add({
        id: `trail-${aircraft.hex}`,
        polyline: {
            positions: positions,
            width: 2,
            material: new Cesium.PolylineGlowMaterialProperty({
                glowPower: 0.2,
                color: aircraft.type === 6
                    ? Cesium.Color.YELLOW.withAlpha(0.6)
                    : Cesium.Color.CYAN.withAlpha(0.6)
            })
        }
    });
}
```

**Probability Cone Rendering (3D):**
```javascript
function displayProbabilityConeIn3D(aircraft) {
    if (!aircraft.probabilityCone || aircraft.probabilityCone.length === 0) return;

    const points = aircraft.probabilityCone.map(prediction => ({
        position: Cesium.Cartesian3.fromDegrees(
            prediction.lon,
            prediction.lat,
            prediction.altitude * 0.3048
        ),
        radius: prediction.radius
    }));

    // Create cone as series of connected circles
    for (let i = 0; i < points.length - 1; i++) {
        const frustum = new Cesium.FrustumGeometry({
            origin: points[i].position,
            orientation: Cesium.Quaternion.IDENTITY,
            verticalLength: Cesium.Cartesian3.distance(points[i].position, points[i+1].position),
            _fovy: Math.atan2(points[i].radius, 1000),
            _xOffset: 0,
            _yOffset: 0
        });

        viewer.scene.primitives.add(new Cesium.Primitive({
            geometryInstances: new Cesium.GeometryInstance({
                geometry: frustum,
                attributes: {
                    color: Cesium.ColorGeometryInstanceAttribute.fromColor(
                        Cesium.Color.ORANGE.withAlpha(0.3)
                    )
                }
            }),
            appearance: new Cesium.PerInstanceColorAppearance({
                translucent: true
            })
        }));
    }
}
```

**Mobile Touch Controls:**
```javascript
// Configure touch-friendly camera controls
const controller = viewer.scene.screenSpaceCameraController;
controller.enableRotate = true;
controller.enableTranslate = true;
controller.enableZoom = true;
controller.enableTilt = true;
controller.enableLook = true;

// Increase zoom sensitivity for touch
controller.zoomEventTypes = [
    Cesium.CameraEventType.PINCH,
    Cesium.CameraEventType.WHEEL,
    Cesium.CameraEventType.RIGHT_DRAG
];
controller.zoomFactor = 5.0;

// Smooth inertia for touch gestures
controller.inertiaSpin = 0.95;
controller.inertiaTranslate = 0.95;
controller.inertiaZoom = 0.9;

// Min/max zoom distances
controller.minimumZoomDistance = 100;      // 100 meters
controller.maximumZoomDistance = 40000000; // 40,000 km
```

**On-Screen Mobile Controls:**
```javascript
// Tilt buttons
document.getElementById('tilt-up').addEventListener('click', () => {
    viewer.camera.lookUp(Cesium.Math.toRadians(10));
});

document.getElementById('tilt-down').addEventListener('click', () => {
    viewer.camera.lookDown(Cesium.Math.toRadians(10));
});

// Rotate buttons
document.getElementById('rotate-left').addEventListener('click', () => {
    viewer.camera.rotateLeft(Cesium.Math.toRadians(15));
});

document.getElementById('rotate-right').addEventListener('click', () => {
    viewer.camera.rotateRight(Cesium.Math.toRadians(15));
});

// Preset view buttons
document.getElementById('view-top').addEventListener('click', () => {
    viewer.camera.setView({
        orientation: {
            heading: viewer.camera.heading,
            pitch: Cesium.Math.toRadians(-90),  // Top-down
            roll: 0.0
        }
    });
});

document.getElementById('view-angle').addEventListener('click', () => {
    viewer.camera.setView({
        orientation: {
            heading: viewer.camera.heading,
            pitch: Cesium.Math.toRadians(-45),  // 45-degree angle
            roll: 0.0
        }
    });
});

document.getElementById('view-horizon').addEventListener('click', () => {
    viewer.camera.setView({
        orientation: {
            heading: viewer.camera.heading,
            pitch: Cesium.Math.toRadians(0),  // Horizon
            roll: 0.0
        }
    });
});
```

### Statistics Dashboard

**File:** `ads/cesium-app.js` (lines 1180-1250)

**Update Function:**
```javascript
function updateStatisticsDashboard(data) {
    const dashboardElement = document.getElementById('statistics-content');
    if (!dashboardElement) return;

    const { summary, altitude_distribution, flight_durations } = data;

    // Build altitude distribution bars
    const altitudeHTML = altitude_distribution.map(item => {
        const maxCount = Math.max(10, ...altitude_distribution.map(i => i.count));
        const barWidth = Math.min((item.count / maxCount) * 100, 100);
        return `
            <div class="stat-bar-container-3d">
                <span class="stat-bar-label-3d">${item.range}</span>
                <div class="stat-bar-bg-3d">
                    <div class="stat-bar-3d" style="width: ${barWidth}%"></div>
                </div>
                <span class="stat-bar-value-3d">${item.count}</span>
            </div>
        `;
    }).join('');

    // Build flight durations list (all active flights)
    const allFlights = flight_durations;
    const durationsHTML = allFlights.map(item => {
        return `
            <div class="duration-item-3d">
                <span class="duration-flight-3d">${item.flight || item.hex}</span>
                <span class="duration-time-3d">${formatDuration(item.duration_seconds)}</span>
            </div>
        `;
    }).join('') || '<div style="color: #7a9a7a; font-size: 10px;">No active flights</div>';

    dashboardElement.innerHTML = `
        <div class="stats-summary-3d">
            <div class="stat-card-3d">
                <div class="stat-card-label-3d">Flights</div>
                <div class="stat-card-value-3d">${summary.total_flights}</div>
            </div>
            <div class="stat-card-3d">
                <div class="stat-card-label-3d">Avg Speed</div>
                <div class="stat-card-value-3d">${summary.avg_speed.toFixed(0)}</div>
            </div>
            <div class="stat-card-3d">
                <div class="stat-card-label-3d">Avg Alt</div>
                <div class="stat-card-value-3d">${(summary.avg_altitude / 1000).toFixed(1)}k</div>
            </div>
        </div>

        <div class="stats-section-3d">
            <h4>Altitude Distribution</h4>
            ${altitudeHTML || '<div style="color: #7a9a7a; font-size: 10px;">No data</div>'}
        </div>

        <div class="stats-section-3d">
            <h4>Flight Durations (All Active)</h4>
            <div class="durations-list-3d">
                ${durationsHTML}
            </div>
        </div>
    `;
}

function formatDuration(seconds) {
    const hours = Math.floor(seconds / 3600);
    const minutes = Math.floor((seconds % 3600) / 60);

    if (hours > 0) {
        return `${hours}h ${minutes}m`;
    } else {
        return `${minutes}m`;
    }
}
```

---

## Kalman Filter and Collision Detection

(This section was comprehensively covered in the agent's report above. The full technical details including state transition matrices, measurement models, coordinate transformations, probability cone generation, and collision detection algorithms are documented in sections 8.1-8.7 of the agent's output.)

### Summary of Kalman Filter Implementation

**State Vector:** 6-dimensional (x, y, z, vx, vy, vz) in ECEF coordinates

**Process:**
1. Initialize from position history or current speed/track
2. Train with historical positions (predict-update cycle)
3. Predict forward to current time
4. Generate future predictions (20 time steps over 5 minutes)
5. Calculate uncertainty growth (covariance matrix expansion)

**Collision Detection:**
- Pairwise comparison of probability cones
- Vertical separation rules (1000 ft / 2000 ft for RVSM)
- Cone intersection based on 3D distance and combined uncertainty
- Probability calculation from overlap ratio

---

## Performance Optimizations

### 1. Database Write Performance

**CSV COPY Method:**
- Throughput: 5,000 operations/second
- 100-1000x faster than individual INSERTs
- Single transaction with bulk loading
- Temporary table strategy for UPSERT

### 2. Database Read Performance

**Batch Queries:**
- Trail fetch: Single query for all aircraft using `hex = ANY($1)`
- Reduces N queries to 1 (25x improvement for 25 aircraft)

**CTE Optimization:**
- Statistics endpoint: 3 aggregations in 1 query
- Eliminates 2 extra database round trips

**Indexed Queries:**
- `seen_at` index: Fast time-window filtering
- `(lat, lon)` index: Spatial queries
- `(hex, timestamp)` composite index: Fast trail retrieval

### 3. API Response Caching

**Two-Level Cache:**
- Full enriched response: 2-second TTL
- Individual probability cones: 3-second TTL

**Cache Performance:**
- Hit rate: ~75% after warm-up
- Response time: 800ms → 50ms on cache hit (16x improvement)
- Shared server-side cache benefits all clients

### 4. Frontend Optimizations

**Smooth Animation:**
- requestAnimationFrame for 60 FPS
- Cubic easing function
- Heading wrap-around handling

**Failed Fetch Cache:**
- Prevents repeated requests for aircraft with no trail data
- Reduces unnecessary HTTP calls

**Position Deduplication:**
- Groups by hex, keeps latest position only
- Prevents duplicate markers

### 5. Connection Pooling

**ADS-B Streamer:** 1-5 connections
**Web Server:** Max 3 connections
**Rationale:** Supabase Session Pooler has strict limits (~15-20)

---

## Deployment and Operations

### System Requirements

**Raspberry Pi (ADS-B Receiver):**
- Model: Raspberry Pi 3/4/5
- OS: Raspberry Pi OS (Debian-based)
- RAM: 1GB minimum, 2GB+ recommended
- Storage: 8GB SD card minimum
- Network: Ethernet or WiFi with internet access

**RTL-SDR Hardware:**
- RTL2832U-based USB dongle
- 1090 MHz antenna (quarter-wave or optimized)
- USB 2.0 or 3.0 port

**Web Server (Cloud):**
- Platform: Railway, Render, Vercel, or similar
- RAM: 512MB minimum
- Node.js 20+ support

**Database:**
- PostgreSQL 13+
- Storage: 1GB minimum (grows with position history)
- Hosted on Supabase (free tier sufficient)

### Installation Steps

**1. Raspberry Pi Setup:**
```bash
# Update system
sudo apt-get update && sudo apt-get upgrade -y

# Install RTL-SDR and dump1090
sudo apt-get install rtl-sdr dump1090-fa -y

# Start dump1090
sudo systemctl enable dump1090-fa
sudo systemctl start dump1090-fa

# Verify reception
telnet localhost 30003
# Should see MSG,... lines
```

**2. Python Streamer Setup:**
```bash
# Clone repository
git clone <repo-url>
cd ads-b-receiver

# Create virtual environment
python3 -m venv venv
source venv/bin/activate

# Install dependencies
pip install psycopg2-binary python-dotenv

# Create .env file
cat > .env << EOF
DATABASE_URL=postgresql://user:pass@host:6543/postgres
EOF

# Run streamer
python3 adsb_streamer_optimized.py
```

**3. Database Setup (Supabase):**
```sql
-- Run schema.sql in Supabase SQL Editor
-- Creates tables: aircraft_real, aircraft_position_history, aircraft_sessions_real
```

**4. Web Server Deployment (Railway):**
```bash
# Install Railway CLI
npm install -g @railway/cli

# Login
railway login

# Deploy
cd ads
railway up

# Add environment variable in Railway dashboard
# DATABASE_URL = <supabase-connection-string>

# Get URL
railway domain
```

### Running as System Services

**ADS-B Streamer Service:**
```bash
# /etc/systemd/system/adsb-streamer.service
[Unit]
Description=ADS-B Data Streamer
After=network.target dump1090-fa.service

[Service]
Type=simple
User=pi
WorkingDirectory=/home/pi/ads-b-receiver
Environment=PATH=/home/pi/ads-b-receiver/venv/bin
ExecStart=/home/pi/ads-b-receiver/venv/bin/python3 adsb_streamer_optimized.py
Restart=always
RestartSec=10

[Install]
WantedBy=multi-user.target
```

```bash
sudo systemctl daemon-reload
sudo systemctl enable adsb-streamer.service
sudo systemctl start adsb-streamer.service
sudo journalctl -u adsb-streamer.service -f
```

### Monitoring and Logs

**ADS-B Streamer Logs:**
```bash
# View live logs
sudo journalctl -u adsb-streamer.service -f

# Expected output:
# Starting ADS-B data streamer (OPTIMIZED VERSION)
# Using CSV COPY for bulk inserts
# Successfully connected to dump1090
# Stats: 3233 msgs (107.1/s), 3233 parsed, 529 buffered, 0 errors
```

**Web Server Logs (Railway):**
```bash
railway logs

# Expected output:
# Connected to Supabase database
# Aircraft Tracker Server running on http://localhost:3000
# Loaded 12534 aircraft types
```

**Performance Metrics:**
- Message rate: 50-200 messages/second (depends on location/traffic)
- Database write throughput: 5,000 ops/sec
- API response time: 50-800ms (cache dependent)
- Cache hit rate: 70-80%

---

## Limitations and Future Work

### Current Limitations

**1. ADS-B Coverage:**
- Line-of-sight only (no coverage below horizon or behind terrain)
- Typical range: 150-250 nautical miles at altitude
- Ground-level aircraft may not be detected
- Requires aircraft to have ADS-B Out transponders (not all general aviation)

**2. Position Accuracy:**
- Dependent on aircraft GPS quality (~10-50 meters)
- No altitude verification (pressure altitude, not GPS altitude)
- Timestamp accuracy limited to SBS-1 protocol resolution

**3. Collision Detection:**
- Assumes constant velocity (Kalman filter model)
- Cannot predict intentional maneuvers (turns, altitude changes)
- Requires at least 2 historical positions for trajectory
- False positives possible during approach/departure

**4. Scalability:**
- In-memory cache limited to single server
- Database connection pool limits concurrent requests
- Client polling creates redundant traffic

**5. No Historical Playback:**
- Position history retention typically 2 hours
- No long-term storage for flight replay
- Session data not preserved after aircraft leaves

### Future Enhancements

**1. WebSocket Implementation:**
- Replace polling with WebSocket push
- Reduce network traffic by 90%
- Enable true real-time updates (<100ms latency)

**2. Advanced Prediction:**
- Machine learning trajectory prediction
- Flight plan integration (if available)
- Weather-aware path prediction
- Turn anticipation based on approach procedures

**3. Historical Replay:**
- Long-term position storage
- Time-slider UI for playback
- Export to KML/KMZ for Google Earth
- CSV export for analysis

**4. Multi-Receiver Support:**
- MLAT (Multilateration) for aircraft without ADS-B
- Receiver coverage visualization
- Data fusion from multiple receivers
- Improved position accuracy via triangulation

**5. Alerts and Notifications:**
- Email/SMS alerts for specific aircraft
- Collision warning notifications
- Low-altitude alerts
- Geofence entry/exit notifications

**6. Aircraft Database Integration:**
- FAA aircraft registry lookup
- Aircraft photos from public databases
- Operator information
- Aircraft type specifications

**7. Weather Integration:**
- METAR/TAF overlay
- Radar precipitation
- Turbulence reports
- Wind visualization

**8. Mobile Application:**
- React Native iOS/Android app
- Offline mode with cached data
- Push notifications
- Augmented reality view (point camera at sky)

**9. Performance Improvements:**
- Redis cache for distributed deployment
- Database read replicas
- CDN for static assets
- Horizontal scaling with load balancer

**10. Advanced Analytics:**
- Traffic flow analysis
- Altitude violation detection
- Noise pollution monitoring
- Flight path optimization suggestions

---

## Running the System

### Prerequisites Check

```bash
# Raspberry Pi: Verify dump1090 is running
systemctl status dump1090-fa
telnet localhost 30003

# Raspberry Pi: Verify database connectivity
psql "postgresql://..." -c "SELECT 1;"

# Raspberry Pi: Check Python environment
python3 --version  # Should be 3.9+
pip list | grep psycopg2

# Cloud: Verify Node.js
node --version  # Should be 20+
npm --version
```

### Starting the System

**Step 1: Start ADS-B Reception (Raspberry Pi)**
```bash
sudo systemctl start dump1090-fa
sudo systemctl status dump1090-fa
# Should show "active (running)"
```

**Step 2: Start Data Streamer (Raspberry Pi)**
```bash
cd ads-b-receiver
source venv/bin/activate
python3 adsb_streamer_optimized.py

# Expected output:
# Starting ADS-B data streamer (OPTIMIZED VERSION)
# Successfully connected to dump1090 at localhost:30003
# Successfully connected to database
# Stats: 125 msgs (125.0/s), 125 parsed, 42 buffered, 0 errors
```

**Step 3: Start Web Server (Cloud or Local)**
```bash
cd ads
npm install  # First time only
node server.js

# Expected output:
# Connected to Supabase database
# Loaded 12534 aircraft types
# Aircraft Tracker Server running on http://localhost:3000
```

**Step 4: Access Web Interface**
- 2D Map: http://localhost:3000 or https://your-app.up.railway.app
- 3D Globe: http://localhost:3000/3d.html

### Stopping the System

```bash
# Stop data streamer (Ctrl+C or)
sudo systemctl stop adsb-streamer.service

# Stop dump1090 (optional)
sudo systemctl stop dump1090-fa

# Stop web server (Ctrl+C or Railway handles automatically)
```

---

## Conclusion

This ADS-B aircraft tracking system successfully demonstrates real-time data acquisition, high-performance processing, advanced predictive analytics, and interactive visualization. Key technical achievements include 100-1000x database write performance improvements through CSV COPY operations, server-side Kalman filtering for trajectory prediction, probabilistic collision detection, and dual 2D/3D visualization with smooth animations.

The system operates continuously, receiving ADS-B signals from aircraft within ~200 nautical miles, processing 50-200 messages per second, and providing sub-second updates to multiple concurrent web clients. The architecture is scalable, maintainable, and built entirely with open-source technologies.

Future work will focus on WebSocket push notifications, machine learning trajectory prediction, historical flight replay, and mobile application development to enhance the system's capabilities and user experience.
