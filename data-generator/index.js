import dotenv from "dotenv";
dotenv.config();

import { Pool } from 'pg';

// Sensor configuration - acts as if there's an ADS-B sensor at this location
const SENSOR_CONFIG = {
    // Sensor location (Champaign, IL) - this is where the "sensor" is located
    lat: 40.1164,
    lon: -88.2434,
    // Maximum detection range in nautical miles (typical ADS-B range: 100-200nm)
    // Can be overridden via MAX_RANGE_NM environment variable
    maxRangeNm: parseFloat(process.env.MAX_RANGE_NM) || 200
};

// Use Session Pooler connection string for IPv4 compatibility
// Format: postgresql://postgres.[ref]:[password]@aws-0-[region].pooler.supabase.com:6543/postgres
const db = new Pool({
    connectionString: process.env.DATABASE_URL,
    ssl: {
        rejectUnauthorized: false
    }
});

// Test database connection
async function testConnection() {
    try {
        const result = await db.query('SELECT NOW()');
        console.log('✅ Connected to Supabase database');
        return true;
    } catch (error) {
        console.error('❌ Database connection failed:', error.message);
        return false;
    }
}

// Calculate distance between two points using Haversine formula
// Returns distance in nautical miles
function calculateDistance(lat1, lon1, lat2, lon2) {
    const R = 3440; // Earth's radius in nautical miles
    
    // Convert degrees to radians
    const dLat = (lat2 - lat1) * Math.PI / 180;
    const dLon = (lon2 - lon1) * Math.PI / 180;
    
    const a = Math.sin(dLat / 2) * Math.sin(dLat / 2) +
              Math.cos(lat1 * Math.PI / 180) * Math.cos(lat2 * Math.PI / 180) *
              Math.sin(dLon / 2) * Math.sin(dLon / 2);
    
    const c = 2 * Math.atan2(Math.sqrt(a), Math.sqrt(1 - a));
    const distance = R * c; // Distance in nautical miles
    
    return distance;
}

// Check if aircraft is within sensor range
function isAircraftInRange(lat, lon) {
    const distance = calculateDistance(SENSOR_CONFIG.lat, SENSOR_CONFIG.lon, lat, lon);
    return distance <= SENSOR_CONFIG.maxRangeNm;
}

// Delete all records for specific aircraft from database
// Also marks their sessions as ended
// Pass cleanupHistory=true to also delete trail and session data (for shutdown cleanup)
async function deleteAircraftFromDB(hexCodes, cleanupHistory = false) {
    if (hexCodes.length === 0) return 0;

    try {
        let totalDeleted = 0;

        if (cleanupHistory) {
            // Delete trail history
            const historyResult = await db.query(`
                DELETE FROM aircraft_history
                WHERE hex = ANY($1::varchar[])
            `, [hexCodes]);
            totalDeleted += historyResult.rowCount;

            // Delete sessions completely
            const sessionsResult = await db.query(`
                DELETE FROM aircraft_sessions
                WHERE hex = ANY($1::varchar[])
            `, [hexCodes]);
            totalDeleted += sessionsResult.rowCount;
        } else {
            // Just mark sessions as ended (keep for historical data)
            await db.query(`
                UPDATE aircraft_sessions
                SET ended_at = NOW()
                WHERE hex = ANY($1::varchar[])
                AND ended_at IS NULL
            `, [hexCodes]);
        }

        // Delete from main aircraft table
        const result = await db.query(`
            DELETE FROM aircraft
            WHERE hex = ANY($1::varchar[])
        `, [hexCodes]);
        totalDeleted += result.rowCount;

        return totalDeleted;
    } catch (error) {
        console.error('Error deleting aircraft from database:', error.message);
        throw error;
    }
}

// Cleanup old history records to prevent database bloat
// Deletes position snapshots older than specified hours (default: 2 hours)
async function cleanupOldHistory(hoursToKeep = 2) {
    try {
        const result = await db.query(`
            DELETE FROM aircraft_history
            WHERE recorded_at < NOW() - INTERVAL '${hoursToKeep} hours'
        `);

        return result.rowCount;
    } catch (error) {
        console.error('Error cleaning up old history:', error.message);
        throw error;
    }
}

