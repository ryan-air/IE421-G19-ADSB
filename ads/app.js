// Store aircraft data and markers
let map;
let currentAircraftData = []; // Global store for live aircraft positions
const aircraftMarkers = new Map();
const aircraftState = new Map(); // Store previous state for interpolation
let updateInterval;
let statisticsInterval;
const INTERPOLATION_DURATION = 2000; // Match polling interval for smooth movement

// Airport visualization
let airportsLayer = null;
let airportsData = null;
let showAirports = false;

// Trail settings
const aircraftTrails = new Map(); // Store trail polylines: hex -> polyline
const trailData = new Map(); // Store trail position data: hex -> positions array
const failedTrailFetches = new Set(); // Track aircraft with no trail data to avoid repeated requests
let trailRefreshInterval = null;
let showAllTrails = false; // Global toggle for all trails

// Colours for everything drawn on the map. Mirrors the tokens in styles.css
// (:root); keep the two in step.
const PALETTE = {
    ink: '#ebe5d6',
    stone: '#8e887c',
    graphite: '#121110',
    ochre: '#b8955a',
    steel: '#7d93a6',
    alert: '#c2603f',
    caution: '#b98340',
    sand: '#c2b598',
    alt: {
        ground: '#77726a',
        low: '#c39a5a',  // < 10,000 ft
        mid: '#c2b598',  // 10-20,000 ft
        high: '#98a8b3', // 20-30,000 ft
        top: '#7590a8',  // >= 30,000 ft
    },
};

// Altitude band of an aircraft, for its colour (see the legend in index.html)
function altitudeBand(aircraft) {
    if (aircraft.on_ground) return 'ground';
    const alt = aircraft.altitude;
    if (alt === null || alt === undefined) return 'mid';
    if (alt < 10000) return 'low';
    if (alt < 20000) return 'mid';
    if (alt < 30000) return 'high';
    return 'top';
}

// One popup layout for aircraft, airports, trails and collisions
function popupHTML(title, rows, extra = '', cls = '') {
    const body = rows
        .filter(([, value]) => value !== null && value !== undefined && value !== '')
        .map(([label, value]) => `<div class="map-popup-row"><span>${label}</span><span>${value}</span></div>`)
        .join('');
    return `<div class="map-popup ${cls}"><div class="map-popup-title">${title}</div>${body}${extra}</div>`;
}

// Initialize map
function initializeMap() {
    // Initialize the map centered on a default location (you can change this)
    map = L.map('map', {
        zoomControl: false, // zoom lives on the edge rail (atlas.js)
        attributionControl: true
    }).setView([41.88, -87.63], 9); // Default: Chicago, IL (receiver location)

    // Overlay panes, bottom to top: recorded trails, live overlays
    // (predictions, conflicts; hidden during playback), the selection
    map.createPane('trailPane').style.zIndex = 405;
    map.createPane('livePane').style.zIndex = 415;
    map.createPane('selectionPane').style.zIndex = 425;

    // Dark basemap for the ATC/radar look: Esri's dark gray canvas plus its
    // labels layer (no API key needed; CARTO's dark_all now requires one).
    const esri = 'https://server.arcgisonline.com/ArcGIS/rest/services/Canvas';
    const esriAttribution = 'Tiles &copy; Esri &mdash; Esri, HERE, Garmin, &copy; OpenStreetMap contributors';
    L.tileLayer(`${esri}/World_Dark_Gray_Base/MapServer/tile/{z}/{y}/{x}`, {
        attribution: esriAttribution,
        maxNativeZoom: 16,
        maxZoom: 19
    }).addTo(map);
    L.tileLayer(`${esri}/World_Dark_Gray_Reference/MapServer/tile/{z}/{y}/{x}`, {
        maxNativeZoom: 16,
        maxZoom: 19,
        opacity: 0.55, // place names recede behind the aircraft
        className: 'reference-tiles'
    }).addTo(map);

    // Load airports data
    loadAirports();
}

// Load airports from GeoJSON file
async function loadAirports() {
    try {
        const response = await fetch('airports.json');
        if (!response.ok) {
            console.warn('Could not load airports.json');
            return;
        }
        airportsData = await response.json();
        console.log(`Loaded ${airportsData.features.length} airports`);
    } catch (error) {
        console.error('Error loading airports:', error);
    }
}

// Toggle airport visibility
function toggleAirports() {
    showAirports = !showAirports;

    if (showAirports) {
        displayAirports();
    } else {
        hideAirports();
    }
}

// Display airports on map
function displayAirports() {
    if (!airportsData || !map) return;

    // Remove existing layer if any
    if (airportsLayer) {
        map.removeLayer(airportsLayer);
    }

    // Create GeoJSON layer with custom markers
    airportsLayer = L.geoJSON(airportsData, {
        pointToLayer: (feature, latlng) => {
            const props = feature.properties;

            // Chart symbols: a ticked ring for large airports, a ring for
            // the rest (their names are drawn by atlas.js)
            const large = props.type === 'large_airport';
            return L.marker(latlng, {
                icon: L.divIcon({
                    className: 'airport-symbol',
                    html: `<i class="${large ? 'apt-large' : 'apt-small'}"></i>`,
                    iconSize: [0, 0],
                }),
                keyboard: false,
            });
        },
        onEachFeature: (feature, layer) => {
            const props = feature.properties;

            // Create popup with airport info
            const popupContent = popupHTML(props.name, [
                ['ICAO', props.icao ? `${props.icao}${props.iata ? ` (${props.iata})` : ''}` : null],
                ['Location', props.municipality],
                ['Elevation', props.elevation ? `${props.elevation} ft` : null],
                ['Type', `<span class="map-popup-note">${props.type.replace('_', ' ')}</span>`],
            ]);
            layer.bindPopup(popupContent);
        }
    });

    airportsLayer.addTo(map);
}

