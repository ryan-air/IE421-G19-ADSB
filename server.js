const express = require('express');
const cors = require('cors');
const path = require('path');
const fs = require('fs');
const initSqlJs = require('sql.js');

const app = express();
const PORT = 3000;

// SQLite database configuration
const DB_PATH = process.env.DB_PATH || './aircraft.db';

let db = null;
let SQL = null;

// Initialize database
async function initializeDatabase() {
    try {
        SQL = await initSqlJs();
        
        // Load existing database or create new one
        if (fs.existsSync(DB_PATH)) {
            const buffer = fs.readFileSync(DB_PATH);
            db = new SQL.Database(buffer);
            console.log('✅ Database loaded from file');
        } else {
            db = new SQL.Database();
            console.log('✅ New database created');
        }
        
        // Create table if it doesn't exist
        db.run(`
            CREATE TABLE IF NOT EXISTS aircraft (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                hex TEXT NOT NULL,
                flight TEXT,
                lat REAL,
                lon REAL,
                altitude INTEGER,
                speed INTEGER,
                track INTEGER,
                messages INTEGER,
                seen_at DATETIME DEFAULT (datetime('now')),
                created_at DATETIME DEFAULT (datetime('now'))
            );
            
            CREATE INDEX IF NOT EXISTS idx_hex ON aircraft(hex);
            CREATE INDEX IF NOT EXISTS idx_seen_at ON aircraft(seen_at);
        `);
        
        // Save database to file
        saveDatabase();
        console.log('✅ Database initialized');
    } catch (error) {
        console.error('❌ Database initialization failed:', error);
        throw error;
    }
}

// Save database to file
function saveDatabase() {
    try {
        const data = db.export();
        const buffer = Buffer.from(data);
        fs.writeFileSync(DB_PATH, buffer);
    } catch (error) {
        console.error('Error saving database:', error);
    }
}

// Initialize database on startup
(async () => {
    await initializeDatabase();
})();

// Middleware
app.use(cors());
app.use(express.json());
app.use(express.static(path.join(__dirname)));

// Fetch aircraft from database (configurable time window)
function fetchAircraftFromDB(timeWindowSeconds = 30) {
    try {
        let timeFilter = '';
        if (timeWindowSeconds > 0) {
            timeFilter = `WHERE seen_at >= datetime('now', '-${timeWindowSeconds} seconds') AND lat IS NOT NULL AND lon IS NOT NULL`;
        } else {
            timeFilter = `WHERE lat IS NOT NULL AND lon IS NOT NULL`;
        }
        
        const query = `
            SELECT 
                hex,
                flight,
                lat,
                lon,
                altitude,
                speed,
                track,
                messages,
                strftime('%s', seen_at) as seen
            FROM aircraft
            ${timeFilter}
            ORDER BY seen_at DESC
        `;
        
        const result = db.exec(query);
        
        if (!result || result.length === 0) {
            return [];
        }
        
        const rows = result[0].values;
        const columns = result[0].columns;
        
        // Convert to array of objects
        const aircraft = rows.map(row => {
            const obj = {};
            columns.forEach((col, idx) => {
                obj[col] = row[idx];
            });
            return obj;
        });
        
        // Group by hex to get latest position for each aircraft
        const aircraftMap = new Map();
        aircraft.forEach(ac => {
            const seen = parseInt(ac.seen);
            const existing = aircraftMap.get(ac.hex);
            
            // Keep the most recent position for each aircraft
            if (!existing || existing.seen < seen) {
                aircraftMap.set(ac.hex, {
                    hex: ac.hex,
                    flight: ac.flight || null,
                    lat: ac.lat,
                    lon: ac.lon,
                    altitude: ac.altitude || null,
                    speed: ac.speed || null,
                    track: ac.track || null,
                    messages: ac.messages || 0,
                    seen: seen
                });
            }
        });
        
        return Array.from(aircraftMap.values());
    } catch (error) {
        console.error('Error fetching from database:', error);
        throw error;
    }
}

// API endpoint to insert aircraft data (for Raspberry Pi to use)
app.post('/api/aircraft', express.json(), (req, res) => {
    try {
        const aircraft = req.body.aircraft || [];
        
        if (!Array.isArray(aircraft)) {
            return res.status(400).json({ error: 'aircraft must be an array' });
        }
        
        let inserted = 0;
        
        // Insert each aircraft
        for (const ac of aircraft) {
            if (ac.hex && ac.lat != null && ac.lon != null) {
                db.run(`
                    INSERT INTO aircraft (hex, flight, lat, lon, altitude, speed, track, messages, seen_at)
                    VALUES (?, ?, ?, ?, ?, ?, ?, ?, datetime('now'))
                `, [
                    ac.hex || null,
                    ac.flight ? ac.flight.trim() : null,
                    ac.lat || null,
                    ac.lon || null,
                    ac.altitude || null,
                    ac.speed || null,
                    ac.track || null,
                    ac.messages || 0
                ]);
                inserted++;
            }
        }
        
        // Save database after inserts
        saveDatabase();
        
        res.json({ 
            success: true, 
            inserted: inserted,
            message: 'Aircraft data inserted successfully' 
        });
    } catch (error) {
        console.error('Error inserting aircraft data:', error);
        res.status(500).json({ 
            error: 'Failed to insert aircraft data',
            message: error.message 
        });
    }
});

// API endpoint to get aircraft data
app.get('/api/aircraft', (req, res) => {
    try {
        // Allow query parameter to control time window (0 = all data, default = 30 seconds)
        const timeWindow = req.query.window ? parseInt(req.query.window) : 30;
        const aircraft = fetchAircraftFromDB(timeWindow);
        
        res.json({
            now: Date.now(),
            aircraft: aircraft,
            timeWindow: timeWindow
        });
    } catch (error) {
        console.error('Error fetching aircraft data:', error);
        res.status(500).json({ 
            error: 'Failed to fetch aircraft data',
            message: error.message 
        });
    }
});

// Health check endpoint
app.get('/api/health', (req, res) => {
    try {
        // Test database connection
        const result = db.exec('SELECT COUNT(*) as count FROM aircraft');
        let count = 0;
        if (result && result.length > 0 && result[0].values.length > 0) {
            count = result[0].values[0][0];
        }
        
        res.json({ 
            status: 'ok', 
            database: 'connected',
            aircraft_count: count,
            timestamp: Date.now() 
        });
    } catch (error) {
        res.status(500).json({ 
            status: 'error',
            database: 'disconnected',
            error: error.message 
        });
    }
});

// Serve the main page
app.get('/', (req, res) => {
    res.sendFile(path.join(__dirname, 'index.html'));
});

// Cleanup on exit
process.on('SIGINT', () => {
    console.log('\n🛑 Shutting down gracefully...');
    if (db) {
        saveDatabase();
        db.close();
    }
    process.exit(0);
});

// Start server
app.listen(PORT, () => {
    console.log(`🚀 Aircraft Tracker Server running on http://localhost:${PORT}`);
    console.log(`📊 Database: ${DB_PATH}`);
    console.log(`💡 POST aircraft data to: http://localhost:${PORT}/api/aircraft`);
    console.log(`   GET aircraft data from: http://localhost:${PORT}/api/aircraft`);
});
