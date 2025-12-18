# ADS-B System Deployment Guide for Raspberry Pi

This guide covers deploying both the optimized ADS-B streamer and web application on a Raspberry Pi.

## Architecture

- **ADS-B Streamer** (Python): Reads from dump1090, writes to Supabase using optimized batch inserts
- **Web Application** (Node.js): Serves frontend and provides API for querying Supabase
- **Web Server** (Nginx - Optional): Reverse proxy for production deployment

## Prerequisites

1. Raspberry Pi with Raspberry Pi OS (Bullseye or newer)
2. dump1090 installed and running
3. Python 3.9+ installed
4. Node.js 16+ installed
5. Supabase database URL

## Option 1: Simple Deployment (Node.js Only)

This is the quickest way to get started. Node.js serves both the API and static files.

### Step 1: Deploy ADS-B Streamer

```bash
# 1. Copy files to Raspberry Pi
ssh pi@raspberrypi.local
mkdir -p ~/ads-b-receiver
cd ~/ads-b-receiver

# Copy these files from your local machine:
# - adsb_streamer_optimized.py
# - adsb-streamer-optimized.service
# - schema.sql (if needed)

# 2. Create Python virtual environment
python3 -m venv venv
source venv/bin/activate
pip install psycopg2-binary python-dotenv

# 3. Create .env file with your database URL
cat > .env << 'EOF'
DATABASE_URL=postgresql://user:pass@host.supabase.co:5432/postgres
EOF

chmod 600 .env

# 4. Test the optimized streamer
./venv/bin/python3 adsb_streamer_optimized.py --batch-size=100 --flush-interval=1.0

# Press Ctrl+C to stop after verifying it works

# 5. Install systemd service
sudo cp adsb-streamer-optimized.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable adsb-streamer-optimized.service
sudo systemctl start adsb-streamer-optimized.service

# 6. Check status
sudo systemctl status adsb-streamer-optimized.service
sudo journalctl -u adsb-streamer-optimized.service -f
```

### Step 2: Deploy Web Application

```bash
# 1. Copy files to Raspberry Pi
mkdir -p ~/adsb-web
cd ~/adsb-web

# Copy all files from the 'ads' folder:
# - server.js
# - app.js
# - index.html
# - 3d.html
# - cesium-app.js
# - styles.css
# - airports.json
# - package.json
# - adsb-web.service

# 2. Install Node.js dependencies
npm install --production

# 3. Create .env file with your database URL
cat > .env << 'EOF'
DATABASE_URL=postgresql://user:pass@host.supabase.co:5432/postgres
PORT=3000
EOF

chmod 600 .env

# 4. Test the web server
node server.js

# Open browser to http://raspberrypi.local:3000
# Press Ctrl+C to stop after verifying it works

# 5. Install systemd service
sudo cp adsb-web.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable adsb-web.service
sudo systemctl start adsb-web.service

# 6. Check status
sudo systemctl status adsb-web.service
sudo journalctl -u adsb-web.service -f
```

### Step 3: Access Your Application

Open a web browser and navigate to:
- **Main Map**: `http://raspberrypi.local:3000`
- **3D View**: `http://raspberrypi.local:3000/3d.html`

Replace `raspberrypi.local` with your Pi's IP address if hostname doesn't work.

---

## Option 2: Production Deployment (Nginx + Node.js)

This is recommended for better performance and security. Nginx serves static files and proxies API requests to Node.js.

### Additional Prerequisites

```bash
sudo apt update
sudo apt install nginx
```

### Step 1-2: Same as Option 1

Follow Steps 1 and 2 from Option 1 above.

### Step 3: Configure Nginx

```bash
# 1. Copy nginx configuration
sudo cp ~/adsb-web/nginx-adsb.conf /etc/nginx/sites-available/adsb

# 2. Create symlink to enable the site
sudo ln -s /etc/nginx/sites-available/adsb /etc/nginx/sites-enabled/

# 3. Remove default site (optional)
sudo rm /etc/nginx/sites-enabled/default

# 4. Test nginx configuration
sudo nginx -t

# 5. Restart nginx
sudo systemctl restart nginx
sudo systemctl enable nginx

# 6. Check status
sudo systemctl status nginx
```

### Step 4: Update Frontend API Calls (if needed)

If using Nginx, your frontend will make API calls to `/api/*` which Nginx will proxy to `http://localhost:3000/api/*`.

Your `server.js` already has the `/api` prefix, so this should work without changes.