// Hide airports from map
function hideAirports() {
    if (airportsLayer && map) {
        map.removeLayer(airportsLayer);
        airportsLayer = null;
    }
}

// Update status indicator
function updateStatus(connected, text) {
    const indicator = document.getElementById('status-indicator');
    const statusText = document.getElementById('status-text');
    
    if (connected) {
        indicator.className = 'status-dot connected';
        statusText.textContent = text || 'Connected';
    } else {
        indicator.className = 'status-dot disconnected';
        statusText.textContent = text || 'Disconnected';
    }
    atlas.onStatus(connected, statusText.textContent);
}

// Format time
function formatTime(timestamp) {
    if (!timestamp) return '--';
    const date = new Date(timestamp);
    return date.toLocaleTimeString();
}

// Format duration (seconds to MM:SS or HH:MM:SS)
function formatDuration(seconds) {
    if (!seconds || seconds < 0) return '0s';
    const hours = Math.floor(seconds / 3600);
    const minutes = Math.floor((seconds % 3600) / 60);
    const secs = seconds % 60;

    if (hours > 0) {
        return `${hours}h ${minutes}m ${secs}s`;
    } else if (minutes > 0) {
        return `${minutes}m ${secs}s`;
    } else {
        return `${secs}s`;
    }
}

// Trail functionality removed

// Create aircraft marker icon with plane SVG and label
// Check if aircraft is a helicopter based on type field from server
function isHelicopter(aircraft) {
    // Type field is provided by server (6 = Rotorcraft/Helicopter)
    return aircraft.type === 6;
}

function createAircraftIcon(aircraft) {
    const heading = aircraft.track || 0;
    const isHeli = isHelicopter(aircraft);
    const band = altitudeBand(aircraft);

    // A directional symbol at the aircraft's position (rotated to its
    // track) and an ATC-style data block on a leader line; atlas.js places
    // the block around the symbol so blocks do not collide.
    const symbol = isHeli ? `
        <svg width="18" height="18" viewBox="-9 -9 18 18" class="aircraft-icon" style="transform: rotate(${heading}deg);">
            <circle r="4.2" fill="none" stroke="currentColor" stroke-width="1.4"/>
            <line x1="-8" y1="0" x2="8" y2="0" stroke="currentColor" stroke-width="1.2"/>
            <line x1="0" y1="-8" x2="0" y2="-4.2" stroke="currentColor" stroke-width="1.4"/>
        </svg>` : `
        <svg width="18" height="18" viewBox="-9 -9 18 18" class="aircraft-icon" style="transform: rotate(${heading}deg);">
            <path d="M0 -8 L5.2 6.4 L0 3.4 L-5.2 6.4 Z" fill="currentColor" stroke="#121110" stroke-width="0.8" stroke-linejoin="round"/>
        </svg>`;

    const markerHTML = `
        <div class="ac" data-band="${band}">
            <svg class="ac-leader" width="1" height="1"><line x1="0" y1="0" x2="0" y2="0"/></svg>
            <div class="ac-symbol">${symbol}</div>
            <div class="ac-label"><b class="ac-callsign">${aircraft.flight || aircraft.hex || 'N/A'}</b><span class="ac-data">${atlasDataLine(aircraft)}</span></div>
        </div>
    `;

    return L.divIcon({
        className: 'aircraft-marker',
        html: markerHTML,
        iconSize: [0, 0], // the symbol is centred on the position by CSS
        iconAnchor: [0, 0]
    });
}

// Second line of a data block: altitude in hundreds of feet, climb or
// descent arrow, ground speed in knots ("350↑241"); GND on the ground
function atlasDataLine(aircraft) {
    if (aircraft.on_ground) return `GND ${aircraft.speed ?? ''}`.trim();
    const alt = aircraft.altitude === null || aircraft.altitude === undefined
        ? '---' : String(Math.round(aircraft.altitude / 100)).padStart(3, '0');
    const vr = aircraft.vertical_rate;
    const arrow = vr > 300 ? '↑' : vr < -300 ? '↓' : ' ';
    return `${alt}${arrow}${aircraft.speed ?? '---'}`;
}

// Interpolate between two angles (handles 0/360 wrap)
function interpolateAngle(start, end, t) {
    let diff = end - start;
    if (diff > 180) diff -= 360;
    if (diff < -180) diff += 360;
    return start + diff * t;
}

