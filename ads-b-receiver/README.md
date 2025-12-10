# ADS-B Data Streamer

Python-based real-time ADS-B data streaming from dump1090 to Supabase. This streamer consumes live aircraft data from your Raspberry Pi running dump1090 and stores it in separate tables from synthetic/mock data.

## Features

- **Real-time streaming** from dump1090 SBS-1 BaseStation format (port 30003)
- **Separate database tables** for real vs synthetic data
- **Multiple receiver support** with automatic handling of duplicate updates
- **Smart deduplication** - accepts data from multiple receivers without conflicts
- **Automatic cleanup** of old aircraft and position history
- **Rate limiting** to prevent database flooding
- **Reconnection handling** for network interruptions
- **Position trail tracking** for flight path visualization

## Architecture

### Data Flow
```
dump1090 (Raspberry Pi) → SBS-1 Port 30003 → Python Streamer → Supabase
```

### Database Tables

Real data tables (separate from synthetic data):

- **aircraft_real** - Current state of all detected aircraft
- **aircraft_history_real** - Position snapshots for trail visualization
- **aircraft_sessions_real** - Flight session tracking (first seen / last seen)
- **receiver_stats** - (Optional) Receiver performance statistics

Synthetic data tables (existing):
- **aircraft** - Mock/generated aircraft data
- **aircraft_history** - Mock position trails
- **aircraft_sessions** - Mock flight sessions

## Setup

### 1. Install Dependencies

On your Raspberry Pi (or wherever you run the streamer):

```bash
cd ads-b-receiver

# Create virtual environment (recommended)
python3 -m venv venv
source venv/bin/activate

# Install dependencies
pip install -r requirements.txt
```

### 2. Configure Database

Run the SQL schema in your Supabase SQL editor:

```bash
# Copy the contents of schema.sql and run it in Supabase SQL editor
cat schema.sql
```

Or use the Supabase CLI:

```bash
supabase db execute --file schema.sql
```

### 3. Set Environment Variables

Create a `.env` file or export the database URL:

```bash
# Option 1: Create .env file
echo "DATABASE_URL=postgresql://postgres.xxx:password@aws-x-xx-x.pooler.supabase.com:5432/postgres" > .env

# Option 2: Export environment variable
export DATABASE_URL="postgresql://postgres.xxx:password@aws-x-xx-x.pooler.supabase.com:5432/postgres"
```

**Important:** Use the **Session Pooler** connection string from Supabase (port 5432 or 6543), not the direct connection string.

### 4. Verify dump1090 is Running

On your Raspberry Pi, verify dump1090 is receiving data:

```bash
# Test connection to dump1090
nc localhost 30003 | head -20

# You should see output like:
# MSG,3,111,11111,A12345,111111,2025/12/08,19:40:05.123,2025/12/08,19:40:05.123,,35000,...
```

## Usage

### Basic Usage

Run the streamer on the same machine as dump1090:

```bash
python3 adsb_streamer.py
```

### Remote dump1090

If dump1090 is on a different machine:

```bash
python3 adsb_streamer.py --host 192.168.1.100 --port 30003
```

### Multiple Receivers

Run multiple streamers with unique receiver IDs:

```bash
# Receiver 1
python3 adsb_streamer.py --receiver-id receiver-pi-1 --host 192.168.1.100

# Receiver 2 (on different machine or connecting to different dump1090)
python3 adsb_streamer.py --receiver-id receiver-pi-2 --host 192.168.1.101
```

The system automatically handles duplicate data - if two receivers see the same plane, updates are merged intelligently.

### Full Options

```bash
python3 adsb_streamer.py \
  --host localhost \
  --port 30003 \
  --receiver-id my-receiver-1 \
  --db-url "postgresql://..."
```

**Arguments:**
- `--host` - dump1090 host address (default: localhost)
- `--port` - dump1090 SBS-1 port (default: 30003)
- `--receiver-id` - Unique receiver identifier (default: hostname)
- `--db-url` - PostgreSQL connection URL (default: DATABASE_URL env var)

### Running as a Service

Create a systemd service to run the streamer automatically:

```bash
# Create service file
sudo nano /etc/systemd/system/adsb-streamer.service
```

Add this content:

```ini
[Unit]
Description=ADS-B Data Streamer
After=network.target dump1090.service

[Service]
Type=simple
User=pi
WorkingDirectory=/home/pi/ads-b-receiver
Environment="DATABASE_URL=postgresql://postgres.xxx:password@aws-x-xx-x.pooler.supabase.com:5432/postgres"
ExecStart=/home/pi/ads-b-receiver/venv/bin/python3 /home/pi/ads-b-receiver/adsb_streamer.py
Restart=always
RestartSec=10

[Install]
WantedBy=multi-user.target
```

