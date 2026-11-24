// Probability cones are now computed server-side for performance
// No need to import ProbabilityCone module client-side anymore

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

// Camera controller setup
const controller = viewer.scene.screenSpaceCameraController;

// Detect if mobile device
const isMobile = /Android|webOS|iPhone|iPad|iPod|BlackBerry|IEMobile|Opera Mini/i.test(navigator.userAgent) || 
                 (window.matchMedia && window.matchMedia('(max-width: 768px)').matches);

if (isMobile) {
    // Mobile-optimized touch controls
    // One finger drag = rotate/orbit
    controller.rotateEventTypes = Cesium.CameraEventType.LEFT_DRAG;
    
    // Two finger pinch = zoom
    controller.zoomEventTypes = Cesium.CameraEventType.PINCH;
    
    // Two finger drag = tilt
    controller.tiltEventTypes = Cesium.CameraEventType.PINCH_MOVE;
    
    // Slower zoom for better control on mobile
    controller.zoomFactor = 5.0;
    
    // Higher inertia for smoother feel on touch
    controller.inertiaSpin = 0.95;
    controller.inertiaTranslate = 0.95;
    controller.inertiaZoom = 0.95;
    
    // Show mobile tip banner
    showMobileTip();
} else {
    // Desktop/trackpad controls
controller.zoomEventTypes = [
    Cesium.CameraEventType.WHEEL,
    Cesium.CameraEventType.PINCH
];
controller.rotateEventTypes = Cesium.CameraEventType.LEFT_DRAG;
controller.tiltEventTypes = [
    Cesium.CameraEventType.RIGHT_DRAG,
    Cesium.CameraEventType.MIDDLE_DRAG,
    {
        eventType: Cesium.CameraEventType.LEFT_DRAG,
        modifier: Cesium.KeyboardEventModifier.SHIFT
    }
];
controller.lookEventTypes = {
    eventType: Cesium.CameraEventType.LEFT_DRAG,
    modifier: Cesium.KeyboardEventModifier.CTRL
};
controller.translateEventTypes = {
    eventType: Cesium.CameraEventType.LEFT_DRAG,
    modifier: Cesium.KeyboardEventModifier.SHIFT | Cesium.KeyboardEventModifier.CTRL
};
    controller.zoomFactor = 3.0;
controller.inertiaSpin = 0.9;
controller.inertiaTranslate = 0.9;
controller.inertiaZoom = 0.9;
}

// Common settings
controller.minimumZoomDistance = 100;
controller.maximumZoomDistance = 40000000;
controller.enableCollisionDetection = true;

// Mobile tip banner function
function showMobileTip() {
    const banner = document.createElement('div');
    banner.id = 'mobile-tip-banner';
    banner.innerHTML = `
        <div class="mobile-tip-content">
            <span>📱 Use the controls on the right to tilt and rotate</span>
            <a href="index.html" class="mobile-tip-link">Switch to 2D</a>
            <button class="mobile-tip-close" onclick="this.parentElement.parentElement.remove()">✕</button>
        </div>
    `;
    document.body.appendChild(banner);
    
    // Auto-dismiss after 8 seconds
    setTimeout(() => {
        if (banner.parentElement) {
            banner.style.opacity = '0';
            setTimeout(() => banner.remove(), 300);
        }
    }, 8000);
}