// Smooth movement interpolation
function smoothMoveMarker(marker, targetLat, targetLon, targetHeading, duration) {
    // With the live feed, updates arrive many times a second: show the
    // latest position at once instead of easing towards it over 2 s
    // (?animate=1 keeps the easing).
    if (liveFeed.synced && !liveFeed.animate) {
        marker.setLatLng([targetLat, targetLon]);
        if (marker._icon) {
            const iconElement = marker._icon.querySelector('.aircraft-icon');
            if (iconElement) {
                iconElement.style.transform = `rotate(${targetHeading}deg)`;
            }
        }
        marker._heading = targetHeading;
        return;
    }
    const currentPos = marker.getLatLng();
    const startLat = currentPos.lat;
    const startLon = currentPos.lng;
    const startTime = Date.now();
    
    // Get current heading from icon rotation if available
    const currentHeading = marker._heading || targetHeading;
    
    const animate = () => {
        const elapsed = Date.now() - startTime;
        const progress = Math.min(elapsed / duration, 1);
        
        // Ease out function for smooth deceleration
        const eased = 1 - Math.pow(1 - progress, 3);
        
        const lat = startLat + (targetLat - startLat) * eased;
        const lon = startLon + (targetLon - startLon) * eased;
        const heading = interpolateAngle(currentHeading, targetHeading, eased);
        
        marker.setLatLng([lat, lon]);
        
        // Update icon rotation
        if (marker._icon) {
            const iconElement = marker._icon.querySelector('.aircraft-icon');
            if (iconElement) {
                iconElement.style.transform = `rotate(${heading}deg)`;
            }
        }
        
        marker._heading = heading;
        
        if (progress < 1) {
            requestAnimationFrame(animate);
        }
    };
    
    animate();
}

function updateAircraftOnMap(aircraftData) {
    if (!map) return; // Map not initialized yet
    
    const currentTime = Date.now();
    const seenAircraftIds = new Set();
    
    // Process all aircraft in the current dataset
    (aircraftData || []).forEach(aircraft => {
        if (!aircraft.lat || !aircraft.lon) return;
        
        const aircraftId = aircraft.hex || aircraft.id || `aircraft-${aircraft.flight}`;
        seenAircraftIds.add(aircraftId);
        
        if (aircraftMarkers.has(aircraftId)) {
            // Update existing marker with smooth movement
            const marker = aircraftMarkers.get(aircraftId);
            
            // Only animate if position changed significantly
            const currentPos = marker.getLatLng();
            const distance = map.distance(currentPos, [aircraft.lat, aircraft.lon]);
            
            if (distance > 100) { // Only animate if moved more than 100m
                smoothMoveMarker(
                    marker,
                    aircraft.lat,
                    aircraft.lon,
                    aircraft.track || 0,
                    INTERPOLATION_DURATION
                );
            } else {
                // Small movement, use shorter animation duration
                smoothMoveMarker(
                    marker,
                    aircraft.lat,
                    aircraft.lon,
                    aircraft.track || 0,
                    500 // Shorter duration for small movements
                );
            }
            
            // Update the data block and icon rotation if needed
            if (marker._icon) {
                const labelElement = marker._icon.querySelector('.ac-callsign');
                const newIdentifier = aircraft.flight || aircraft.hex || 'N/A';
                if (labelElement && marker._identifier !== newIdentifier) {
                    labelElement.textContent = newIdentifier;
                    marker._identifier = newIdentifier;
                }
                const dataLine = atlasDataLine(aircraft);
                if (marker._dataLine !== dataLine) {
                    const dataElement = marker._icon.querySelector('.ac-data');
                    if (dataElement) dataElement.textContent = dataLine;
                    marker._dataLine = dataLine;
                }
                // Recolour when the aircraft changes altitude band
                const band = altitudeBand(aircraft);
                if (marker._band !== band) {
                    const container = marker._icon.querySelector('.ac');
                    if (container) container.dataset.band = band;
                    marker._band = band;
                }
                // Update icon rotation for small movements
                const iconElement = marker._icon.querySelector('.aircraft-icon');
                if (iconElement && aircraft.track !== undefined) {
                    iconElement.style.transform = `rotate(${aircraft.track}deg)`;
                    marker._heading = aircraft.track;
                }
            }
            
        } else {
            // Create new marker
            const marker = L.marker([aircraft.lat, aircraft.lon], {
                icon: createAircraftIcon(aircraft),
                zIndexOffset: 1000 // Ensure aircraft are above map tiles
            }).addTo(map);
            
            marker._heading = aircraft.track || 0;
            marker._identifier = aircraft.flight || aircraft.hex || 'N/A';
            marker._band = altitudeBand(aircraft);
            marker._dataLine = atlasDataLine(aircraft);
            marker._hex = aircraft.hex;

            // Selecting an aircraft opens its annotation (atlas.js); the
            // gateway/database trail stays one click away in it
            marker.on('click', (e) => {
                L.DomEvent.stopPropagation(e);
                atlas.select(aircraft.hex);
            });
            marker.on('mouseover', () => atlas.hover(aircraft.hex));
            marker.on('mouseout', () => atlas.hover(null));

            aircraftMarkers.set(aircraftId, marker);
        }

        // Store current state for next update
        aircraftState.set(aircraftId, {
            lat: aircraft.lat,
            lon: aircraft.lon,
            track: aircraft.track || 0,
            timestamp: currentTime
        });
    });
    
    // Remove markers for aircraft that are no longer visible
    aircraftMarkers.forEach((marker, id) => {
        if (!seenAircraftIds.has(id)) {
            // Remove marker from map
            if (map.hasLayer(marker)) {
                map.removeLayer(marker);
            }
            // Clean up
            aircraftMarkers.delete(id);
            aircraftState.delete(id);
        }
    });
    
    atlas.afterMapUpdate();
}