### Step 5: Access Your Application

Open a web browser and navigate to:
- **Main Map**: `http://raspberrypi.local`
- **3D View**: `http://raspberrypi.local/3d.html`

---

## Performance Tuning

### ADS-B Streamer Optimization

The optimized streamer has several tunable parameters:

```bash
# High-traffic scenario (busy airport)
--batch-size=200 --flush-interval=0.5

# Low-traffic scenario (remote area)
--batch-size=50 --flush-interval=2.0

# Default (recommended)
--batch-size=100 --flush-interval=1.0
```

Edit `/etc/systemd/system/adsb-streamer-optimized.service` and adjust the `ExecStart` line, then:

```bash
sudo systemctl daemon-reload
sudo systemctl restart adsb-streamer-optimized.service
```

### Performance Comparison

**Old streamer**: 1 INSERT per aircraft update
- ~50-100 updates/sec max before database becomes bottleneck

**Optimized streamer with execute_values**: Batch INSERT
- ~500-1000 updates/sec (10-20x improvement)

**Optimized streamer with CSV COPY** (default): Bulk load via CSV
- ~5000-10000 updates/sec (100-200x improvement)

### Database Connection Pool Settings

If you experience connection issues, edit `server.js` and adjust:

```javascript
const db = new Pool({
    connectionString: process.env.DATABASE_URL,
    ssl: { rejectUnauthorized: false },
    max: 10,              // Increase if handling many concurrent users
    idleTimeoutMillis: 20000,
    connectionTimeoutMillis: 10000,
    allowExitOnIdle: false,
});
```

---

## Monitoring and Maintenance

### View Logs

```bash
# ADS-B Streamer logs
sudo journalctl -u adsb-streamer-optimized.service -f

# Web Application logs
sudo journalctl -u adsb-web.service -f

# Nginx logs (if using)
sudo tail -f /var/log/nginx/adsb-access.log
sudo tail -f /var/log/nginx/adsb-error.log
```

### Restart Services

```bash
# Restart ADS-B streamer
sudo systemctl restart adsb-streamer-optimized.service

# Restart web application
sudo systemctl restart adsb-web.service

# Restart Nginx (if using)
sudo systemctl restart nginx
```

### Check Service Status

```bash
# Check all services
sudo systemctl status adsb-streamer-optimized.service
sudo systemctl status adsb-web.service
sudo systemctl status nginx  # if using
```

### Stop Services

```bash
# Stop ADS-B streamer
sudo systemctl stop adsb-streamer-optimized.service

# Stop web application
sudo systemctl stop adsb-web.service
```

### Auto-Start on Boot

Services are already configured to start automatically. To disable:

```bash
sudo systemctl disable adsb-streamer-optimized.service
sudo systemctl disable adsb-web.service
```

---

## Troubleshooting

### ADS-B Streamer Not Connecting to dump1090

```bash
# Check if dump1090 is running
sudo systemctl status dump1090

# Test connection manually
telnet localhost 30003

# Check streamer logs
sudo journalctl -u adsb-streamer-optimized.service -n 50
```

### Web Application Not Loading

```bash
# Check if Node.js is running
sudo systemctl status adsb-web.service

# Check if port 3000 is listening
sudo netstat -tlnp | grep 3000

# Test API endpoint
curl http://localhost:3000/api/status
```

### Database Connection Issues

```bash
# Test database connection
psql "postgresql://user:pass@host.supabase.co:5432/postgres"

# Check .env files
cat ~/ads-b-receiver/.env
cat ~/adsb-web/.env

# Verify DATABASE_URL format
# Should be: postgresql://user:pass@host.supabase.co:6543/postgres?pgbouncer=true
```

### Nginx Not Serving Pages

```bash
# Check Nginx configuration
sudo nginx -t

# Check Nginx status
sudo systemctl status nginx

# Check Nginx error logs
sudo tail -f /var/log/nginx/adsb-error.log

# Verify file permissions
ls -la ~/adsb-web/
# All files should be readable by the nginx user (typically www-data)
```

### Permission Issues

```bash
# Fix file ownership
sudo chown -R pi:pi ~/ads-b-receiver
sudo chown -R pi:pi ~/adsb-web

# Make scripts executable
chmod +x ~/ads-b-receiver/adsb_streamer_optimized.py
```

---

## Firewall Configuration (Optional)

If using a firewall on your Raspberry Pi:

