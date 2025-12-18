// Set your Cesium Ion access token
Cesium.Ion.defaultAccessToken = 'eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJqdGkiOiI5MDdjYmVjNC05NjZiLTQxZDMtYjA1YS00ZmI3ZjA2ZjRiYjQiLCJpZCI6MzY5MDk5LCJpYXQiOjE3NjU1MjUxMDZ9.vixLzkzcNmRUX1wvntDVQ9If5gVlx5ufYNo4Fytsh8A';

// Initialize Cesium Viewer with terrain provider
const viewer = new Cesium.Viewer('cesiumContainer', {
    baseLayerPicker: false,
    animation: false,
    timeline: false,
    vrButton: false,
    sceneModePicker: false,
    geocoder: false,
    homeButton: false,
    navigationHelpButton: false,
    selectionIndicator: true,
    infoBox: false,
    terrain: Cesium.Terrain.fromWorldTerrain()
});

// Enable lighting for realistic building shadows
viewer.scene.globe.enableLighting = true;

// Enable depth testing so buildings sit properly on terrain
viewer.scene.globe.depthTestAgainstTerrain = true;

// Enable 3D buildings from Cesium OSM Buildings (free with Cesium Ion)
(async function() {
    try {
        const osmBuildingsTileset = await Cesium.createOsmBuildingsAsync();
        viewer.scene.primitives.add(osmBuildingsTileset);

        console.log('3D buildings loaded successfully');
    } catch (error) {
        console.warn('Could not load 3D buildings:', error);
        // Continue without buildings - they're optional
    }
})();

// Optimized camera controls for Mac trackpad
const controller = viewer.scene.screenSpaceCameraController;

// Zoom: Two-finger scroll on trackpad
controller.zoomEventTypes = [
    Cesium.CameraEventType.WHEEL,
    Cesium.CameraEventType.PINCH
];

// Rotate: One-finger drag (left click + drag)
controller.rotateEventTypes = Cesium.CameraEventType.LEFT_DRAG;

// Tilt: Two-finger drag (right click + drag) or Shift + drag
controller.tiltEventTypes = [
    Cesium.CameraEventType.RIGHT_DRAG,
    Cesium.CameraEventType.MIDDLE_DRAG,
    {
        eventType: Cesium.CameraEventType.LEFT_DRAG,
        modifier: Cesium.KeyboardEventModifier.SHIFT
    }
];

// Look: Ctrl/Cmd + Left drag
controller.lookEventTypes = {
    eventType: Cesium.CameraEventType.LEFT_DRAG,
    modifier: Cesium.KeyboardEventModifier.CTRL
};

// Pan: Cmd/Ctrl + Shift + drag (useful for fine adjustments)
controller.translateEventTypes = {
    eventType: Cesium.CameraEventType.LEFT_DRAG,
    modifier: Cesium.KeyboardEventModifier.SHIFT | Cesium.KeyboardEventModifier.CTRL
};

// Fine-tune sensitivity for Mac trackpad
controller.zoomFactor = 3.0; // Slower zoom for better control
controller.rotateEventTypes = Cesium.CameraEventType.LEFT_DRAG;
controller.minimumZoomDistance = 100; // Allow closer zoom to see buildings
controller.maximumZoomDistance = 40000000;

// Smooth inertia for natural feel
controller.inertiaSpin = 0.9;
controller.inertiaTranslate = 0.9;
controller.inertiaZoom = 0.9;

// Prevent camera from going below ground
controller.enableCollisionDetection = true;

// Initial camera configuration
const INITIAL_CAMERA_CONFIG = {
    longitude: -87.6298,  // Chicago longitude
    latitude: 41.8781,    // Chicago latitude
    altitude: 500000,     // 500km above ground
    heading: 0,           // Degrees (0 = North)
    pitch: -100,           // Degrees (negative = looking down)
    roll: 0               // Degrees
};

// Aircraft entities storage
const aircraftEntities = new Map();
const aircraftTrails = new Map();
let airportEntities = [];
let showAirports = false;
let showTrails = false;
let showTerrain = true;
let followMode = false;
let followedAircraft = null;