// Update aircraft list
function updateAircraftList(aircraftData) {
    atlas.renderIndex(aircraftData);
}

// Get current time window from selector
function getTimeWindow() {
    // Always return 0 to show all data
    return 0;
}

// Fetch aircraft data from backend (the 2 s poll; skipped while the live
// feed from the gateway is in sync, see "Live feed" below)
async function fetchAircraftData() {
    if (liveFeed.synced) return;
    try {
        const timeWindow = getTimeWindow();
        const response = await fetch(`/api/aircraft?window=${timeWindow}`);

        if (!response.ok) {
            throw new Error('Failed to fetch aircraft data');
        }

        const data = await response.json();
        if (liveFeed.synced) return; // the live feed took over meanwhile

        updateStatus(true);
        renderAircraftData(data);
    } catch (error) {
        console.error('Error fetching aircraft data:', error);
        updateStatus(false, 'Connection Error');
    }
}

// Shows one aircraft table ({now, aircraft: [...]}) on the map and the list.
function renderAircraftData(data) {
        document.getElementById('aircraft-count').textContent = data.aircraft?.length || 0;
        document.getElementById('last-update').textContent = formatTime(data.now || Date.now());

        // Store aircraft data globally for collision detection to use
        currentAircraftData = data.aircraft || [];

        // The timeline records every table it is shown; while it plays back
        // the past, the map shows the recording instead of the live table
        atlas.record(currentAircraftData, data.now || Date.now());

        // Update map and list (always update map, even if empty)
        if (!atlas.isPlayback()) updateAircraftOnMap(currentAircraftData);
        updateAircraftList(currentAircraftData);

        // If global trails are enabled, show trails for new aircraft
        if (showAllTrails) {
            currentAircraftData.forEach(aircraft => {
                if (!aircraftTrails.has(aircraft.hex)) {
                    fetchAndDisplayTrail(aircraft.hex);
                }
            });
        }
}

/**
 * Toggle trail display for a specific aircraft
 * @param {string} hex - Aircraft ICAO hex code
 */
window.toggleAircraftTrail = async function(hex) {
    // Disable global trails mode when manually toggling individual trails
    if (showAllTrails) {
        showAllTrails = false;
        const button = document.getElementById('toggle-trails');
        if (button) button.classList.remove('active');
    }

    if (aircraftTrails.has(hex)) {
        // Remove trail
        const trail = aircraftTrails.get(hex);
        map.removeLayer(trail);
        aircraftTrails.delete(hex);
        trailData.delete(hex);

        // Stop refresh if no more trails
        if (aircraftTrails.size === 0 && trailRefreshInterval) {
            clearInterval(trailRefreshInterval);
            trailRefreshInterval = null;
        }
    } else {
        // Show trail
        await fetchAndDisplayTrail(hex);

        // Start refresh interval if not running
        if (!trailRefreshInterval) {
            trailRefreshInterval = setInterval(() => {
                // Refresh all active trails
                aircraftTrails.forEach((_trail, hexCode) => {
                    fetchAndDisplayTrail(hexCode);
                });
            }, 5000); // Refresh every 5 seconds
        }
    }

    // Update popup to reflect new state
    updateAircraftOnMap(currentAircraftData);
};

/**
 * Fetch and display trail for specific aircraft
 * @param {string} hex - Aircraft ICAO hex code
 */
async function fetchAndDisplayTrail(hex) {
    // Skip if we already know this aircraft has no trail data
    if (failedTrailFetches.has(hex)) {
        return;
    }

    try {
        const response = await fetch(`/api/aircraft/${hex}/trail?minutes=120&limit=500`);
        if (!response.ok) {
            failedTrailFetches.add(hex);
            return;
        }

        const data = await response.json();

        if (!data.trail || data.trail.length === 0) {
            // Mark as failed to avoid repeated requests
            failedTrailFetches.add(hex);
            return;
        }

        // Store trail data
        trailData.set(hex, data.trail);

        // Convert trail to Leaflet coordinates
        const coordinates = data.trail.map(point => [point.lat, point.lon]);

        // Remove existing trail if present
        if (aircraftTrails.has(hex)) {
            map.removeLayer(aircraftTrails.get(hex));
        }

        // Check if this is a helicopter (find aircraft in current data)
        const aircraft = currentAircraftData.find(ac => ac.hex === hex);
        const isHeli = aircraft && isHelicopter(aircraft);

        // Create polyline for trail - ochre for helicopters, ivory for fixed-wing
        const trail = L.polyline(coordinates, {
            pane: 'trailPane',
            color: isHeli ? PALETTE.ochre : PALETTE.ink,
            weight: 1.5,
            opacity: 0.55,
            smoothFactor: 1
        }).addTo(map);

        // Add popup with trail info
        trail.bindPopup(popupHTML(`Trail ${hex}`, [
            ['Aircraft', isHeli ? 'Helicopter' : null],
            ['Points', data.trail.length],
            ['Duration', `${Math.round((data.trail[0].seen - data.trail[data.trail.length - 1].seen) / 60)} min`],
        ]));

        aircraftTrails.set(hex, trail);

    } catch (error) {
        console.error(`Error fetching trail for ${hex}:`, error);
    }
}

