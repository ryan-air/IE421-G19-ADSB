// Store aircraft data and markers
let map;
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
let showTrails = false;
let trailDuration = 120; // minutes (2 hours to match cleanup interval)
let selectedAircraftForTrail = null;
// APOORV'S CODE
const collisionLines = new Map(); // Store collision lines on map
let collisionInterval;

// Initialize map
function initializeMap() {
    // Initialize the map centered on a default location (you can change this)
    map = L.map('map', {
        zoomControl: true,
        attributionControl: true
    }).setView([40.11, -88.24], 10); // Default: Champaign, IL

    // Add dark CartoDB tiles for ATC/radar look
    L.tileLayer('https://{s}.basemaps.cartocdn.com/dark_all/{z}/{x}/{y}{r}.png', {
        attribution: '© OpenStreetMap contributors © CARTO',
        maxZoom: 19,
        subdomains: 'abcd'
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

            // Different colors by airport type
            let color = '#e8a628'; // Default: orange for small/medium
            if (props.type === 'large_airport') {
                color = '#ff5533'; // Red for large airports
            }

            return L.circleMarker(latlng, {
                radius: props.type === 'large_airport' ? 5 : 3,
                fillColor: color,
                color: '#000',
                weight: 1,
                opacity: 0.8,
                fillOpacity: 0.6
            });
        },
        onEachFeature: (feature, layer) => {
            const props = feature.properties;

            // Create popup with airport info
            const popupContent = `
                <div style="font-family: 'Courier New', monospace; color: #e8a628;">
                    <strong style="color: #ffa628;">${props.name}</strong><br>
                    ${props.icao ? `ICAO: ${props.icao}` : ''}${props.iata ? ` (${props.iata})` : ''}<br>
                    ${props.municipality ? `Location: ${props.municipality}` : ''}<br>
                    ${props.elevation ? `Elevation: ${props.elevation} ft` : ''}<br>
                    <em style="color: #888;">${props.type.replace('_', ' ')}</em>
                </div>
            `;
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
function createAircraftIcon(aircraft) {
    const heading = aircraft.track || 0;
    const identifier = aircraft.flight || aircraft.hex || 'N/A';
    
    const planeSVG = `
        <div class="aircraft-marker-container">
            <div class="aircraft-label">${identifier}</div>
            <svg width="24" height="24" viewBox="0 0 24 24" class="aircraft-icon" style="transform: rotate(${heading}deg);">
                <path d="M12 2L2 7L12 12L22 7L12 2Z" fill="#7a9a7a" stroke="#5a8a5a" stroke-width="0.5"/>
                <path d="M2 17L12 22L22 17" stroke="#7a9a7a" stroke-width="1" fill="none"/>
                <circle cx="12" cy="12" r="2" fill="#6b8e6b"/>
            </svg>
        </div>
    `;
    
    return L.divIcon({
        className: 'aircraft-marker',
        html: planeSVG,
        iconSize: [60, 40], // Wider to accommodate label
        iconAnchor: [30, 35] // Anchor at bottom center (below label)
    });
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

// Update aircraft on map with smooth movement
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
            
            // Update label and icon rotation if needed
            if (marker._icon) {
                const labelElement = marker._icon.querySelector('.aircraft-label');
                const newIdentifier = aircraft.flight || aircraft.hex || 'N/A';
                if (labelElement && marker._identifier !== newIdentifier) {
                    labelElement.textContent = newIdentifier;
                    marker._identifier = newIdentifier;
                }
                // Update icon rotation for small movements
                const iconElement = marker._icon.querySelector('.aircraft-icon');
                if (iconElement && aircraft.track !== undefined) {
                    iconElement.style.transform = `rotate(${aircraft.track}deg)`;
                    marker._heading = aircraft.track;
                }
            }
            
            // Update popup content
            const popupContent = `
                <div style="font-family: 'Courier New', monospace; color: #7a9a7a;">
                    <strong style="color: #8a9a8a;">${aircraft.flight || 'N/A'}</strong><br>
                    ${aircraft.hex ? `ICAO: ${aircraft.hex}` : ''}<br>
                    ${aircraft.altitude ? `ALT: ${aircraft.altitude} ft` : ''}<br>
                    ${aircraft.speed ? `SPD: ${aircraft.speed} kt` : ''}<br>
                    ${aircraft.track ? `HDG: ${aircraft.track}°` : ''}<br>
                </div>
            `;
            marker.setPopupContent(popupContent);
        } else {
            // Create new marker
            const marker = L.marker([aircraft.lat, aircraft.lon], {
                icon: createAircraftIcon(aircraft),
                zIndexOffset: 1000 // Ensure aircraft are above map tiles
            }).addTo(map);
            
            marker._heading = aircraft.track || 0;
            marker._identifier = aircraft.flight || aircraft.hex || 'N/A';

            // Add popup with aircraft info
            const popupContent = `
                <div style="font-family: 'Courier New', monospace; color: #7a9a7a;">
                    <strong style="color: #8a9a8a;">${aircraft.flight || 'N/A'}</strong><br>
                    ${aircraft.hex ? `ICAO: ${aircraft.hex}` : ''}<br>
                    ${aircraft.altitude ? `ALT: ${aircraft.altitude} ft` : ''}<br>
                    ${aircraft.speed ? `SPD: ${aircraft.speed} kt` : ''}<br>
                    ${aircraft.track ? `HDG: ${aircraft.track}°` : ''}<br>
                </div>
            `;
            marker.bindPopup(popupContent);

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
    
    // Debug log (can be removed later)
    console.log(`Map updated: ${seenAircraftIds.size} aircraft visible, ${aircraftMarkers.size} markers on map`);
}

// Update aircraft list
function updateAircraftList(aircraftData) {
    const listContainer = document.getElementById('aircraft-list');
    
    if (aircraftData.length === 0) {
        listContainer.innerHTML = '<div class="empty-state"><p>No aircraft detected</p></div>';
        return;
    }

    listContainer.innerHTML = aircraftData.map(aircraft => {
        return `
            <div class="aircraft-card">
                <h3>${aircraft.flight || aircraft.hex || 'Unknown'}</h3>
                <div class="aircraft-info">
                    ${aircraft.hex ? `<span><strong>ICAO:</strong> ${aircraft.hex}</span>` : ''}
                    ${aircraft.altitude ? `<span><strong>Altitude:</strong> ${aircraft.altitude} ft</span>` : ''}
                    ${aircraft.speed ? `<span><strong>Speed:</strong> ${aircraft.speed} kt</span>` : ''}
                    ${aircraft.track ? `<span><strong>Heading:</strong> ${aircraft.track}°</span>` : ''}
                    ${aircraft.lat && aircraft.lon ? `<span><strong>Position:</strong> ${aircraft.lat.toFixed(4)}, ${aircraft.lon.toFixed(4)}</span>` : ''}
                </div>
            </div>
        `;
    }).join('');
}

// Get current time window from selector
function getTimeWindow() {
    // Always return 0 to show all data
    return 0;
}

// Fetch aircraft data from backend
async function fetchAircraftData() {
    try {
        const timeWindow = getTimeWindow();
        const response = await fetch(`/api/aircraft?window=${timeWindow}`);

        if (!response.ok) {
            throw new Error('Failed to fetch aircraft data');
        }

        const data = await response.json();

        updateStatus(true);
        document.getElementById('aircraft-count').textContent = data.aircraft?.length || 0;
        document.getElementById('last-update').textContent = formatTime(data.now || Date.now());

        // Update map and list (always update map, even if empty)
        updateAircraftOnMap(data.aircraft || []);
        updateAircraftList(data.aircraft || []);
    } catch (error) {
        console.error('Error fetching aircraft data:', error);
        updateStatus(false, 'Connection Error');
    }
}

// Trail functionality removed

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
    const dashboardElement = document.getElementById('statistics-content');
    if (!dashboardElement) {
        console.error('Statistics dashboard element not found');
        return;
    }

    console.log('Updating statistics dashboard with:', data);
    const { summary, altitude_distribution, flight_durations } = data;

    // Build altitude distribution bars
    const altitudeHTML = altitude_distribution.map(item => {
        const maxCount = 10; // Assume max 10 aircraft for bar width
        const barWidth = Math.min((item.count / maxCount) * 100, 100);
        return `
            <div class="stat-bar-container">
                <span class="stat-label">${item.range} ft</span>
                <div class="stat-bar-bg">
                    <div class="stat-bar" style="width: ${barWidth}%"></div>
                </div>
                <span class="stat-value">${item.count}</span>
            </div>
        `;
    }).join('');

    // Build flight durations list
    const durationsHTML = flight_durations.map(item => {
        return `
            <div class="duration-item">
                <span class="duration-flight">${item.flight || item.hex}</span>
                <span class="duration-time">${formatDuration(item.duration_seconds)}</span>
            </div>
        `;
    }).join('') || '<div class="empty-state"><p>No active flights</p></div>';

    dashboardElement.innerHTML = `
        <div class="stats-summary">
            <div class="stat-card">
                <div class="stat-card-label">Total Flights</div>
                <div class="stat-card-value">${summary.total_flights}</div>
            </div>
            <div class="stat-card">
                <div class="stat-card-label">Avg Speed</div>
                <div class="stat-card-value">${summary.avg_speed.toFixed(0)} kt</div>
            </div>
            <div class="stat-card">
                <div class="stat-card-label">Avg Altitude</div>
                <div class="stat-card-value">${(summary.avg_altitude / 1000).toFixed(1)}k ft</div>
            </div>
        </div>

        <div class="stats-section">
            <h3>Altitude Distribution</h3>
            ${altitudeHTML || '<div class="empty-state"><p>No data</p></div>'}
        </div>

        <div class="stats-section">
            <h3>Flight Durations</h3>
            <div class="durations-list">
                ${durationsHTML}
            </div>
        </div>
    `;
}

// APOORV'S CODE

// Fetch and display collision alerts
async function fetchCollisions() {
    try {
        const response = await fetch('/api/collisions');
        if (!response.ok) return;
        
        const data = await response.json();
        displayCollisions(data.alerts || []);
        document.getElementById('collision-count').textContent = data.alerts?.length || 0;
    } catch (error) {
        console.error('Error fetching collisions:', error);
    }
}

// Display collision lines on map
function displayCollisions(alerts) {
    if (!map) return;
    
    // Remove old collision lines
    collisionLines.forEach(line => {
        if (map.hasLayer(line)) {
            map.removeLayer(line);
        }
    });
    collisionLines.clear();
    
    // Draw new collision lines
    alerts.forEach(alert => {
        const color = alert.severity === 'critical' ? '#ff0000' : 
                     alert.severity === 'caution' ? '#ff8800' : '#ffaa00';
        
        const line = L.polyline(
            [[alert.lat1, alert.lon1], [alert.lat2, alert.lon2]],
            {
                color: color,
                weight: 3,
                opacity: 0.8,
                dashArray: '10, 5'
            }
        ).addTo(map);
        
        // Add popup with alert info
        line.bindPopup(`
            <div style="font-family: 'Courier New', monospace;">
                <strong style="color: ${color};">⚠️ COLLISION ALERT</strong><br>
                ${alert.flight1 || alert.hex1} ↔ ${alert.flight2 || alert.hex2}<br>
                Horizontal: ${alert.horizontal_distance_nm.toFixed(2)} nm<br>
                Vertical: ${alert.vertical_distance_ft} ft<br>
                Severity: ${alert.severity.toUpperCase()}
            </div>
        `);
        
        collisionLines.set(`${alert.hex1}-${alert.hex2}`, line);
        
        // Highlight aircraft markers
        const marker1 = aircraftMarkers.get(alert.hex1);
        const marker2 = aircraftMarkers.get(alert.hex2);
        if (marker1 && marker1._icon) {
            marker1._icon.style.filter = `drop-shadow(0 0 8px ${color})`;
        }
        if (marker2 && marker2._icon) {
            marker2._icon.style.filter = `drop-shadow(0 0 8px ${color})`;
        }
    });
}

// Start polling for aircraft data and statistics
function startPolling() {
    fetchAircraftData(); // Initial fetch
    fetchStatistics(); // Initial statistics fetch
    fetchCollisions(); // Initial collision fetch

    updateInterval = setInterval(fetchAircraftData, 2000); // Update every 2 seconds
    statisticsInterval = setInterval(fetchStatistics, 5000); // Update statistics every 5 seconds
    // trailRefreshInterval = setInterval(refreshActiveTrails, 10000); // Refresh trails every 10 seconds (function not implemented)
    collisionInterval = setInterval(fetchCollisions, 3000); // Check collisions every 3 seconds
}

// Stop polling
function stopPolling() {
    if (updateInterval) {
        clearInterval(updateInterval);
    }
    if (statisticsInterval) {
        clearInterval(statisticsInterval);
    }
    if (trailRefreshInterval) {
        clearInterval(trailRefreshInterval);
    }
    if (collisionInterval) {  // Add this
        clearInterval(collisionInterval);
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
        });
    }

    if (closeButton) {
        closeButton.addEventListener('click', () => {
            listPanel.classList.remove('open');
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
            updateAircraftListWidth();
        });
    }

    if (closeStatsButton) {
        closeStatsButton.addEventListener('click', () => {
            statsPanel.classList.remove('open');
            updateAircraftListWidth();
        });
    }

    // Airports toggle control
    const toggleAirportsButton = document.getElementById('toggle-airports');
    if (toggleAirportsButton) {
        toggleAirportsButton.addEventListener('click', () => {
            toggleAirports();
        });
    }
}

// Initialize when page loads
document.addEventListener('DOMContentLoaded', () => {
    initializeMap();
    updateStatus(false, 'Connecting...');
    setupControls();
    startPolling();
});

// Cleanup on page unload
window.addEventListener('beforeunload', stopPolling);