// Update intervals
let updateInterval;

// Set initial camera position
viewer.camera.setView({
    destination: Cesium.Cartesian3.fromDegrees(
        INITIAL_CAMERA_CONFIG.longitude,
        INITIAL_CAMERA_CONFIG.latitude,
        INITIAL_CAMERA_CONFIG.altitude
    ),
    orientation: {
        heading: Cesium.Math.toRadians(INITIAL_CAMERA_CONFIG.heading),
        pitch: Cesium.Math.toRadians(INITIAL_CAMERA_CONFIG.pitch),
        roll: Cesium.Math.toRadians(INITIAL_CAMERA_CONFIG.roll)
    }
});

// Aircraft model (simple 3D cone/cylinder to represent plane)
function createAircraftEntity(aircraft) {
    const position = Cesium.Cartesian3.fromDegrees(
        aircraft.lon,
        aircraft.lat,
        aircraft.altitude || 0 // altitude in feet, Cesium uses meters but we'll scale
    );

    // Convert altitude from feet to meters for Cesium
    const altitudeMeters = (aircraft.altitude || 0) * 0.3048;
    const positionMeters = Cesium.Cartesian3.fromDegrees(
        aircraft.lon,
        aircraft.lat,
        altitudeMeters
    );

    const identifier = aircraft.flight || aircraft.hex || 'N/A';

    // Create entity with Cesium airplane model
    const entity = viewer.entities.add({
        id: aircraft.hex,
        position: positionMeters,
        model: {
            uri: 'https://raw.githubusercontent.com/CesiumGS/cesium/main/Apps/SampleData/models/CesiumAir/Cesium_Air.glb',
            minimumPixelSize: 64,
            maximumScale: 20000,
            color: Cesium.Color.fromCssColorString('#7a9a7a')
        },
        orientation: Cesium.Transforms.headingPitchRollQuaternion(
            positionMeters,
            new Cesium.HeadingPitchRoll(
                Cesium.Math.toRadians(aircraft.track || 0),
                Cesium.Math.toRadians(0),
                0
            )
        ),
        label: {
            text: identifier,
            font: '12px Courier New',
            fillColor: Cesium.Color.fromCssColorString('#7a9a7a'),
            outlineColor: Cesium.Color.BLACK,
            outlineWidth: 2,
            style: Cesium.LabelStyle.FILL_AND_OUTLINE,
            verticalOrigin: Cesium.VerticalOrigin.BOTTOM,
            pixelOffset: new Cesium.Cartesian2(0, -40),
            disableDepthTestDistance: Number.POSITIVE_INFINITY
        },
        properties: {
            hex: aircraft.hex,
            flight: aircraft.flight,
            altitude: aircraft.altitude,
            speed: aircraft.speed,
            track: aircraft.track
        }
    });

    return entity;
}

// Update aircraft position with smooth interpolation
function updateAircraftEntity(entity, aircraft) {
    const altitudeMeters = (aircraft.altitude || 0) * 0.3048;
    const newPosition = Cesium.Cartesian3.fromDegrees(
        aircraft.lon,
        aircraft.lat,
        altitudeMeters
    );

    // Smooth position update
    entity.position = newPosition;

    // Update orientation based on heading
    entity.orientation = Cesium.Transforms.headingPitchRollQuaternion(
        newPosition,
        new Cesium.HeadingPitchRoll(
            Cesium.Math.toRadians(aircraft.track || 0),
            Cesium.Math.toRadians(0),
            0
        )
    );

    // Update label
    const identifier = aircraft.flight || aircraft.hex || 'N/A';
    entity.label.text = identifier;

    // Update properties
    entity.properties = {
        hex: aircraft.hex,
        flight: aircraft.flight,
        altitude: aircraft.altitude,
        speed: aircraft.speed,
        track: aircraft.track
    };

    // Add to trail if enabled
    if (showTrails) {
        addToTrail(aircraft.hex, newPosition);
    }
}