// Generate mock aircraft data (within sensor range) with realistic movement patterns
function generateMockAircraft() {
    const flights = ['UAL123', 'AAL456', 'SWA789', 'DAL321', 'JBU654', 'FDX901', 'UPS234', 'N12345'];
    const hexCodes = ['A1B2C3', 'B2C3D4', 'C3D4E5', 'D4E5F6', 'E5F6A1', 'F6A1B2', 'A2B3C4', 'B3C4D5'];
    const categories = ['A1', 'A2', 'A3', 'A5', 'B2', 'C2']; // Light, Small, Large, Heavy, etc.

    // Generate 3-8 aircraft
    const numAircraft = Math.floor(Math.random() * 6) + 3;
    const aircraft = [];

    for (let i = 0; i < numAircraft; i++) {
        // Generate position within sensor range (spread around sensor location)
        const angle = Math.random() * Math.PI * 2; // Random angle
        const maxInitialDistance = SENSOR_CONFIG.maxRangeNm * 0.7;
        const distanceNm = Math.random() * maxInitialDistance;
        const distanceDeg = distanceNm / 60;

        const lat = SENSOR_CONFIG.lat + Math.cos(angle) * distanceDeg;
        const lon = SENSOR_CONFIG.lon + Math.sin(angle) * distanceDeg;

        if (!isAircraftInRange(lat, lon)) {
            continue;
        }

        // Generate realistic flight data
        const flight = flights[Math.floor(Math.random() * flights.length)];
        const hex = hexCodes[i] || `TEST${i.toString(16).toUpperCase().padStart(2, '0')}`;
        const category = categories[Math.floor(Math.random() * categories.length)];

        // Random track direction (not necessarily away from sensor)
        const track = Math.floor(Math.random() * 360);

        const speed = Math.floor(Math.random() * 400) + 200; // 200-600 kt
        const altitude = Math.floor(Math.random() * 30000) + 10000; // 10,000-40,000 ft
        const verticalRate = Math.floor((Math.random() - 0.5) * 4000); // -2000 to +2000 ft/min

        // Assign movement pattern
        const patterns = ['straight', 'circling', 'waypoint'];
        const pattern = patterns[Math.floor(Math.random() * patterns.length)];

        aircraft.push({
            hex: hex,
            flight: flight,
            lat: lat,
            lon: lon,
            altitude: altitude,
            speed: speed,
            track: track,
            verticalRate: verticalRate,
            category: category,
            messages: Math.floor(Math.random() * 500) + 100,
            // Movement pattern metadata
            pattern: pattern,
            targetTrack: track, // For gradual turns
            waypointLat: pattern === 'waypoint' ? SENSOR_CONFIG.lat + (Math.random() - 0.5) * 2 : null,
            waypointLon: pattern === 'waypoint' ? SENSOR_CONFIG.lon + (Math.random() - 0.5) * 2 : null
        });
    }

    return aircraft;
}