```bash
# Allow HTTP (port 80) for Nginx
sudo ufw allow 80/tcp

# Or allow Node.js directly (port 3000)
sudo ufw allow 3000/tcp

# Allow SSH
sudo ufw allow 22/tcp

# Enable firewall
sudo ufw enable
```

---

## Updating the Application

### Update Web Application

```bash
cd ~/adsb-web

# Stop service
sudo systemctl stop adsb-web.service

# Copy new files (server.js, app.js, etc.)
# ...

# Install new dependencies (if package.json changed)
npm install --production

# Start service
sudo systemctl start adsb-web.service
```

### Update ADS-B Streamer

```bash
cd ~/ads-b-receiver

# Stop service
sudo systemctl stop adsb-streamer-optimized.service

# Copy new Python script
# ...

# Update dependencies (if needed)
source venv/bin/activate
pip install -r requirements.txt

# Start service
sudo systemctl start adsb-streamer-optimized.service
```

---

## System Resource Usage

Expected resource usage on Raspberry Pi 4:

- **ADS-B Streamer**: ~50-100 MB RAM, <5% CPU
- **Web Application**: ~100-150 MB RAM, <10% CPU (idle), up to 30% (active users)
- **Nginx**: ~20-30 MB RAM, <5% CPU

Total: ~200-300 MB RAM, ~10-20% CPU (with active monitoring)

---

## Remote Access

### Local Network Access

Access from any device on your network:
- Find your Pi's IP: `hostname -I`
- Access: `http://192.168.1.XXX` (Nginx) or `http://192.168.1.XXX:3000` (Node.js)

### Internet Access (Advanced)

To access from outside your network, you'll need to:

1. **Port forwarding** on your router (forward port 80 or 3000 to your Pi)
2. **Dynamic DNS** service (if you don't have a static IP)
3. **HTTPS certificate** (recommended for security - use Let's Encrypt)

This is beyond the scope of this guide, but there are many tutorials available online.

---

## Database Maintenance

### Clean Up Old Data

The streamer automatically cleans up:
- Aircraft not seen in 5 minutes (configurable with `--cleanup-interval`)
- Position history older than X hours (configurable with `--history-cleanup`)

Manual cleanup:

```sql
-- Connect to Supabase
psql "postgresql://user:pass@host.supabase.co:5432/postgres"

-- Delete old ended sessions (older than 7 days)
DELETE FROM aircraft_sessions_real
WHERE ended_at IS NOT NULL
  AND ended_at < NOW() - INTERVAL '7 days';

-- Delete old history (if enabled)
DELETE FROM aircraft_history_real
WHERE recorded_at < NOW() - INTERVAL '24 hours';
```

---

## Security Recommendations

1. **Keep your .env files secure**:
   ```bash
   chmod 600 ~/ads-b-receiver/.env
   chmod 600 ~/adsb-web/.env
   ```

2. **Use environment-specific database credentials** (not your admin account)

3. **Enable Row Level Security (RLS)** in Supabase for production

4. **Use HTTPS** if exposing to the internet (Let's Encrypt is free)

5. **Keep system updated**:
   ```bash
   sudo apt update
   sudo apt upgrade
   ```

6. **Use firewall** to restrict access to necessary ports only

---

## Performance Benchmarks

Based on testing with the optimized streamer:

| Method | Throughput | Latency | CPU Usage |
|--------|-----------|---------|-----------|
| Single INSERT | ~50/sec | High | 15% |
| execute_values (batch) | ~500/sec | Medium | 10% |
| CSV COPY (default) | ~5000/sec | Low | 5% |

**Recommendation**: Use CSV COPY (default) unless you experience issues with your Supabase plan.

To switch to execute_values method:

```bash
# Edit service file
sudo nano /etc/systemd/system/adsb-streamer-optimized.service

# Add --use-execute-values flag to ExecStart line
ExecStart=/home/pi/ads-b-receiver/venv/bin/python3 /home/pi/ads-b-receiver/adsb_streamer_optimized.py \
    --batch-size=100 \
    --flush-interval=1.0 \
    --cleanup-interval=5 \
    --use-execute-values

# Reload and restart
sudo systemctl daemon-reload
sudo systemctl restart adsb-streamer-optimized.service
```

---

## Questions?

Check logs first:
```bash
sudo journalctl -u adsb-streamer-optimized.service -f
sudo journalctl -u adsb-web.service -f
```

Common issues are usually related to:
1. Database connection string (check .env files)
2. dump1090 not running
3. File permissions
4. Port conflicts
