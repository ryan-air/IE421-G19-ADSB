// Store aircraft data and markers
let map;
const aircraftMarkers = new Map();
const aircraftState = new Map(); // Store previous state for interpolation
const aircraftTrails = new Map(); // Store trail polylines for each aircraft
let updateInterval;
let statisticsInterval;
let trailRefreshInterval;
const INTERPOLATION_DURATION = 2000; // Match polling interval for smooth movement

// Trail settings
let showTrails = false;
let trailDuration = 30; // minutes
let selectedAircraftForTrail = null;

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

// Fetch aircraft trail from API
async function fetchAircraftTrail(hex) {
    try {
        const response = await fetch(`http://localhost:3000/api/aircraft/${hex}/trail?minutes=${trailDuration}&limit=200`);

        if (!response.ok) {
            throw new Error('Failed to fetch trail data');
        }

        const data = await response.json();
        return data.trail || [];
    } catch (error) {
        console.error(`Error fetching trail for ${hex}:`, error);
        return [];
    }
}

// Draw trail on map with gradient effect
async function drawAircraftTrail(hex) {
    if (!map) return;

    // Remove existing trail if any
    removeAircraftTrail(hex);

    // Fetch trail data
    const trail = await fetchAircraftTrail(hex);

    if (trail.length === 0) {
        console.log(`No trail data available for ${hex}`);
        return;
    }

    let polyline = null;
    const circles = [];

    // Only draw polyline if we have at least 2 points
    if (trail.length >= 2) {
        // Convert trail to latlng array
        const latlngs = trail.map(point => [point.lat, point.lon]);

        // Create polyline with gradient effect (older = more transparent)
        polyline = L.polyline(latlngs, {
            color: '#7a9a7a',
            weight: 2,
            opacity: 0.7,
            smoothFactor: 1
        }).addTo(map);
    }

    // Add small circles at each position for better visibility
    trail.forEach((point, index) => {
        const opacity = trail.length === 1 ? 0.8 : 0.3 + (index / trail.length) * 0.5;
        const circle = L.circleMarker([point.lat, point.lon], {
            radius: trail.length === 1 ? 3 : 2,
            fillColor: '#7a9a7a',
            fillOpacity: opacity,
            color: '#5a8a5a',
            weight: 1,
            opacity: opacity
        }).addTo(map);
        circles.push(circle);
    });

    // Store trail elements
    aircraftTrails.set(hex, { polyline, circles });

    console.log(`Drew trail for ${hex}: ${trail.length} points`);
}

// Remove aircraft trail from map
function removeAircraftTrail(hex) {
    if (aircraftTrails.has(hex)) {
        const { polyline, circles } = aircraftTrails.get(hex);

        // Remove polyline (if it exists)
        if (polyline && map.hasLayer(polyline)) {
            map.removeLayer(polyline);
        }

        // Remove circles
        circles.forEach(circle => {
            if (map.hasLayer(circle)) {
                map.removeLayer(circle);
            }
        });

        aircraftTrails.delete(hex);
    }
}

// Toggle trail for specific aircraft
async function toggleAircraftTrail(hex) {
    if (aircraftTrails.has(hex)) {
        removeAircraftTrail(hex);
        selectedAircraftForTrail = null;
    } else {
        // Remove any other selected trail first (only show one at a time when not in "show all" mode)
        if (selectedAircraftForTrail && !showTrails) {
            removeAircraftTrail(selectedAircraftForTrail);
        }
        await drawAircraftTrail(hex);
        if (!showTrails) {
            selectedAircraftForTrail = hex;
        }
    }

    // Update the popup for this aircraft to reflect trail state
    updateAircraftPopup(hex);
}

// Update popup content for a specific aircraft
function updateAircraftPopup(hex) {
    const marker = aircraftMarkers.get(hex);
    if (!marker) return;

    // Get aircraft data from current markers
    const hasTrail = aircraftTrails.has(hex);
    const popup = marker.getPopup();
    if (popup) {
        const content = popup.getContent();
        // Update button text in popup
        const updatedContent = content.replace(
            /(Show|Hide) Trail/,
            hasTrail ? 'Hide Trail' : 'Show Trail'
        );
        popup.setContent(updatedContent);
    }
}

// Toggle all trails
async function toggleAllTrails() {
    showTrails = !showTrails;

    if (showTrails) {
        // Draw trails for all aircraft
        const hexCodes = Array.from(aircraftMarkers.keys());
        for (const hex of hexCodes) {
            await drawAircraftTrail(hex);
        }
    } else {
        // Remove all trails
        aircraftTrails.forEach((_, hex) => {
            removeAircraftTrail(hex);
        });
        selectedAircraftForTrail = null;
    }

    // Update button state
    updateTrailButtonState();

    // Update all popups to reflect new trail state
    aircraftMarkers.forEach((_, hex) => {
        updateAircraftPopup(hex);
    });
}

