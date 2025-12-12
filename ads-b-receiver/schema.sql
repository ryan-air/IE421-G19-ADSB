-- Schema for Real ADS-B Data Tables
-- Separate from synthetic/mock data tables
-- Run this in your Supabase SQL editor to create the tables

-- Table: aircraft_real
-- Stores current state of real aircraft detected by ADS-B receivers
-- Similar to the 'aircraft' table but for real data
CREATE TABLE IF NOT EXISTS aircraft_real (
    hex VARCHAR(6) PRIMARY KEY,           -- Aircraft ICAO 24-bit hex identifier
    flight VARCHAR(10),                   -- Flight number/callsign
    lat DOUBLE PRECISION,                 -- Latitude in degrees
    lon DOUBLE PRECISION,                 -- Longitude in degrees
    altitude INTEGER,                     -- Altitude in feet
    speed INTEGER,                        -- Ground speed in knots
    track INTEGER,                        -- Track/heading in degrees (0-359)
    vertical_rate INTEGER,                -- Vertical rate in feet/minute
    squawk VARCHAR(4),                    -- Squawk code (transponder code)
    receiver_id VARCHAR(255),             -- ID of the receiver that last saw this aircraft
    seen_at TIMESTAMPTZ DEFAULT NOW(),    -- Last time this aircraft was seen
    created_at TIMESTAMPTZ DEFAULT NOW()  -- First time this aircraft was added
);

-- Index for quick lookups by position (for range queries)
CREATE INDEX IF NOT EXISTS idx_aircraft_real_position ON aircraft_real(lat, lon);

-- Index for quick lookups by receiver
CREATE INDEX IF NOT EXISTS idx_aircraft_real_receiver ON aircraft_real(receiver_id);

-- Index for cleanup queries (finding old aircraft)
CREATE INDEX IF NOT EXISTS idx_aircraft_real_seen_at ON aircraft_real(seen_at);


-- Table: aircraft_history_real
-- Stores position history for trail visualization
-- Each row is a snapshot of aircraft position at a specific time
CREATE TABLE IF NOT EXISTS aircraft_history_real (
    id BIGSERIAL PRIMARY KEY,
    hex VARCHAR(6) NOT NULL,              -- Aircraft hex identifier
    lat DOUBLE PRECISION NOT NULL,        -- Latitude
    lon DOUBLE PRECISION NOT NULL,        -- Longitude
    altitude INTEGER,                     -- Altitude in feet
    speed INTEGER,                        -- Ground speed in knots
    track INTEGER,                        -- Track in degrees
    receiver_id VARCHAR(255),             -- Receiver that recorded this position
    recorded_at TIMESTAMPTZ DEFAULT NOW() -- When this position was recorded
);

-- Index for querying history by aircraft
CREATE INDEX IF NOT EXISTS idx_aircraft_history_real_hex ON aircraft_history_real(hex, recorded_at DESC);

-- Index for cleanup queries (removing old history)
CREATE INDEX IF NOT EXISTS idx_aircraft_history_real_recorded_at ON aircraft_history_real(recorded_at);

-- Index for position-based queries
CREATE INDEX IF NOT EXISTS idx_aircraft_history_real_position ON aircraft_history_real(lat, lon);


-- Table: aircraft_sessions_real
-- Tracks when aircraft are first/last seen (flight sessions)
-- Useful for analytics and understanding flight durations
CREATE TABLE IF NOT EXISTS aircraft_sessions_real (
    hex VARCHAR(6) PRIMARY KEY,           -- Aircraft hex identifier
    flight VARCHAR(10),                   -- Flight number/callsign
    receiver_id VARCHAR(255),             -- Primary receiver for this session
    first_seen TIMESTAMPTZ NOT NULL,      -- When aircraft was first detected
    last_seen TIMESTAMPTZ NOT NULL,       -- When aircraft was last detected
    ended_at TIMESTAMPTZ,                 -- When session ended (NULL if active)
    created_at TIMESTAMPTZ DEFAULT NOW()
);

-- Index for finding active sessions
CREATE INDEX IF NOT EXISTS idx_aircraft_sessions_real_active ON aircraft_sessions_real(ended_at)
WHERE ended_at IS NULL;

-- Index for time-based queries
CREATE INDEX IF NOT EXISTS idx_aircraft_sessions_real_times ON aircraft_sessions_real(first_seen, last_seen);


-- Table: receiver_stats (optional - for tracking receiver performance)
-- Tracks statistics about each receiver
CREATE TABLE IF NOT EXISTS receiver_stats (
    receiver_id VARCHAR(255) PRIMARY KEY,
    last_message_at TIMESTAMPTZ,          -- Last time a message was received
    total_messages BIGINT DEFAULT 0,      -- Total messages received
    total_aircraft_seen BIGINT DEFAULT 0, -- Total unique aircraft seen
    updated_at TIMESTAMPTZ DEFAULT NOW()
);