// Insert or update aircraft data into Supabase
// Uses UPSERT to update existing records (since hex is primary key)
// Also inserts into aircraft_history for trail visualization
// And updates aircraft_sessions for flight duration tracking
async function insertAircraftData(aircraft) {
    if (aircraft.length === 0) return 0;

    try {
        let processed = 0;

        for (const ac of aircraft) {
            // Update main aircraft table
            await db.query(`
                INSERT INTO aircraft (hex, flight, lat, lon, altitude, speed, track, messages, seen_at)
                VALUES ($1, $2, $3, $4, $5, $6, $7, $8, NOW())
                ON CONFLICT (hex)
                DO UPDATE SET
                    flight = EXCLUDED.flight,
                    lat = EXCLUDED.lat,
                    lon = EXCLUDED.lon,
                    altitude = EXCLUDED.altitude,
                    speed = EXCLUDED.speed,
                    track = EXCLUDED.track,
                    messages = EXCLUDED.messages,
                    seen_at = EXCLUDED.seen_at
            `, [
                ac.hex,
                ac.flight || null,
                ac.lat,
                ac.lon,
                Math.floor(ac.altitude || 0), // Ensure integer
                Math.floor(ac.speed || 0), // Ensure integer
                Math.floor(ac.track || 0), // Ensure integer
                Math.floor(ac.messages || 0) // Ensure integer
            ]);

            // Insert position snapshot into aircraft_history for trail visualization
            // Store every position update (every 2 seconds)
            await db.query(`
                INSERT INTO aircraft_history (hex, lat, lon, altitude, speed, track, recorded_at)
                VALUES ($1, $2, $3, $4, $5, $6, NOW())
            `, [
                ac.hex,
                ac.lat,
                ac.lon,
                Math.floor(ac.altitude || 0),
                Math.floor(ac.speed || 0),
                Math.floor(ac.track || 0)
            ]);

            // Update aircraft_sessions for flight duration tracking
            // UPSERT: create session if new, update last_seen if existing
            await db.query(`
                INSERT INTO aircraft_sessions (hex, flight, first_seen, last_seen)
                VALUES ($1, $2, NOW(), NOW())
                ON CONFLICT (hex)
                DO UPDATE SET
                    flight = EXCLUDED.flight,
                    last_seen = NOW()
            `, [
                ac.hex,
                ac.flight || null
            ]);

            processed++;
        }

        return processed;
    } catch (error) {
        console.error('Error inserting/updating aircraft data:', error.message);
        throw error;
    }
}

// Simulate realistic aircraft movement with different patterns
function updateAircraftPositions(aircraft) {
    return aircraft.map(ac => {
        const timeStepSeconds = 2;
        const speedKts = ac.speed;
        const distanceNm = (speedKts / 3600) * timeStepSeconds;

        const nmPerDegreeLat = 60;
        const nmPerDegreeLon = 60 * Math.cos(ac.lat * Math.PI / 180);

        let newTrack = ac.track;
        let newAltitude = ac.altitude;
        let newVerticalRate = ac.verticalRate || 0;

        // Update movement based on pattern
        if (ac.pattern === 'circling') {
            // Circling: turn 3-5 degrees every update (smooth circle)
            const turnRate = 3 + Math.random() * 2; // 3-5 degrees per 2 seconds
            newTrack = (ac.track + turnRate) % 360;
        } else if (ac.pattern === 'waypoint' && ac.waypointLat && ac.waypointLon) {
            // Navigate toward waypoint
            const bearing = calculateBearing(ac.lat, ac.lon, ac.waypointLat, ac.waypointLon);
            const distance = calculateDistance(ac.lat, ac.lon, ac.waypointLat, ac.waypointLon);

            // Gradually turn toward waypoint
            let trackDiff = bearing - ac.track;
            if (trackDiff > 180) trackDiff -= 360;
            if (trackDiff < -180) trackDiff += 360;

            // Turn up to 5 degrees per update toward waypoint
            if (Math.abs(trackDiff) > 5) {
                newTrack = ac.track + (trackDiff > 0 ? 5 : -5);
            } else {
                newTrack = bearing;
            }
            newTrack = ((newTrack % 360) + 360) % 360;

            // If close to waypoint, pick new one
            if (distance < 10) { // Within 10nm
                ac.waypointLat = SENSOR_CONFIG.lat + (Math.random() - 0.5) * 2;
                ac.waypointLon = SENSOR_CONFIG.lon + (Math.random() - 0.5) * 2;
            }
        } else {
            // Straight flight with occasional minor course corrections
            if (Math.random() < 0.02) { // 2% chance
                newTrack = ac.track + (Math.random() - 0.5) * 10; // ±5 degrees
                newTrack = ((newTrack % 360) + 360) % 360;
            }
        }

        // Update altitude based on vertical rate
        newAltitude = ac.altitude + (newVerticalRate * timeStepSeconds) / 60; // Convert ft/min to ft/2s

        // Level off at cruise altitudes or change climb/descent rate occasionally
        if (Math.random() < 0.05) { // 5% chance to change vertical rate
            if (newAltitude > 38000) {
                newVerticalRate = -500; // Start descending
            } else if (newAltitude < 12000) {
                newVerticalRate = 500; // Start climbing
            } else {
                // Random: level off, climb, or descend
                const options = [0, 500, -500, 1000, -1000];
                newVerticalRate = options[Math.floor(Math.random() * options.length)];
            }
        }

        // Clamp altitude to reasonable range
        newAltitude = Math.max(10000, Math.min(42000, newAltitude));

        // Calculate new position
        const trackRad = (newTrack * Math.PI) / 180;
        const latChange = (distanceNm / nmPerDegreeLat) * Math.cos(trackRad);
        const lonChange = (distanceNm / nmPerDegreeLon) * Math.sin(trackRad);

        return {
            ...ac,
            lat: ac.lat + latChange,
            lon: ac.lon + lonChange,
            altitude: Math.floor(newAltitude),
            track: Math.floor(newTrack),
            verticalRate: newVerticalRate
        };
    });
}

