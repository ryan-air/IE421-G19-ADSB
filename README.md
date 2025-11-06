# group_19_project

## About Me

<div align="center">
  <img src="saksham.png" alt="Saksham" width="200" style="border-radius: 50%;">
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

## About Me

<div align="center">
  <img src="apoorv.jpg" alt="Apoorv" width="200" style="border-radius: 50%;">
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
- 💾 SQLite database for aircraft data storage

### Database Setup

The application uses SQLite to store aircraft data. The database file (`aircraft.db`) is created automatically when you first start the server.

**Database Schema:**
- Stores aircraft positions, flight data, altitude, speed, heading
- Automatically indexes aircraft by hex code and timestamp
- Keeps last 30 seconds of data visible

**API Endpoints:**

1. **GET `/api/aircraft`** - Retrieve current aircraft (used by frontend)
   - Returns aircraft from the last 30 seconds
   - Automatically groups by hex code to show latest position

2. **POST `/api/aircraft`** - Insert aircraft data (for Raspberry Pi)
   ```bash
   curl -X POST http://localhost:3000/api/aircraft \
     -H "Content-Type: application/json" \
     -d '{"aircraft": [{"hex": "ABC123", "flight": "TEST01", "lat": 40.11, "lon": -88.24, ...}]}'
   ```

3. **GET `/api/health`** - Check database connection status

### Adding Aircraft Data

To add aircraft data from your Raspberry Pi or other source, POST JSON data to `/api/aircraft`:

```json
{
  "aircraft": [
    {
      "hex": "ABC123",
      "flight": "UAL123",
      "lat": 40.11,
      "lon": -88.24,
      "altitude": 35000,
      "speed": 450,
      "track": 90,
      "messages": 100
    }
  ]
}
```

The frontend will automatically pick up new data and display it on the map.

### Project Structure

- `index.html` - Main HTML page
- `styles.css` - Styling
- `app.js` - Frontend JavaScript (map and data fetching)
- `server.js` - Backend Express server
- `package.json` - Dependencies and scripts

---