// Add position to aircraft trail
function addToTrail(hex, position) {
    if (!aircraftTrails.has(hex)) {
        const trail = viewer.entities.add({
            polyline: {
                positions: [],
                width: 2,
                material: new Cesium.PolylineGlowMaterialProperty({
                    glowPower: 0.2,
                    color: Cesium.Color.fromCssColorString('#7a9a7a').withAlpha(0.6)
                }),
                clampToGround: false
            }
        });
        aircraftTrails.set(hex, trail);
    }

    const trail = aircraftTrails.get(hex);
    const positions = trail.polyline.positions.getValue(Cesium.JulianDate.now());

    // Keep last 50 positions (about 100 seconds at 2s update rate)
    if (positions.length > 50) {
        positions.shift();
    }

    positions.push(position);
    trail.polyline.positions = positions;
}

// Clear all trails
function clearTrails() {
    aircraftTrails.forEach(trail => {
        viewer.entities.remove(trail);
    });
    aircraftTrails.clear();
}

// Fetch and update aircraft data
async function fetchAircraftData() {
    try {
        const response = await fetch('/api/aircraft?window=0');
        if (!response.ok) {
            throw new Error('Failed to fetch aircraft data');
        }

        const data = await response.json();
        updateStatus(true);
        document.getElementById('aircraft-count').textContent = data.aircraft?.length || 0;

        updateAircraftOn3DGlobe(data.aircraft || []);
    } catch (error) {
        console.error('Error fetching aircraft data:', error);
        updateStatus(false, 'Connection Error');
    }
}

// Update aircraft on 3D globe
function updateAircraftOn3DGlobe(aircraftData) {
    const seenAircraftIds = new Set();

    aircraftData.forEach(aircraft => {
        if (!aircraft.lat || !aircraft.lon) return;

        seenAircraftIds.add(aircraft.hex);

        if (aircraftEntities.has(aircraft.hex)) {
            // Update existing aircraft
            const entity = aircraftEntities.get(aircraft.hex);
            updateAircraftEntity(entity, aircraft);
        } else {
            // Create new aircraft
            const entity = createAircraftEntity(aircraft);
            aircraftEntities.set(aircraft.hex, entity);
        }
    });

    // Remove aircraft that are no longer visible
    aircraftEntities.forEach((entity, hex) => {
        if (!seenAircraftIds.has(hex)) {
            viewer.entities.remove(entity);
            aircraftEntities.delete(hex);

            // Remove trail
            if (aircraftTrails.has(hex)) {
                viewer.entities.remove(aircraftTrails.get(hex));
                aircraftTrails.delete(hex);
            }
        }
    });

    // Follow mode: keep camera on followed aircraft
    if (followMode && followedAircraft && aircraftEntities.has(followedAircraft)) {
        const entity = aircraftEntities.get(followedAircraft);
        const position = entity.position.getValue(Cesium.JulianDate.now());

        // Get aircraft heading for better camera angle
        const heading = entity.properties.track || 0;
        const headingRadians = Cesium.Math.toRadians(heading);

        // Calculate offset position behind and above the aircraft
        const distance = 5000; // 5km behind the aircraft
        const heightOffset = 2000; // 2km above

        // Create offset from aircraft position
        const cartographic = Cesium.Cartographic.fromCartesian(position);
        const offsetPosition = Cesium.Cartesian3.fromRadians(
            cartographic.longitude - Math.sin(headingRadians) * (distance / 6371000),
            cartographic.latitude - Math.cos(headingRadians) * (distance / 6371000),
            cartographic.height + heightOffset
        );

        // Smoothly move camera to follow position
        viewer.camera.flyTo({
            destination: offsetPosition,
            orientation: {
                heading: headingRadians,
                pitch: Cesium.Math.toRadians(-20), // Look slightly down at the aircraft
                roll: 0.0
            },
            duration: 1.5,
            easingFunction: Cesium.EasingFunction.LINEAR_NONE
        });
    }

    console.log(`3D Globe updated: ${seenAircraftIds.size} aircraft visible`);
}