// Calculate bearing from point A to point B
function calculateBearing(lat1, lon1, lat2, lon2) {
    const dLon = (lon2 - lon1) * Math.PI / 180;
    const lat1Rad = lat1 * Math.PI / 180;
    const lat2Rad = lat2 * Math.PI / 180;

    const y = Math.sin(dLon) * Math.cos(lat2Rad);
    const x = Math.cos(lat1Rad) * Math.sin(lat2Rad) -
              Math.sin(lat1Rad) * Math.cos(lat2Rad) * Math.cos(dLon);

    let bearing = Math.atan2(y, x) * 180 / Math.PI;
    return ((bearing + 360) % 360);
}

// Store current aircraft state for movement simulation
let currentAircraft = [];

// Track all hex codes that this data generator has created
// This allows us to clean up only our own data on shutdown
const trackedHexCodes = new Set();

// Generate a unique hex code that isn't already in use
function generateUniqueHexCode(existingHexCodes) {
    const hexCodes = ['A1B2C3', 'B2C3D4', 'C3D4E5', 'D4E5F6', 'E5F6A1', 'F6A1B2', 'A2B3C4', 'B3C4D5'];
    
    // Try to find an unused hex from the predefined list
    const availableHexes = hexCodes.filter(hex => !existingHexCodes.has(hex));
    if (availableHexes.length > 0) {
        return availableHexes[Math.floor(Math.random() * availableHexes.length)];
    }
    
    // If all predefined hex codes are used, generate a unique one
    let newHex;
    let counter = 1000; // Prevent infinite loop
    do {
        newHex = Math.floor(Math.random() * 0xFFFFFF).toString(16).toUpperCase().padStart(6, '0');
        counter--;
    } while (existingHexCodes.has(newHex) && counter > 0);
    
    return newHex;
}

