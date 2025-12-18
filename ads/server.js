import dotenv from "dotenv";
dotenv.config();

import express from 'express';
import cors from 'cors';
import path from 'path';
import { fileURLToPath } from 'url';
import { Pool } from 'pg';
import { detectCollisions, formatCollision } from './probability-cone.js';

// ES module equivalent of __dirname
const __filename = fileURLToPath(import.meta.url);
const __dirname = path.dirname(__filename);

const app = express();
const PORT = process.env.PORT || 3000;

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

// Kalman filter-based collision detection with probability cones
app.get('/api/collisions/kalman', async (req, res) => {
    try {
        // Get prediction time horizon (default 5 minutes = 300 seconds)
        const timeHorizon = parseInt(req.query.time_horizon) || 300;

        // Get confidence level (default 95%)
        const confidence = parseFloat(req.query.confidence) || 0.95;

        // Get all active aircraft with current positions
        const aircraftResult = await db.query(`
            SELECT
                hex,
                flight,
                lat,
                lon,
                altitude,
                speed,
                track,
                vertical_rate,
                last_seen
            FROM aircraft_sessions_real
            WHERE ended_at IS NULL
              AND lat IS NOT NULL
              AND lon IS NOT NULL
              AND altitude IS NOT NULL
              AND last_seen > NOW() - INTERVAL '2 minutes'
        `);

        const aircraft = aircraftResult.rows;

        // Get position history for each aircraft (last 10 minutes)
        const aircraftWithHistory = await Promise.all(
            aircraft.map(async (ac) => {
                const historyResult = await db.query(`
                    SELECT lat, lon, altitude, speed, track, vertical_rate, timestamp
                    FROM aircraft_position_history
                    WHERE hex = $1
                      AND timestamp > NOW() - INTERVAL '10 minutes'
                    ORDER BY timestamp ASC
                `, [ac.hex]);

                return {
                    ...ac,
                    positionHistory: historyResult.rows
                };
            })
        );

        // Only use aircraft with sufficient history (at least 2 data points)
        const validAircraft = aircraftWithHistory.filter(
            ac => ac.positionHistory && ac.positionHistory.length >= 2
        );

        if (validAircraft.length < 2) {
            return res.json({
                collisions: [],
                message: 'Not enough aircraft with sufficient position history',
                aircraft_count: validAircraft.length,
                time_horizon: timeHorizon,
                confidence_level: confidence
            });
        }

        // Detect collisions using Kalman filter probability cones
        const collisions = detectCollisions(validAircraft, timeHorizon, confidence);

        // Format collisions for response
        const formattedCollisions = collisions.map(formatCollision);

        res.json({
            collisions: formattedCollisions,
            aircraft_count: validAircraft.length,
            time_horizon: timeHorizon,
            confidence_level: confidence,
            timestamp: Date.now()
        });

    } catch (error) {
        console.error('Error detecting collisions:', error);
        res.status(500).json({
            error: 'Failed to detect collisions',
            message: error.message
        });
    }
});


// Serve the main page
app.get('/', (req, res) => {
    res.sendFile(path.join(__dirname, 'index.html'));
});

// Serve the 3D page
app.get('/3d.html', (req, res) => {
    res.sendFile(path.join(__dirname, '3d.html'));
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