-- Enable Row Level Security (RLS) if needed
-- Uncomment these lines if you want to enable RLS
-- ALTER TABLE aircraft_real ENABLE ROW LEVEL SECURITY;
-- ALTER TABLE aircraft_history_real ENABLE ROW LEVEL SECURITY;
-- ALTER TABLE aircraft_sessions_real ENABLE ROW LEVEL SECURITY;
-- ALTER TABLE receiver_stats ENABLE ROW LEVEL SECURITY;


-- Optional: Create views for easy querying

-- View: active_aircraft_real
-- Shows only aircraft that have been seen recently (within last 5 minutes)
CREATE OR REPLACE VIEW active_aircraft_real AS
SELECT
    a.*,
    EXTRACT(EPOCH FROM (NOW() - a.seen_at)) AS seconds_since_seen
FROM aircraft_real a
WHERE a.seen_at > NOW() - INTERVAL '5 minutes'
ORDER BY a.seen_at DESC;


-- View: active_sessions_real
-- Shows only active flight sessions (not ended)
CREATE OR REPLACE VIEW active_sessions_real AS
SELECT
    s.*,
    EXTRACT(EPOCH FROM (s.last_seen - s.first_seen)) AS session_duration_seconds
FROM aircraft_sessions_real s
WHERE s.ended_at IS NULL
ORDER BY s.last_seen DESC;


-- Function: cleanup_old_data_real
-- Automated cleanup function for old data
-- Call this periodically (e.g., via cron or scheduled job)
CREATE OR REPLACE FUNCTION cleanup_old_data_real(
    history_hours INTEGER DEFAULT 2,
    session_days INTEGER DEFAULT 7
) RETURNS TABLE (
    history_deleted BIGINT,
    sessions_deleted BIGINT
) AS $$
DECLARE
    history_count BIGINT;
    session_count BIGINT;
BEGIN
    -- Delete old history records
    DELETE FROM aircraft_history_real
    WHERE recorded_at < NOW() - (history_hours || ' hours')::INTERVAL;

    GET DIAGNOSTICS history_count = ROW_COUNT;

    -- Delete old ended sessions
    DELETE FROM aircraft_sessions_real
    WHERE ended_at IS NOT NULL
    AND ended_at < NOW() - (session_days || ' days')::INTERVAL;

    GET DIAGNOSTICS session_count = ROW_COUNT;

    RETURN QUERY SELECT history_count, session_count;
END;
$$ LANGUAGE plpgsql;


-- Grant permissions (adjust as needed for your setup)
-- GRANT SELECT, INSERT, UPDATE, DELETE ON aircraft_real TO authenticated;
-- GRANT SELECT, INSERT, DELETE ON aircraft_history_real TO authenticated;
-- GRANT SELECT, INSERT, UPDATE ON aircraft_sessions_real TO authenticated;
-- GRANT SELECT, UPDATE ON receiver_stats TO authenticated;


-- Table: collision_alerts
-- Tracks proximity violations between aircraft pairs
CREATE TABLE IF NOT EXISTS collision_alerts (
    id BIGSERIAL PRIMARY KEY,
    hex1 VARCHAR(6) NOT NULL,              -- First aircraft hex
    hex2 VARCHAR(6) NOT NULL,              -- Second aircraft hex
    flight1 VARCHAR(10),                   -- First aircraft flight number
    flight2 VARCHAR(10),                   -- Second aircraft flight number
    horizontal_distance_nm DOUBLE PRECISION, -- Horizontal distance in nautical miles
    vertical_distance_ft INTEGER,         -- Vertical distance in feet
    severity VARCHAR(20),                  -- 'warning', 'caution', 'critical'
    detected_at TIMESTAMPTZ DEFAULT NOW(), -- When alert was detected
    resolved_at TIMESTAMPTZ,               -- When alert was resolved (NULL if active)
    CONSTRAINT unique_alert_pair UNIQUE(hex1, hex2, resolved_at)
);

-- Index for active alerts
CREATE INDEX IF NOT EXISTS idx_collision_alerts_active ON collision_alerts(hex1, hex2, resolved_at)
WHERE resolved_at IS NULL;

-- Index for cleanup queries
CREATE INDEX IF NOT EXISTS idx_collision_alerts_detected_at ON collision_alerts(detected_at);

-- Grant permissions
GRANT SELECT, INSERT, UPDATE, DELETE ON collision_alerts TO authenticated;