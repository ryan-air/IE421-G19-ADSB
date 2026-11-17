# group_19_project

> ⚠️ **To be updated:** This document describes the Fall 2025 system and needs to be updated for the current project extension. See [README_UPDATES.md](../README_UPDATES.md) for the new plan.

## About Me (To be updated)

<div align="center">
</div>

### Education & Career Timeline

- **Expected Graduation:** Spring 2027
- **Major:** Computer Science
- **University:** University of Illinois Urbana-Champaign

### Work Availability

- **Part-time Consulting/Interning:** [Available immediately / Starting Spring 2025]
- **Full-time Internships:** [Summer 2026]

### Contact

- **Email:** [saksham9@illinois.edu]
- **LinkedIn:** [https://www.linkedin.com/in/saksham---gupta/]
- **GitHub:** [http://github.com/socksham]

---

## About Me (To be updated)

<div align="center">
</div>

### Education & Career Timeline

- **Expected Graduation:** Spring 2027 (MS)
- **Major:** Computer Science
- **University:** University of Illinois Urbana-Champaign

### Work Availability

- **Part-time Consulting/Interning:** [Available Spring 2026]
- **Full-time Internships:** [Summer 2026]

### Contact

- **Email:** [apitt3@illinois.edu]
- **LinkedIn:** [https://www.linkedin.com/in/apoorv-sagar-pitta/]
- **GitHub:** [https://github.com/ApoorvSagarPitta]

---
## Live Aircraft Tracker

A web application to display live aircraft data from a SQLite database.

### Setup Instructions

1. **Install dependencies:**
   ```bash
   npm install
   ```

2. **Start the server:**
   ```bash
   npm start
   ```
   Or for development with auto-reload:
   ```bash
   npm run dev
   ```

3. **Open your browser:**
   Navigate to `http://localhost:3000`

### Features

- 🗺️ Interactive map showing aircraft positions
- 📊 Real-time aircraft tracking
- 📋 Aircraft list with details (altitude, speed, heading, etc.)
- 🔄 Auto-updates every 2 seconds
- 🎨 ATC-style interface with smooth aircraft movement
- ☁️ Supabase (PostgreSQL) cloud database for aircraft data storage

### Database Setup

The application uses **Supabase** (PostgreSQL) to store aircraft data. The Raspberry Pi writes directly to the database, and the web server reads from it.

**Setup Steps:**

1. **Create Supabase Project:**
   - Sign up at https://supabase.com
   - Create a new project
   - Note your database password

2. **Get Connection String:**
   - Go to Settings → Database
   - Copy the "Connection string" → "URI"
   - It looks like: `postgresql://postgres:[PASSWORD]@db.xxxxx.supabase.co:5432/postgres`

3. **Create `.env` file:**
   ```bash
   DATABASE_URL=postgresql://postgres:YOUR_PASSWORD@db.xxxxx.supabase.co:5432/postgres
   ```

4. **Create Database Table:**
   - In Supabase dashboard, go to SQL Editor
   - Run this SQL:
   ```sql
   CREATE TABLE IF NOT EXISTS aircraft (
       id SERIAL PRIMARY KEY,
       hex VARCHAR(6) NOT NULL,
       flight VARCHAR(10),
       lat DECIMAL(10, 7),
       lon DECIMAL(10, 7),
       altitude INTEGER,
       speed INTEGER,
       track INTEGER,
       messages INTEGER,
       seen_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
       created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
   );
   
   CREATE INDEX IF NOT EXISTS idx_hex ON aircraft(hex);
   CREATE INDEX IF NOT EXISTS idx_seen_at ON aircraft(seen_at);
   ```

**API Endpoints:**

1. **GET `/api/aircraft`** - Retrieve current aircraft (used by frontend)
   - Returns aircraft from the last 30 seconds (configurable via `?window=60` query param)
   - Automatically groups by hex code to show latest position

2. **GET `/api/health`** - Check database connection status
   - Returns connection status and total aircraft count

**Note:** The Raspberry Pi writes directly to Supabase using the same connection string. The web server only reads from the database.

### Project Structure

- `index.html` - Main HTML page
- `styles.css` - Styling
- `app.js` - Frontend JavaScript (map and data fetching)
- `server.js` - Backend Express server
- `package.json` - Dependencies and scripts

---