/**
 * Clear all trails
 */
function clearAllTrails() {
    aircraftTrails.forEach((trail) => {
        map.removeLayer(trail);
    });
    aircraftTrails.clear();
    trailData.clear();
    failedTrailFetches.clear(); // Reset failed fetches cache

    if (trailRefreshInterval) {
        clearInterval(trailRefreshInterval);
        trailRefreshInterval = null;
    }
}

/**
 * Toggle trails for all aircraft
 */
async function toggleAllTrails() {
    const button = document.getElementById('toggle-trails');

    if (showAllTrails) {
        // Hide all trails
        clearAllTrails();
        showAllTrails = false;
        if (button) button.classList.remove('active');
    } else {
        // Show trails for all current aircraft
        showAllTrails = true;
        if (button) button.classList.add('active');

        // Fetch and display trails for all aircraft
        const promises = currentAircraftData.map(aircraft =>
            fetchAndDisplayTrail(aircraft.hex)
        );
        await Promise.all(promises);

        // Start refresh interval
        if (!trailRefreshInterval) {
            trailRefreshInterval = setInterval(() => {
                if (showAllTrails) {
                    // Refresh trails for all current aircraft
                    currentAircraftData.forEach(aircraft => {
                        fetchAndDisplayTrail(aircraft.hex);
                    });
                }
            }, 5000); // Refresh every 5 seconds
        }
    }
}

// Fetch statistics from backend
async function fetchStatistics() {
    try {
        const timeWindow = getTimeWindow();
        const response = await fetch(`/api/statistics?window=${timeWindow}`);

        if (!response.ok) {
            throw new Error('Failed to fetch statistics');
        }

        const data = await response.json();
        console.log('Statistics data received:', data);
        updateStatisticsDashboard(data);
    } catch (error) {
        console.error('Error fetching statistics:', error);
    }
}

// Update statistics dashboard
function updateStatisticsDashboard(data) {
    atlas.renderAnalysis(data);
}

// APOORV'S CODE

// Kalman filter-based collision detection
let collisionLines = [];
let collisionInterval;

// Fetch and display collisions using Kalman filter
async function fetchCollisions() {
    try {
        const response = await fetch('/api/collisions/kalman');
        const data = await response.json();

        if (data.error) {
            console.error('Collision detection error:', data.error);
            return;
        }

        atlas.onCollisions(data);
        displayCollisions(data.collisions);
    } catch (error) {
        console.error('Error fetching collisions:', error);
    }
}

// Display collision warnings on the map
function displayCollisions(collisions) {
    // Remove old collision lines
    collisionLines.forEach(line => map.removeLayer(line));
    collisionLines = [];

    if (!collisions || collisions.length === 0) {
        return;
    }

    // Draw trajectory lines showing current position → predicted collision position
    collisions.forEach(collision => {
        // Look up actual current aircraft positions from live data
        const aircraft1 = currentAircraftData.find(ac => ac.hex === collision.aircraft1);
        const aircraft2 = currentAircraftData.find(ac => ac.hex === collision.aircraft2);

        // Skip if we can't find current positions
        if (!aircraft1 || !aircraft2) {
            return;
        }

        // Use live current positions
        const currentPos1 = [aircraft1.lat, aircraft1.lon];
        const currentPos2 = [aircraft2.lat, aircraft2.lon];

        // Predicted collision positions for each aircraft
        const futurePos1 = [parseFloat(collision.position1.lat), parseFloat(collision.position1.lon)];
        const futurePos2 = [parseFloat(collision.position2.lat), parseFloat(collision.position2.lon)];

        // Calculate the collision point as the midpoint between the two predicted positions
        const collisionLat = (futurePos1[0] + futurePos2[0]) / 2;
        const collisionLon = (futurePos1[1] + futurePos2[1]) / 2;
        const collisionPointPos = [collisionLat, collisionLon];

        // Color based on probability and time
        let color;
        const probability = parseFloat(collision.probability);
        const time = collision.timeToCollision;

        if (probability > 15 || time < 60) {
            color = PALETTE.alert; // High risk
        } else if (probability > 10 || time < 120) {
            color = PALETTE.caution; // Medium risk
        } else {
            color = PALETTE.ochre; // Low risk
        }

        // Draw trajectory line for aircraft 1: current position → collision point
        const trajectory1 = L.polyline([currentPos1, collisionPointPos], {
            pane: 'livePane',
            color: color,
            weight: 2,
            opacity: 0.6,
            dashArray: '4, 5'
        }).addTo(map);
        collisionLines.push(trajectory1);

        // Draw trajectory line for aircraft 2: current position → collision point
        const trajectory2 = L.polyline([currentPos2, collisionPointPos], {
            pane: 'livePane',
            color: color,
            weight: 2,
            opacity: 0.6,
            dashArray: '5, 5'
        }).addTo(map);
        collisionLines.push(trajectory2);

        // Draw collision point marker at the midpoint (where they would collide)
        const collisionPoint = L.circle(collisionPointPos, {
            pane: 'livePane',
            color: color,
            fillColor: color,
            fillOpacity: 0.18,
            radius: 500, // 500 meters radius
            weight: 1.5
        }).addTo(map);
        collisionLines.push(collisionPoint);

        // Add popup with collision details to the collision point
        const aircraft1Display = collision.flight1 || collision.aircraft1;
        const aircraft2Display = collision.flight2 || collision.aircraft2;

        const popupContent = popupHTML('Predicted Collision', [
            ['Aircraft 1', aircraft1Display],
            ['Aircraft 2', aircraft2Display],
            ['Time to collision', `${collision.timeToCollision}s (${Math.round(collision.timeToCollision/60)}m)`],
            ['Probability', collision.probability],
            ['Separation', `${(collision.distance/1000).toFixed(2)} km`],
            ['Vertical sep', `${collision.verticalSeparation} ft`],
        ], `<div class="map-popup-section">Predicted positions</div>` + [
            ['AC1 alt', `${collision.position1.alt} ft`],
            ['AC2 alt', `${collision.position2.alt} ft`],
        ].map(([l, v]) => `<div class="map-popup-row"><span>${l}</span><span>${v}</span></div>`).join(''), 'alert');

        collisionPoint.bindPopup(popupContent);
    });

    // Update collision count in header
    const collisionCountEl = document.getElementById('collision-count');
    if (collisionCountEl) {
        collisionCountEl.textContent = collisions.length;
        collisionCountEl.classList.toggle('has-alerts', collisions.length > 0);
    }

    console.log(`🚨 ${collisions.length} potential collision(s) detected`);
}

