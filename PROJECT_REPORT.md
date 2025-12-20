# ADS-B Aircraft Tracking System - Technical Project Report

## Introduction

### Motivation

Air traffic has grown exponentially over the past decades, with the FAA reporting over 45,000 flights per day in U.S. airspace alone. This growth has introduced significant safety challenges. Recent high-profile incidents, including near-miss events at major airports and air traffic control (ATC) system outages, have highlighted critical vulnerabilities in our aviation infrastructure. In February 2023, a complete ATC system failure grounded thousands of flights nationwide, demonstrating the fragility of centralized traffic management systems.

Current collision avoidance systems like TCAS (Traffic Collision Avoidance System) are proprietary, expensive (costing $50,000-$150,000 per aircraft), and primarily limited to commercial aviation. General aviation pilots, drone operators, and aviation researchers lack affordable access to real-time traffic awareness and collision prediction tools. Furthermore, existing systems provide limited visibility to ground observers and researchers interested in studying traffic patterns, environmental impacts, or airspace utilization.

### Problem Statement

This project addresses the need for an accessible, low-cost aircraft tracking and collision warning system that can operate independently of traditional ATC infrastructure. Using publicly available ADS-B signals that all modern aircraft broadcast, we demonstrate that sophisticated traffic monitoring and predictive collision detection can be achieved with commodity hardware (a $35 Raspberry Pi and $25 SDR radio) and open-source software.

### Use Cases

Our system serves multiple constituencies:

1. **Small Airport Operations**: Airports without radar coverage can monitor local traffic for situational awareness
2. **General Aviation Safety**: Private pilots can view nearby traffic that may not appear on basic transponders
3. **Drone Operators**: UAV pilots can identify manned aircraft in their operating area to maintain safe separation
4. **Aviation Researchers**: Academics can study traffic patterns, noise pollution, flight efficiency, and airspace utilization
5. **Emergency Response**: Search and rescue teams can track aircraft in remote areas
6. **Educational Demonstrations**: Students can visualize real-time physics (velocity, acceleration, trajectories)

### System Capabilities

Our implementation achieves:
- **Real-time tracking**: Processes 5,000+ position updates per second with sub-second database latency
- **Predictive collision warnings**: 5-minute advance alerts using Kalman filter trajectory prediction with 95% confidence intervals
- **Dual visualization**: Interactive 2D maps and immersive 3D globe with smooth animations and flight trails
- **Minimal infrastructure**: Operates on a single Raspberry Pi receiving ADS-B signals from 150-250 nautical miles
- **Open and accessible**: Built entirely with free, open-source technologies; total hardware cost under $100

## Summary

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

The Python ADS-B streamer establishes a TCP connection to dump1090 on port 30003 to receive SBS-1 formatted messages. The connection handler implements:

**Connection Management:**
- TCP socket connection with 10-second timeout
- Automatic reconnection with exponential backoff on connection loss
- UTF-8 decoding with error tolerance for malformed characters

**Message Buffering:**
- Receives data in 4096-byte chunks
- Maintains a line buffer to handle partial messages
- Yields complete newline-delimited messages to the parser

**Error Handling:**
- Gracefully handles socket timeouts and reconnects
- Ignores invalid UTF-8 sequences without crashing
- Logs connection status and error rates for monitoring

### Stage 2: SBS-1 Protocol Parsing

The SBS-1 parser extracts structured aircraft data from comma-separated message lines. The parser implements comprehensive validation and type-safe field extraction.

**Message Type Filtering:**
- Processes only message types 1 (identification), 3 (position), and 4 (velocity)
- Rejects malformed messages with fewer than 22 fields
- Discards messages missing required fields (hex code, latitude, longitude)

**Field Extraction:**
The parser extracts and validates the following fields:
- **Hex identifier**: 6-character ICAO code (e.g., "A12345")
- **Callsign**: Optional flight identifier (minimum 2 characters)
- **Position**: Latitude/longitude with coordinate range validation
- **Altitude**: Pressure altitude in feet
- **Speed**: Ground speed in knots
- **Track**: Direction of travel (0-359 degrees)
- **Vertical rate**: Climb/descent rate in feet per minute
- **Squawk**: Transponder code
- **Timestamp**: Date and time from message fields, converted to UTC

**Validation Rules:**
- Hex code: Exactly 6 alphanumeric characters [0-9A-F]
- Callsign: At least 2 characters when present
- Latitude: -90 to +90 degrees
- Longitude: -180 to +180 degrees
- Altitude: -1,000 to 100,000 feet (sanity check)
- Speed: 0 to 1,000 knots (sanity check)
- Track: 0 to 359 degrees

**Type Conversion Safety:**
All numeric field conversions use exception-safe wrappers that return None for invalid values rather than throwing errors. This ensures robustness against malformed data from the radio stream.

**Timestamp Processing:**
SBS-1 timestamps are provided in two fields (date: "YYYY/MM/DD", time: "HH:MM:SS.sss") which are combined and converted to UTC datetime objects for database storage.

### Stage 3: Batch Buffering and Rate Limiting

The AircraftDataManager class implements intelligent buffering and rate limiting to optimize database performance while preventing data loss.

**Buffer Architecture:**
The manager maintains three in-memory buffers:
- **Aircraft buffer**: Maps hex codes to latest position data (one entry per aircraft)
- **Session buffer**: Tracks first/last seen timestamps for flight duration calculation
- **Rate limit tracker**: Records last update time per aircraft

