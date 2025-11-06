// Initialize the map centered on a default location (you can change this)
const map = L.map('map').setView([40.11, -88.24], 10); // Default: Champaign, IL

// Add dark CartoDB tiles for ATC/radar look
L.tileLayer('https://{s}.basemaps.cartocdn.com/dark_all/{z}/{x}/{y}{r}.png', {
    attribution: '© OpenStreetMap contributors © CARTO',
    maxZoom: 19,
    subdomains: 'abcd'
}).addTo(map);

// Store aircraft data and markers
const aircraftMarkers = new Map();
const aircraftState = new Map(); // Store previous state for interpolation
let updateInterval;
const INTERPOLATION_DURATION = 2000; // Match polling interval for smooth movement

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

// Create aircraft marker icon with plane SVG
function createAircraftIcon(aircraft) {
    const heading = aircraft.track || 0;
    const planeSVG = `
        <svg width="24" height="24" viewBox="0 0 24 24" class="aircraft-icon" style="transform: rotate(${heading}deg);">
            <path d="M12 2L2 7L12 12L22 7L12 2Z" fill="#7a9a7a" stroke="#5a8a5a" stroke-width="0.5"/>
            <path d="M2 17L12 22L22 17" stroke="#7a9a7a" stroke-width="1" fill="none"/>
            <circle cx="12" cy="12" r="2" fill="#6b8e6b"/>
        </svg>
    `;
    
    return L.divIcon({
        className: 'aircraft-marker',
        html: planeSVG,
        iconSize: [24, 24],
        iconAnchor: [12, 12]
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
    const currentTime = Date.now();
    const seenAircraftIds = new Set();
    
    aircraftData.forEach(aircraft => {
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
            map.removeLayer(marker);
            aircraftMarkers.delete(id);
            aircraftState.delete(id);
        }
    });
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

// Fetch aircraft data from backend
async function fetchAircraftData() {
    try {
        const response = await fetch('http://localhost:3000/api/aircraft?window=0');
        
        if (!response.ok) {
            throw new Error('Failed to fetch aircraft data');
        }

        const data = await response.json();
        
        updateStatus(true);
        document.getElementById('aircraft-count').textContent = data.aircraft?.length || 0;
        document.getElementById('last-update').textContent = formatTime(data.now || Date.now());

        // Update map and list
        if (data.aircraft && data.aircraft.length > 0) {
            updateAircraftOnMap(data.aircraft);
            updateAircraftList(data.aircraft);
        } else {
            updateAircraftList([]);
        }
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

// Initialize when page loads
document.addEventListener('DOMContentLoaded', () => {
    updateStatus(false, 'Connecting...');
    startPolling();
});

// Cleanup on page unload
window.addEventListener('beforeunload', stopPolling);
