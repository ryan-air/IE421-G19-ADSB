# Quick Start Guide

Get your ADS-B streamer running in 5 minutes!

## 1. Copy to Raspberry Pi

Transfer this entire `ads-b-receiver` folder to your Raspberry Pi:

```bash
# From your laptop
scp -r ads-b-receiver/ pi@raspberrypi.local:~/
```

## 2. Setup on Raspberry Pi

SSH into your Pi and run:

```bash
cd ~/ads-b-receiver

# Create .env file with your database credentials
cp .env.example .env
nano .env
# Add your Supabase DATABASE_URL
```

## 3. Create Database Tables

In Supabase SQL Editor, run the contents of [schema.sql](schema.sql):

```bash
# Copy the SQL and paste in Supabase dashboard → SQL Editor
cat schema.sql
```

## 4. Test Everything

```bash
# Test connections before starting
python3 test_connection.py
```

Should show:
```
✓ dump1090 connection: PASS
✓ Database connection: PASS
✓ Schema check: PASS
```

## 5. Run the Streamer

**Quick start:**
```bash
./run.sh
```

**Or manually:**
```bash
python3 -m venv venv
source venv/bin/activate
pip install -r requirements.txt
python3 adsb_streamer.py
```

**Remote dump1090:**
```bash
./run.sh --host 192.168.1.100 --port 30003
```

## 6. Run as Background Service (Optional)

```bash
# Copy service file
sudo cp adsb-streamer.service /etc/systemd/system/

# Edit paths if needed
sudo nano /etc/systemd/system/adsb-streamer.service

# Enable and start
sudo systemctl enable adsb-streamer
sudo systemctl start adsb-streamer

# Check status
sudo systemctl status adsb-streamer

# View logs
sudo journalctl -u adsb-streamer -f
```

## Verify It's Working

### Check the logs:
```
2025-12-09 19:45:30 - INFO - Stats: 1245 msgs (41.5/s), 1123 parsed, 456 DB updates, 0 errors
```

### Query Supabase:
```sql
SELECT COUNT(*) FROM aircraft_real;
SELECT * FROM active_aircraft_real LIMIT 10;
```

## Troubleshooting

**No data appearing?**
1. Check dump1090 is running: `nc localhost 30003`
2. Verify antenna is connected
3. Check you're in an area with aircraft traffic

**Database errors?**
1. Verify DATABASE_URL is correct in `.env`
2. Make sure you ran `schema.sql` in Supabase
3. Check network connection

**Connection refused?**
1. Ensure dump1090 is running: `ps aux | grep dump1090`
2. Check port 30003 is open: `netstat -ln | grep 30003`

## File Structure

```
ads-b-receiver/
├── adsb_streamer.py        # Main streamer application
├── schema.sql              # Database schema (run in Supabase)
├── requirements.txt        # Python dependencies
├── .env.example           # Environment template
├── .gitignore             # Git ignore rules
├── run.sh                 # Quick start script
├── test_connection.py     # Connection test utility
├── adsb-streamer.service  # Systemd service file
├── README.md              # Full documentation
└── QUICKSTART.md          # This file
```

## Key Differences from Synthetic Data

| Aspect | Synthetic (`aircraft`) | Real (`aircraft_real`) |
|--------|----------------------|---------------------|
| Source | Mock generator | dump1090 receiver |
| Tables | `aircraft`, `aircraft_history` | `aircraft_real`, `aircraft_history_real` |
| Data | Simulated flights | Real ADS-B signals |
| Coverage | Within configured range | Actual radio range |

Both can run simultaneously!

## Next Steps

- Configure your frontend to display `aircraft_real` table
- Set up multiple receivers for better coverage
- Monitor receiver stats
- Adjust cleanup intervals as needed

For more details, see [README.md](README.md)
