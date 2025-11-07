# Aircraft Data Generator

A separate service that continuously generates and inserts mock aircraft data into Supabase for testing purposes.

## Setup

1. **Install dependencies:**
   ```bash
   cd data-generator
   npm install
   ```

2. **Configure database:**
   - Make sure the parent directory has a `.env` file with `DATABASE_URL` set
   - Or create a `.env` file in this directory with your Supabase connection string

3. **Run the service:**
   ```bash
   npm start
   ```

## How it works

- Generates 3-8 mock aircraft around Champaign, IL
- Updates aircraft positions every 2 seconds (simulating movement)
- Aircraft move in their current heading direction
- Occasionally adds/removes aircraft to simulate traffic
- Inserts all data directly into Supabase

## Features

- Realistic aircraft movement simulation
- Random flight numbers and hex codes
- Varying altitudes (10,000-40,000 ft)
- Varying speeds (200-600 kt)
- Continuous updates every 2 seconds

## Usage

Run this service in a separate terminal while your main web server is running. The website will automatically display the aircraft as they're added to the database.

