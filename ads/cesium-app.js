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
    infoBox: false,  // Using custom info panel instead
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
let currentAircraftData = []; // Store current aircraft data for collision line updates

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
            color: Cesium.Color.WHITE, // White for better visibility
            silhouetteColor: Cesium.Color.CYAN,
            silhouetteSize: 1.0
        },
        orientation: Cesium.Transforms.headingPitchRollQuaternion(
            positionMeters,
            new Cesium.HeadingPitchRoll(
                // Cesium Air model needs -90 degree offset to align nose with track direction
                Cesium.Math.toRadians((aircraft.track || 0) - 90),
                Cesium.Math.toRadians(0),
                0
            )
        ),
        label: {
            text: identifier,
            font: 'bold 13px Courier New',
            fillColor: Cesium.Color.CYAN, // Bright cyan for visibility
            outlineColor: Cesium.Color.BLACK,
            outlineWidth: 3,
            style: Cesium.LabelStyle.FILL_AND_OUTLINE,
            verticalOrigin: Cesium.VerticalOrigin.BOTTOM,
            pixelOffset: new Cesium.Cartesian2(0, -40),
            disableDepthTestDistance: Number.POSITIVE_INFINITY,
            showBackground: true,
            backgroundColor: Cesium.Color.BLACK.withAlpha(0.5)
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
            // Cesium Air model needs -90 degree offset to align nose with track direction
            Cesium.Math.toRadians((aircraft.track || 0) - 90),
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
                width: 3,
                material: new Cesium.PolylineGlowMaterialProperty({
                    glowPower: 0.3,
                    color: Cesium.Color.CYAN.withAlpha(0.7)
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

        currentAircraftData = data.aircraft || []; // Store for collision line updates
        updateAircraftOn3DGlobe(currentAircraftData);
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

// Kalman filter-based collision detection and probability cone visualization
let collisionLines = [];
let probabilityCones = [];
let showProbabilityCones = false;
let collisionInterval;

// Fetch and display collisions with probability cones
async function fetchCollisions() {
    try {
        const response = await fetch('/api/collisions/kalman');
        const data = await response.json();

        if (data.error) {
            console.error('Collision detection error:', data.error);
            return;
        }

        displayCollisions3D(data.collisions);
    } catch (error) {
        console.error('Error fetching collisions:', error);
    }
}

// Display collision warnings on the 3D globe
function displayCollisions3D(collisions) {
    // Remove old collision lines
    collisionLines.forEach(entity => viewer.entities.remove(entity));
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
        const currentPos1 = Cesium.Cartesian3.fromDegrees(
            aircraft1.lon,
            aircraft1.lat,
            (aircraft1.altitude || 10000) * 0.3048
        );
        const currentPos2 = Cesium.Cartesian3.fromDegrees(
            aircraft2.lon,
            aircraft2.lat,
            (aircraft2.altitude || 10000) * 0.3048
        );

        // Predicted positions for each aircraft
        const futurePos1Lat = parseFloat(collision.position1.lat);
        const futurePos1Lon = parseFloat(collision.position1.lon);
        const futurePos1Alt = parseFloat(collision.position1.alt) * 0.3048;

        const futurePos2Lat = parseFloat(collision.position2.lat);
        const futurePos2Lon = parseFloat(collision.position2.lon);
        const futurePos2Alt = parseFloat(collision.position2.alt) * 0.3048;

        // Calculate the collision point as the midpoint between the two predicted positions
        const collisionLat = (futurePos1Lat + futurePos2Lat) / 2;
        const collisionLon = (futurePos1Lon + futurePos2Lon) / 2;
        const collisionAlt = (futurePos1Alt + futurePos2Alt) / 2;

        const collisionPointPos = Cesium.Cartesian3.fromDegrees(collisionLon, collisionLat, collisionAlt);

        // Color based on probability and time
        let color;
        const probability = parseFloat(collision.probability);
        const time = collision.timeToCollision;

        if (probability > 15 || time < 60) {
            color = Cesium.Color.RED;
        } else if (probability > 10 || time < 120) {
            color = Cesium.Color.ORANGE;
        } else {
            color = Cesium.Color.YELLOW;
        }

        // Draw trajectory line for aircraft 1: current position → collision point
        const trajectory1 = viewer.entities.add({
            name: `${collision.aircraft1} trajectory`,
            polyline: {
                positions: [currentPos1, collisionPointPos],
                width: 3,
                material: new Cesium.PolylineDashMaterialProperty({
                    color: color,
                    dashLength: 12.0
                })
            }
        });
        collisionLines.push(trajectory1);

        // Draw trajectory line for aircraft 2: current position → collision point
        const trajectory2 = viewer.entities.add({
            name: `${collision.aircraft2} trajectory`,
            polyline: {
                positions: [currentPos2, collisionPointPos],
                width: 3,
                material: new Cesium.PolylineDashMaterialProperty({
                    color: color,
                    dashLength: 12.0
                })
            }
        });
        collisionLines.push(trajectory2);

        // Create collision point marker at the midpoint (where they would collide)
        const aircraft1Display = collision.flight1 || collision.aircraft1;
        const aircraft2Display = collision.flight2 || collision.aircraft2;

        const collisionPoint = viewer.entities.add({
            name: `Collision: ${aircraft1Display} ↔ ${aircraft2Display}`,
            position: collisionPointPos,
            point: {
                pixelSize: 15,
                color: color,
                outlineColor: Cesium.Color.WHITE,
                outlineWidth: 2
            },
            properties: {
                isCollision: true,
                aircraft1: aircraft1Display,
                aircraft2: aircraft2Display,
                timeToCollision: collision.timeToCollision,
                probability: collision.probability,
                distance: collision.distance,
                verticalSeparation: collision.verticalSeparation,
                alt1: collision.position1.alt,
                alt2: collision.position2.alt,
                riskColor: color.toCssColorString()
            }
        });
        collisionLines.push(collisionPoint);
    });

    // Update collision count in header
    const collisionCount = document.getElementById('collision-count');
    if (collisionCount) {
        collisionCount.textContent = collisions.length;
    }

    console.log(`🚨 ${collisions.length} potential collision(s) detected in 3D`);
}

// Fetch and display probability cones for all aircraft
async function fetchAndDisplayProbabilityCones() {
    if (!showProbabilityCones) {
        return;
    }

    try {
        // Get all active aircraft
        const response = await fetch('/api/aircraft');
        const data = await response.json();

        // Extract aircraft array from response
        const aircraft = Array.isArray(data) ? data : (data.aircraft || []);

        // Fetch position history and generate cones for each aircraft
        displayProbabilityCones(aircraft);
    } catch (error) {
        console.error('Error fetching probability cones:', error);
    }
}

// Display probability cones in front of each aircraft
function displayProbabilityCones(aircraft) {
    // Remove old cones
    probabilityCones.forEach(entity => viewer.entities.remove(entity));
    probabilityCones = [];

    if (!showProbabilityCones || !aircraft || aircraft.length === 0) {
        return;
    }

    // Prediction time horizon in seconds (5 minutes)
    const predictionTime = 300;
    // Cone spread angle in degrees (how wide the cone fans out)
    const coneSpreadAngle = 12;
    const numSegments = 16; // Number of segments around the cone edge

    let conesCreated = 0;

    // For each aircraft, create a cone showing predicted trajectory with uncertainty
    aircraft.forEach(ac => {
        // Only require position and altitude - use defaults for speed/track if missing
        if (!ac.lat || !ac.lon) return;

        const altitude = ac.altitude || 10000; // Default 10,000 ft if missing
        const speed = (ac.speed || 250) * 0.514444; // knots to m/s, default 250 kt
        const trackDeg = ac.track || 0; // heading in degrees (0 = North, clockwise)
        const altitudeMeters = altitude * 0.3048;

        // Calculate the distance the aircraft will travel
        const distance = speed * predictionTime;

        // Skip if distance is too small (stationary aircraft)
        if (distance < 1000) return;

        // Apex position (current aircraft position)
        const apexPosition = Cesium.Cartesian3.fromDegrees(ac.lon, ac.lat, altitudeMeters);

        // Use raw track for cone direction (no offset needed - cones follow actual heading)
        const trackRad = (trackDeg * Math.PI) / 180;
        
        // Distance in degrees (approximate for short distances)
        const metersPerDegreeLat = 111320;
        const metersPerDegreeLon = 111320 * Math.cos(ac.lat * Math.PI / 180);
        
        // Calculate offset: sin(track) for East-West, cos(track) for North-South
        const deltaLat = (distance * Math.cos(trackRad)) / metersPerDegreeLat;
        const deltaLon = (distance * Math.sin(trackRad)) / metersPerDegreeLon;

        const futureLatDeg = ac.lat + deltaLat;
        const futureLonDeg = ac.lon + deltaLon;

        // Calculate future altitude
        const futureAlt = Math.max(0, altitude + (ac.vertical_rate || 0) * (predictionTime / 60));
        const futureAltMeters = futureAlt * 0.3048;

        // Future center position
        const futurePosition = Cesium.Cartesian3.fromDegrees(futureLonDeg, futureLatDeg, futureAltMeters);

        // Calculate cone radius at the far end
        const coneRadius = distance * Math.tan((coneSpreadAngle * Math.PI) / 180);

        // Calculate the direction vector from apex to future position
        const direction = Cesium.Cartesian3.subtract(futurePosition, apexPosition, new Cesium.Cartesian3());
        Cesium.Cartesian3.normalize(direction, direction);

        // Get a perpendicular vector (use cross product with up vector)
        const up = Cesium.Cartesian3.normalize(apexPosition, new Cesium.Cartesian3());
        let perpendicular = Cesium.Cartesian3.cross(direction, up, new Cesium.Cartesian3());

        // Handle case where direction is parallel to up
        if (Cesium.Cartesian3.magnitude(perpendicular) < 0.001) {
            perpendicular = new Cesium.Cartesian3(1, 0, 0);
        }
        Cesium.Cartesian3.normalize(perpendicular, perpendicular);

        // Get second perpendicular vector
        const perpendicular2 = Cesium.Cartesian3.cross(direction, perpendicular, new Cesium.Cartesian3());
        Cesium.Cartesian3.normalize(perpendicular2, perpendicular2);

        // Generate edge positions around the cone base (proper circle in 3D)
        const edgePositions = [];
        for (let i = 0; i <= numSegments; i++) {
            const angle = (i / numSegments) * 2 * Math.PI;

            // Calculate offset using the two perpendicular vectors
            const offset = new Cesium.Cartesian3();
            Cesium.Cartesian3.multiplyByScalar(perpendicular, coneRadius * Math.cos(angle), offset);
            const offset2 = new Cesium.Cartesian3();
            Cesium.Cartesian3.multiplyByScalar(perpendicular2, coneRadius * Math.sin(angle), offset2);
            Cesium.Cartesian3.add(offset, offset2, offset);

            // Add offset to future position
            const edgePoint = Cesium.Cartesian3.add(futurePosition, offset, new Cesium.Cartesian3());
            edgePositions.push(edgePoint);
        }

        // Color based on altitude
        let color;
        if (altitude < 10000) {
            color = Cesium.Color.LIME;
        } else if (altitude < 25000) {
            color = Cesium.Color.YELLOW;
        } else {
            color = Cesium.Color.ORANGE;
        }

        // Create the cone surface using triangular polygons
        for (let i = 0; i < numSegments; i++) {
            const coneSegment = viewer.entities.add({
                name: `Cone: ${ac.flight || ac.hex}`,
                polygon: {
                    hierarchy: new Cesium.PolygonHierarchy([
                        apexPosition,
                        edgePositions[i],
                        edgePositions[i + 1]
                    ]),
                    material: color.withAlpha(0.12),
                    outline: false,
                    perPositionHeight: true
                }
            });
            probabilityCones.push(coneSegment);
        }

        // Center trajectory line
        const centerLine = viewer.entities.add({
            polyline: {
                positions: [apexPosition, futurePosition],
                width: 2,
                material: color.withAlpha(0.8)
            }
        });
        probabilityCones.push(centerLine);

        // Cone base outline
        const coneOutline = viewer.entities.add({
            polyline: {
                positions: edgePositions,
                width: 1,
                material: color.withAlpha(0.5)
            }
        });
        probabilityCones.push(coneOutline);

        // Edge lines from apex to base (every 4th point for cleaner look)
        for (let i = 0; i < numSegments; i += 4) {
            const edgeLine = viewer.entities.add({
                polyline: {
                    positions: [apexPosition, edgePositions[i]],
                    width: 1,
                    material: color.withAlpha(0.3)
                }
            });
            probabilityCones.push(edgeLine);
        }

        conesCreated++;
    });

    console.log(`📊 Displayed ${conesCreated} probability cones for ${aircraft.length} aircraft`);
}

// Toggle probability cone visibility
function toggleProbabilityCones() {
    showProbabilityCones = !showProbabilityCones;

    if (showProbabilityCones) {
        fetchAndDisplayProbabilityCones();
    } else {
        // Remove all cones
        probabilityCones.forEach(entity => viewer.entities.remove(entity));
        probabilityCones = [];
    }

    const button = document.getElementById('toggle-probability-cones');
    if (button) {
        button.textContent = showProbabilityCones ? '🔵 Hide Prediction Cones' : '⚪ Show Prediction Cones';
    }
}

// Create airport icon as a data URL
function createAirportIcon(isLarge) {
    const size = isLarge ? 24 : 16;
    const color = isLarge ? '#ff5533' : '#e8a628';
    const strokeWidth = isLarge ? 2 : 1.5;

    // Create a simple runway/airport symbol
    const svg = `
        <svg xmlns="http://www.w3.org/2000/svg" width="${size}" height="${size}" viewBox="0 0 24 24">
            <circle cx="12" cy="12" r="10" fill="${color}" fill-opacity="0.3" stroke="${color}" stroke-width="${strokeWidth}"/>
            <circle cx="12" cy="12" r="4" fill="${color}"/>
            <line x1="12" y1="2" x2="12" y2="6" stroke="${color}" stroke-width="${strokeWidth}"/>
            <line x1="12" y1="18" x2="12" y2="22" stroke="${color}" stroke-width="${strokeWidth}"/>
            <line x1="2" y1="12" x2="6" y2="12" stroke="${color}" stroke-width="${strokeWidth}"/>
            <line x1="18" y1="12" x2="22" y2="12" stroke="${color}" stroke-width="${strokeWidth}"/>
        </svg>
    `;

    return 'data:image/svg+xml;base64,' + btoa(svg);
}

// Load and display airports
async function loadAirports() {
    try {
        const response = await fetch('airports.json');
        if (!response.ok) return;

        const geojson = await response.json();

        geojson.features.forEach(feature => {
            const coords = feature.geometry.coordinates;
            const props = feature.properties;

            const isLarge = props.type === 'large_airport';
            const color = isLarge ?
                Cesium.Color.fromCssColorString('#ff5533') :
                Cesium.Color.fromCssColorString('#e8a628');

            // Elevate airports slightly above terrain for visibility
            const elevation = 100; // 100 meters above ground

            const entity = viewer.entities.add({
                position: Cesium.Cartesian3.fromDegrees(coords[0], coords[1], elevation),
                // Use billboard for better visibility from all angles
                billboard: {
                    image: createAirportIcon(isLarge),
                    width: isLarge ? 24 : 16,
                    height: isLarge ? 24 : 16,
                    verticalOrigin: Cesium.VerticalOrigin.CENTER,
                    horizontalOrigin: Cesium.HorizontalOrigin.CENTER,
                    disableDepthTestDistance: Number.POSITIVE_INFINITY, // Always visible
                    heightReference: Cesium.HeightReference.RELATIVE_TO_GROUND
                },
                label: {
                    text: props.iata || props.icao || props.name?.substring(0, 10) || '',
                    font: isLarge ? 'bold 12px Courier New' : '10px Courier New',
                    fillColor: color,
                    outlineColor: Cesium.Color.BLACK,
                    outlineWidth: 2,
                    style: Cesium.LabelStyle.FILL_AND_OUTLINE,
                    verticalOrigin: Cesium.VerticalOrigin.TOP,
                    pixelOffset: new Cesium.Cartesian2(0, 14),
                    disableDepthTestDistance: Number.POSITIVE_INFINITY, // Always visible
                    show: true // Always show labels
                },
                show: false // Hidden by default until toggled
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
    const infoTitle = document.getElementById('info-title');

    if (selectedEntity && selectedEntity.properties) {
        // Check if this is a collision marker
        const isCollision = selectedEntity.properties.isCollision?.valueOf();

        if (isCollision) {
            // Show collision info
            const aircraft1 = selectedEntity.properties.aircraft1?.valueOf();
            const aircraft2 = selectedEntity.properties.aircraft2?.valueOf();
            const timeToCollision = selectedEntity.properties.timeToCollision?.valueOf();
            const probability = selectedEntity.properties.probability?.valueOf();
            const distance = selectedEntity.properties.distance?.valueOf();
            const verticalSep = selectedEntity.properties.verticalSeparation?.valueOf();
            const alt1 = selectedEntity.properties.alt1?.valueOf();
            const alt2 = selectedEntity.properties.alt2?.valueOf();
            const riskColor = selectedEntity.properties.riskColor?.valueOf() || '#ff6600';

            infoTitle.textContent = '⚠️ Collision Warning';
            infoTitle.style.color = riskColor;
            infoPanel.classList.add('collision-info');
            infoPanel.style.borderColor = riskColor;

            infoContent.innerHTML = `
                <strong>Aircraft 1:</strong> ${aircraft1}<br>
                <strong>Aircraft 2:</strong> ${aircraft2}<br>
                <strong style="color: ${riskColor};">Time:</strong> <span style="color: ${riskColor};">${timeToCollision}s (${Math.round(timeToCollision/60)}m)</span><br>
                <strong style="color: ${riskColor};">Probability:</strong> <span style="color: ${riskColor};">${probability}</span><br>
                <strong>Separation:</strong> ${(distance/1000).toFixed(2)} km<br>
                <strong>Vertical:</strong> ${verticalSep} ft<br>
                <strong>AC1 Alt:</strong> ${alt1} ft<br>
                <strong>AC2 Alt:</strong> ${alt2} ft
            `;
            infoPanel.style.display = 'block';
            selectedAircraft = null;
        } else {
            // Show aircraft info
            const hex = selectedEntity.properties.hex?.valueOf() || selectedEntity.id;
            const flight = selectedEntity.properties.flight?.valueOf();
            const altitude = selectedEntity.properties.altitude?.valueOf();
            const speed = selectedEntity.properties.speed?.valueOf();
            const track = selectedEntity.properties.track?.valueOf();

            // Store the selected aircraft hex code
            selectedAircraft = hex;
            console.log(`Selected aircraft: ${flight || hex} (hex: ${hex})`);

            const isFollowing = followMode && followedAircraft === hex;

            infoTitle.textContent = 'Aircraft Information';
            infoTitle.style.color = '#7a9a7a';
            infoPanel.classList.remove('collision-info');
            infoPanel.style.borderColor = '#7a9a7a';

            infoContent.innerHTML = `
                <strong>Flight:</strong> ${flight || 'N/A'}<br>
                <strong>ICAO:</strong> ${hex}<br>
                <strong>Altitude:</strong> ${altitude || 'N/A'} ft<br>
                <strong>Speed:</strong> ${speed || 'N/A'} kt<br>
                <strong>Heading:</strong> ${track || 'N/A'}°
                ${isFollowing ? '<br><em style="color: #9aba9a;">Following this aircraft</em>' : '<br><em style="color: #8aaa8a;">Click "Follow" to track</em>'}
            `;
            infoPanel.style.display = 'block';
        }
    } else {
        selectedAircraft = null;
        infoPanel.style.display = 'none';
        infoPanel.classList.remove('collision-info');
        infoPanel.style.borderColor = '#7a9a7a';
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

// Controls panel collapse toggle
document.getElementById('collapse-controls').addEventListener('click', () => {
    const controlsOverlay = document.getElementById('controls-overlay');
    const collapseBtn = document.getElementById('collapse-controls');

    controlsOverlay.classList.toggle('collapsed');

    if (controlsOverlay.classList.contains('collapsed')) {
        collapseBtn.textContent = '▶';
        collapseBtn.title = 'Expand Controls';
    } else {
        collapseBtn.textContent = '▼';
        collapseBtn.title = 'Collapse Controls';
    }
});

// Start polling
function startPolling() {
    loadAirports(); // Load airports once at start
    fetchAircraftData(); // Initial fetch
    fetchCollisions(); // Initial collision check

    updateInterval = setInterval(fetchAircraftData, 2000); // Update every 2 seconds
    collisionInterval = setInterval(fetchCollisions, 3000); // Check collisions every 3 seconds

    // Update probability cones when enabled
    setInterval(() => {
        if (showProbabilityCones) {
            fetchAndDisplayProbabilityCones();
        }
    }, 5000); // Update cones every 5 seconds
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