// Generate a single new aircraft with a unique hex code
function generateNewAircraft(existingHexCodes) {
    const flights = ['UAL123', 'AAL456', 'SWA789', 'DAL321', 'JBU654', 'FDX901', 'UPS234', 'N12345'];
    const categories = ['A1', 'A2', 'A3', 'A5', 'B2', 'C2'];

    // Generate position within sensor range
    const angle = Math.random() * Math.PI * 2;
    const maxInitialDistance = SENSOR_CONFIG.maxRangeNm * 0.7;
    const distanceNm = Math.random() * maxInitialDistance;
    const distanceDeg = distanceNm / 60;

    const lat = SENSOR_CONFIG.lat + Math.cos(angle) * distanceDeg;
    const lon = SENSOR_CONFIG.lon + Math.sin(angle) * distanceDeg;

    // Only generate if within range
    if (!isAircraftInRange(lat, lon)) {
        return null;
    }

    // Generate unique hex code
    const hex = generateUniqueHexCode(existingHexCodes);
    const flight = flights[Math.floor(Math.random() * flights.length)];
    const category = categories[Math.floor(Math.random() * categories.length)];

    // Random track direction (not necessarily away from sensor)
    const track = Math.floor(Math.random() * 360);
    const speed = Math.floor(Math.random() * 400) + 200;
    const altitude = Math.floor(Math.random() * 30000) + 10000;
    const verticalRate = Math.floor((Math.random() - 0.5) * 4000);

    // Assign movement pattern
    const patterns = ['straight', 'circling', 'waypoint'];
    const pattern = patterns[Math.floor(Math.random() * patterns.length)];

    return {
        hex: hex,
        flight: flight,
        lat: lat,
        lon: lon,
        altitude: altitude,
        speed: speed,
        track: track,
        verticalRate: verticalRate,
        category: category,
        messages: Math.floor(Math.random() * 500) + 100,
        // Movement pattern metadata
        pattern: pattern,
        targetTrack: track,
        waypointLat: pattern === 'waypoint' ? SENSOR_CONFIG.lat + (Math.random() - 0.5) * 2 : null,
        waypointLon: pattern === 'waypoint' ? SENSOR_CONFIG.lon + (Math.random() - 0.5) * 2 : null
    };
}