// Prediction wedges for 2D map
let predictionWedges = [];
let showPredictionWedges = false;
let predictionWedgeInterval;

// Toggle prediction wedges visibility
function togglePredictionWedges() {
    showPredictionWedges = !showPredictionWedges;
    
    const button = document.getElementById('toggle-predictions');
    if (button) {
        button.classList.toggle('active', showPredictionWedges);
    }
    
    if (showPredictionWedges) {
        displayPredictionWedges();
    } else {
        clearPredictionWedges();
    }
}

// Clear all prediction wedges
function clearPredictionWedges() {
    predictionWedges.forEach(wedge => map.removeLayer(wedge));
    predictionWedges = [];
}

// Display prediction wedges for all aircraft
function displayPredictionWedges() {
    clearPredictionWedges();
    
    if (!showPredictionWedges || !currentAircraftData || currentAircraftData.length === 0) {
        return;
    }
    
    const predictionTime = 300; // 5 minutes prediction
    const spreadAngle = 12; // degrees spread on each side
    
    currentAircraftData.forEach(ac => {
        if (!ac.lat || !ac.lon || !ac.speed || !ac.track) return;
        
        const speed = (ac.speed || 250) * 0.514444; // knots to m/s
        const distance = speed * predictionTime;
        
        // Skip stationary aircraft
        if (distance < 1000) return;
        
        const trackRad = (ac.track * Math.PI) / 180;
        const spreadRad = (spreadAngle * Math.PI) / 180;
        
        // Calculate future position (center of wedge)
        const metersPerDegreeLat = 111320;
        const metersPerDegreeLon = 111320 * Math.cos(ac.lat * Math.PI / 180);
        
        const deltaLat = (distance * Math.cos(trackRad)) / metersPerDegreeLat;
        const deltaLon = (distance * Math.sin(trackRad)) / metersPerDegreeLon;
        
        const futureLat = ac.lat + deltaLat;
        const futureLon = ac.lon + deltaLon;
        
        // Calculate wedge edges (left and right bounds)
        const leftTrackRad = trackRad - spreadRad;
        const rightTrackRad = trackRad + spreadRad;
        
        const leftDeltaLat = (distance * Math.cos(leftTrackRad)) / metersPerDegreeLat;
        const leftDeltaLon = (distance * Math.sin(leftTrackRad)) / metersPerDegreeLon;
        const rightDeltaLat = (distance * Math.cos(rightTrackRad)) / metersPerDegreeLat;
        const rightDeltaLon = (distance * Math.sin(rightTrackRad)) / metersPerDegreeLon;
        
        const leftLat = ac.lat + leftDeltaLat;
        const leftLon = ac.lon + leftDeltaLon;
        const rightLat = ac.lat + rightDeltaLat;
        const rightLon = ac.lon + rightDeltaLon;
        
        // Colour of the aircraft's altitude band, as on its marker
        const color = PALETTE.alt[altitudeBand(ac)];
        
        // Create wedge polygon (triangle from current position to future bounds)
        const wedgeCoords = [
            [ac.lat, ac.lon],      // Current position (apex)
            [leftLat, leftLon],    // Left edge of future position
            [futureLat, futureLon], // Center of future position
            [rightLat, rightLon],  // Right edge of future position
            [ac.lat, ac.lon]       // Back to apex
        ];
        
        const wedge = L.polygon(wedgeCoords, {
            pane: 'livePane',
            color: color,
            weight: 1,
            opacity: 0.45,
            fillColor: color,
            fillOpacity: 0.05
        }).addTo(map);
        
        predictionWedges.push(wedge);
        
        // Add center trajectory line
        const trajectoryLine = L.polyline([[ac.lat, ac.lon], [futureLat, futureLon]], {
            pane: 'livePane',
            color: color,
            weight: 1.2,
            opacity: 0.65,
            dashArray: '3, 5'
        }).addTo(map);
        
        predictionWedges.push(trajectoryLine);
    });
    
    console.log(`📊 Displayed ${predictionWedges.length / 2} prediction wedges`);
}