// Mobile camera control functions
function setupMobileControls() {
    const tiltUp = document.getElementById('tilt-up');
    const tiltDown = document.getElementById('tilt-down');
    const rotateLeft = document.getElementById('rotate-left');
    const rotateRight = document.getElementById('rotate-right');
    const viewTop = document.getElementById('view-top');
    const viewAngle = document.getElementById('view-angle');
    const viewHorizon = document.getElementById('view-horizon');
    
    if (!tiltUp) return; // Controls not in DOM
    
    const TILT_STEP = 10; // degrees
    const ROTATE_STEP = 15; // degrees
    
    // Helper to adjust camera pitch (tilt)
    function adjustPitch(deltaDegrees) {
        const camera = viewer.camera;
        const currentPitch = Cesium.Math.toDegrees(camera.pitch);
        let newPitch = currentPitch + deltaDegrees;
        // Clamp pitch between -90 (straight down) and 0 (horizon)
        newPitch = Math.max(-90, Math.min(0, newPitch));
        
        camera.setView({
            destination: camera.position,
            orientation: {
                heading: camera.heading,
                pitch: Cesium.Math.toRadians(newPitch),
                roll: camera.roll
            }
        });
    }
    
    // Helper to adjust camera heading (rotate)
    function adjustHeading(deltaDegrees) {
        const camera = viewer.camera;
        const newHeading = camera.heading + Cesium.Math.toRadians(deltaDegrees);
        
        camera.setView({
            destination: camera.position,
            orientation: {
                heading: newHeading,
                pitch: camera.pitch,
                roll: camera.roll
            }
        });
    }
    
    // Helper to set preset view
    function setPresetView(pitchDegrees) {
        const camera = viewer.camera;
        camera.setView({
            destination: camera.position,
            orientation: {
                heading: camera.heading,
                pitch: Cesium.Math.toRadians(pitchDegrees),
                roll: 0
            }
        });
    }
    
    // Tilt controls
    tiltUp.addEventListener('click', () => adjustPitch(TILT_STEP));
    tiltDown.addEventListener('click', () => adjustPitch(-TILT_STEP));
    
    // For continuous press on tilt
    let tiltInterval = null;
    
    function startContinuousTilt(direction) {
        if (tiltInterval) return;
        adjustPitch(direction * TILT_STEP);
        tiltInterval = setInterval(() => adjustPitch(direction * TILT_STEP * 0.5), 100);
    }
    
    function stopContinuousTilt() {
        if (tiltInterval) {
            clearInterval(tiltInterval);
            tiltInterval = null;
        }
    }
    
    // Long press for continuous tilt
    tiltUp.addEventListener('touchstart', (e) => { e.preventDefault(); startContinuousTilt(1); });
    tiltUp.addEventListener('touchend', stopContinuousTilt);
    tiltUp.addEventListener('touchcancel', stopContinuousTilt);
    
    tiltDown.addEventListener('touchstart', (e) => { e.preventDefault(); startContinuousTilt(-1); });
    tiltDown.addEventListener('touchend', stopContinuousTilt);
    tiltDown.addEventListener('touchcancel', stopContinuousTilt);
    
    // Rotate controls
    rotateLeft.addEventListener('click', () => adjustHeading(-ROTATE_STEP));
    rotateRight.addEventListener('click', () => adjustHeading(ROTATE_STEP));
    
    // Continuous rotate on long press
    let rotateInterval = null;
    
    function startContinuousRotate(direction) {
        if (rotateInterval) return;
        adjustHeading(direction * ROTATE_STEP);
        rotateInterval = setInterval(() => adjustHeading(direction * ROTATE_STEP * 0.5), 100);
    }
    
    function stopContinuousRotate() {
        if (rotateInterval) {
            clearInterval(rotateInterval);
            rotateInterval = null;
        }
    }
    
    rotateLeft.addEventListener('touchstart', (e) => { e.preventDefault(); startContinuousRotate(-1); });
    rotateLeft.addEventListener('touchend', stopContinuousRotate);
    rotateLeft.addEventListener('touchcancel', stopContinuousRotate);
    
    rotateRight.addEventListener('touchstart', (e) => { e.preventDefault(); startContinuousRotate(1); });
    rotateRight.addEventListener('touchend', stopContinuousRotate);
    rotateRight.addEventListener('touchcancel', stopContinuousRotate);
    
    // Preset view buttons
    viewTop.addEventListener('click', () => setPresetView(-90)); // Straight down
    viewAngle.addEventListener('click', () => setPresetView(-45)); // 45 degree angle
    viewHorizon.addEventListener('click', () => setPresetView(-15)); // Near horizon
    
    console.log('Mobile controls initialized');
}

// Initialize mobile controls when DOM is ready
if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', setupMobileControls);
} else {
    setupMobileControls();
}

