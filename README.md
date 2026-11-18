# ADS-B Aircraft Tracking System

> ⚠️ **To be updated:** This document describes the Fall 2025 system and needs to be updated for the current project extension. See [README_UPDATES.md](README_UPDATES.md) for the new plan, [SETUP.md](SETUP.md) to build and test the new code, and [TEAM.md](TEAM.md) for who owns what, what is left, and how to contribute (pull requests reviewed by everyone).

A real-time aircraft tracking system that captures ADS-B (Automatic Dependent Surveillance-Broadcast) data from aircraft, streams it to a cloud database, and visualizes it on an interactive web map.

![ADS-B Tracker](https://img.shields.io/badge/ADS--B-Tracker-blue)
![Node.js](https://img.shields.io/badge/Node.js-20.x-green)
![Python](https://img.shields.io/badge/Python-3.9+-blue)
![PostgreSQL](https://img.shields.io/badge/Database-PostgreSQL-blue)


## Features

### Real-Time Aircraft Tracking
- **Live ADS-B Data Streaming**: Receives aircraft position data from dump1090 via SBS-1 BaseStation format
- **Interactive Map Visualization**: Display aircraft positions on Leaflet maps with custom icons
- **3D Visualization**: Cesium-based 3D globe view for immersive tracking experience
- **Aircraft Trails**: Visual flight paths showing historical positions
- **Real-time Updates**: Aircraft positions update every 2 seconds

### Advanced Analytics
- **Flight Statistics Dashboard**:
  - Total aircraft tracked
  - Average altitude and speed
  - Altitude distribution charts
  - Flight duration analysis
- **Collision Detection System**: Monitors aircraft proximity and alerts on potential conflicts
- **Session Tracking**: Records first/last seen timestamps for each aircraft
- **Time-based Filtering**: View data from last 5 minutes, 1 hour, 6 hours, or all time

### Data Management
- **Optimized Database Writes**: Batch processing with CSV COPY for 100-1000x performance improvement
- **Automatic Cleanup**: Removes stale aircraft data and old position history
- **Multi-receiver Support**: Handles data from multiple ADS-B receivers simultaneously
- **Deduplication**: Smart rate-limiting prevents duplicate entries

### Web Interface
- **Responsive Design**: Works on desktop, tablet, and mobile devices
- **Dark-themed UI**: Easy on the eyes for extended monitoring sessions
- **Airport Overlays**: Shows nearby airports with runway information
- **Search & Filter**: Find specific aircraft by callsign or hex code
- **Control Panel**:
  - Toggle trails and collision detection
  - Switch between 2d and 3d views

## System Architecture

```
┌─────────────────┐
│   dump1090       │  Receives ADS-B radio signals from aircraft
│  (SDR Software)  │
└────────┬─────────┘
         │ SBS-1 Port 30003
         ▼
┌─────────────────────┐
│ ADS-B Streamer      │  Python service - Optimized batch processing
│ (Raspberry Pi)      │  • Parses SBS-1 messages
│                     │  • Buffers aircraft positions
│                     │  • Bulk inserts via CSV COPY
└────────┬────────────┘
         │ PostgreSQL Connection
         ▼
┌─────────────────────┐
│   Supabase DB       │  Cloud PostgreSQL database
│   (PostgreSQL)      │  • aircraft_real table
│                     │  • aircraft_sessions_real
│                     │  • collision_alerts
└────────┬────────────┘
         │ REST API
         ▼
┌─────────────────────┐
│   Web Application   │  Node.js + Express backend
│   (Railway/Cloud)   │  • Serves static frontend
│                     │  • Provides REST API
│                     │  • Real-time collision detection
└────────┬────────────┘
         │ HTTPS
         ▼
┌─────────────────────┐
│   End Users         │  Web browsers
│                     │  • 2D Leaflet maps
│                     │  • 3D Cesium globe
│                     │  • Statistics dashboard
└─────────────────────┘
```

## Prerequisites

### For Raspberry Pi (ADS-B Streamer)
- Raspberry Pi 3/4/5 with Raspberry Pi OS
- dump1090 or dump1090-fa installed and running
- RTL-SDR dongle for receiving ADS-B signals
- Python 3.9 or higher
- Internet connection

### For Web Application
- Node.js 20.x or higher
- npm 10.x or higher
- Supabase account (free tier works)

### For Cloud Deployment (Optional)
- Railway.app account (or Render.com, Vercel, etc.)
- Domain name (optional)

## Installation

### 1. Database Setup (Supabase)

1. Create a free account at [supabase.com](https://supabase.com)
2. Create a new project
3. Go to **SQL Editor** and run the schema:

```bash
# Use the schema from ads-b-receiver/schema.sql
```

4. Get your connection string:
   - Go to **Settings** → **Database**
   - Copy **Connection string** (Session mode for IPv4 compatibility)
   - Format: `postgresql://postgres.[ref]:[password]@aws-0-[region].pooler.supabase.com:6543/postgres`

### 2. Raspberry Pi Setup (ADS-B Streamer)

```bash
# SSH into your Raspberry Pi
ssh pi@raspberrypi.local

# Clone the repository
git clone https://your-repo-url/group_19_project.git
cd group_19_project/ads-b-receiver

# Create Python virtual environment
python3 -m venv venv
source venv/bin/activate

# Install dependencies
pip install psycopg2-binary python-dotenv

# Create .env file
nano .env
```

Add to `.env`:
```env
DATABASE_URL=postgresql://your-supabase-connection-string
```

Save and secure:
```bash
chmod 600 .env
```

### 3. Web Application Setup

```bash
cd ~/group_19_project/ads

# Install Node.js dependencies
npm install

# Create .env file
nano .env
```

Add to `.env`:
```env
DATABASE_URL=postgresql://your-supabase-connection-string
PORT=3000
```

Save and secure:
```bash
chmod 600 .env
```

## ⚙️ Configuration

### ADS-B Streamer Configuration

The optimized streamer supports several performance tuning options:

```bash
python3 adsb_streamer_optimized.py \
    --host localhost \                    # dump1090 host
    --port 30003 \                       # dump1090 SBS-1 port
    --receiver-id raspberrypi \          # Unique receiver identifier
    --batch-size 100 \                   # Records per batch (default: 100)
    --flush-interval 1.0 \               # Seconds between auto-flushes (default: 1.0)
    --cleanup-interval 5 \               # Minutes before removing stale aircraft
    --history-cleanup 0 \                # Hours to keep position history (0 = disabled)
    --use-execute-values                 # Use execute_values instead of CSV COPY
```

**Performance Recommendations:**
- **High traffic** (busy airport): `--batch-size=200 --flush-interval=0.5`
- **Medium traffic** (default): `--batch-size=100 --flush-interval=1.0`
- **Low traffic** (remote area): `--batch-size=50 --flush-interval=2.0`

### Web Application Configuration

Edit `server.js` to adjust:

```javascript
// Database connection pool
const db = new Pool({
    connectionString: process.env.DATABASE_URL,
    ssl: { rejectUnauthorized: false },
    max: 10,                    // Max connections
    idleTimeoutMillis: 20000,   // Close idle clients after 20s
    connectionTimeoutMillis: 10000,
    allowExitOnIdle: false,
});

// Collision detection thresholds
const COLLISION_THRESHOLDS = {
    critical: { horizontal: 1.0, vertical: 500 },   // 1 NM, 500 ft
    warning: { horizontal: 3.0, vertical: 1000 },   // 3 NM, 1000 ft
    caution: { horizontal: 5.0, vertical: 2000 }    // 5 NM, 2000 ft
};
```

## 🎮 Usage

### Running Locally

#### Start ADS-B Streamer (Raspberry Pi)

```bash
cd ~/group_19_project/ads-b-receiver
source venv/bin/activate
python3 adsb_streamer_optimized.py
```

You should see:
```
2025-12-18 00:00:00 - INFO - Starting ADS-B data streamer (OPTIMIZED VERSION)
2025-12-18 00:00:00 - INFO - Using CSV COPY for bulk inserts
2025-12-18 00:00:00 - INFO - Successfully connected to dump1090
2025-12-18 00:00:30 - INFO - Stats: 3233 msgs (107.1/s), 3233 parsed, 529 buffered, 0 errors
```

#### Start Web Application

```bash
cd ~/group_19_project/ads
node server.js
```

You should see:
```
✅ Connected to Supabase database
🚀 Aircraft Tracker Server running on http://localhost:3000
```

Open browser: `http://localhost:3000`

### Running as System Services (Recommended)

#### Install ADS-B Streamer Service

```bash
# Edit service file to match your paths
sudo nano /etc/systemd/system/adsb-streamer-optimized.service

# Enable and start
sudo systemctl daemon-reload
sudo systemctl enable adsb-streamer-optimized.service
sudo systemctl start adsb-streamer-optimized.service

# Check status
sudo systemctl status adsb-streamer-optimized.service

# View logs
sudo journalctl -u adsb-streamer-optimized.service -f
```

#### Install Web Application Service

```bash
# Edit service file to match your paths
sudo nano /etc/systemd/system/adsb-web.service

# Enable and start
sudo systemctl daemon-reload
sudo systemctl enable adsb-web.service
sudo systemctl start adsb-web.service

# Check status
sudo systemctl status adsb-web.service
```

## API Documentation

Base URL: `https://your-domain.com` or `http://localhost:3000`

### Get Aircraft Data

```http
GET /api/aircraft?window={seconds}
```

**Parameters:**
- `window` (optional): Time window in seconds (0 = all data, 300 = last 5 min)

**Response:**
```json
{
  "aircraft": [
    {
      "hex": "A12345",
      "flight": "AAL123",
      "lat": 40.7128,
      "lon": -74.0060,
      "altitude": 35000,
      "speed": 450,
      "track": 180,
      "vertical_rate": 0,
      "squawk": "1200",
      "receiver_id": "raspberrypi",
      "seen_at": "2025-12-18T06:00:00Z"
    }
  ],
  "count": 1,
  "timestamp": 1734505200000
}
```

### Get Statistics

```http
GET /api/statistics?window={seconds}
```

**Response:**
```json
{
  "summary": {
    "total_aircraft": 25,
    "avg_altitude": 32500,
    "avg_speed": 425,
    "max_altitude": 41000,
    "min_altitude": 5000
  },
  "altitude_distribution": [
    {"range": "0-10000", "count": 5},
    {"range": "10000-20000", "count": 3},
    {"range": "20000-40000", "count": 15},
    {"range": "40000+", "count": 2}
  ],
  "flight_durations": [
    {"hex": "A12345", "flight": "AAL123", "duration": 3600}
  ],
  "timestamp": 1734505200000
}
```

### Get Collision Alerts

```http
GET /api/collisions
```

**Response:**
```json
{
  "alerts": [
    {
      "id": 1,
      "hex1": "A12345",
      "hex2": "B67890",
      "flight1": "AAL123",
      "flight2": "UAL456",
      "horizontal_distance_nm": 2.5,
      "vertical_distance_ft": 800,
      "severity": "warning",
      "detected_at": "2025-12-18T06:00:00Z"
    }
  ],
  "count": 1
}
```

### Health Check

```http
GET /api/health
```

**Response:**
```json
{
  "status": "ok",
  "database": "connected",
  "timestamp": 1734505200000
}
```

## Troubleshooting

### ADS-B Streamer Issues

**Problem:** "Failed to connect to dump1090"
```bash
# Check if dump1090 is running
sudo systemctl status dump1090

# Test connection manually
telnet localhost 30003

# Check firewall
sudo ufw status
```

**Problem:** "Database connection error"
```bash
# Test database connection
psql "your-database-url"

# Check .env file
cat .env

# Verify DATABASE_URL format
```

**Problem:** "No data being inserted"
```bash
# Check logs
sudo journalctl -u adsb-streamer-optimized.service -f

# Verify aircraft are being received
# Look for "X msgs parsed, Y buffered" in logs
```

### Web Application Issues

**Problem:** "Cannot GET /api/aircraft"
```bash
# Check if Node.js is running
sudo systemctl status adsb-web.service

# Check port
sudo netstat -tlnp | grep 3000

# Test API directly
curl http://localhost:3000/api/aircraft
```

**Problem:** "Map not loading"
- Check browser console for errors
- Verify Leaflet CSS/JS are loaded
- Check API calls in Network tab

**Problem:** "CORS errors"
```javascript
// In server.js, CORS is already configured for all origins
app.use(cors());
```

### Railway Deployment Issues

**Problem:** "Application failed to respond"
- Check Railway logs: `railway logs`
- Verify `PORT` environment variable is set correctly
- Ensure `process.env.PORT` is used in `server.js`

**Problem:** "Database connection timeout"
- Verify `DATABASE_URL` is set in Railway dashboard
- Check Supabase connection pooler is used (not direct connection)
- Verify SSL settings in database connection

## Acknowledgments

- **dump1090**: ADS-B decoder by antirez and FlightAware
- **Leaflet**: Interactive maps library
- **Cesium**: 3D globe visualization
- **Supabase**: Cloud PostgreSQL database
- **Railway**: Cloud deployment platform
- **OpenStreetMap**: Map data contributors

## Team (To be updated)

<div align="center">

### Saksham Gupta

Saksham is a Master's student in Computer Science at the University of Illinois Urbana-Champaign, expected to graduate in May 2027. He is currently open to full-time new graduate roles.

**Email:** saksham9@illinois.edu
**LinkedIn:** https://www.linkedin.com/in/saksham---gupta/

---

### Apoorv Sagar Pitta

Apoorv is a Master's student in Computer Science at the University of Illinois Urbana-Champaign, expected to graduate in May 2027. He is currently open to full-time new graduate roles.

**Email:** apitt3@illinois.edu
**LinkedIn:** https://www.linkedin.com/in/apoorv-sagar-pitta

</div>

---

*Advised by Professor David Lariviere*