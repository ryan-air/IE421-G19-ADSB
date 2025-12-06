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
    maxRangeNm: parseFloat(process.env.MAX_RANGE_NM) || 10
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
async function deleteAircraftFromDB(hexCodes) {
    if (hexCodes.length === 0) return 0;
    
    try {
        const result = await db.query(`
            DELETE FROM aircraft 
            WHERE hex = ANY($1::varchar[])
        `, [hexCodes]);
        
        return result.rowCount;
    } catch (error) {
        console.error('Error deleting aircraft from database:', error.message);
        throw error;
    }
}

// Generate mock aircraft data (within sensor range)
function generateMockAircraft() {
    const flights = ['UAL123', 'AAL456', 'SWA789', 'DAL321', 'JBU654', 'FDX901', 'UPS234', 'N12345'];
    const hexCodes = ['A1B2C3', 'B2C3D4', 'C3D4E5', 'D4E5F6', 'E5F6A1', 'F6A1B2', 'A2B3C4', 'B3C4D5'];
    
    // Generate 3-8 aircraft
    const numAircraft = Math.floor(Math.random() * 6) + 3;
    const aircraft = [];
    
    for (let i = 0; i < numAircraft; i++) {
        // Generate position within sensor range (spread around sensor location)
        // Use smaller initial distance to ensure aircraft start within range
        const angle = (Math.PI * 2 * i) / numAircraft + Math.random() * 0.5;
        // Generate within 70% of max range to ensure room for movement
        const maxInitialDistance = SENSOR_CONFIG.maxRangeNm * 0.7;
        const distanceNm = Math.random() * maxInitialDistance;
        const distanceDeg = distanceNm / 60; // Rough conversion: 1 degree ≈ 60 nm
        
        const lat = SENSOR_CONFIG.lat + Math.cos(angle) * distanceDeg;
        const lon = SENSOR_CONFIG.lon + Math.sin(angle) * distanceDeg;
        
        // Only add if within range (safety check)
        if (!isAircraftInRange(lat, lon)) {
            continue; // Skip this aircraft if somehow out of range
        }
        
        // Generate realistic flight data
        const flight = flights[Math.floor(Math.random() * flights.length)];
        const hex = hexCodes[i] || `TEST${i.toString(16).toUpperCase().padStart(2, '0')}`;
        
        // Calculate initial track (heading) based on angle from sensor
        // In generation: angle uses cos() for lat (north-south) and sin() for lon (east-west)
        // So: angle 0 = North (cos=1, sin=0), π/2 = East (cos=0, sin=1)
        // Track in aviation: 0° = North, 90° = East, 180° = South, 270° = West
        // Therefore: track = angle * 180 / π (direct conversion)
        const track = ((angle * 180 / Math.PI) % 360 + 360) % 360;
        
        const speed = Math.floor(Math.random() * 400) + 200; // 200-600 kt
        const altitude = Math.floor(Math.random() * 30000) + 10000; // 10,000-40,000 ft
        
        aircraft.push({
            hex: hex,
            flight: flight,
            lat: lat,
            lon: lon,
            altitude: altitude,
            speed: speed,
            track: Math.floor(track),
            messages: Math.floor(Math.random() * 500) + 100
        });
    }
    
    return aircraft;
}

// Insert or update aircraft data into Supabase
// Uses UPSERT to update existing records (since hex is primary key)
async function insertAircraftData(aircraft) {
    if (aircraft.length === 0) return 0;
    
    try {
        let processed = 0;
        
        for (const ac of aircraft) {
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
            processed++;
        }
        
        return processed;
    } catch (error) {
        console.error('Error inserting/updating aircraft data:', error.message);
        throw error;
    }
}

// Simulate aircraft movement (update positions slightly)
function updateAircraftPositions(aircraft) {
    return aircraft.map(ac => {
        // Convert track (heading) to radians
        // Track 0° = North, 90° = East, 180° = South, 270° = West
        const trackRad = (ac.track * Math.PI) / 180;
        const speedKts = ac.speed;
        
        // Calculate distance traveled in 2 seconds
        // Speed is in knots (nautical miles per hour)
        // Convert to nautical miles per second, then multiply by time step
        const timeStepSeconds = 2;
        const distanceNm = (speedKts / 3600) * timeStepSeconds;
        
        // Convert distance to degrees
        // 1 degree of latitude ≈ 60 nautical miles (constant)
        // 1 degree of longitude ≈ 60 * cos(latitude) nautical miles (varies with latitude)
        const nmPerDegreeLat = 60;
        const nmPerDegreeLon = 60 * Math.cos(ac.lat * Math.PI / 180);
        
        // Calculate position change
        // Track 0° (North): cos(0) = 1, sin(0) = 0 → moves north (+lat)
        // Track 90° (East): cos(90) = 0, sin(90) = 1 → moves east (+lon)
        // Track 180° (South): cos(180) = -1, sin(180) = 0 → moves south (-lat)
        // Track 270° (West): cos(270) = 0, sin(270) = -1 → moves west (-lon)
        const latChange = (distanceNm / nmPerDegreeLat) * Math.cos(trackRad);
        const lonChange = (distanceNm / nmPerDegreeLon) * Math.sin(trackRad);
        
        // Calculate new position
        const newLat = ac.lat + latChange;
        const newLon = ac.lon + lonChange;
        
        // Only make small random track adjustments occasionally (reduce randomness)
        let newTrack = ac.track;
        if (Math.random() < 0.05) { // 5% chance of small heading change (down from higher chance)
            // Small heading adjustment: ±2 degrees max (reduced from ±5)
            newTrack = ac.track + (Math.random() - 0.5) * 2;
        }
        newTrack = ((newTrack % 360) + 360) % 360; // Normalize to 0-360
        
        return {
            ...ac,
            lat: newLat,
            lon: newLon,
            track: Math.floor(newTrack)
        };
    });
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
    const track = ((angle * 180 / Math.PI) % 360 + 360) % 360;
    const speed = Math.floor(Math.random() * 400) + 200;
    const altitude = Math.floor(Math.random() * 30000) + 10000;
    
    return {
        hex: hex,
        flight: flight,
        lat: lat,
        lon: lon,
        altitude: altitude,
        speed: speed,
        track: Math.floor(track),
        messages: Math.floor(Math.random() * 500) + 100
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
            if (aircraftToDelete.length > 0) {
                const hexCodes = aircraftToDelete.map(ac => ac.hex);
                const deletedCount = await deleteAircraftFromDB(hexCodes);
                
                aircraftToDelete.forEach(ac => {
                    console.log(`✈️  Aircraft ${ac.hex} (${ac.flight}) out of range (${ac.distance} nm) - deleted from database`);
                });
                console.log(`🗑️  Deleted ${deletedCount} database records for ${aircraftToDelete.length} out-of-range aircraft`);
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
    const cleanup = async () => {
        if (trackedHexCodes.size > 0) {
            console.log(`\n🧹 Cleaning up ${trackedHexCodes.size} aircraft from database...`);
            try {
                const hexCodesArray = Array.from(trackedHexCodes);
                const deletedCount = await deleteAircraftFromDB(hexCodesArray);
                console.log(`✅ Deleted ${deletedCount} database records for ${trackedHexCodes.size} aircraft`);
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