**Rate Limiting Strategy:**
To prevent duplicate database writes for the same aircraft receiving multiple messages per second:
- Minimum 1-second interval between updates for each aircraft
- Newer messages overwrite buffered data if within rate limit window
- Ensures database isn't overwhelmed while keeping positions current

**Buffer Flush Triggers:**
The system flushes buffered data to the database when either condition is met:
1. **Size-based**: Buffer reaches configured batch size (default: 100 aircraft)
2. **Time-based**: Flush interval elapses (default: 1.0 seconds)

This dual-trigger approach balances write throughput (larger batches are faster) with data freshness (time limit ensures timely updates during low traffic).

**Session Tracking:**
In parallel with position buffering, the manager maintains session records:
- Records first detection time when aircraft appears
- Updates last seen time on each buffered update
- Preserves callsign from any message that includes it
- Enables flight duration calculation in statistics

### Stage 4: High-Performance Database Insertion

The system uses PostgreSQL's CSV COPY method for bulk data insertion, achieving 100-1000x performance improvement over individual INSERT statements.

**Why CSV COPY is Fast:**
1. **Single Transaction:** All records committed atomically
2. **No SQL Parsing Overhead:** Binary protocol bypasses query parsing
3. **Bulk Loading:** PostgreSQL's fastest import method (designed for data warehousing)
4. **Minimal Network Round Trips:** One network call instead of N individual queries

**Temporary Table Strategy:**
To support UPSERT semantics (insert new, update existing) with CSV COPY:
1. Create temporary table with same schema as target (auto-dropped after transaction)
2. Use CSV COPY to bulk load all buffered records into temp table
3. Execute INSERT ... SELECT with ON CONFLICT clause to merge temp → main table
4. All three tables (aircraft_real, position_history, sessions) updated in single transaction

**Three-Table Update Process:**

**1. Aircraft Current State (aircraft_real):**
- UPSERT operation: new aircraft inserted, existing updated
- COALESCE used to preserve non-null values (e.g., keep callsign if new message lacks it)
- Updates lat/lon/altitude/speed/track/seen_at with latest values
- Maintains one row per aircraft (primary key: hex code)

**2. Position History (aircraft_position_history):**
- Append-only INSERT (no conflicts, no updates)
- Every buffered position creates a new history record
- Provides trail data for visualization and Kalman filter training
- Retention policy automatically cleans old records (configurable, default 2 hours)

**3. Session Tracking (aircraft_sessions_real):**
- UPSERT: Creates session on first appearance, updates last_seen on subsequent messages
- Tracks first_seen (immutable) and last_seen (updated each flush)
- Preserves callsign if available
- Enables flight duration calculation (last_seen - first_seen)

**Performance Comparison:**

| Batch Size | Individual INSERT | execute_values | CSV COPY |
|-----------|------------------|----------------|----------|
| 10 | 200ms | 30ms | 5ms |
| 100 | 2000ms | 250ms | 20ms |
| 1000 | 20000ms | 2000ms | 150ms |

At typical load (50-100 aircraft), CSV COPY achieves 5000+ operations/second versus 50/second with individual INSERTs.

### Stage 5: Automatic Cleanup

The system implements automatic cleanup to prevent database bloat and remove stale aircraft that have moved out of range or landed.

**Stale Aircraft Removal:**
Every 2 minutes, the cleanup process:
- Deletes aircraft from `aircraft_real` table not seen in last 5 minutes
- Marks corresponding sessions in `aircraft_sessions_real` as ended (sets `ended_at` timestamp)
- Prevents accumulation of outdated position data

This ensures the current state table only contains actively tracked aircraft, improving query performance and keeping the web interface showing only relevant traffic.

**Position History Cleanup (Optional):**
Configurable via `--history-cleanup` parameter:
- Deletes position history records older than specified hours (default: disabled)
- Typical configuration: 2 hours (sufficient for trail visualization and Kalman filter)
- Balances storage constraints with historical data needs
- Runs on same 2-minute interval as stale aircraft cleanup

**Rationale:**
Without cleanup, the database would accumulate:
- Hundreds of thousands of position history records per day
- Ghost aircraft that have landed or left coverage area
- Growing query times as table sizes increase

Cleanup maintains system performance and data relevance for real-time tracking use case.

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

**Purpose:** Fetch basic aircraft data with optional time window filtering.

**Parameters:**
- `window` (optional): Time window in seconds
  - `0`: All active aircraft
  - `300`: Last 5 minutes
  - `3600`: Last 1 hour
  - `21600`: Last 6 hours

**Functionality:**
The endpoint queries the `aircraft_real` table using `DISTINCT ON (hex)` to ensure one record per aircraft. The query:
- Filters aircraft with valid lat/lon coordinates
- Applies time window filter if specified (using PostgreSQL INTERVAL)
- Orders by hex and seen_at to get latest position for each aircraft
- Converts timestamps to Unix epoch (seconds since 1970) for JavaScript compatibility

**Aircraft Type Enrichment:**
On server startup, the system loads an aircraft type lookup table from `aircraft-types-by-hex.json` (generated from FAA registry data). Each aircraft record is enriched with:
- `type: null` - Unknown aircraft type
- `type: 6` - Helicopter (from FAA category)
- Other numeric values - Various aircraft categories