// Start polling for aircraft data and statistics
function startPolling() {
    fetchAircraftData(); // Initial fetch
    fetchStatistics(); // Initial statistics fetch
    fetchCollisions(); // Initial collision check

    updateInterval = setInterval(fetchAircraftData, 2000); // Update every 2 seconds
    statisticsInterval = setInterval(fetchStatistics, 5000); // Update statistics every 5 seconds
    collisionInterval = setInterval(fetchCollisions, 3000); // Check collisions every 3 seconds
    
    // Update prediction wedges when enabled
    predictionWedgeInterval = setInterval(() => {
        if (showPredictionWedges) {
            displayPredictionWedges();
        }
    }, 3000); // Update wedges every 3 seconds
}

// Stop polling
function stopPolling() {
    if (updateInterval) {
        clearInterval(updateInterval);
    }
    if (statisticsInterval) {
        clearInterval(statisticsInterval);
    }
    if (collisionInterval) {
        clearInterval(collisionInterval);
    }
    if (trailRefreshInterval) {
        clearInterval(trailRefreshInterval);
    }
}

// Toggle aircraft list panel
function setupControls() {
    const toggleButton = document.getElementById('toggle-list');
    const closeButton = document.getElementById('close-list');
    const listPanel = document.getElementById('aircraft-list-panel');

    if (toggleButton) {
        toggleButton.addEventListener('click', () => {
            listPanel.classList.toggle('open');
            toggleButton.classList.toggle('active', listPanel.classList.contains('open'));
        });
    }

    if (closeButton) {
        closeButton.addEventListener('click', () => {
            listPanel.classList.remove('open');
            toggleButton?.classList.remove('active');
        });
    }

    // Statistics panel controls
    const toggleStatsButton = document.getElementById('toggle-statistics');
    const closeStatsButton = document.getElementById('close-statistics');
    const statsPanel = document.getElementById('statistics-panel');

    // Function to update aircraft list panel width based on stats panel state
    function updateAircraftListWidth() {
        if (statsPanel.classList.contains('open')) {
            listPanel.style.right = '380px';
            listPanel.style.width = 'calc(100% - 240px - 380px)';
        } else {
            listPanel.style.right = '0';
            listPanel.style.width = 'calc(100% - 240px)';
        }
    }

    if (toggleStatsButton) {
        toggleStatsButton.addEventListener('click', () => {
            statsPanel.classList.toggle('open');
            toggleStatsButton.classList.toggle('active', statsPanel.classList.contains('open'));
            updateAircraftListWidth();
        });
    }

    if (closeStatsButton) {
        closeStatsButton.addEventListener('click', () => {
            statsPanel.classList.remove('open');
            toggleStatsButton?.classList.remove('active');
            updateAircraftListWidth();
        });
    }

    // Airports toggle control
    const toggleAirportsButton = document.getElementById('toggle-airports');
    if (toggleAirportsButton) {
        toggleAirportsButton.addEventListener('click', () => {
            toggleAirports();
            toggleAirportsButton.classList.toggle('active', showAirports);
        });
    }

    // Trails toggle control
    const toggleTrailsButton = document.getElementById('toggle-trails');
    if (toggleTrailsButton) {
        toggleTrailsButton.addEventListener('click', () => {
            toggleAllTrails();
        });
    }
}

// ---------------------------------------------------------------------------
// Live feed
//
// Served by the WebSocket gateway (gateway/), the page keeps a live copy of
// the track table from /gateway/ws and redraws within `render_ms` (default
// 100 ms) of each update, instead of polling /api/aircraft every 2 s. It:
//   - acknowledges frames (flow control: window 8, an ack every 4 frames), so
//     a page that falls behind gets conflated frames instead of a backlog;
//   - reconnects with backoff, and the 2 s poll covers the time out of sync;
//   - shows markers at their latest position (?animate=1 keeps the easing).
// Served by the Fall 2025 server.js there is no gateway: the protocol module
// fails to load and the page keeps polling, as before.
// window.__gateway is the same object, for tests and latency measurement.
// ---------------------------------------------------------------------------
const liveFeed = {
    connected: false,
    synced: false, // first full table received
    animate: new URLSearchParams(location.search).get('animate') === '1',
    renderMs: Math.max(0, Number(new URLSearchParams(location.search).get('render_ms') ?? 100)),
    frames: 0,
    renders: 0,
    reconnects: 0,
    bytes: 0, // binary WebSocket payload received
    updates: 0, // aircraft updates in delta frames
    updatesBy: new Map(), // icao -> updates since it was first seen (for the analysis)
    renderTimes: new Float64Array(240), // ms per redraw, ring
    renderIdx: 0,
    connectedAt: 0,
    asOfSeq: 0,
    flags: 0,
    lastFrameAt: 0,
    lastRenderAt: 0,
    seen: new Map(), // icao -> wall ms of the last update
    table: null,
    status: null,
    backoff: 250,
};
window.__gateway = liveFeed;
let liveProto = null;
let liveRenderTimer = null;
let liveRenderScheduled = false;