// Old collision detection removed - will be replaced with Kalman filter system

// Load and display airports
async function loadAirports() {
    try {
        const response = await fetch('airports.json');
        if (!response.ok) return;

        const geojson = await response.json();

        geojson.features.forEach(feature => {
            const coords = feature.geometry.coordinates;
            const props = feature.properties;

            const color = props.type === 'large_airport' ?
                Cesium.Color.fromCssColorString('#ff5533') :
                Cesium.Color.fromCssColorString('#e8a628');

            const entity = viewer.entities.add({
                position: Cesium.Cartesian3.fromDegrees(coords[0], coords[1], 0),
                point: {
                    pixelSize: props.type === 'large_airport' ? 8 : 5,
                    color: color,
                    outlineColor: Cesium.Color.BLACK,
                    outlineWidth: 1
                },
                label: {
                    text: props.iata || props.icao || '',
                    font: '10px Courier New',
                    fillColor: color,
                    outlineColor: Cesium.Color.BLACK,
                    outlineWidth: 1,
                    style: Cesium.LabelStyle.FILL_AND_OUTLINE,
                    verticalOrigin: Cesium.VerticalOrigin.TOP,
                    pixelOffset: new Cesium.Cartesian2(0, 10),
                    show: false // Only show on hover/zoom
                },
                show: false // Hidden by default
            });

            airportEntities.push(entity);
        });

        console.log(`Loaded ${airportEntities.length} airports`);
    } catch (error) {
        console.error('Error loading airports:', error);
    }
}

// Toggle airports visibility
function toggleAirports() {
    showAirports = !showAirports;
    airportEntities.forEach(entity => {
        entity.show = showAirports;
    });
    document.getElementById('toggle-airports').classList.toggle('active', showAirports);
}

// Toggle trails
function toggleTrails() {
    showTrails = !showTrails;
    if (!showTrails) {
        clearTrails();
    }
    document.getElementById('toggle-trails').classList.toggle('active', showTrails);
}

// Toggle terrain (now toggles imagery instead since we have no auth for terrain)
function toggleTerrain() {
    showTerrain = !showTerrain;
    viewer.scene.globe.show = showTerrain;
    document.getElementById('toggle-terrain').classList.toggle('active', showTerrain);
}

