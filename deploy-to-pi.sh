#!/bin/bash
# Deployment script for ADS-B system to Raspberry Pi
# Run this on your local machine to deploy to Raspberry Pi

set -e

# Configuration
PI_HOST="${PI_HOST:-pi@raspberrypi.local}"
STREAMER_DIR="/home/pi/ads-b-receiver"
WEB_DIR="/home/pi/adsb-web"

echo "=========================================="
echo "ADS-B System Deployment Script"
echo "=========================================="
echo ""
echo "Target: $PI_HOST"
echo "This script will deploy both the ADS-B streamer and web application"
echo ""
read -p "Continue? (y/n) " -n 1 -r
echo
if [[ ! $REPLY =~ ^[Yy]$ ]]; then
    echo "Deployment cancelled."
    exit 1
fi

# Check if SSH connection works
echo ""
echo "Testing SSH connection..."
if ! ssh -o ConnectTimeout=5 "$PI_HOST" "echo 'Connection successful'"; then
    echo "ERROR: Cannot connect to $PI_HOST"
    echo "Make sure:"
    echo "  1. Your Raspberry Pi is powered on and connected to network"
    echo "  2. SSH is enabled on the Pi"
    echo "  3. You can connect with: ssh $PI_HOST"
    echo ""
    echo "You can set a custom host with: export PI_HOST=pi@192.168.1.XXX"
    exit 1
fi

echo ""
echo "=========================================="
echo "Step 1: Deploying ADS-B Streamer"
echo "=========================================="

# Create directory on Pi
echo "Creating directory $STREAMER_DIR..."
ssh "$PI_HOST" "mkdir -p $STREAMER_DIR"

# Copy streamer files
echo "Copying ADS-B streamer files..."
scp ads-b-receiver/adsb_streamer_optimized.py "$PI_HOST:$STREAMER_DIR/"
scp ads-b-receiver/adsb-streamer-optimized.service "$PI_HOST:$STREAMER_DIR/"
scp ads-b-receiver/schema.sql "$PI_HOST:$STREAMER_DIR/" 2>/dev/null || echo "Note: schema.sql not found, skipping"

# Install Python dependencies
echo "Installing Python dependencies..."
ssh "$PI_HOST" << 'EOF'
cd ~/ads-b-receiver
if [ ! -d "venv" ]; then
    echo "Creating Python virtual environment..."
    python3 -m venv venv
fi
source venv/bin/activate
pip install -q psycopg2-binary python-dotenv
echo "Python dependencies installed."
EOF

echo ""
echo "=========================================="
echo "Step 2: Deploying Web Application"
echo "=========================================="

# Create directory on Pi
echo "Creating directory $WEB_DIR..."
ssh "$PI_HOST" "mkdir -p $WEB_DIR"

# Copy web application files
echo "Copying web application files..."
scp ads/server.js "$PI_HOST:$WEB_DIR/"
scp ads/app.js "$PI_HOST:$WEB_DIR/"
scp ads/index.html "$PI_HOST:$WEB_DIR/"
scp ads/3d.html "$PI_HOST:$WEB_DIR/"
scp ads/cesium-app.js "$PI_HOST:$WEB_DIR/"
scp ads/styles.css "$PI_HOST:$WEB_DIR/"
scp ads/airports.json "$PI_HOST:$WEB_DIR/"
scp ads/package.json "$PI_HOST:$WEB_DIR/"
scp ads/adsb-web.service "$PI_HOST:$WEB_DIR/"
scp ads/nginx-adsb.conf "$PI_HOST:$WEB_DIR/" 2>/dev/null || echo "Note: nginx-adsb.conf not found, skipping"

# Install Node.js dependencies
echo "Installing Node.js dependencies..."
ssh "$PI_HOST" << 'EOF'
cd ~/adsb-web
npm install --production
echo "Node.js dependencies installed."
EOF

echo ""
echo "=========================================="
echo "Step 3: Configuration"
echo "=========================================="

# Check if .env files exist
echo "Checking for .env files..."
if ssh "$PI_HOST" "test -f $STREAMER_DIR/.env"; then
    echo "✓ ADS-B streamer .env file exists"
else
    echo "✗ ADS-B streamer .env file NOT found"
    echo ""
    echo "You need to create $STREAMER_DIR/.env on your Raspberry Pi with:"
    echo "  DATABASE_URL=postgresql://user:pass@host.supabase.co:5432/postgres"
    echo ""
fi

if ssh "$PI_HOST" "test -f $WEB_DIR/.env"; then
    echo "✓ Web application .env file exists"
else
    echo "✗ Web application .env file NOT found"
    echo ""
    echo "You need to create $WEB_DIR/.env on your Raspberry Pi with:"
    echo "  DATABASE_URL=postgresql://user:pass@host.supabase.co:5432/postgres"
    echo "  PORT=3000"
    echo ""
fi

echo ""
echo "=========================================="
echo "Deployment Complete!"
echo "=========================================="
echo ""
echo "Next steps:"
echo ""
echo "1. SSH into your Raspberry Pi:"
echo "   ssh $PI_HOST"
echo ""
echo "2. Create .env files if they don't exist (see above)"
echo ""
echo "3. Install systemd services:"
echo "   sudo cp $STREAMER_DIR/adsb-streamer-optimized.service /etc/systemd/system/"
echo "   sudo cp $WEB_DIR/adsb-web.service /etc/systemd/system/"
echo "   sudo systemctl daemon-reload"
echo "   sudo systemctl enable adsb-streamer-optimized.service"
echo "   sudo systemctl enable adsb-web.service"
echo "   sudo systemctl start adsb-streamer-optimized.service"
echo "   sudo systemctl start adsb-web.service"
echo ""
echo "4. Check status:"
echo "   sudo systemctl status adsb-streamer-optimized.service"
echo "   sudo systemctl status adsb-web.service"
echo ""
echo "5. Access your web application:"
echo "   http://raspberrypi.local:3000"
echo ""
echo "For detailed instructions, see DEPLOYMENT.md"
echo ""