function liveAircraftData() {
    const aircraft = [];
    for (const [icao, u] of liveFeed.table.tracks) {
        const seen = liveFeed.seen.get(icao);
        const a = liveProto.toAircraft(u, seen ? Math.floor(seen / 1000) : null);
        if (a.lat === null || a.lon === null) continue;
        aircraft.push(a);
    }
    return { now: Date.now(), aircraft, timeWindow: 0, source: 'gateway-ws', as_of_seq: liveFeed.asOfSeq };
}

function updateLiveStatus() {
    if (!liveFeed.connected) updateStatus(false, 'Gateway disconnected');
    else if (!liveFeed.synced) updateStatus(false, 'Syncing...');
    else if (liveFeed.flags & liveProto.FrameFlags.kStale) updateStatus(false, 'Recovering feed...');
    else if (liveFeed.flags & liveProto.FrameFlags.kEnded) updateStatus(false, 'Feed ended');
    else updateStatus(true, 'Live (WebSocket)');
}

function renderLive() {
    liveRenderScheduled = false;
    liveFeed.lastRenderAt = performance.now();
    liveFeed.renders++;
    if (liveFeed.synced) {
        try {
            const t0 = performance.now();
            renderAircraftData(liveAircraftData());
            liveFeed.renderTimes[liveFeed.renderIdx++ % liveFeed.renderTimes.length] = performance.now() - t0;
        } catch (error) {
            console.error('Error drawing the live feed:', error);
        }
    }
    updateLiveStatus();
}

function scheduleLiveRender() {
    if (liveRenderScheduled) return;
    liveRenderScheduled = true;
    const wait = Math.max(0, liveFeed.lastRenderAt + liveFeed.renderMs - performance.now());
    clearTimeout(liveRenderTimer);
    liveRenderTimer = setTimeout(() => requestAnimationFrame(renderLive), wait);
}

function connectLiveFeed() {
    const ws = new WebSocket(`${location.protocol === 'https:' ? 'wss' : 'ws'}://${location.host}/gateway/ws`);
    ws.binaryType = 'arraybuffer';
    let processed = 0;
    let acked = 0;
    const ack = new Uint8Array(5);
    ack[0] = 0x41; // 'A'
    const ackView = new DataView(ack.buffer);
    ws.onopen = () => {
        ws.send(JSON.stringify({ type: 'hello', window: 8 }));
        liveFeed.connected = true;
        liveFeed.connectedAt = Date.now();
        liveFeed.backoff = 250;
        updateLiveStatus();
    };
    ws.onmessage = (ev) => {
        if (typeof ev.data === 'string') {
            try {
                liveFeed.status = JSON.parse(ev.data);
            } catch {
                // ignore
            }
            return;
        }
        // Count every binary frame, even one that fails to decode, or the
        // gateway would wait for its ack forever. An ack every 4 frames keeps
        // a window of 8 from running dry.
        processed++;
        liveFeed.bytes += ev.data.byteLength;
        if (processed - acked >= 4) {
            acked = processed;
            ackView.setUint32(1, acked, true);
            ws.send(ack);
        }
        let frame;
        try {
            frame = liveProto.decodeFrame(ev.data);
        } catch (error) {
            console.error('gateway frame', error);
            return;
        }
        const now = Date.now();
        const touched = liveFeed.table.apply(frame);
        if (touched === null) { // a full table
            liveFeed.seen.clear();
            for (const icao of liveFeed.table.tracks.keys()) liveFeed.seen.set(icao, now);
            liveFeed.synced = true;
        } else {
            liveFeed.updates += touched.length;
            for (const icao of touched) {
                if (liveFeed.table.tracks.has(icao)) {
                    liveFeed.seen.set(icao, now);
                    liveFeed.updatesBy.set(icao, (liveFeed.updatesBy.get(icao) ?? 0) + 1);
                } else {
                    liveFeed.seen.delete(icao);
                    liveFeed.updatesBy.delete(icao);
                }
            }
        }
        liveFeed.frames++;
        liveFeed.asOfSeq = frame.asOfSeq;
        liveFeed.flags = frame.flags;
        liveFeed.lastFrameAt = performance.now();
        scheduleLiveRender();
    };
    ws.onclose = () => {
        liveFeed.connected = false;
        liveFeed.synced = false; // the 2 s poll takes over until the next full table
        updateLiveStatus();
        liveFeed.reconnects++;
        const backoff = Math.min(5000, liveFeed.backoff);
        liveFeed.backoff = backoff * 2;
        setTimeout(connectLiveFeed, backoff);
    };
}

function startLiveFeed() {
    import('/gateway/gateway_protocol.mjs').then((m) => {
        liveProto = m;
        liveFeed.table = new m.TrackTable();
        connectLiveFeed();
    }, () => {
        // No gateway (the Fall 2025 server): keep polling.
    });
}

// Initialize when page loads
// Load aircraft types for helicopter detection
document.addEventListener('DOMContentLoaded', () => {
    initializeMap();
    atlas.init(map);
    updateStatus(false, 'Connecting...');
    setupControls();
    startPolling();
    startLiveFeed();
});

// Cleanup on page unload
window.addEventListener('beforeunload', stopPolling);