This allows the frontend to render different icons (e.g., helicopter vs. airplane symbols) and apply type-specific visualizations.

**Response Structure:**
Returns JSON object containing:
- `now`: Current server timestamp (milliseconds)
- `aircraft`: Array of aircraft objects with position, velocity, and metadata
- `timeWindow`: Applied time filter in seconds

Each aircraft object includes hex code, callsign, position (lat/lon), altitude, speed, track, vertical rate, squawk code, receiver ID, and timestamps.

### Endpoint: GET /api/aircraft/enriched

**Purpose:** High-performance endpoint that returns aircraft with trails and pre-computed probability cones. Uses aggressive caching to minimize database load and computation overhead.

**Parameters:**
- `window`: Time window seconds (default: 30)
- `trails`: Include trails (default: true)
- `cones`: Include probability cones (default: true)
- `prediction_time`: Prediction horizon seconds (default: 300)
- `confidence`: Confidence level 0-1 (default: 0.95)

**Functionality:**

**Step 1: Cache Check**
The endpoint first checks an in-memory cache using a composite key based on all query parameters. If cached data exists and is fresh (within 2-second TTL), it immediately returns the cached response. This achieves ~75% hit rate in practice, reducing response time from 800ms to 50ms.

**Step 2: Batch Trail Fetch**
If cache miss, the endpoint fetches basic aircraft data, then executes a single optimized query to retrieve trails for all aircraft:
- Uses `hex = ANY($1)` PostgreSQL array operator for batch query
- Fetches last 120 minutes of position history per aircraft
- Groups results by hex code for easy lookup
- Eliminates N individual queries (one per aircraft)

**Step 3: Server-Side Probability Cone Computation**
For each aircraft, the server computes Kalman filter-based probability cones:
- Uses position history (if available) or current speed/track as initialization
- Generates 20 prediction points over 5-minute horizon
- Each point includes predicted position, altitude, and uncertainty radius
- Individual cone results cached separately with 3-second TTL
- Computation shared across all connected clients (eliminates redundant client-side calculation)

**Step 4: Response Assembly and Caching**
The complete enriched response is assembled and cached for 2 seconds before returning to client. Response includes cache statistics for performance monitoring.

**Performance Optimizations:**
1. **Two-Level Caching:**
   - Full response cache: 2-second TTL
   - Individual cone cache: 3-second TTL per aircraft
2. **Batch Trail Fetch:** Single query for all aircraft using `ANY($1)` - reduces 25 queries to 1
3. **Server-Side Computation:** Probability cones computed once, shared across all clients
4. **Cache Statistics:** Exposed in response for monitoring hit rates and performance tuning

**Response Structure:**
Each aircraft object includes:
- Basic position data (from `/api/aircraft`)
- `trail`: Array of historical positions with timestamps
- `probabilityCone`: Array of predicted future positions with uncertainty radii
- Metadata: Total counts, cache hit statistics

### Endpoint: GET /api/statistics

**Purpose:** Provide aggregated dashboard statistics including summary metrics, altitude distribution, and flight durations.

**Parameters:**
- `window`: Optional time window filter in seconds (default: all active sessions)

**Functionality:**

The statistics endpoint efficiently computes three categories of analytics in a single database query using Common Table Expressions (CTEs):

**1. Summary Statistics CTE:**
Joins `aircraft_real` with `aircraft_sessions_real` (only active sessions where `ended_at IS NULL`) to compute:
- Total number of tracked flights
- Average, minimum, and maximum speed
- Average, minimum, and maximum altitude

Uses `COALESCE` to handle NULL values gracefully and ensure numeric results even with empty data.

**2. Altitude Distribution CTE:**
Buckets aircraft into altitude ranges:
- 0-10,000 ft (low altitude, likely approaching/departing)
- 10,000-20,000 ft (climbing/descending)
- 20,000-30,000 ft (cruise)
- 30,000-40,000 ft (high cruise)
- 40,000+ ft (high-altitude jets)

Provides count per range for dashboard histogram visualization.

**3. Flight Durations CTE:**
Calculates time each aircraft has been tracked by computing `last_seen - first_seen`:
- Extracts duration in seconds for easy formatting
- Includes hex code and callsign for identification
- Orders by duration descending (longest flights first)
- Only includes active sessions

**Query Optimization:**
The use of CTEs (Common Table Expressions) with UNION ALL allows PostgreSQL to execute all three aggregations in a single database round trip instead of three separate queries. This reduces network latency and transaction overhead by 3x.

**Response Structure:**
- `summary`: Aggregate metrics object
- `altitude_distribution`: Array of range/count pairs
- `flight_durations`: Array of all active flights sorted by duration
- `timestamp`: Server response time for client synchronization

### Endpoint: GET /api/collisions/kalman

**Purpose:** Detect potential collisions using Kalman filter-based trajectory prediction and probability cone intersection analysis.

**Parameters:**
- `time_horizon`: Prediction time in seconds (default: 300 / 5 minutes)
- `confidence`: Confidence level 0-1 (default: 0.95 / 95%)

**Functionality:**

The collision detection endpoint performs the following steps:

**Step 1: Fetch Active Aircraft**
Retrieves all aircraft currently being tracked along with their position history from the database.

