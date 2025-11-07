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
    // Connection pool settings for better performance
    max: 20, // Maximum number of clients in the pool
    idleTimeoutMillis: 30000, // Close idle clients after 30 seconds
    connectionTimeoutMillis: 10000, // Return an error after 10 seconds if connection could not be established
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
                    messages,
                    EXTRACT(EPOCH FROM seen_at)::bigint as seen
                FROM aircraft
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
                    messages,
                    EXTRACT(EPOCH FROM seen_at)::bigint as seen
                FROM aircraft
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

// Health check endpoint
app.get('/api/health', async (req, res) => {
    try {
        // Test database connection and get count
        const result = await db.query('SELECT COUNT(*) as count FROM aircraft');
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
