import dotenv from "dotenv";
dotenv.config();

import { Pool } from 'pg';

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

// Generate mock aircraft data
function generateMockAircraft() {
    const flights = ['UAL123', 'AAL456', 'SWA789', 'DAL321', 'JBU654', 'FDX901', 'UPS234', 'N12345'];
    const hexCodes = ['A1B2C3', 'B2C3D4', 'C3D4E5', 'D4E5F6', 'E5F6A1', 'F6A1B2', 'A2B3C4', 'B3C4D5'];
    
    // Base location (Champaign, IL)
    const baseLat = 40.1164;
    const baseLon = -88.2434;
    
    // Generate 3-8 aircraft
    const numAircraft = Math.floor(Math.random() * 6) + 3;
    const aircraft = [];
    
    for (let i = 0; i < numAircraft; i++) {
        // Generate position in a wider area around Champaign
        const angle = (Math.PI * 2 * i) / numAircraft + Math.random() * 0.5;
        const distance = 0.1 + Math.random() * 0.3; // 0.1 to 0.4 degrees (~11-44 km)
        const lat = baseLat + Math.cos(angle) * distance;
        const lon = baseLon + Math.sin(angle) * distance;
        
        // Generate realistic flight data
        const flight = flights[Math.floor(Math.random() * flights.length)];
        const hex = hexCodes[i] || `TEST${i.toString(16).toUpperCase().padStart(2, '0')}`;
        
        // Simulate movement - aircraft move in their current direction
        const track = (angle * 180 / Math.PI + 90) % 360; // Convert to degrees, adjust for map orientation
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

// Insert aircraft data into Supabase
async function insertAircraftData(aircraft) {
    try {
        let inserted = 0;
        
        for (const ac of aircraft) {
            await db.query(`
                INSERT INTO aircraft (hex, flight, lat, lon, altitude, speed, track, messages, seen_at)
                VALUES ($1, $2, $3, $4, $5, $6, $7, $8, NOW())
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
            inserted++;
        }
        
        return inserted;
    } catch (error) {
        console.error('Error inserting aircraft data:', error.message);
        throw error;
    }
}

// Simulate aircraft movement (update positions slightly)
function updateAircraftPositions(aircraft) {
    return aircraft.map(ac => {
        // Move aircraft slightly in the direction of travel
        const trackRad = (ac.track * Math.PI) / 180;
        const speedKts = ac.speed;
        const speedDegreesPerSecond = speedKts / 60 / 60; // Rough conversion (1 degree ≈ 60 nautical miles)
        const timeStep = 2; // 2 seconds between updates
        
        // Calculate new position
        const latChange = Math.cos(trackRad) * speedDegreesPerSecond * timeStep;
        const lonChange = Math.sin(trackRad) * speedDegreesPerSecond * timeStep;
        
        return {
            ...ac,
            lat: ac.lat + latChange,
            lon: ac.lon + lonChange,
            // Occasionally change heading slightly (ensure it's an integer)
            track: Math.floor((ac.track + (Math.random() - 0.5) * 5 + 360) % 360)
        };
    });
}

// Store current aircraft state for movement simulation
let currentAircraft = [];

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
    console.log('💡 Generating aircraft data every 2 seconds...');
    console.log('   Press Ctrl+C to stop\n');
    
    // Initialize with first batch
    currentAircraft = generateMockAircraft();
    await insertAircraftData(currentAircraft);
    console.log(`✅ Inserted ${currentAircraft.length} aircraft at ${new Date().toLocaleTimeString()}`);
    
    // Update loop - every 2 seconds
    const interval = setInterval(async () => {
        try {
            // Update positions
            currentAircraft = updateAircraftPositions(currentAircraft);
            
            // Occasionally add/remove aircraft (simulate aircraft entering/leaving area)
            if (Math.random() < 0.1) { // 10% chance
                if (currentAircraft.length < 8 && Math.random() > 0.5) {
                    // Add new aircraft
                    const newAircraft = generateMockAircraft();
                    currentAircraft.push(newAircraft[0]);
                } else if (currentAircraft.length > 3) {
                    // Remove an aircraft
                    currentAircraft.splice(Math.floor(Math.random() * currentAircraft.length), 1);
                }
            }
            
            // Insert updated data
            await insertAircraftData(currentAircraft);
            console.log(`✅ Updated ${currentAircraft.length} aircraft at ${new Date().toLocaleTimeString()}`);
        } catch (error) {
            console.error('❌ Error in update loop:', error.message);
        }
    }, 2000); // Update every 2 seconds
    
    // Cleanup on exit
    process.on('SIGINT', async () => {
        console.log('\n🛑 Shutting down data generator...');
        clearInterval(interval);
        await db.end();
        process.exit(0);
    });
}

// Start the generator
runDataGenerator().catch(error => {
    console.error('Fatal error:', error);
    process.exit(1);
});