Enable and start the service:

```bash
sudo systemctl daemon-reload
sudo systemctl enable adsb-streamer
sudo systemctl start adsb-streamer

# Check status
sudo systemctl status adsb-streamer

# View logs
sudo journalctl -u adsb-streamer -f
```

## How It Works

### SBS-1 Message Format

dump1090 outputs messages in SBS-1 BaseStation format:

```
MSG,transmission_type,session_id,aircraft_id,hex,flight_id,
date_gen,time_gen,date_log,time_log,callsign,altitude,speed,
track,lat,lon,vertical_rate,squawk,alert,emergency,spi,is_on_ground
```

Example:
```
MSG,3,111,11111,A12345,111111,2025/12/08,19:40:05.123,2025/12/08,19:40:05.123,UAL123,35000,450,180,40.1164,-88.2434,0,1200,,,,,
```

### Deduplication Strategy

The streamer handles multiple receivers gracefully:

1. **Rate Limiting** - Updates for the same aircraft are rate-limited (1 update per second per aircraft)
2. **COALESCE Updates** - When multiple receivers provide data, non-null values are preferred
3. **Receiver Tracking** - The `receiver_id` field tracks which receiver last saw the aircraft
4. **No Data Loss** - All position history is stored, even from multiple receivers

This means:
- If receiver A sees position updates but not altitude
- And receiver B sees altitude updates but not position
- Both updates will be merged correctly

### Automatic Cleanup

The streamer automatically:
- Removes aircraft not seen in 5 minutes
- Deletes position history older than 2 hours
- Marks flight sessions as ended when aircraft disappear

You can adjust these values in the code or create a scheduled cleanup job.

## Monitoring

### Statistics

The streamer prints statistics every 30 seconds:

```
2025-12-09 19:45:30 - INFO - Stats: 1245 msgs (41.5/s), 1123 parsed, 456 DB updates, 0 errors
```

### Database Queries

Check active aircraft:

```sql
SELECT * FROM active_aircraft_real;
```

Check flight sessions:

```sql
SELECT * FROM active_sessions_real;
```

Get aircraft count by receiver:

```sql
SELECT receiver_id, COUNT(*) as aircraft_count
FROM aircraft_real
GROUP BY receiver_id;
```

View recent trails for a specific aircraft:

```sql
SELECT hex, lat, lon, altitude, recorded_at
FROM aircraft_history_real
WHERE hex = 'A12345'
ORDER BY recorded_at DESC
LIMIT 100;
```

## Troubleshooting

### Connection Issues

```bash
# Test dump1090 connection
nc localhost 30003

# Check if dump1090 is running
ps aux | grep dump1090

# Check port
netstat -ln | grep 30003
```

### Database Issues

```bash
# Test database connection
psql "$DATABASE_URL" -c "SELECT NOW();"

# Check if tables exist
psql "$DATABASE_URL" -c "\dt aircraft_real"
```

### No Data Appearing

1. Verify dump1090 is receiving aircraft signals
2. Check that your antenna is connected
3. Ensure you're in an area with aircraft traffic
4. Check streamer logs for errors
5. Verify database credentials

### Performance Issues

If the database is getting overwhelmed:

1. Increase rate limiting interval in `AircraftDataManager.__init__`
2. Reduce history retention time
3. Add database indexes (already included in schema.sql)
4. Use connection pooling (already implemented)

## Differences from Synthetic Data

| Feature | Synthetic Data | Real Data |
|---------|---------------|-----------|
| Tables | `aircraft`, `aircraft_history`, `aircraft_sessions` | `aircraft_real`, `aircraft_history_real`, `aircraft_sessions_real` |
| Source | Mock data generator | dump1090 ADS-B receiver |
| Movement | Simulated patterns | Real aircraft movements |
| Coverage | Within configured sensor range | Actual receiver range (~100-200nm) |
| Reliability | Always consistent | Depends on signal quality |

Both datasets can coexist in the same database. Your visualization can choose which dataset to display or show both simultaneously.

## Security Notes

- Never commit your `.env` file with database credentials
- Use environment variables or secret management
- Consider using read-only database credentials if possible
- Enable Row Level Security (RLS) in Supabase if needed

## License

Part of the IE421 Group 19 Project

## Support

For issues or questions, check:
1. dump1090 documentation
2. Supabase documentation
3. This README troubleshooting section
