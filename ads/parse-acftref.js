/**
 * Parse ACFTREF.txt to create aircraft type lookup table
 * Maps MFR_MDL_CODE to aircraft type (6 = Rotorcraft/Helicopter)
 */

import fs from 'fs';
import { parse } from 'csv-parse/sync';
import path from 'path';
import { fileURLToPath } from 'url';

const __filename = fileURLToPath(import.meta.url);
const __dirname = path.dirname(__filename);

const ACFTREF_PATH = path.join(__dirname, '../ReleasableAircraft/ACFTREF.txt');
const OUTPUT_PATH = path.join(__dirname, 'aircraft-types.json');

console.log('📖 Reading ACFTREF.txt...');

// Read the file
const fileContent = fs.readFileSync(ACFTREF_PATH, 'utf-8');

// Parse CSV (comma-delimited)
const records = parse(fileContent, {
    columns: true,
    skip_empty_lines: true,
    trim: true,
    bom: true, // Handle BOM character
    relax_quotes: true, // Allow quotes to appear in unquoted fields
    relax_column_count: true, // Allow variable column counts
    quote: false // Disable quote processing entirely
});

console.log(`✅ Parsed ${records.length} aircraft reference records`);

// Build lookup table: MFR_MDL_CODE -> TYPE-ACFT
const aircraftTypes = {};
let helicopterCount = 0;

records.forEach(record => {
    const code = record.CODE;
    const typeAircraft = record['TYPE-ACFT'];

    if (code && typeAircraft) {
        aircraftTypes[code] = parseInt(typeAircraft);

        if (typeAircraft === '6') {
            helicopterCount++;
        }
    }
});

console.log(`🚁 Found ${helicopterCount} helicopter/rotorcraft models`);
console.log(`✈️  Total aircraft models: ${Object.keys(aircraftTypes).length}`);

// Save to JSON file
fs.writeFileSync(OUTPUT_PATH, JSON.stringify(aircraftTypes, null, 2));
console.log(`💾 Saved aircraft type lookup to: ${OUTPUT_PATH}`);

// Print some example helicopters
console.log('\n📋 Sample helicopter models:');
const helicopterExamples = records
    .filter(r => r['TYPE-ACFT'] === '6')
    .slice(0, 10)
    .map(r => `  ${r.CODE}: ${r.MFR.trim()} ${r.MODEL.trim()}`);

helicopterExamples.forEach(ex => console.log(ex));
