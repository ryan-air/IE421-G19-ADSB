/**
 * Parse FAA data files to create hex code → aircraft type lookup
 * This creates a direct mapping from ICAO hex code to aircraft type
 * for helicopter detection
 */

import fs from 'fs';
import { parse } from 'csv-parse/sync';
import path from 'path';
import { fileURLToPath } from 'url';

const __filename = fileURLToPath(import.meta.url);
const __dirname = path.dirname(__filename);

const MASTER_PATH = path.join(__dirname, '../ReleasableAircraft/MASTER.txt');
const ACFTREF_PATH = path.join(__dirname, '../ReleasableAircraft/ACFTREF.txt');
const OUTPUT_PATH = path.join(__dirname, 'aircraft-types-by-hex.json');

console.log('📖 Step 1: Reading ACFTREF.txt to build MFR_MDL_CODE → TYPE lookup...');

// Read ACFTREF and build CODE -> TYPE mapping
const acftrefContent = fs.readFileSync(ACFTREF_PATH, 'utf-8');
const acftrefRecords = parse(acftrefContent, {
    columns: true,
    skip_empty_lines: true,
    trim: true,
    bom: true,
    relax_quotes: true,
    relax_column_count: true,
    quote: false
});

const codeToType = {};
acftrefRecords.forEach(record => {
    const code = record.CODE;
    const typeAircraft = record['TYPE-ACFT'];
    if (code && typeAircraft) {
        codeToType[code] = parseInt(typeAircraft);
    }
});

console.log(`✅ Loaded ${Object.keys(codeToType).length} aircraft model type mappings`);

console.log('\n📖 Step 2: Reading MASTER.txt to build MODE_S_CODE_HEX → MFR_MDL_CODE...');

// Read MASTER and build HEX -> MFR_MDL_CODE mapping
const masterContent = fs.readFileSync(MASTER_PATH, 'utf-8');
const masterRecords = parse(masterContent, {
    columns: true,
    skip_empty_lines: true,
    trim: true,
    bom: true,
    relax_quotes: true,
    relax_column_count: true,
    quote: false
});

const hexToType = {};
let helicopterCount = 0;
let processedCount = 0;

masterRecords.forEach(record => {
    const hex = record['MODE S CODE HEX'];
    const mfrMdlCode = record['MFR MDL CODE'];

    if (hex && mfrMdlCode && codeToType[mfrMdlCode] !== undefined) {
        // Clean up hex code (remove spaces, convert to uppercase)
        const cleanHex = hex.trim().toUpperCase();
        const aircraftType = codeToType[mfrMdlCode];

        hexToType[cleanHex] = aircraftType;
        processedCount++;

        if (aircraftType === 6) {
            helicopterCount++;
        }
    }
});

console.log(`✅ Processed ${processedCount} aircraft registrations`);
console.log(`🚁 Found ${helicopterCount} registered helicopters/rotorcraft`);
console.log(`✈️  Total mapped aircraft: ${Object.keys(hexToType).length}`);

// Save to JSON file
fs.writeFileSync(OUTPUT_PATH, JSON.stringify(hexToType, null, 2));
console.log(`\n💾 Saved hex → type lookup to: ${OUTPUT_PATH}`);

// Print some example helicopters
console.log('\n📋 Sample registered helicopters (by hex code):');
const helicopterExamples = Object.entries(hexToType)
    .filter(([hex, type]) => type === 6)
    .slice(0, 10)
    .map(([hex, type]) => `  ${hex}: Type ${type} (Rotorcraft)`);

helicopterExamples.forEach(ex => console.log(ex));

// Stats
const typeStats = {};
Object.values(hexToType).forEach(type => {
    typeStats[type] = (typeStats[type] || 0) + 1;
});

console.log('\n📊 Aircraft type distribution:');
const typeNames = {
    1: 'Glider',
    2: 'Balloon',
    3: 'Blimp/Dirigible',
    4: 'Fixed wing single engine',
    5: 'Fixed wing multi engine',
    6: 'Rotorcraft',
    7: 'Weight-shift-control',
    8: 'Powered Parachute',
    9: 'Gyroplane'
};

Object.entries(typeStats)
    .sort((a, b) => b[1] - a[1])
    .forEach(([type, count]) => {
        const typeName = typeNames[type] || `Type ${type}`;
        console.log(`  ${typeName}: ${count}`);
    });