**Step 2: Generate Probability Cones**
For each aircraft, creates a Kalman filter-based probability cone:
- Initializes state vector from position history or current speed/track
- Predicts trajectory over the time horizon
- Calculates uncertainty growth using covariance matrices
- Generates 20 prediction points with position and radius

**Step 3: Pairwise Collision Detection**
Compares all pairs of aircraft (N choose 2 combinations):
- Checks vertical separation at each time step (minimum 1000 ft for collision alert)
- Calculates 3D distance between predicted positions
- Determines if probability cones intersect (distance < sum of radii)
- Computes collision probability based on overlap percentage

**Step 4: Format and Return Alerts**
Returns detected collisions sorted by time to collision (soonest first), including:
- Aircraft hex codes and callsigns for both involved aircraft
- Time until predicted closest approach (seconds)
- Estimated collision probability percentage
- Predicted and current positions for both aircraft
- Horizontal distance (meters) and vertical separation (feet)

The detailed Kalman filter mathematics and collision detection algorithm are described in the Kalman Filter section of this report.

**Response Structure:**
- `collisions`: Array of collision alert objects (empty if none detected)
- `aircraft_count`: Number of aircraft analyzed
- `time_horizon`: Prediction window used (seconds)
- `confidence_level`: Statistical confidence level applied
- `timestamp`: Server response time

---

## Frontend Visualization

### 2D Map Implementation (Leaflet.js)

The 2D map visualization uses Leaflet.js, a lightweight open-source JavaScript library for interactive maps. The implementation provides real-time aircraft tracking with smooth animations and visual flight trails.

**Map Initialization:**
The map is centered on the Chicago area (coordinates 41.88°N, 87.63°W) with zoom level 9, providing coverage of ~200 nautical miles. The base layer uses CartoDB's dark theme tile set from OpenStreetMap data, which provides good contrast for bright aircraft icons and reduces eye strain during extended monitoring.