// Main loop
async function runDataGenerator() {
    console.log('🛫 Starting Aircraft Data Generator');
    console.log('📊 Connecting to Supabase...');
    
    const connected = await testConnection();
    if (!connected) {
        console.error('❌ Cannot connect to database. Exiting.');
        console.error('   Make sure DATABASE_URL is set in .env file');
        process.exit(1);
    }
    
    console.log('✅ Data generator ready');
    console.log(`📍 Sensor location: ${SENSOR_CONFIG.lat}, ${SENSOR_CONFIG.lon} (Champaign, IL)`);
    console.log(`📏 Maximum detection range: ${SENSOR_CONFIG.maxRangeNm} nautical miles`);
    console.log('💡 Generating aircraft data every 2 seconds...');
    console.log('   Aircraft beyond range will be automatically deleted from database');
    console.log('   All generated aircraft will be cleaned up on shutdown');
    console.log('   Press Ctrl+C to stop\n');
    
    // Initialize with first batch
    currentAircraft = generateMockAircraft();
    // Track hex codes of generated aircraft
    currentAircraft.forEach(ac => trackedHexCodes.add(ac.hex));
    await insertAircraftData(currentAircraft);
    console.log(`✅ Inserted ${currentAircraft.length} aircraft at ${new Date().toLocaleTimeString()}`);

    // Cleanup old history periodically (every 5 minutes)
    const cleanupInterval = setInterval(async () => {
        try {
            const deleted = await cleanupOldHistory(2); // Keep last 2 hours
            if (deleted > 0) {
                console.log(`🧹 Cleaned up ${deleted} old history records (older than 2 hours)`);
            }
        } catch (error) {
            console.error('❌ Error during history cleanup:', error.message);
        }
    }, 5 * 60 * 1000); // Every 5 minutes

    // Update loop - every 2 seconds
    const interval = setInterval(async () => {
        try {
            // Update positions
            currentAircraft = updateAircraftPositions(currentAircraft);
            
            // Check for aircraft that have moved out of range
            const aircraftToDelete = [];
            const aircraftInRange = [];
            
            currentAircraft.forEach(ac => {
                if (isAircraftInRange(ac.lat, ac.lon)) {
                    aircraftInRange.push(ac);
                } else {
                    const distance = calculateDistance(
                        SENSOR_CONFIG.lat, 
                        SENSOR_CONFIG.lon, 
                        ac.lat, 
                        ac.lon
                    );
                    aircraftToDelete.push({
                        hex: ac.hex,
                        flight: ac.flight || 'N/A',
                        distance: distance.toFixed(2)
                    });
                }
            });
            
            // Delete out-of-range aircraft from database
            // Also cleanup trail history since hex codes will be reused
            if (aircraftToDelete.length > 0) {
                const hexCodes = aircraftToDelete.map(ac => ac.hex);
                const deletedCount = await deleteAircraftFromDB(hexCodes, true); // true = cleanup history

                aircraftToDelete.forEach(ac => {
                    console.log(`✈️  Aircraft ${ac.hex} (${ac.flight}) out of range (${ac.distance} nm) - deleted from database`);
                });
                console.log(`🗑️  Deleted ${deletedCount} total records (aircraft, trails, sessions) for ${aircraftToDelete.length} out-of-range aircraft`);
            }
            
            // Update current aircraft list (only keep those in range)
            currentAircraft = aircraftInRange;
            
            // Occasionally add new aircraft (simulate aircraft entering sensor range)
            if (Math.random() < 0.1 && currentAircraft.length < 8) { // 10% chance, max 8 aircraft
                // Get set of currently active hex codes to ensure uniqueness
                // Only check against currently active aircraft, not all tracked ones
                // (trackedHexCodes is kept for cleanup purposes, but hex codes can be reused)
                const activeHexCodes = new Set(currentAircraft.map(ac => ac.hex));
                
                // Generate a single new aircraft with unique hex code
                const newAc = generateNewAircraft(activeHexCodes);
                if (newAc) {
                    currentAircraft.push(newAc);
                    // Track the new aircraft's hex code (for cleanup on shutdown)
                    trackedHexCodes.add(newAc.hex);
                    console.log(`➕ New aircraft entered range: ${newAc.hex} (${newAc.flight || 'N/A'})`);
                }
            }
            
            // Insert updated data for aircraft still in range
            if (currentAircraft.length > 0) {
                await insertAircraftData(currentAircraft);
                console.log(`✅ Updated ${currentAircraft.length} aircraft in range at ${new Date().toLocaleTimeString()}`);
            } else {
                console.log(`⚠️  No aircraft in sensor range at ${new Date().toLocaleTimeString()}`);
            }
        } catch (error) {
            console.error('❌ Error in update loop:', error.message);
        }
    }, 2000); // Update every 2 seconds
    
    // Cleanup function to delete all tracked aircraft from database
    // Also removes their trail history and session data on shutdown
    const cleanup = async () => {
        if (trackedHexCodes.size > 0) {
            console.log(`\n🧹 Cleaning up ${trackedHexCodes.size} aircraft from database...`);
            try {
                const hexCodesArray = Array.from(trackedHexCodes);
                const deletedCount = await deleteAircraftFromDB(hexCodesArray, true); // true = cleanup history
                console.log(`✅ Deleted ${deletedCount} total records (aircraft, trails, and sessions) for ${trackedHexCodes.size} aircraft`);
            } catch (error) {
                console.error('❌ Error during cleanup:', error.message);
            }
        } else {
            console.log('\n🧹 No aircraft to clean up');
        }
    };
    
    // Cleanup on exit signals
    const shutdown = async (signal) => {
        console.log(`\n🛑 Received ${signal}, shutting down data generator...`);
        clearInterval(interval);
        clearInterval(cleanupInterval);

        // Clean up all tracked aircraft
        await cleanup();

        // Close database connection
        try {
            await db.end();
            console.log('✅ Database connection closed');
        } catch (error) {
            console.error('❌ Error closing database:', error.message);
        }

        process.exit(0);
    };
    
    // Handle various exit signals
    process.on('SIGINT', () => shutdown('SIGINT'));
    process.on('SIGTERM', () => shutdown('SIGTERM'));
    
    // Handle uncaught exceptions
    process.on('uncaughtException', async (error) => {
        console.error('❌ Uncaught exception:', error);
        await cleanup();
        await db.end();
        process.exit(1);
    });
    
    // Handle unhandled promise rejections
    process.on('unhandledRejection', async (reason, promise) => {
        console.error('❌ Unhandled rejection at:', promise, 'reason:', reason);
        await cleanup();
        await db.end();
        process.exit(1);
    });
}

// Start the generator
runDataGenerator().catch(error => {
    console.error('Fatal error:', error);
    process.exit(1);
});

