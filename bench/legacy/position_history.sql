-- aircraft_position_history: written by the Fall 2025 streamer
-- (adsb_streamer_optimized.py) and read by the web server (ads/server.js), but
-- missing from ads-b-receiver/schema.sql. Definition copied from
-- PROJECT_REPORT.md ("Table: aircraft_position_history").
CREATE TABLE IF NOT EXISTS aircraft_position_history (
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
CREATE INDEX IF NOT EXISTS idx_aircraft_position_history_hex
    ON aircraft_position_history(hex, timestamp DESC);
CREATE INDEX IF NOT EXISTS idx_aircraft_position_history_timestamp
    ON aircraft_position_history(timestamp);