// Update trail button visual state
function updateTrailButtonState() {
    const toggleButton = document.getElementById('toggle-trails');
    if (toggleButton) {
        toggleButton.textContent = showTrails ? 'Hide All Trails' : 'Show All Trails';
        toggleButton.classList.toggle('active', showTrails);
    }
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
            
            // Update popup content with trail toggle button
            const hasTrail = aircraftTrails.has(aircraftId);
            const popupContent = `
                <div style="font-family: 'Courier New', monospace; color: #7a9a7a;">
                    <strong style="color: #8a9a8a;">${aircraft.flight || 'N/A'}</strong><br>
                    ${aircraft.hex ? `ICAO: ${aircraft.hex}` : ''}<br>
                    ${aircraft.altitude ? `ALT: ${aircraft.altitude} ft` : ''}<br>
                    ${aircraft.speed ? `SPD: ${aircraft.speed} kt` : ''}<br>
                    ${aircraft.track ? `HDG: ${aircraft.track}°` : ''}<br>
                    <button onclick="toggleAircraftTrail('${aircraft.hex}')" style="margin-top: 8px; padding: 4px 8px; background: #5a8a5a; color: #1a1a1a; border: none; cursor: pointer; font-family: 'Courier New', monospace;">
                        ${hasTrail ? 'Hide' : 'Show'} Trail
                    </button>
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

            // Add popup with aircraft info and trail toggle button
            const popupContent = `
                <div style="font-family: 'Courier New', monospace; color: #7a9a7a;">
                    <strong style="color: #8a9a8a;">${aircraft.flight || 'N/A'}</strong><br>
                    ${aircraft.hex ? `ICAO: ${aircraft.hex}` : ''}<br>
                    ${aircraft.altitude ? `ALT: ${aircraft.altitude} ft` : ''}<br>
                    ${aircraft.speed ? `SPD: ${aircraft.speed} kt` : ''}<br>
                    ${aircraft.track ? `HDG: ${aircraft.track}°` : ''}<br>
                    <button onclick="toggleAircraftTrail('${aircraft.hex}')" style="margin-top: 8px; padding: 4px 8px; background: #5a8a5a; color: #1a1a1a; border: none; cursor: pointer; font-family: 'Courier New', monospace;">
                        Show Trail
                    </button>
                </div>
            `;
            marker.bindPopup(popupContent);

            aircraftMarkers.set(aircraftId, marker);

            // If "Show All Trails" is active, draw trail for new aircraft
            if (showTrails) {
                drawAircraftTrail(aircraft.hex);
            }
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
            // Remove trail if it exists
            removeAircraftTrail(id);
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

// Refresh all active trails (called periodically)
async function refreshActiveTrails() {
    if (showTrails || selectedAircraftForTrail) {
        const hexCodes = Array.from(aircraftTrails.keys());
        for (const hex of hexCodes) {
            await drawAircraftTrail(hex);
        }
    }
}

// Fetch statistics from backend
async function fetchStatistics() {
    try {
        const timeWindow = getTimeWindow();
        const response = await fetch(`http://localhost:3000/api/statistics?window=${timeWindow}`);

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

// Start polling for aircraft data and statistics
function startPolling() {
    fetchAircraftData(); // Initial fetch
    fetchStatistics(); // Initial statistics fetch

    updateInterval = setInterval(fetchAircraftData, 2000); // Update every 2 seconds
    statisticsInterval = setInterval(fetchStatistics, 5000); // Update statistics every 5 seconds
    trailRefreshInterval = setInterval(refreshActiveTrails, 10000); // Refresh trails every 10 seconds
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

    // Trail controls
    const toggleTrailsButton = document.getElementById('toggle-trails');
    const trailDurationSelector = document.getElementById('trail-duration');

    if (toggleTrailsButton) {
        toggleTrailsButton.addEventListener('click', toggleAllTrails);
    }

    if (trailDurationSelector) {
        trailDurationSelector.addEventListener('change', (e) => {
            trailDuration = parseInt(e.target.value);
            // Redraw trails if any are visible
            if (showTrails || selectedAircraftForTrail) {
                aircraftTrails.forEach((_, hex) => {
                    drawAircraftTrail(hex);
                });
            }
        });
    }

    // Update data when time window changes
    if (timeWindowSelector) {
        timeWindowSelector.addEventListener('change', () => {
            fetchAircraftData(); // Immediately fetch with new window
            fetchStatistics(); // Also update statistics
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

// Make functions globally accessible for onclick handlers in popups
window.toggleAircraftTrail = toggleAircraftTrail;
