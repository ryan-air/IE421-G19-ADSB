#!/bin/bash
# Quick start script for ADS-B streamer

set -e

# Colors for output
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
NC='\033[0m' # No Color

echo -e "${GREEN}ADS-B Data Streamer${NC}"
echo "===================="
echo ""

# Check if virtual environment exists
if [ ! -d "venv" ]; then
    echo -e "${YELLOW}Virtual environment not found. Creating...${NC}"
    python3 -m venv venv
    echo -e "${GREEN}✓ Virtual environment created${NC}"
fi

# Activate virtual environment
echo -e "${YELLOW}Activating virtual environment...${NC}"
source venv/bin/activate

# Check if dependencies are installed
if ! python -c "import psycopg2" 2>/dev/null; then
    echo -e "${YELLOW}Installing dependencies...${NC}"
    pip install -q -r requirements.txt
    echo -e "${GREEN}✓ Dependencies installed${NC}"
fi

# Check if .env file exists
if [ ! -f ".env" ]; then
    echo -e "${RED}Error: .env file not found${NC}"
    echo "Please copy .env.example to .env and configure your database URL"
    echo ""
    echo "  cp .env.example .env"
    echo "  nano .env"
    echo ""
    exit 1
fi

# Load environment variables
source .env

# Check if DATABASE_URL is set
if [ -z "$DATABASE_URL" ]; then
    echo -e "${RED}Error: DATABASE_URL not set in .env file${NC}"
    exit 1
fi

echo -e "${GREEN}✓ Configuration loaded${NC}"
echo ""

# Parse command line arguments or use defaults
HOST="${DUMP1090_HOST:-localhost}"
PORT="${DUMP1090_PORT:-30003}"
RECEIVER="${RECEIVER_ID:-$(hostname)}"

# Allow command line overrides
while [[ $# -gt 0 ]]; do
    case $1 in
        --host)
            HOST="$2"
            shift 2
            ;;
        --port)
            PORT="$2"
            shift 2
            ;;
        --receiver-id)
            RECEIVER="$2"
            shift 2
            ;;
        *)
            echo "Unknown option: $1"
            echo "Usage: $0 [--host HOST] [--port PORT] [--receiver-id ID]"
            exit 1
            ;;
    esac
done

echo "Configuration:"
echo "  dump1090 host: $HOST"
echo "  dump1090 port: $PORT"
echo "  Receiver ID:   $RECEIVER"
echo ""
echo -e "${GREEN}Starting streamer...${NC}"
echo "Press Ctrl+C to stop"
echo ""

# Run the streamer
python3 adsb_streamer.py \
    --host "$HOST" \
    --port "$PORT" \
    --receiver-id "$RECEIVER"