// Reset view to default
function resetView() {
    followMode = false;
    followedAircraft = null;
    document.getElementById('view-follow').classList.remove('active');
    document.getElementById('view-globe').classList.add('active');

    viewer.camera.flyTo({
        destination: Cesium.Cartesian3.fromDegrees(
            INITIAL_CAMERA_CONFIG.longitude,
            INITIAL_CAMERA_CONFIG.latitude,
            INITIAL_CAMERA_CONFIG.altitude
        ),
        orientation: {
            heading: Cesium.Math.toRadians(INITIAL_CAMERA_CONFIG.heading),
            pitch: Cesium.Math.toRadians(INITIAL_CAMERA_CONFIG.pitch),
            roll: Cesium.Math.toRadians(INITIAL_CAMERA_CONFIG.roll)
        },
        duration: 2.0
    });
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

// Track selected aircraft (for follow mode)
let selectedAircraft = null;

// Handle entity selection
viewer.selectedEntityChanged.addEventListener((selectedEntity) => {
    const infoPanel = document.getElementById('info-panel');
    const infoContent = document.getElementById('info-content');

    if (selectedEntity && selectedEntity.properties) {
        // Get property values correctly from Cesium properties
        const hex = selectedEntity.properties.hex?.valueOf() || selectedEntity.id;
        const flight = selectedEntity.properties.flight?.valueOf();
        const altitude = selectedEntity.properties.altitude?.valueOf();
        const speed = selectedEntity.properties.speed?.valueOf();
        const track = selectedEntity.properties.track?.valueOf();

        // Store the selected aircraft hex code
        selectedAircraft = hex;
        console.log(`Selected aircraft: ${flight || hex} (hex: ${hex})`);

        const isFollowing = followMode && followedAircraft === hex;

        infoContent.innerHTML = `
            <strong>Flight:</strong> ${flight || 'N/A'}<br>
            <strong>ICAO:</strong> ${hex}<br>
            <strong>Altitude:</strong> ${altitude || 'N/A'} ft<br>
            <strong>Speed:</strong> ${speed || 'N/A'} kt<br>
            <strong>Heading:</strong> ${track || 'N/A'}°
            ${isFollowing ? '<br><em style="color: #9aba9a;">Following this aircraft</em>' : '<br><em style="color: #8aaa8a;">Click "Follow" to track this aircraft</em>'}
        `;
        infoPanel.style.display = 'block';
    } else {
        selectedAircraft = null;
        infoPanel.style.display = 'none';
    }
});

// Setup controls
document.getElementById('toggle-airports').addEventListener('click', toggleAirports);
document.getElementById('toggle-trails').addEventListener('click', toggleTrails);
document.getElementById('toggle-terrain').addEventListener('click', () => {
    toggleTerrain();
});
document.getElementById('reset-view').addEventListener('click', resetView);

// View mode buttons
document.getElementById('view-globe').addEventListener('click', () => {
    followMode = false;
    followedAircraft = null;
    document.getElementById('view-follow').classList.remove('active');
    document.getElementById('view-globe').classList.add('active');

    // Hide follow hint
    const hint = document.getElementById('follow-hint');
    if (hint) hint.style.display = 'none';
});

document.getElementById('view-follow').addEventListener('click', () => {
    followMode = true;

    // Use the currently selected aircraft if available
    if (selectedAircraft && aircraftEntities.has(selectedAircraft)) {
        followedAircraft = selectedAircraft;
        const entity = aircraftEntities.get(followedAircraft);

        // Get property values correctly
        const flight = entity.properties.flight?.valueOf();
        const hex = entity.properties.hex?.valueOf() || entity.id;
        const altitude = entity.properties.altitude?.valueOf();
        const speed = entity.properties.speed?.valueOf();
        const track = entity.properties.track?.valueOf();

        console.log(`Following selected aircraft: ${flight || hex} (hex: ${hex})`);

        // Update info panel to show following status
        const infoPanel = document.getElementById('info-panel');
        const infoContent = document.getElementById('info-content');
        if (infoPanel.style.display === 'block') {
            infoContent.innerHTML = `
                <strong>Flight:</strong> ${flight || 'N/A'}<br>
                <strong>ICAO:</strong> ${hex}<br>
                <strong>Altitude:</strong> ${altitude || 'N/A'} ft<br>
                <strong>Speed:</strong> ${speed || 'N/A'} kt<br>
                <strong>Heading:</strong> ${track || 'N/A'}°
                <br><em style="color: #9aba9a;">Following this aircraft</em>
            `;
        }
    } else if (aircraftEntities.size > 0) {
        // If no aircraft selected, follow the first one
        followedAircraft = aircraftEntities.keys().next().value;
        const entity = aircraftEntities.get(followedAircraft);

        const flight = entity.properties.flight?.valueOf();
        const hex = entity.properties.hex?.valueOf() || entity.id;

        console.log(`No aircraft selected. Following first aircraft: ${flight || hex} (hex: ${hex})`);

        // Show hint
        const hint = document.getElementById('follow-hint');
        if (hint) {
            hint.style.display = 'block';
            setTimeout(() => {
                hint.style.display = 'none';
            }, 3000);
        }
    } else {
        console.log('No aircraft available to follow.');
    }

    document.getElementById('view-globe').classList.remove('active');
    document.getElementById('view-follow').classList.add('active');
});

// Start polling
function startPolling() {
    loadAirports(); // Load airports once at start
    fetchAircraftData(); // Initial fetch

    updateInterval = setInterval(fetchAircraftData, 2000); // Update every 2 seconds
}

// Stop polling
function stopPolling() {
    if (updateInterval) {
        clearInterval(updateInterval);
    }
}

// Initialize
updateStatus(false, 'Connecting...');
startPolling();

// Cleanup on page unload
window.addEventListener('beforeunload', stopPolling);