// Initial camera configuration
const INITIAL_CAMERA_CONFIG = {
    longitude: -87.6298,  // Chicago longitude
    latitude: 36.0,       // Further south to see Chicago area with tilt
    altitude: 550000,     // 550km above ground
    heading: 0,           // Degrees (0 = North)
    pitch: -45,           // Degrees (-45 = nice tilted view, -90 = straight down)
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

/**
 * Check if an aircraft is a helicopter
 * @param {Object} aircraft - Aircraft object with type field from server
 * @returns {boolean} True if helicopter (type 6)
 */
function isHelicopter(aircraft) {
    // Type field is provided by server (6 = Rotorcraft/Helicopter)
    return aircraft.type === 6;
}

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
    const isHeli = isHelicopter(aircraft);

    // Create entity with appropriate model (airplane or helicopter)
    const entity = viewer.entities.add({
        id: aircraft.hex,
        position: positionMeters,
        model: {
            uri: isHeli ? './helicopter.glb' : 'https://raw.githubusercontent.com/CesiumGS/cesium/main/Apps/SampleData/models/CesiumAir/Cesium_Air.glb',
            minimumPixelSize: 64,
            maximumScale: 20000,
            color: Cesium.Color.WHITE,
            silhouetteColor: isHeli ? Cesium.Color.YELLOW : Cesium.Color.CYAN,
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
            fillColor: isHeli ? Cesium.Color.YELLOW : Cesium.Color.CYAN, // Yellow for helicopters
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
            track: aircraft.track,
            vertical_rate: aircraft.vertical_rate
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
        track: aircraft.track,
        vertical_rate: aircraft.vertical_rate
    };

    // Add to trail if enabled: a point every 2 s (the poll's rate), so 50
    // points still span ~100 s when the live feed updates 10 times a second
    if (showTrails) {
        const now = performance.now();
        if (now - (lastTrailPointAt.get(aircraft.hex) ?? -Infinity) >= TRAIL_POINT_MS) {
            lastTrailPointAt.set(aircraft.hex, now);
            addToTrail(aircraft.hex, newPosition);
        }
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
    lastTrailPointAt.clear();
}

// Fetch and update aircraft data
// NEW: Use enriched endpoint with server-computed probability cones
async function fetchAircraftData() {
    try {
        // Use enriched endpoint that includes trails and pre-computed probability cones
        const response = await fetch('/api/aircraft/enriched?window=0&trails=true&cones=true');
        if (!response.ok) {
            throw new Error('Failed to fetch aircraft data');
        }

        const data = await response.json();
        // The server-side extras the WebSocket frames do not carry (aircraft
        // type, probability cones, trail). While the live feed is in sync it
        // draws the aircraft; the poll only refreshes these.
        liveMeta = new Map((data.aircraft || []).map((ac) => [ac.hex, ac]));
        if (liveFeed.synced) return;
        updateStatus(true);
        document.getElementById('aircraft-count').textContent = data.aircraft?.length || 0;

        currentAircraftData = data.aircraft || []; // Store for collision line updates

        // Log cache performance
        if (data.cacheStats) {
            console.log(`📦 Cache: ${data.cacheStats.hitRate} hit rate, ${data.cacheStats.size} entries`);
        }

        // Log enriched data stats
        if (data.stats) {
            console.log(`📊 Enriched data: ${data.stats.total} aircraft, ${data.stats.withTrails} with trails, ${data.stats.withCones} with cones`);
        }

        // Debug: Log vertical rate statistics
        const vrStats = currentAircraftData.reduce((acc, ac) => {
            if (ac.vertical_rate !== null && ac.vertical_rate !== undefined) {
                if (Math.abs(ac.vertical_rate) > 100) {
                    acc.active++;
                    if (ac.vertical_rate > 100) acc.climbing++;
                    if (ac.vertical_rate < -100) acc.descending++;
                } else {
                    acc.level++;
                }
            } else {
                acc.missing++;
            }
            return acc;
        }, { climbing: 0, descending: 0, level: 0, missing: 0, active: 0 });

        if (vrStats.active > 0 || vrStats.missing > 0) {
            console.log(`Vertical Rate: ${vrStats.climbing}↑ climbing, ${vrStats.descending}↓ descending, ${vrStats.level} level, ${vrStats.missing} missing data`);
        }

        updateAircraftOn3DGlobe(currentAircraftData);

        // Update aircraft list if it's open
        const listPanel = document.getElementById('aircraft-list-panel');
        if (listPanel && listPanel.classList.contains('open')) {
            updateAircraftList();
        }
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

        let entity;
        if (aircraftEntities.has(aircraft.hex)) {
            // Update existing aircraft
            entity = aircraftEntities.get(aircraft.hex);
            updateAircraftEntity(entity, aircraft);
        } else {
            // Create new aircraft
            entity = createAircraftEntity(aircraft);
            aircraftEntities.set(aircraft.hex, entity);
        }
        
        // Apply grounded filter
        if (hideGroundedPlanes) {
            const altitude = aircraft.altitude || 0;
            entity.show = altitude >= 100; // Hide if below 100 feet
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
            lastTrailPointAt.delete(hex);
        }
    });

    // Follow mode: keep camera on followed aircraft
    // At most one camera flight per FOLLOW_FLY_MS: each lasts 1.5 s, and the
    // live feed redraws far more often than that.
    const followDue = performance.now() - lastFollowFlyAt >= FOLLOW_FLY_MS;
    if (followMode && followedAircraft && aircraftEntities.has(followedAircraft) && followDue) {
        lastFollowFlyAt = performance.now();
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

    if (!liveFeed.synced) console.log(`3D Globe updated: ${seenAircraftIds.size} aircraft visible`);
}

// Kalman filter-based collision detection and probability cone visualization
let collisionLines = [];
let probabilityCones = [];
let probabilityConesByHex = new Map(); // Track cones by aircraft hex for efficient updates
let showProbabilityCones = false;
let hideGroundedPlanes = false;
let showStatistics = false;
let collisionInterval;
let statisticsInterval;
let isUpdatingCones = false; // Prevent concurrent cone updates
let lastConeUpdateTimestamp = 0; // Track when we last updated cones to prevent unnecessary refreshes

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
        const dashMaterial1 = new Cesium.PolylineDashMaterialProperty({
            color: color,
            dashLength: 12.0
        });
        const trajectory1 = viewer.entities.add({
            name: `${collision.aircraft1} trajectory`,
            polyline: {
                positions: [currentPos1, collisionPointPos],
                width: 3,
                material: dashMaterial1,
                depthFailMaterial: dashMaterial1  // Show even when behind terrain
            }
        });
        collisionLines.push(trajectory1);

        // Draw trajectory line for aircraft 2: current position → collision point
        const dashMaterial2 = new Cesium.PolylineDashMaterialProperty({
            color: color,
            dashLength: 12.0
        });
        const trajectory2 = viewer.entities.add({
            name: `${collision.aircraft2} trajectory`,
            polyline: {
                positions: [currentPos2, collisionPointPos],
                width: 3,
                material: dashMaterial2,
                depthFailMaterial: dashMaterial2  // Show even when behind terrain
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
                outlineWidth: 2,
                disableDepthTestDistance: Number.POSITIVE_INFINITY
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
// NEW: Simplified - uses server-computed cones from enriched endpoint
async function fetchAndDisplayProbabilityCones() {
    if (!showProbabilityCones) {
        return;
    }

    // Use currentAircraftData which now includes pre-computed probability cones
    // No need to fetch separately - cones are already computed server-side
    displayProbabilityCones(currentAircraftData);
}

// NEW: Display probability cones using server-computed predictions
async function displayProbabilityCones(aircraft) {
    // Check if we should show cones BEFORE doing anything
    if (!showProbabilityCones || !aircraft || aircraft.length === 0) {
        return;
    }

    // Prevent concurrent updates
    if (isUpdatingCones) {
        console.log('⏭️  Skipping cone update - already in progress');
        return;
    }

    // Use the server's 'now' timestamp to detect when data actually changes
    // This prevents unnecessary updates while still allowing smooth refreshes
    const dataTimestamp = Date.now();
    const timeSinceLastUpdate = dataTimestamp - lastConeUpdateTimestamp;

    // Only update if enough time has passed (2 seconds) OR if we have no cones yet
    // This matches the server cache TTL and prevents flickering
    if (timeSinceLastUpdate < 2000 && probabilityCones.length > 0) {
        // Too soon since last update, skip to prevent flickering
        return;
    }

    isUpdatingCones = true;

    try {
        let conesCreated = 0;
        let conesUpdated = 0;
        let conesReused = 0;
        const numSegments = 16; // Number of segments around the cone edge
        const newCones = []; // Build new cones in temporary array
        const processedHexCodes = new Set(); // Track which aircraft we've processed

        // Process each aircraft
        for (const ac of aircraft) {
            // Early exit if cones were toggled off during update
            if (!showProbabilityCones) {
                console.log('⏹️ Cones toggled off - stopping cone creation');
                break;
            }

            if (!ac.lat || !ac.lon || !ac.hex) continue;

            try {
                // Use server-computed probability cone (no need to fetch or compute client-side!)
                const predictions = ac.probabilityCone;

                // Skip if no predictions available from server
                if (!predictions || predictions.length === 0) continue;

                // Track that we've processed this aircraft
                processedHexCodes.add(ac.hex);

                // Check if we already have cones for this aircraft
                const existingConeData = probabilityConesByHex.get(ac.hex);

                // If cones exist and predictions haven't changed significantly, reuse them
                if (existingConeData && existingConeData.predictions) {
                    const lastPred = existingConeData.predictions[existingConeData.predictions.length - 1];
                    const currentLastPred = predictions[predictions.length - 1];

                    // Check if predictions are similar (within 100m in lat/lon/alt)
                    const latDiff = Math.abs(lastPred.lat - currentLastPred.lat);
                    const lonDiff = Math.abs(lastPred.lon - currentLastPred.lon);
                    const altDiff = Math.abs(lastPred.altitude - currentLastPred.altitude);

                    // If predictions haven't changed much, reuse existing cones
                    if (latDiff < 0.001 && lonDiff < 0.001 && altDiff < 100) {
                        // Reuse existing cone entities
                        conesReused++;
                        continue;
                    } else {
                        // Predictions changed - remove old cones for this aircraft
                        if (existingConeData.entities) {
                            existingConeData.entities.forEach(entity => viewer.entities.remove(entity));
                        }
                        conesUpdated++;
                    }
                }

                // Create new cone entities for this aircraft
                const aircraftConeEntities = [];

                const altitude = ac.altitude || 10000;
                const altitudeMeters = altitude * 0.3048;

                // Color based on altitude
                let color;
                if (altitude < 10000) {
                    color = Cesium.Color.LIME;
                } else if (altitude < 25000) {
                    color = Cesium.Color.YELLOW;
                } else {
                    color = Cesium.Color.ORANGE;
                }

                // Validate prediction data before creating cone
                if (!predictions || predictions.length < 2) {
                    console.warn(`Not enough predictions for ${ac.hex}`);
                    continue;
                }

                // Apex position (current aircraft position)
                if (!ac.lon || !ac.lat || !ac.altitude) {
                    console.warn(`Invalid aircraft position for ${ac.hex}`);
                    continue;
                }
                const apexPosition = Cesium.Cartesian3.fromDegrees(ac.lon, ac.lat, altitudeMeters);

                // Use current position and a future prediction point to get trajectory direction
                // Use a point further out (e.g., index 5) for better direction calculation
                const futureIndex = Math.min(5, predictions.length - 1);
                const futurePrediction = predictions[futureIndex];

                // Validate prediction coordinates
                if (!futurePrediction.lat || !futurePrediction.lon || !futurePrediction.altitude) {
                    console.warn(`Invalid prediction coordinates for ${ac.hex}`);
                    continue;
                }

                // Calculate direction from current position to future prediction
                const futurePredPos = Cesium.Cartesian3.fromDegrees(
                    futurePrediction.lon,
                    futurePrediction.lat,
                    futurePrediction.altitude * 0.3048
                );

                const direction = Cesium.Cartesian3.subtract(futurePredPos, apexPosition, new Cesium.Cartesian3());

                // Validate direction vector (use 100 meters minimum - about 5-10 seconds of flight)
                const directionMagnitude = Cesium.Cartesian3.magnitude(direction);
                if (directionMagnitude < 100) {
                    // Aircraft hasn't moved enough to establish a clear direction
                    continue;
                }
                Cesium.Cartesian3.normalize(direction, direction);

                // Use the last prediction point for the cone endpoint
                const lastPrediction = predictions[predictions.length - 1];

                // Validate last prediction
                if (!lastPrediction.lat || !lastPrediction.lon || !lastPrediction.altitude || !lastPrediction.radius) {
                    console.warn(`Invalid last prediction for ${ac.hex}`);
                    continue;
                }

                // The cone endpoint is the last prediction point
                const futureAltMeters = lastPrediction.altitude * 0.3048;
                const predictedPosition = Cesium.Cartesian3.fromDegrees(
                    lastPrediction.lon,
                    lastPrediction.lat,
                    futureAltMeters
                );

                // Use the radius from Kalman filter uncertainty
                const coneRadius = lastPrediction.radius;

                // Server-computed predictions are already correct - use them directly!
                // No need for mirror fix anymore since Kalman filter runs server-side
                const futurePosition = predictedPosition;

                // Direction from current position to future position
                const actualDirection = Cesium.Cartesian3.subtract(futurePosition, apexPosition, new Cesium.Cartesian3());

                // Debug: Log positions for one aircraft to check direction
                if (conesCreated === 0) {
                    console.log(`[${ac.hex}] Current: ${ac.lat.toFixed(4)}, ${ac.lon.toFixed(4)}, ${ac.altitude}ft`);
                    console.log(`[${ac.hex}] Track: ${ac.track}°, Speed: ${ac.speed} kts`);
                }

                Cesium.Cartesian3.normalize(actualDirection, actualDirection);

                // Get perpendicular vectors for creating the cone base circle
                const up = Cesium.Cartesian3.normalize(apexPosition, new Cesium.Cartesian3());
                let perpendicular = Cesium.Cartesian3.cross(actualDirection, up, new Cesium.Cartesian3());

                if (Cesium.Cartesian3.magnitude(perpendicular) < 0.001) {
                    perpendicular = new Cesium.Cartesian3(1, 0, 0);
                }
                Cesium.Cartesian3.normalize(perpendicular, perpendicular);

                const perpendicular2 = Cesium.Cartesian3.cross(actualDirection, perpendicular, new Cesium.Cartesian3());
                Cesium.Cartesian3.normalize(perpendicular2, perpendicular2);

                // Generate edge positions around the cone base
                const edgePositions = [];
                for (let i = 0; i <= numSegments; i++) {
                    const angle = (i / numSegments) * 2 * Math.PI;

                    const offset = new Cesium.Cartesian3();
                    Cesium.Cartesian3.multiplyByScalar(perpendicular, coneRadius * Math.cos(angle), offset);
                    const offset2 = new Cesium.Cartesian3();
                    Cesium.Cartesian3.multiplyByScalar(perpendicular2, coneRadius * Math.sin(angle), offset2);
                    Cesium.Cartesian3.add(offset, offset2, offset);

                    const edgePoint = Cesium.Cartesian3.add(futurePosition, offset, new Cesium.Cartesian3());
                    edgePositions.push(edgePoint);
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
                    aircraftConeEntities.push(coneSegment);
                    newCones.push(coneSegment);
                }

                // Center trajectory line
                const centerLine = viewer.entities.add({
                    polyline: {
                        positions: [apexPosition, futurePosition],
                        width: 2,
                        material: color.withAlpha(0.8)
                    }
                });
                aircraftConeEntities.push(centerLine);
                newCones.push(centerLine);

                // Cone base outline
                const coneOutline = viewer.entities.add({
                    polyline: {
                        positions: edgePositions,
                        width: 1,
                        material: color.withAlpha(0.5)
                    }
                });
                aircraftConeEntities.push(coneOutline);
                newCones.push(coneOutline);

                // Edge lines from apex to base (every 4th point)
                for (let i = 0; i < numSegments; i += 4) {
                    const edgeLine = viewer.entities.add({
                        polyline: {
                            positions: [apexPosition, edgePositions[i]],
                            width: 1,
                            material: color.withAlpha(0.3)
                        }
                    });
                    aircraftConeEntities.push(edgeLine);
                    newCones.push(edgeLine);
                }

                // Store cone entities and predictions for this aircraft
                probabilityConesByHex.set(ac.hex, {
                    entities: aircraftConeEntities,
                    predictions: predictions
                });

                conesCreated++;
            } catch (error) {
                console.error(`Error creating cone for ${ac.hex}:`, error);
            }
        }

        // Check again if cones should be shown (user may have toggled off during update)
        if (!showProbabilityCones) {
            // User toggled off - remove any new cones we created
            newCones.forEach(entity => viewer.entities.remove(entity));
            console.log('⏹️ Cones disabled during update - cleaned up');
            return;
        }

        // Remove cones for aircraft that are no longer present
        let conesRemoved = 0;
        for (const [hex, coneData] of probabilityConesByHex.entries()) {
            if (!processedHexCodes.has(hex)) {
                // Aircraft no longer exists - remove its cones
                if (coneData.entities) {
                    coneData.entities.forEach(entity => viewer.entities.remove(entity));
                }
                probabilityConesByHex.delete(hex);
                conesRemoved++;
            }
        }

        // Update the global array with all current cones
        probabilityCones = newCones;

        // Update timestamp to track this cone generation
        lastConeUpdateTimestamp = dataTimestamp;

        // Log update statistics
        if (conesCreated > 0 || conesUpdated > 0 || conesRemoved > 0) {
            console.log(`📊 Cones: ${conesCreated} created, ${conesUpdated} updated, ${conesReused} reused, ${conesRemoved} removed`);
        } else if (conesReused > 0) {
            console.log(`📊 Cones: ${conesReused} reused (no changes needed)`);
        }

    } finally {
        // Reset the flag to allow next update
        isUpdatingCones = false;
    }
}

// Toggle probability cone visibility
function toggleProbabilityCones() {
    showProbabilityCones = !showProbabilityCones;

    if (showProbabilityCones) {
        fetchAndDisplayProbabilityCones();
    } else {
        // Remove all cones from our tracking array
        probabilityCones.forEach(entity => {
            try { viewer.entities.remove(entity); } catch(e) {}
        });
        probabilityCones = [];

        // Clear the hex-based tracking map and remove all associated entities
        for (const [hex, coneData] of probabilityConesByHex.entries()) {
            if (coneData.entities) {
                coneData.entities.forEach(entity => {
                    try { viewer.entities.remove(entity); } catch(e) {}
                });
            }
        }
        probabilityConesByHex.clear();

        // Also remove any cone entities that might have been added during an in-progress update
        // by scanning all entities and removing ones with "Cone:" prefix
        const entitiesToRemove = [];
        viewer.entities.values.forEach(entity => {
            if (entity.name && entity.name.startsWith('Cone:')) {
                entitiesToRemove.push(entity);
            }
        });
        entitiesToRemove.forEach(entity => {
            try { viewer.entities.remove(entity); } catch(e) {}
        });

        console.log(`🗑️ Removed ${entitiesToRemove.length} cone entities`);
    }

    const button = document.getElementById('toggle-probability-cones');
    if (button) {
        button.textContent = showProbabilityCones ? '🔵 Prediction Cones' : '⚪ Prediction Cones';
    }
}

// Toggle hide grounded planes
function toggleHideGroundedPlanes() {
    hideGroundedPlanes = !hideGroundedPlanes;
    
    // Update visibility of existing aircraft
    aircraftEntities.forEach((entity, hex) => {
        const altitude = entity.properties.altitude?.valueOf() || 0;
        const isGrounded = altitude < 100; // Less than 100 feet = grounded
        
        if (hideGroundedPlanes && isGrounded) {
            entity.show = false;
        } else {
            entity.show = true;
        }
    });
    
    const button = document.getElementById('toggle-grounded');
    if (button) {
        button.textContent = hideGroundedPlanes ? '🔵 Grounded Hidden' : '⚪ Hide Grounded';
    }
}

// Statistics functions
function toggleStatistics() {
    showStatistics = !showStatistics;
    const panel = document.getElementById('statistics-panel');
    if (panel) {
        panel.classList.toggle('open', showStatistics);
    }

    const button = document.getElementById('toggle-statistics');
    if (button) {
        button.textContent = showStatistics ? '📊 Stats ✓' : '📊 Stats';
    }

    // Close aircraft list panel if statistics is being opened (both on right side)
    if (showStatistics) {
        const listPanel = document.getElementById('aircraft-list-panel');
        if (listPanel && listPanel.classList.contains('open')) {
            toggleAircraftList();
        }
        fetchStatistics();
    }
}

// Format duration from seconds
function formatDuration(seconds) {
    if (!seconds || seconds < 0) return '0s';
    const hours = Math.floor(seconds / 3600);
    const minutes = Math.floor((seconds % 3600) / 60);
    const secs = Math.floor(seconds % 60);

    if (hours > 0) {
        return `${hours}h ${minutes}m`;
    } else if (minutes > 0) {
        return `${minutes}m ${secs}s`;
    } else {
        return `${secs}s`;
    }
}

// Fetch statistics from backend
async function fetchStatistics() {
    try {
        const response = await fetch('/api/statistics?window=60');
        if (!response.ok) {
            throw new Error('Failed to fetch statistics');
        }
        const data = await response.json();
        updateStatisticsDashboard(data);
    } catch (error) {
        console.error('Error fetching statistics:', error);
    }
}

// Update statistics dashboard
function updateStatisticsDashboard(data) {
    const dashboardElement = document.getElementById('statistics-content');
    if (!dashboardElement) return;

    const { summary, altitude_distribution, flight_durations } = data;

    // Build altitude distribution bars
    const altitudeHTML = altitude_distribution.map(item => {
        const maxCount = Math.max(10, ...altitude_distribution.map(i => i.count));
        const barWidth = Math.min((item.count / maxCount) * 100, 100);
        return `
            <div class="stat-bar-container-3d">
                <span class="stat-bar-label-3d">${item.range}</span>
                <div class="stat-bar-bg-3d">
                    <div class="stat-bar-3d" style="width: ${barWidth}%"></div>
                </div>
                <span class="stat-bar-value-3d">${item.count}</span>
            </div>
        `;
    }).join('');

    // Build flight durations list (all active flights)
    const allFlights = flight_durations;
    const durationsHTML = allFlights.map(item => {
        return `
            <div class="duration-item-3d">
                <span class="duration-flight-3d">${item.flight || item.hex}</span>
                <span class="duration-time-3d">${formatDuration(item.duration_seconds)}</span>
            </div>
        `;
    }).join('') || '<div style="color: #7a9a7a; font-size: 10px;">No active flights</div>';

    dashboardElement.innerHTML = `
        <div class="stats-summary-3d">
            <div class="stat-card-3d">
                <div class="stat-card-label-3d">Flights</div>
                <div class="stat-card-value-3d">${summary.total_flights}</div>
            </div>
            <div class="stat-card-3d">
                <div class="stat-card-label-3d">Avg Speed</div>
                <div class="stat-card-value-3d">${summary.avg_speed.toFixed(0)}</div>
            </div>
            <div class="stat-card-3d">
                <div class="stat-card-label-3d">Avg Alt</div>
                <div class="stat-card-value-3d">${(summary.avg_altitude / 1000).toFixed(1)}k</div>
            </div>
        </div>

        <div class="stats-section-3d">
            <h4>Altitude Distribution</h4>
            ${altitudeHTML || '<div style="color: #7a9a7a; font-size: 10px;">No data</div>'}
        </div>

        <div class="stats-section-3d">
            <h4>Flight Durations (All Active)</h4>
            <div class="durations-list-3d">
                ${durationsHTML}
            </div>
        </div>
    `;
}

// Aircraft list functions
function toggleAircraftList() {
    const panel = document.getElementById('aircraft-list-panel');
    if (!panel) return;

    const isOpen = panel.classList.contains('open');
    panel.classList.toggle('open', !isOpen);

    const button = document.getElementById('toggle-list');
    if (button) {
        button.textContent = !isOpen ? '📋 Aircraft List ✓' : '📋 Aircraft List';
    }

    // Close statistics panel if aircraft list is being opened (both on right side)
    if (!isOpen) {
        const statsPanel = document.getElementById('statistics-panel');
        if (statsPanel && statsPanel.classList.contains('open')) {
            toggleStatistics();
        }
        updateAircraftList();
    }
}

// Update aircraft list panel
function updateAircraftList() {
    const listElement = document.getElementById('aircraft-list');
    if (!listElement) return;

    if (!currentAircraftData || currentAircraftData.length === 0) {
        listElement.innerHTML = '<div class="loading-state">No aircraft detected</div>';
        return;
    }

    // Sort by altitude (descending)
    const sortedAircraft = [...currentAircraftData].sort((a, b) => (b.altitude || 0) - (a.altitude || 0));

    const html = sortedAircraft.map(ac => {
        const callsign = ac.flight ? ac.flight.trim() : 'N/A';
        const hex = ac.hex || 'Unknown';
        const altitude = ac.altitude ? `${Math.round(ac.altitude)} ft` : 'N/A';
        const speed = ac.speed ? `${Math.round(ac.speed)} kts` : 'N/A';
        const heading = ac.track !== null && ac.track !== undefined ? `${Math.round(ac.track)}°` : 'N/A';
        const vRate = ac.vertical_rate ? `${ac.vertical_rate > 0 ? '+' : ''}${ac.vertical_rate} fpm` : 'Level';

        return `
            <div class="aircraft-item-3d" onclick="focusOnAircraft('${hex}')">
                <div class="aircraft-item-header-3d">
                    <span class="aircraft-callsign-3d">${callsign}</span>
                    <span class="aircraft-hex-3d">${hex}</span>
                </div>
                <div class="aircraft-details-3d">
                    <div class="aircraft-detail-3d"><strong>Alt:</strong> ${altitude}</div>
                    <div class="aircraft-detail-3d"><strong>Spd:</strong> ${speed}</div>
                    <div class="aircraft-detail-3d"><strong>Hdg:</strong> ${heading}</div>
                    <div class="aircraft-detail-3d"><strong>V/S:</strong> ${vRate}</div>
                </div>
            </div>
        `;
    }).join('');

    listElement.innerHTML = html;
}

// Focus camera on specific aircraft
function focusOnAircraft(hex) {
    const aircraft = currentAircraftData.find(ac => ac.hex === hex);
    if (!aircraft || !aircraft.lat || !aircraft.lon) return;

    const altitude = (aircraft.altitude || 10000) * 0.3048; // Convert to meters
    const position = Cesium.Cartesian3.fromDegrees(aircraft.lon, aircraft.lat, altitude);

    // Fly to aircraft with nice animation - use lookAt for proper offset
    const distance = 100000; // 100km distance
    const pitch = Cesium.Math.toRadians(-30);
    const heading = Cesium.Math.toRadians(aircraft.track || 0);

    viewer.camera.flyToBoundingSphere(
        new Cesium.BoundingSphere(position, 1),
        {
            duration: 2,
            offset: new Cesium.HeadingPitchRange(heading, pitch, distance)
        }
    );
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
                    height: isLarge ? 16 : 12,
                    verticalOrigin: Cesium.VerticalOrigin.CENTER,
                    horizontalOrigin: Cesium.HorizontalOrigin.CENTER,
                    // Only disable depth test when close - allows Earth to occlude distant airports
                    disableDepthTestDistance: isLarge ? 500000 : 200000,
                    heightReference: Cesium.HeightReference.RELATIVE_TO_GROUND
                },
                label: {
                    text: props.iata || props.icao || '',
                    font: isLarge ? 'bold 11px Courier New' : '9px Courier New',
                    fillColor: color,
                    outlineColor: Cesium.Color.BLACK,
                    outlineWidth: 2,
                    style: Cesium.LabelStyle.FILL_AND_OUTLINE,
                    verticalOrigin: Cesium.VerticalOrigin.TOP,
                    pixelOffset: new Cesium.Cartesian2(0, 12),
                    // Only disable depth test when close
                    disableDepthTestDistance: isLarge ? 500000 : 200000,
                    show: true
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

// Toggle trails - now fetches historical data like 2D view
async function toggleTrails() {
    showTrails = !showTrails;

    if (!showTrails) {
        clearTrails();
    } else {
        // Fetch historical trails for all current aircraft
        const promises = currentAircraftData.map(aircraft =>
            fetchAndDisplayHistoricalTrail(aircraft.hex)
        );
        await Promise.all(promises);
    }

    document.getElementById('toggle-trails').classList.toggle('active', showTrails);
}

/**
 * Fetch and display historical trail for a specific aircraft in 3D
 * @param {string} hex - Aircraft ICAO hex code
 */
async function fetchAndDisplayHistoricalTrail(hex) {
    try {
        const response = await fetch(`/api/aircraft/${hex}/trail?minutes=120&limit=500`);
        if (!response.ok) {
            return;
        }

        const data = await response.json();

        if (!data.trail || data.trail.length === 0) {
            return;
        }

        // Convert trail to Cesium positions (ECEF coordinates with altitude in meters)
        const positions = data.trail.map(point =>
            Cesium.Cartesian3.fromDegrees(
                point.lon,
                point.lat,
                point.altitude * 0.3048 // Convert feet to meters
            )
        );

        // Remove existing trail if present
        if (aircraftTrails.has(hex)) {
            viewer.entities.remove(aircraftTrails.get(hex));
        }

        // Check if this is a helicopter
        const aircraft = currentAircraftData.find(ac => ac.hex === hex);
        const isHeli = aircraft && aircraft.type === 6;

        // Create polyline for historical trail
        const trail = viewer.entities.add({
            polyline: {
                positions: positions,
                width: 3,
                material: new Cesium.PolylineGlowMaterialProperty({
                    glowPower: 0.3,
                    color: isHeli ? Cesium.Color.YELLOW.withAlpha(0.7) : Cesium.Color.CYAN.withAlpha(0.7)
                }),
                clampToGround: false
            }
        });

        aircraftTrails.set(hex, trail);

    } catch (error) {
        console.error(`Error fetching historical trail for ${hex}:`, error);
    }
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

        // Update last update timestamp
        const now = new Date();
        const timeString = now.toLocaleTimeString('en-US', {
            hour12: false,
            hour: '2-digit',
            minute: '2-digit',
            second: '2-digit'
        });
        const lastUpdateElement = document.getElementById('last-update');
        if (lastUpdateElement) {
            lastUpdateElement.textContent = timeString;
        }
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
        const verticalRate = selectedEntity.properties.vertical_rate?.valueOf();

        // Store the selected aircraft hex code
        selectedAircraft = hex;
        console.log(`Selected aircraft: ${flight || hex} (hex: ${hex})`);

        const isFollowing = followMode && followedAircraft === hex;

            infoTitle.textContent = 'Aircraft Information';
            infoTitle.style.color = '#7a9a7a';
            infoPanel.classList.remove('collision-info');
            infoPanel.style.borderColor = '#7a9a7a';

        // Format vertical rate display
        let vrDisplay = 'Level';
        if (verticalRate && Math.abs(verticalRate) > 64) { // Ignore noise below ~64 ft/min
            const direction = verticalRate > 0 ? '↑' : '↓';
            vrDisplay = `${direction} ${Math.abs(verticalRate)} ft/min`;
        }

        infoContent.innerHTML = `
            <strong>Flight:</strong> ${flight || 'N/A'}<br>
            <strong>ICAO:</strong> ${hex}<br>
            <strong>Altitude:</strong> ${altitude || 'N/A'} ft<br>
            <strong>Vertical Rate:</strong> ${vrDisplay}<br>
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

// ---------------------------------------------------------------------------
// Live feed (live-feed.js, shared with the 2D map)
//
// Served by the WebSocket gateway, the globe draws the live track table within
// 100 ms of each update instead of every 2 s. The 2 s poll keeps running: it
// draws while the feed is out of sync (and always without a gateway, under the
// Fall 2025 server.js), and otherwise only refreshes the server-side extras
// (type, probability cones, trail) merged in here.
// ---------------------------------------------------------------------------
const TRAIL_POINT_MS = 2000;
const FOLLOW_FLY_MS = 1500;
const LIST_REFRESH_MS = 1000;
const META_FIELDS = ['type', 'probabilityCone', 'trail'];
let liveFeed = { synced: false };
let liveMeta = new Map(); // hex -> the last poll's aircraft
const lastTrailPointAt = new Map(); // hex -> performance.now() of the last trail point
let lastFollowFlyAt = -Infinity;
let lastListRefreshAt = -Infinity;

function renderLive3D(aircraft) {
    for (const a of aircraft) {
        const m = liveMeta.get(a.hex);
        if (!m) continue;
        for (const k of META_FIELDS) if (m[k] !== undefined) a[k] = m[k];
    }
    document.getElementById('aircraft-count').textContent = aircraft.length;
    currentAircraftData = aircraft;
    updateAircraftOn3DGlobe(currentAircraftData);
    const listPanel = document.getElementById('aircraft-list-panel');
    if (listPanel && listPanel.classList.contains('open') &&
        performance.now() - lastListRefreshAt >= LIST_REFRESH_MS) {
        lastListRefreshAt = performance.now();
        updateAircraftList();
    }
}

function startLiveFeed3D() {
    liveFeed = window.startLiveFeed({ onRender: renderLive3D, onStatus: updateStatus });
}
// For tests and measurement, like window.__gateway (the feed itself).
window.__globe = { viewer, aircraftEntities, aircraft: () => currentAircraftData };

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
    
    // Update statistics when panel is open
    statisticsInterval = setInterval(() => {
        if (showStatistics) {
            fetchStatistics();
        }
    }, 5000); // Update stats every 5 seconds
}

// Stop polling
function stopPolling() {
    if (updateInterval) {
        clearInterval(updateInterval);
    }
}

// Expose functions to global scope for HTML onclick handlers
window.toggleProbabilityCones = toggleProbabilityCones;
window.toggleHideGroundedPlanes = toggleHideGroundedPlanes;
window.toggleStatistics = toggleStatistics;
window.toggleAircraftList = toggleAircraftList;
window.focusOnAircraft = focusOnAircraft;

// Initialize
updateStatus(false, 'Connecting...');
startPolling();
startLiveFeed3D();

// Cleanup on page unload
window.addEventListener('beforeunload', stopPolling);
