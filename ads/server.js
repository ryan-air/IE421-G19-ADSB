import dotenv from "dotenv";
dotenv.config();

import express from 'express';
import cors from 'cors';
import path from 'path';
import { fileURLToPath } from 'url';
import { Pool } from 'pg';

// ES module equivalent of __dirname
const __filename = fileURLToPath(import.meta.url);
const __dirname = path.dirname(__filename);

const app = express();
const PORT = 3000;

// Check if DATABASE_URL is set
if (!process.env.DATABASE_URL) {
    console.error('❌ DATABASE_URL not found in environment variables');
    console.error('   Make sure you have a .env file with DATABASE_URL set');
    console.error('   Example: DATABASE_URL=postgresql://postgres:password@host:5432/postgres');
    process.exit(1);
}

// Parse connection string to show host (for debugging, without password)
let dbHostname, dbPort;
try {
    const url = new URL(process.env.DATABASE_URL);
    dbHostname = url.hostname;
    dbPort = url.port || 5432;
    console.log(`📊 Connecting to: ${dbHostname}:${dbPort}`);
    
    // Check if using direct connection (IPv6-only) instead of pooler
    if (dbHostname.includes('.supabase.co') && !dbHostname.includes('.pooler.')) {
        console.warn('⚠️  WARNING: Using Direct Connection (may not work on IPv4 networks)');
        console.warn('   Consider using Session Pooler connection string instead:');
        console.warn('   Settings → Database → Connection string → Session mode');
        console.warn('   Format: postgresql://postgres.[ref]:[password]@aws-0-[region].pooler.supabase.com:6543/postgres');
    }
} catch (e) {
    console.error('❌ Invalid DATABASE_URL format');
    process.exit(1);
}

// Supabase PostgreSQL connection
// Use Session Pooler connection string for IPv4 compatibility
// Format: postgresql://postgres.[ref]:[password]@aws-0-[region].pooler.supabase.com:6543/postgres
const db = new Pool({
    connectionString: process.env.DATABASE_URL,
    ssl: {
        rejectUnauthorized: false  // Supabase requires SSL
    },
    // Connection pool settings optimized for Supabase
    max: 10, // Maximum number of clients in the pool
    idleTimeoutMillis: 20000, // Close idle clients after 20 seconds (Supabase pooler timeout is ~60s)
    connectionTimeoutMillis: 10000, // Return an error after 10 seconds if connection could not be established
    allowExitOnIdle: false, // Keep pool alive even when all clients are idle
});

// Handle pool errors to prevent crashes
db.on('error', (err, client) => {
    console.error('❌ Unexpected database pool error:', err.message);
    if (err.message.includes('db_termination') || err.message.includes('Connection terminated')) {
        console.log('💡 Database connection will be automatically recreated on next query');
    }
});

// Test database connection
async function testConnection() {
    try {
        const result = await db.query('SELECT NOW()');
        console.log('✅ Connected to Supabase database');
        console.log(`   Database time: ${result.rows[0].now}`);
    } catch (error) {
        console.error('❌ Database connection failed:', error.message);
        
        if (error.message.includes('ENOTFOUND') || error.message.includes('getaddrinfo')) {
            console.error('\n   🔍 DNS Resolution Error - This usually means:');
            console.error('   1. Your network is IPv4-only and the hostname is IPv6-only');
            console.error('   2. You need to use the Session Pooler connection string');
            console.error('\n   ✅ Solution:');
            console.error('   1. Go to Supabase Dashboard → Settings → Database');
            console.error('   2. Under "Connection string", select "Session mode" (NOT "Direct connection")');
            console.error('   3. Copy the connection string (should have .pooler. in hostname and port 6543)');
            console.error('   4. Update your .env file with the Session Pooler connection string');
            console.error('\n   Session Pooler format:');
            console.error('   postgresql://postgres.[ref]:[password]@aws-0-[region].pooler.supabase.com:6543/postgres');
        } else {
            console.error('\n   Troubleshooting:');
            console.error('   1. Check your .env file has DATABASE_URL set correctly');
            console.error('   2. Verify the connection string from Supabase dashboard');
            console.error('   3. Make sure your password doesn\'t contain special characters that need URL encoding');
            console.error('   4. Check your internet connection');
        }
        process.exit(1);
    }
}

