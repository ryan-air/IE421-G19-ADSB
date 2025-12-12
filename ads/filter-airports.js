#!/usr/bin/env node

/**
 * Filter airports.csv to create a smaller JSON file for browser use
 * Filters to US airports with commercial service or medium/large airports
 */

import fs from 'fs';
import { parse } from 'csv-parse/sync';

console.log('🛫 Filtering airports...\n');

// Read the CSV file
const csvContent = fs.readFileSync('airports.csv', 'utf-8');

// Parse CSV
const records = parse(csvContent, {
    columns: true,
    skip_empty_lines: true,
    trim: true
});

console.log(`📊 Total airports in CSV: ${records.length}`);

// Filter airports
const filtered = records.filter(airport => {
    // Must be in United States
    if (airport.iso_country !== 'US') return false;

    // Must have valid coordinates
    if (!airport.latitude_deg || !airport.longitude_deg) return false;

    // Include if:
    // 1. Large or medium airport, OR
    // 2. Has scheduled commercial service
    const isLargeOrMedium = airport.type === 'large_airport' || airport.type === 'medium_airport';
    const hasScheduledService = airport.scheduled_service === 'yes';

    return isLargeOrMedium || hasScheduledService;
});

console.log(`✓ Filtered to ${filtered.length} US airports`);

// Convert to simplified GeoJSON format
const geojson = {
    type: 'FeatureCollection',
    features: filtered.map(airport => ({
        type: 'Feature',
        geometry: {
            type: 'Point',
            coordinates: [
                parseFloat(airport.longitude_deg),
                parseFloat(airport.latitude_deg)
            ]
        },
        properties: {
            name: airport.name,
            icao: airport.icao_code || null,
            iata: airport.iata_code || null,
            type: airport.type,
            elevation: airport.elevation_ft ? parseInt(airport.elevation_ft) : null,
            municipality: airport.municipality || null
        }
    }))
};

// Write to JSON file
const outputPath = 'airports.json';
fs.writeFileSync(outputPath, JSON.stringify(geojson, null, 2));

const stats = fs.statSync(outputPath);
const sizeKB = (stats.size / 1024).toFixed(1);

console.log(`✓ Created ${outputPath} (${sizeKB} KB)`);

// Show breakdown by type
const typeBreakdown = {};
filtered.forEach(airport => {
    typeBreakdown[airport.type] = (typeBreakdown[airport.type] || 0) + 1;
});

console.log('\n📈 Airport breakdown:');
Object.entries(typeBreakdown).forEach(([type, count]) => {
    console.log(`  ${type}: ${count}`);
});

console.log('\n✅ Done! Use airports.json in your frontend.');