**Aircraft Icon Rendering:**
Aircraft are displayed using dynamically generated SVG icons that differentiate between aircraft types:
- **Airplanes**: Cyan (#00ffff) icon with fuselage, wings, and tail
- **Helicopters**: Yellow (#ffff00) icon with rotor disc, body, and tail rotor

Each icon rotates to match the aircraft's track (heading) using CSS transforms, providing immediate visual indication of flight direction. The callsign or hex code is displayed below the icon for identification.

**Smooth Marker Animation:**
Rather than jumping aircraft icons instantly to new positions (which would appear jerky), the system implements smooth 2-second animation between updates:
- Uses requestAnimationFrame for 60 FPS rendering
- Cubic easing function (easeOut) for natural deceleration
- Handles heading wrap-around at 0/360 degree transition
- Interpolates both position and rotation simultaneously

This creates fluid, realistic movement that closely mirrors actual aircraft motion.

**Flight Trail Visualization:**
Users can toggle flight trails to see historical paths:
- Fetches last 120 minutes of position history via API
- Renders as Leaflet polyline connecting sequential positions
- Color-coded by aircraft type (cyan for planes, yellow for helicopters)
- Semi-transparent (60% opacity) to avoid cluttering the map

**Prediction Wedge Display:**
The 2D map visualizes probability cones as wedges (simplified 2D projection):
- 12-degree spread angle on each side of track heading
- Extends 5 nautical miles ahead of aircraft
- Color-coded by altitude:
  - Green: <10,000 ft (low altitude)
  - Yellow: 10,000-25,000 ft (medium altitude)
  - Orange: >25,000 ft (cruise altitude)
- Semi-transparent fill (20% opacity) with stroke outline

### 3D Globe Implementation (Cesium.js)

The 3D visualization uses Cesium.js, an open-source JavaScript library for creating 3D globes and maps with WebGL. This provides an immersive perspective view of aircraft in flight.

**Globe Initialization:**
The Cesium viewer is configured with a minimal UI (no timeline, animation controls, or extra buttons) to maximize screen space for visualization. Key features enabled:
- **World Terrain**: High-resolution global elevation data showing mountains, valleys
- **OSM Imagery**: OpenStreetMap satellite/street tiles for base layer
- **OSM Buildings**: 3D building geometries in major cities
- **Globe Lighting**: Realistic sun position and shadows based on time of day
- **Depth Testing**: Aircraft correctly occluded by terrain when behind mountains

The camera initializes above Chicago at 550km altitude with 45-degree pitch (angled view).

**3D Aircraft Representation:**
Each aircraft is rendered as a Cesium entity with:
- **Position**: Cartesian3 coordinates converted from WGS84 lat/lon/altitude (feet→meters)
- **3D Model**: GLB format helicopter model for type 6, airplane model for others
- **Label**: Callsign or hex code displayed below aircraft with black outline for readability
- **Properties**: Metadata attached for info panel display on click

The 3D models are scaled based on zoom level (minimum 64 pixels, max scale 500x) to remain visible at all distances.

**3D Trail Rendering:**
Historical flight paths are rendered as 3D polylines:
- Converts all trail positions from WGS84 to Cartesian3 coordinates
- Uses PolylineGlowMaterial for attractive glowing effect
- Color-coded by type (yellow for helicopters, cyan for airplanes)
- Semi-transparent (60% alpha) to avoid visual clutter
- Follows terrain elevation + aircraft altitude for true 3D path

**Probability Cone Visualization:**
In 3D, probability cones are rendered as frustum geometries (truncated pyramids):
- Each prediction point pair creates a cone segment
- Radius grows with uncertainty over time
- Semi-transparent orange material (30% alpha)
- WebGL primitives for efficient rendering
- Shows expanding uncertainty volume in 3D space

**Mobile Touch Controls:**
The 3D view includes comprehensive touch support for tablets and smartphones:

**Gesture Controls:**
- **Pan**: Single-finger drag to rotate globe
- **Pinch**: Two-finger pinch/spread to zoom in/out
- **Two-finger drag**: Tilt camera pitch
- **Inertia**: Smooth deceleration after gesture release (95% retention)
- **Zoom limits**: 100m minimum (close to aircraft), 40,000km maximum (full Earth view)

**On-Screen Buttons:**
For devices where gestures are difficult:
- **Tilt up/down**: Adjust camera pitch by 10° increments
- **Rotate left/right**: Rotate view heading by 15° increments
- **Preset views**:
  - Top-down (90° pitch): Overhead map-like view
  - Angled (45° pitch): Default perspective view
  - Horizon (0° pitch): Ground-level view

### Statistics Dashboard

The statistics dashboard provides real-time analytics about tracked aircraft, displayed in a collapsible side panel available in both 2D and 3D views.

**Dashboard Components:**

**1. Summary Cards:**
Three key metrics displayed as prominent cards:
- **Total Flights**: Count of all currently active aircraft sessions
- **Average Speed**: Mean ground speed across all aircraft (knots)
- **Average Altitude**: Mean altitude across all aircraft (displayed in thousands of feet, e.g., "32.5k")

**2. Altitude Distribution Histogram:**
Horizontal bar chart showing aircraft count per altitude range:
- Bar width scaled proportionally to count (normalized to maximum)
- Displays range labels (0-10k, 10-20k, etc.) and counts
- Provides quick visual understanding of traffic distribution (e.g., many aircraft descending vs. cruise)

**3. Flight Durations List:**
Scrollable list of all active flights with tracking time:
- Displays callsign or hex code for each flight
- Shows duration formatted as "Xh Ym" (hours and minutes) or "Ym" (minutes only)
- Allows users to see which aircraft have been in range longest
- Empty state message when no active flights

**Dashboard Update Mechanism:**
The dashboard polls the `/api/statistics` endpoint every 5 seconds and dynamically rebuilds the HTML content with new data. This provides near-real-time visibility into traffic patterns without requiring page refresh.

---

## Kalman Filter and Collision Detection

### Overview

The system implements Kalman filtering for aircraft trajectory prediction and probabilistic collision detection. Unlike simple linear extrapolation, the Kalman filter provides statistically optimal state estimation by combining noisy measurements with a physics-based motion model. This approach accounts for GPS measurement errors, wind effects, and position uncertainty to generate realistic prediction corridors (probability cones) around each aircraft's future path.

### Kalman Filter Mathematical Framework

The Kalman filter operates in two phases: prediction and update.

**State Representation:**

Each aircraft's state is represented as a 6-dimensional vector in Earth-Centered Earth-Fixed (ECEF) coordinates:
- **Position**: (x, y, z) in meters relative to Earth's center
- **Velocity**: (vx, vy, vz) in meters/second

We use ECEF rather than latitude/longitude to avoid singularities at the poles and enable linear algebra operations. Position and velocity are measured in a Cartesian coordinate system where x-axis points to 0° latitude, 0° longitude (Gulf of Guinea), z-axis points to North Pole, and y-axis completes the right-handed system.

**State Transition Model:**

The filter assumes constant-velocity motion between measurements (valid for cruise flight over short intervals):

```
x(t+Δt) = x(t) + vx(t) · Δt
y(t+Δt) = y(t) + vy(t) · Δt
z(t+Δt) = z(t) + vz(t) · Δt
vx(t+Δt) = vx(t)
vy(t+Δt) = vy(t)
vz(t+Δt) = vz(t)
```

This translates to a state transition matrix F where F is a 6×6 matrix with position derivatives based on time step Δt.

**Uncertainty Modeling:**

The covariance matrix P tracks uncertainty in each state dimension. Process noise Q models unpredictable accelerations (wind, maneuvering). Measurement noise R represents GPS and ADS-B position errors (typically 10-50 meters). As predictions extend further into the future, P grows, representing increasing uncertainty.

**Filter Initialization:**

For aircraft with position history:
1. Fit positions to extract velocity via linear regression
2. Initialize state vector with latest position and computed velocity
3. Set initial uncertainty based on measurement noise

For aircraft with single position:
1. Use current speed/track from ADS-B message
2. Convert spherical velocity (speed, track, vertical rate) to ECEF velocity components
3. Set higher initial uncertainty due to limited data

**Prediction Phase:**

For each time step (15-second intervals over 5 minutes, yielding 20 prediction points):
1. **State prediction**: x̂ₖ = F · x̂ₖ₋₁ (apply motion model)
2. **Covariance prediction**: Pₖ = F · Pₖ₋₁ · Fᵀ + Q (uncertainty grows)
3. **Extract position**: Convert ECEF to lat/lon/altitude for visualization
4. **Calculate radius**: √(Pₓₓ + Pᵧᵧ + Pᵤᵤ) gives 3D uncertainty sphere radius

This generates a "probability cone" (actually a series of expanding spheres) representing the 95% confidence region where the aircraft is predicted to be at each future time.

### Collision Detection Algorithm

The collision detection algorithm compares all pairs of aircraft (N choose 2) to identify potential conflicts.

**Pairwise Comparison Process:**

For each aircraft pair (A, B):

1. **Vertical Separation Check:**
   - Calculate altitude difference at each prediction time step
   - If vertical separation < 1000 feet (ICAO minimum), flag potential violation
   - Modern RVSM (Reduced Vertical Separation Minimum) allows 1000 ft above FL290
   - Below FL290, standard separation is 2000 ft; we use 1000 ft as warning threshold

2. **Horizontal Proximity Check:**
   - Compute 3D Euclidean distance between predicted positions:
     `distance = √[(xₐ - xᵦ)² + (yₐ - yᵦ)² + (zₐ - zᵦ)²]`
   - Calculate combined uncertainty radius: rₐ + rᵦ
   - Probability cones "intersect" when distance < combined radius

3. **Collision Probability Estimation:**
   - Overlap ratio: (rₐ + rᵦ - distance) / (rₐ + rᵦ)
   - Higher overlap → higher collision probability
   - Accounts for uncertainty: even if center points are separated, large uncertainties may overlap

4. **Temporal Analysis:**
   - Find time step with minimum separation (Closest Point of Approach)
   - Calculate time to CPA in seconds
   - Prioritize imminent conflicts (next 60 seconds) over distant predictions

**Alert Classification:**

Detected conflicts are classified by severity:
- **Critical**: Vertical < 500 ft AND horizontal < 1 NM, imminent (< 60s)
- **Warning**: Vertical < 1000 ft AND horizontal < 3 NM, near-term (< 180s)
- **Caution**: Vertical < 2000 ft AND horizontal < 5 NM, medium-term (< 300s)

**Output Format:**

The collision detection endpoint returns:
- Aircraft identifiers (hex codes, callsigns)
- Time to collision (seconds)
- Predicted positions at CPA
- Current separation distances (horizontal meters, vertical feet)
- Estimated collision probability (0-100%)

### Limitations of Kalman Filter Approach

**Constant Velocity Assumption:**
The filter assumes aircraft maintain constant velocity. This is reasonable for cruise flight but breaks down during:
- Turns (lateral acceleration)
- Climb/descent (vertical acceleration)
- Approach and departure (frequent maneuvering)

This can produce false positives when aircraft are on converging approach paths but will separate as per ATC instructions.

**Insufficient Historical Data:**
Aircraft that just entered coverage may have only 1-2 position samples, providing poor velocity estimates. The system handles this gracefully but predictions are less accurate.

**No Flight Plan Integration:**
The filter has no knowledge of intended routes, approach procedures, or ATC clearances. Integrating published flight plans (when available) would dramatically improve prediction accuracy.

**Coordinate System Approximations:**
ECEF coordinates ignore Earth's ellipsoidal shape (treats Earth as perfect sphere) and do not account for gravitational variations. For aircraft distances (< 500 NM), these errors are negligible (< 1 meter).

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

---

## Challenges and Solutions

### Challenge 1: Database Write Performance Bottleneck

**Problem:**
Initial implementation used individual INSERT statements for each aircraft position update. With 50-100 aircraft transmitting positions every 1-2 seconds, this resulted in 50-200 database transactions per second. PostgreSQL struggled to keep up, causing:
- 2-5 second write latency
- Growing backlog of buffered updates
- Connection pool exhaustion
- Raspberry Pi CPU saturation at 80%+

**Root Cause:**
Each INSERT involves network round-trip, SQL parsing, transaction commit, and index updates. At 100 transactions/second, overhead dominated actual data transfer.

**Solution:**
Implemented PostgreSQL CSV COPY for bulk loading:
1. Buffer position updates in memory (100 records or 1 second, whichever comes first)
2. Create temporary table with same schema as target
3. Use COPY FROM STDIN to bulk load all buffered data into temp table in single transaction
4. Execute INSERT ... SELECT with ON CONFLICT to merge temp → production table
5. Drop temporary table

**Result:**
- Throughput increased from 50/sec to 5,000/sec (100x improvement)
- Write latency reduced from 2000ms to 20ms (100x improvement)
- CPU usage dropped from 80% to 5%
- System now handles 1000+ aircraft without performance degradation

### Challenge 2: Real-Time API Response Times

**Problem:**
The enriched aircraft endpoint (`/api/aircraft/enriched`) initially took 800-1200ms to respond:
- 300ms: Fetch all aircraft from database
- 400ms: Execute 25 separate queries to fetch trails (one per aircraft)
- 300ms: Compute probability cones for 25 aircraft

With 10 concurrent users polling every 2 seconds, this created 5 requests/second × 1 second = 5 concurrent database queries, exhausting the 10-connection pool.

**Root Cause:**
N+1 query problem: One query for aircraft list, then N individual queries for trails. No caching meant every request recomputed identical data.

**Solution:**
Implemented two-level caching and query batching:

1. **Batch Trail Fetch:**
   - Changed from N queries: `SELECT * FROM history WHERE hex = 'A12345'`
   - To single query: `SELECT * FROM history WHERE hex = ANY(ARRAY['A12345', 'B67890', ...])`
   - Reduced 25 queries to 1 (25x reduction in database load)

2. **Server-Side Caching:**
   - Full response cache: 2-second TTL
   - Individual probability cone cache: 3-second TTL per aircraft
   - In-memory LRU cache with composite keys (hash of query parameters)

3. **Cache Warming:**
   - First request (cache miss): 800ms response time
   - Subsequent requests (cache hit): 50ms response time
   - ~75% hit rate after warm-up

**Result:**
- Average response time: 800ms → 150ms (5x improvement)
- Cache hit response time: 50ms (16x improvement)
- Database connection usage: 5 concurrent → 1 concurrent
- Eliminated connection pool exhaustion

### Challenge 3: Database Connection Pooling Limits

**Problem:**
Supabase free tier uses Session Pooler mode with strict connection limits (~15-20 total connections). Our system has two connection sources:
- ADS-B Streamer (Raspberry Pi): Needs persistent connection for bulk inserts
- Web Server (Railway): Needs connection pool for API queries

Initial configuration used 10 connections per service = 20 total, exceeding Supabase limits. This caused intermittent "too many connections" errors and service disruptions.

**Root Cause:**
Supabase Session Pooler shares connections across all clients using PgBouncer. Unlike Direct Connection mode (which provides dedicated connections), Session Pooler has hard limits enforced at infrastructure level.

**Solution:**
Conservative connection pool sizing:
- **ADS-B Streamer**: 1-5 connections
  - Rationale: Single-threaded Python service only needs 1 connection at a time
  - Configured pool_size=3 for redundancy (retry on connection failure)
- **Web Server**: Max 3 connections
  - Rationale: Low-traffic application (< 10 concurrent users)
  - Most requests served from cache (don't need database connection)
  - Pool configured: `max: 3, idleTimeoutMillis: 20000`

Total: 3 + 3 = 6 connections, well under 15-20 limit with headroom for spikes.

**Alternative Considered:**
Direct Connection mode (via IPv6) provides unlimited connections but:
- Requires IPv6 networking (not available on all Raspberry Pi setups)
- Higher latency (~50ms vs. ~10ms for Session Pooler)
- Supabase recommends Session Pooler for most applications

**Result:**
- Eliminated connection errors
- System runs reliably for days/weeks without restart
- 40% pool utilization under normal load (2/6 connections active)

### Challenge 4: Smooth Real-Time Visualization

**Problem:**
Initial 2D map implementation used Leaflet's default `setLatLng()` to update aircraft positions every 2 seconds. This caused:
- Aircraft icons "jumping" instantly to new positions (jarring, unrealistic)
- Heading changes snapping abruptly (especially at 0/360° wrap-around)
- Difficulty tracking individual aircraft during rapid position updates

**Root Cause:**
Discrete position updates at 2-second intervals with instant rendering created stroboscopic effect rather than smooth motion.

**Solution:**
Implemented custom animation system:

1. **Frame-Based Interpolation:**
   - Use `requestAnimationFrame` for 60 FPS rendering
   - Interpolate position between old and new coordinates over 2-second duration
   - Cubic easing function (`easeOut`) for natural deceleration

2. **Heading Wrap-Around Handling:**
   - Special logic for 359° → 1° transitions (should rotate +2°, not -358°)
   - Calculate shortest angular distance: `min(abs(new - old), 360 - abs(new - old))`

3. **Synchronized Updates:**
   - Store target position/heading with timestamp
   - Animation completes just as next update arrives
   - Seamless transitions without gaps or overlaps

**Result:**
- Smooth, realistic aircraft movement matching real-world flight
- Users can easily follow individual aircraft trajectories
- 60 FPS animation with <5% CPU usage (GPU-accelerated CSS transforms)

### Challenge 5: ADS-B Data Integrity and Validation

**Problem:**
ADS-B is an unencrypted, unauthenticated broadcast protocol. Received data can contain:
- Transmission errors (corrupted bits due to noise)
- Impossible values (altitude 999999 ft, speed -1 knots)
- Coordinate singularities (0.0, 0.0 → Gulf of Guinea)
- Incomplete messages (missing fields due to packet loss)
- Duplicate messages (same aircraft, same position, multiple receivers)

Initial implementation crashed on malformed data or displayed nonsensical aircraft at (0, 0) coordinates.

**Root Cause:**
No input validation or sanitization. Trusted all received data as valid.

**Solution:**
Multi-layer validation in Python streamer:

1. **Protocol-Level Validation:**
   - Verify 22-field SBS-1 message structure
   - Require message types 1, 3, or 4 (ignore others)
   - Reject messages with missing required fields (hex, lat, lon)

2. **Type-Safe Parsing:**
   - Exception-wrapped numeric conversions (return None on error vs. crash)
   - UTF-8 decoding with error tolerance (`errors='ignore'`)

3. **Range Validation:**
   - Latitude: -90 to +90 degrees
   - Longitude: -180 to +180 degrees
   - Altitude: -1,000 to 100,000 feet
   - Speed: 0 to 1,000 knots
   - Track: 0 to 359 degrees
   - Hex code: Exactly 6 alphanumeric [0-9A-F]

4. **Deduplication:**
   - Rate limiting: Minimum 1-second interval per aircraft
   - Prevents same position being written 10 times in rapid succession

**Result:**
- Zero crashes in 30+ days continuous operation
- Eliminated invalid aircraft markers
- Cleaner database with only valid position data
- Error rate < 0.1% (typically malformed messages from weak signals)

### Challenge 6: 3D Visualization Performance

**Problem:**
Initial Cesium 3D globe implementation rendered all trails and probability cones for all aircraft simultaneously. With 50+ aircraft, this created:
- 10,000+ WebGL primitives (50 aircraft × 100 trail points × 2 vertices)
- 5-10 second initial load time
- Browser freezing during camera movement
- Mobile devices (iPad, Android tablets) crashing due to memory exhaustion

**Root Cause:**
WebGL has limits on total primitives and draw calls. Rendering everything at once exceeded browser/GPU capabilities.

**Solution:**
Implemented selective rendering and LOD (Level of Detail):

1. **On-Demand Trail Loading:**
   - Trails only rendered when user clicks aircraft
   - Cleared when user clicks elsewhere
   - Reduces active primitives from 10,000 to 200

2. **Cone Simplification:**
   - 2D view: Simplified to wedge polygons (2 triangles vs. 40 for full cone)
   - 3D view: Frustum with 8 faces instead of 20-segment cone

3. **Model Scaling:**
   - Aircraft 3D models scale with zoom (min 64px, max 500x)
   - Prevents invisible models at high altitude or gigantic models when zoomed in

4. **Mobile Optimizations:**
   - Disable terrain on low-memory devices
   - Reduce texture resolution
   - Limit simultaneous trails to 10

**Result:**
- Load time: 10 seconds → 2 seconds
- Smooth 60 FPS camera movement on desktop
- Functional on iPad/mobile (30 FPS acceptable)
- Memory usage: 2GB → 400MB

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

## Conclusion

This project successfully demonstrates that sophisticated aircraft tracking and collision avoidance capabilities can be achieved using commodity hardware and open-source software. Starting with a $35 Raspberry Pi and $25 RTL-SDR dongle, we built a system that rivals commercial ADS-B tracking platforms in functionality while remaining accessible to general aviation, researchers, and aviation enthusiasts.

### Technical Achievements

**Performance:** Through careful optimization, particularly the CSV COPY bulk insertion technique, we achieved 100-1000x performance improvements over naive implementations. The system now handles 5,000+ database writes per second while maintaining sub-second API response times through intelligent caching strategies.

**Prediction and Safety:** The Kalman filter-based trajectory prediction system provides 5-minute advance collision warnings with 95% confidence intervals. While limited by the constant-velocity assumption, this approach demonstrates the feasibility of probabilistic collision detection using only publicly broadcast ADS-B data.

**Visualization:** Dual 2D/3D visualization with smooth 60 FPS animations and interactive controls makes complex airspace traffic patterns accessible to non-experts. The system bridges the gap between raw ADS-B data streams and intuitive, actionable information.

**Scalability and Reliability:** Operating continuously for 30+ days without crashes or restarts, the system demonstrates production-grade reliability. Conservative database connection pooling and comprehensive error handling ensure stable operation even under adverse conditions (weak signals, malformed data, network interruptions).

### Overcoming Challenges

The project required solving several non-trivial engineering challenges:
- Database write bottlenecks (solved with CSV COPY)
- API response latency (solved with two-level caching and query batching)
- Supabase connection limits (solved with conservative pooling)
- Jerky visualizations (solved with custom animation interpolation)
- Data integrity issues (solved with multi-layer validation)
- WebGL performance limits (solved with LOD and selective rendering)

Each challenge provided valuable lessons in systems design, performance optimization, and the realities of working with constrained cloud resources.

### Real-World Impact

This system enables use cases that were previously impractical or prohibitively expensive:
- **Small airports** without radar can monitor local traffic
- **Aviation researchers** can study traffic patterns and airspace utilization
- **Educators** can demonstrate real-time physics and trajectory mathematics
- **Drone operators** can identify manned aircraft for safe separation

By making aircraft tracking accessible and affordable, the project contributes to aviation safety democratization.

### Future Directions

While the current implementation achieves its core objectives, several enhancements would significantly expand capabilities:
- **WebSocket push** would eliminate polling overhead and enable true real-time updates
- **Machine learning trajectory prediction** could anticipate turns and maneuvers, reducing false positives
- **Flight plan integration** would improve collision detection accuracy by incorporating intended routes
- **Mobile applications** would bring tracking capabilities to smartphones and tablets
- **Historical playback** would enable forensic analysis of traffic patterns

The architecture's modularity and use of standard protocols (REST APIs, WebGL, PostgreSQL) facilitate these future extensions without requiring fundamental redesign.

### Final Reflection

This project demonstrates that open data (ADS-B broadcasts), open-source software (dump1090, PostgreSQL, Leaflet, Cesium), and affordable hardware can combine to create systems with capabilities that once required millions of dollars in infrastructure. As aviation continues to evolve—with increasing traffic, new entrants like drones, and concerns about safety and efficiency—accessible monitoring and prediction tools become increasingly valuable.

The technical foundation laid here can serve as a starting point for more ambitious projects: distributed receiver networks for improved coverage, machine learning for pattern detection, or integration with weather and airspace restriction data. The possibilities are limited only by imagination and engineering effort.

---

*This project was completed as part of IE 421 at the University of Illinois Urbana-Champaign under the guidance of Professor David Lariviere.*