// Initialize connection on startup
testConnection();

// Middleware
app.use(cors());
app.use(express.json());
app.use(express.static(path.join(__dirname)));

// Fetch aircraft from database (last 30 seconds)
async function fetchAircraftFromDB(timeWindowSeconds = 30) {
    try {
        let query;
        let params = [];
        
        if (timeWindowSeconds > 0) {
            query = `
                SELECT 
                    hex,
                    flight,
                    lat,
                    lon,
                    altitude,
                    speed,
                    track,
                    EXTRACT(EPOCH FROM seen_at)::bigint as seen
                FROM aircraft_real
                WHERE seen_at >= NOW() - INTERVAL '${timeWindowSeconds} seconds'
                AND lat IS NOT NULL
                AND lon IS NOT NULL
                ORDER BY seen_at DESC
            `;
        } else {
            query = `
                SELECT 
                    hex,
                    flight,
                    lat,
                    lon,
                    altitude,
                    speed,
                    track,
                    EXTRACT(EPOCH FROM seen_at)::bigint as seen
                FROM aircraft_real
                WHERE lat IS NOT NULL
                AND lon IS NOT NULL
                ORDER BY seen_at DESC
            `;
        }
        
        const result = await db.query(query, params);
        
        // Group by hex to get latest position for each aircraft
        const aircraftMap = new Map();
        result.rows.forEach(ac => {
            const seen = parseInt(ac.seen);
            const existing = aircraftMap.get(ac.hex);
            
            // Keep the most recent position for each aircraft
            if (!existing || existing.seen < seen) {
                aircraftMap.set(ac.hex, {
                    hex: ac.hex,
                    flight: ac.flight || null,
                    lat: parseFloat(ac.lat),
                    lon: parseFloat(ac.lon),
                    altitude: ac.altitude || null,
                    speed: ac.speed || null,
                    track: ac.track || null,
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

// API endpoint to get aircraft data
app.get('/api/aircraft', async (req, res) => {
    try {
        // Allow query parameter to control time window (0 = all data, default = 30 seconds)
        const timeWindow = req.query.window ? parseInt(req.query.window) : 30;
        const aircraft = await fetchAircraftFromDB(timeWindow);
        
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

// Get aircraft trail (historical positions)
app.get('/api/aircraft/:hex/trail', async (req, res) => {
    try {
        const { hex } = req.params;
        const limit = req.query.limit ? parseInt(req.query.limit) : 100;
        const minutes = req.query.minutes ? parseInt(req.query.minutes) : 120; // Increased to 120 minutes (2 hours) to match cleanup interval

        const result = await db.query(`
            SELECT
                lat,
                lon,
                altitude,
                speed,
                track,
                EXTRACT(EPOCH FROM recorded_at)::bigint as timestamp
            FROM aircraft_history_real
            WHERE hex = $1
            AND recorded_at >= NOW() - INTERVAL '${minutes} minutes'
            ORDER BY recorded_at ASC
            LIMIT $2
        `, [hex.toUpperCase(), limit]);

        res.json({
            hex: hex.toUpperCase(),
            trail: result.rows.map(row => ({
                lat: parseFloat(row.lat),
                lon: parseFloat(row.lon),
                altitude: row.altitude,
                speed: row.speed,
                track: row.track,
                timestamp: parseInt(row.timestamp)
            })),
            count: result.rows.length
        });
    } catch (error) {
        console.error('Error fetching aircraft trail:', error);
        res.status(500).json({
            error: 'Failed to fetch aircraft trail',
            message: error.message
        });
    }
});

// Get statistics dashboard data
app.get('/api/statistics', async (req, res) => {
    try {
        const timeWindow = req.query.window ? parseInt(req.query.window) : 30;

        // Build WHERE clause based on time window
        const timeFilter = timeWindow > 0
            ? `WHERE seen_at >= NOW() - INTERVAL '${timeWindow} seconds'`
            : 'WHERE 1=1'; // No time filter for window=0

        // Get overall statistics
        const statsResult = await db.query(`
            SELECT
                COUNT(DISTINCT hex) as total_flights,
                ROUND(AVG(speed)::numeric, 1) as avg_speed,
                ROUND(AVG(altitude)::numeric, 0) as avg_altitude,
                MIN(altitude) as min_altitude,
                MAX(altitude) as max_altitude,
                MIN(speed) as min_speed,
                MAX(speed) as max_speed
            FROM aircraft_real
            ${timeFilter}
            AND lat IS NOT NULL
            AND lon IS NOT NULL
        `);

        // Get altitude distribution (buckets: 0-10k, 10-20k, 20-30k, 30-40k, 40k+)
        const altitudeDistResult = await db.query(`
            SELECT
                CASE
                    WHEN altitude < 10000 THEN '0-10k'
                    WHEN altitude < 20000 THEN '10-20k'
                    WHEN altitude < 30000 THEN '20-30k'
                    WHEN altitude < 40000 THEN '30-40k'
                    ELSE '40k+'
                END as altitude_range,
                COUNT(DISTINCT hex) as count
            FROM aircraft_real
            ${timeFilter}
            AND altitude IS NOT NULL
            GROUP BY altitude_range
            ORDER BY MIN(altitude)
        `);

        // Build time filter for sessions (same logic)
        const sessionTimeFilter = timeWindow > 0
            ? `AND a.seen_at >= NOW() - INTERVAL '${timeWindow} seconds'`
            : ''; // No time filter for window=0

        // Get flight durations for active aircraft
        const durationsResult = await db.query(`
            SELECT
                s.hex,
                s.flight,
                EXTRACT(EPOCH FROM (s.last_seen - s.first_seen))::integer as duration_seconds,
                EXTRACT(EPOCH FROM s.first_seen)::bigint as first_seen,
                EXTRACT(EPOCH FROM s.last_seen)::bigint as last_seen
            FROM aircraft_sessions_real s
            INNER JOIN aircraft_real a ON s.hex = a.hex
            WHERE s.ended_at IS NULL
            ${sessionTimeFilter}
            ORDER BY duration_seconds DESC
        `);

        const stats = statsResult.rows[0];

        res.json({
            summary: {
                total_flights: parseInt(stats.total_flights) || 0,
                avg_speed: parseFloat(stats.avg_speed) || 0,
                avg_altitude: parseInt(stats.avg_altitude) || 0,
                min_altitude: stats.min_altitude || 0,
                max_altitude: stats.max_altitude || 0,
                min_speed: stats.min_speed || 0,
                max_speed: stats.max_speed || 0
            },
            altitude_distribution: altitudeDistResult.rows.map(row => ({
                range: row.altitude_range,
                count: parseInt(row.count)
            })),
            flight_durations: durationsResult.rows.map(row => ({
                hex: row.hex,
                flight: row.flight,
                duration_seconds: row.duration_seconds,
                first_seen: parseInt(row.first_seen),
                last_seen: parseInt(row.last_seen)
            })),
            timestamp: Date.now()
        });
    } catch (error) {
        console.error('Error fetching statistics:', error);
        res.status(500).json({
            error: 'Failed to fetch statistics',
            message: error.message
        });
    }
});

// Health check endpoint
app.get('/api/health', async (req, res) => {
    try {
        // Test database connection and get count
        const result = await db.query('SELECT COUNT(*) as count FROM aircraft_real');
        const count = parseInt(result.rows[0].count);

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


//APOORV'S CODE

// Collision detection constants (aviation standards)
const MIN_HORIZONTAL_SEPARATION_NM = 5.0;  // 5 nautical miles
const MIN_VERTICAL_SEPARATION_FT = 1000;  // 1000 feet

// Calculate distance between two coordinates (Haversine formula)
function calculateDistance(lat1, lon1, lat2, lon2) {
    const R = 3440; // Earth radius in nautical miles
    const dLat = (lat2 - lat1) * Math.PI / 180;
    const dLon = (lon2 - lon1) * Math.PI / 180;
    const a = Math.sin(dLat/2) * Math.sin(dLat/2) +
              Math.cos(lat1 * Math.PI / 180) * Math.cos(lat2 * Math.PI / 180) *
              Math.sin(dLon/2) * Math.sin(dLon/2);
    const c = 2 * Math.atan2(Math.sqrt(a), Math.sqrt(1-a));
    return R * c;
}

// Detect collision risks between aircraft pairs
async function detectCollisions() {
    try {
        const aircraft = await fetchAircraftFromDB(60); // Check last 60 seconds
        
        // Filter aircraft with valid positions and altitudes
        const validAircraft = aircraft.filter(ac => 
            ac.lat && ac.lon && ac.altitude !== null && ac.altitude !== undefined
        );
        
        const alerts = [];
        const activeAlerts = new Set();
        
        // Check all pairs
        for (let i = 0; i < validAircraft.length; i++) {
            for (let j = i + 1; j < validAircraft.length; j++) {
                const ac1 = validAircraft[i];
                const ac2 = validAircraft[j];
                
                // Calculate horizontal distance
                const horizontalDist = calculateDistance(ac1.lat, ac1.lon, ac2.lat, ac2.lon);
                
                // Calculate vertical distance
                const verticalDist = Math.abs(ac1.altitude - ac2.altitude);
                
                // Check if separation is violated
                if (horizontalDist < MIN_HORIZONTAL_SEPARATION_NM && 
                    verticalDist < MIN_VERTICAL_SEPARATION_FT) {
                    
                    // Determine severity
                    let severity = 'warning';
                    if (horizontalDist < 2.0 || verticalDist < 500) {
                        severity = 'critical';
                    } else if (horizontalDist < 3.0 || verticalDist < 750) {
                        severity = 'caution';
                    }
                    
                    const alertKey = `${ac1.hex}-${ac2.hex}`;
                    activeAlerts.add(alertKey);
                    
                    alerts.push({
                        hex1: ac1.hex,
                        hex2: ac2.hex,
                        flight1: ac1.flight,
                        flight2: ac2.flight,
                        horizontal_distance_nm: horizontalDist,
                        vertical_distance_ft: verticalDist,
                        severity: severity
                    });
                }
            }
        }
        
        // Upsert alerts into database
        for (const alert of alerts) {
            // Check if active alert already exists for this pair
            const existing = await db.query(`
                SELECT id FROM collision_alerts
                WHERE hex1 = $1 AND hex2 = $2 AND resolved_at IS NULL
                LIMIT 1
            `, [alert.hex1, alert.hex2]);
            
            if (existing.rows.length > 0) {
                // Update existing alert
                await db.query(`
                    UPDATE collision_alerts
                    SET horizontal_distance_nm = $1,
                        vertical_distance_ft = $2,
                        severity = $3,
                        detected_at = NOW()
                    WHERE hex1 = $4 AND hex2 = $5 AND resolved_at IS NULL
                `, [
                    alert.horizontal_distance_nm,
                    alert.vertical_distance_ft,
                    alert.severity,
                    alert.hex1,
                    alert.hex2
                ]);
            } else {
                // Insert new alert
                await db.query(`
                    INSERT INTO collision_alerts (
                        hex1, hex2, flight1, flight2,
                        horizontal_distance_nm, vertical_distance_ft, severity
                    )
                    VALUES ($1, $2, $3, $4, $5, $6, $7)
                `, [
                    alert.hex1, alert.hex2, alert.flight1, alert.flight2,
                    alert.horizontal_distance_nm, alert.vertical_distance_ft, alert.severity
                ]);
            }
        }
        
        // Resolve alerts that are no longer active
        // Build a set of active alert pairs (both directions for matching)
        const activePairs = new Set();
        alerts.forEach(a => {
            activePairs.add(`${a.hex1}-${a.hex2}`);
            activePairs.add(`${a.hex2}-${a.hex1}`); // Add reverse for matching
        });
        
        if (activePairs.size > 0) {
            // Get all unresolved alerts from database
            const unresolvedAlerts = await db.query(`
                SELECT hex1, hex2 FROM collision_alerts
                WHERE resolved_at IS NULL
            `);
            
            // Resolve alerts that are no longer in the active pairs set
            for (const alert of unresolvedAlerts.rows) {
                const pairKey1 = `${alert.hex1}-${alert.hex2}`;
                const pairKey2 = `${alert.hex2}-${alert.hex1}`;
                
                if (!activePairs.has(pairKey1) && !activePairs.has(pairKey2)) {
                    await db.query(`
                        UPDATE collision_alerts
                        SET resolved_at = NOW()
                        WHERE hex1 = $1 AND hex2 = $2 AND resolved_at IS NULL
                    `, [alert.hex1, alert.hex2]);
                }
            }
        } else {
            // No active alerts, resolve all
            await db.query(`
                UPDATE collision_alerts
                SET resolved_at = NOW()
                WHERE resolved_at IS NULL
            `);
        }
        
    } catch (error) {
        console.error('Error detecting collisions:', error);
    }
}

// API endpoint to get active collision alerts
app.get('/api/collisions', async (req, res) => {
    try {
        const result = await db.query(`
            SELECT
                hex1, hex2, flight1, flight2,
                horizontal_distance_nm, vertical_distance_ft, severity,
                EXTRACT(EPOCH FROM detected_at)::bigint as detected_at
            FROM collision_alerts
            WHERE resolved_at IS NULL
            ORDER BY 
                CASE severity
                    WHEN 'critical' THEN 1
                    WHEN 'caution' THEN 2
                    WHEN 'warning' THEN 3
                END,
                horizontal_distance_nm ASC
        `);
        
        // Get aircraft positions for visualization
        const alerts = [];
        for (const row of result.rows) {
            const ac1 = await db.query('SELECT lat, lon, altitude FROM aircraft_real WHERE hex = $1', [row.hex1]);
            const ac2 = await db.query('SELECT lat, lon, altitude FROM aircraft_real WHERE hex = $1', [row.hex2]);
            
            if (ac1.rows[0] && ac2.rows[0]) {
                alerts.push({
                    hex1: row.hex1,
                    hex2: row.hex2,
                    flight1: row.flight1,
                    flight2: row.flight2,
                    lat1: parseFloat(ac1.rows[0].lat),
                    lon1: parseFloat(ac1.rows[0].lon),
                    alt1: ac1.rows[0].altitude,
                    lat2: parseFloat(ac2.rows[0].lat),
                    lon2: parseFloat(ac2.rows[0].lon),
                    alt2: ac2.rows[0].altitude,
                    horizontal_distance_nm: parseFloat(row.horizontal_distance_nm),
                    vertical_distance_ft: row.vertical_distance_ft,
                    severity: row.severity,
                    detected_at: parseInt(row.detected_at)
                });
            }
        }
        
        res.json({ alerts });
    } catch (error) {
        console.error('Error fetching collisions:', error);
        res.status(500).json({ error: 'Failed to fetch collisions', message: error.message });
    }
});

// Run collision detection every 5 seconds
setInterval(detectCollisions, 5000);


// Serve the main page
app.get('/', (req, res) => {
    res.sendFile(path.join(__dirname, 'index.html'));
});

// Cleanup on exit
process.on('SIGINT', async () => {
    console.log('\n🛑 Shutting down gracefully...');
    await db.end();
    process.exit(0);
});

// Start server
app.listen(PORT, () => {
    console.log(`🚀 Aircraft Tracker Server running on http://localhost:${PORT}`);
    console.log(`📊 Database: Supabase PostgreSQL`);
    console.log(`💡 GET aircraft data from: http://localhost:${PORT}/api/aircraft`);
    console.log(`   Health check: http://localhost:${PORT}/api/health`);
});
