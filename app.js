// Store aircraft data and markers
let map;
const aircraftMarkers = new Map();
const aircraftState = new Map(); // Store previous state for interpolation
let updateInterval;
const INTERPOLATION_DURATION = 2000; // Match polling interval for smooth movement

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
                    ${aircraft.track ? `HDG: ${aircraft.track}°` : ''}
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
                    ${aircraft.track ? `HDG: ${aircraft.track}°` : ''}
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
    const selector = document.getElementById('time-window');
    return selector ? parseInt(selector.value) : 30;
}

// Fetch aircraft data from backend
async function fetchAircraftData() {
    try {
        const timeWindow = getTimeWindow();
        const response = await fetch(`http://localhost:3000/api/aircraft?window=${timeWindow}`);
        
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

// Start polling for aircraft data
function startPolling() {
    fetchAircraftData(); // Initial fetch
    updateInterval = setInterval(fetchAircraftData, 2000); // Update every 2 seconds
}

// Stop polling
function stopPolling() {
    if (updateInterval) {
        clearInterval(updateInterval);
    }
}

// Toggle aircraft list panel
function setupControls() {
    const toggleButton = document.getElementById('toggle-list');
    const closeButton = document.getElementById('close-list');
    const listPanel = document.getElementById('aircraft-list-panel');
    const timeWindowSelector = document.getElementById('time-window');
    
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
    
    // Update data when time window changes
    if (timeWindowSelector) {
        timeWindowSelector.addEventListener('change', () => {
            fetchAircraftData(); // Immediately fetch with new window
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
