// ---------------------------------------------------------------------------
// ADS-B Atlas, 3D view (Cesium).
//
// The same data and logic as before: aircraft from /api/aircraft/enriched
// every 2 s, Kalman collision warnings and prediction cones from the Fall
// 2025 server (the gateway returns none and says so), source trails, follow
// mode. What changed is how it looks: the atlas palette and type, the edge
// rail and sheets of the map, altitude stems, data-block labels, and a
// floating annotation tied to the selected aircraft instead of an info box.
// ---------------------------------------------------------------------------

// Cesium Ion access token (terrain and OSM buildings)
Cesium.Ion.defaultAccessToken = 'eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJqdGkiOiI5MDdjYmVjNC05NjZiLTQxZDMtYjA1YS00ZmI3ZjA2ZjRiYjQiLCJpZCI6MzY5MDk5LCJpYXQiOjE3NjU1MjUxMDZ9.vixLzkzcNmRUX1wvntDVQ9If5gVlx5ufYNo4Fytsh8A';

// Colours mirror styles.css / PALETTE in app.js
const C = {
    bg: '#121110', ink: '#ebe5d6', ink2: '#cbc4b4', stone: '#8e887c', ochre: '#b8955a',
    alert: '#c2603f', caution: '#b98340',
    band: { ground: '#77726a', low: '#c39a5a', mid: '#c2b598', high: '#98a8b3', top: '#7590a8' },
};
const col = (hex, a = 1) => Cesium.Color.fromCssColorString(hex).withAlpha(a);
const RECEIVER = [41.88, -87.63];
const LABEL_RANGE = new Cesium.DistanceDisplayCondition(0, 320000); // m from the camera
const STEM_ALPHA = 0.55;
const EMERGENCY = { '7500': 'Unlawful interference', '7600': 'Radio failure', '7700': 'General emergency' };
const $ = (id) => document.getElementById(id);
const esc = (s) => String(s ?? '').replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
const fmt = (n) => (n === null || n === undefined || Number.isNaN(n) ? '—' : Math.round(n).toLocaleString('en-US'));

function altitudeBand(a) {
    if (a.on_ground || (a.altitude !== null && a.altitude !== undefined && a.altitude < 100)) return 'ground';
    const alt = a.altitude;
    if (alt === null || alt === undefined) return 'mid';
    if (alt < 10000) return 'low';
    if (alt < 20000) return 'mid';
    if (alt < 30000) return 'high';
    return 'top';
}

function dataLine(a) {
    if (a.on_ground) return `GND ${a.speed ?? '—'}`;
    const fl = a.altitude === null || a.altitude === undefined ? '---' : String(Math.round(a.altitude / 100)).padStart(3, '0');
    const vr = a.vertical_rate;
    const arrow = vr > 300 ? '↑' : vr < -300 ? '↓' : ' ';
    return `${fl}${arrow}${a.speed ?? '—'}`;
}

// ---------------------------------------------------------------------------
// Viewer: dark chart imagery, terrain, buildings in stone
// ---------------------------------------------------------------------------

const viewer = new Cesium.Viewer('cesiumContainer', {
    baseLayer: false,
    baseLayerPicker: false,
    animation: false,
    timeline: false,
    vrButton: false,
    sceneModePicker: false,
    geocoder: false,
    homeButton: false,
    fullscreenButton: false,
    navigationHelpButton: false,
    selectionIndicator: false,
    infoBox: false,
    terrain: Cesium.Terrain.fromWorldTerrain(),
});

const scene = viewer.scene;
const globe = scene.globe;
// Esri's keyless dark canvas, warmed by letting a graphite base show through
const baseLayer = viewer.imageryLayers.addImageryProvider(new Cesium.UrlTemplateImageryProvider({
    url: 'https://server.arcgisonline.com/ArcGIS/rest/services/Canvas/World_Dark_Gray_Base/MapServer/tile/{z}/{y}/{x}',
    maximumLevel: 16,
    credit: 'Tiles © Esri — Esri, HERE, Garmin, © OpenStreetMap contributors',
}));
baseLayer.alpha = 0.8;
baseLayer.saturation = 0.4;
baseLayer.brightness = 0.98;
// No raster place-name layer: draped on the globe it stretches in perspective.
// Large airports are named instead (see loadAirports), as on the map.
globe.baseColor = col('#3a3226');
globe.enableLighting = false; // no night side: the chart reads the same at any hour
globe.showGroundAtmosphere = false;
globe.depthTestAgainstTerrain = true;
scene.backgroundColor = col(C.bg);
if (scene.skyBox) scene.skyBox.show = false;
if (scene.sun) scene.sun.show = false;
if (scene.moon) scene.moon.show = false;
// no glowing sky: the horizon meets plain graphite, like the edge of a printed chart
scene.skyAtmosphere.show = false;
scene.fog.enabled = true;
scene.fog.density = 2.0e-4;

let buildings = null;
(async function () {
    try {
        buildings = await Cesium.createOsmBuildingsAsync();
        buildings.style = new Cesium.Cesium3DTileStyle({ color: "color('#57524a', 0.9)" });
        scene.primitives.add(buildings);
    } catch (error) {
        console.warn('Could not load 3D buildings:', error);
    }
})();

// ---------------------------------------------------------------------------
// Camera controls (unchanged behaviour)
// ---------------------------------------------------------------------------

const controller = scene.screenSpaceCameraController;
const isMobile = /Android|webOS|iPhone|iPad|iPod|BlackBerry|IEMobile|Opera Mini/i.test(navigator.userAgent)
    || (window.matchMedia && window.matchMedia('(max-width: 768px)').matches);

if (isMobile) {
    controller.rotateEventTypes = Cesium.CameraEventType.LEFT_DRAG;
    controller.zoomEventTypes = Cesium.CameraEventType.PINCH;
    controller.tiltEventTypes = Cesium.CameraEventType.PINCH_MOVE;
    controller.zoomFactor = 5.0;
    controller.inertiaSpin = 0.95;
    controller.inertiaTranslate = 0.95;
    controller.inertiaZoom = 0.95;
} else {
    controller.zoomEventTypes = [Cesium.CameraEventType.WHEEL, Cesium.CameraEventType.PINCH];
    controller.rotateEventTypes = Cesium.CameraEventType.LEFT_DRAG;
    controller.tiltEventTypes = [
        Cesium.CameraEventType.RIGHT_DRAG,
        Cesium.CameraEventType.MIDDLE_DRAG,
        { eventType: Cesium.CameraEventType.LEFT_DRAG, modifier: Cesium.KeyboardEventModifier.SHIFT },
    ];
    controller.lookEventTypes = { eventType: Cesium.CameraEventType.LEFT_DRAG, modifier: Cesium.KeyboardEventModifier.CTRL };
    controller.translateEventTypes = {
        eventType: Cesium.CameraEventType.LEFT_DRAG,
        modifier: Cesium.KeyboardEventModifier.SHIFT | Cesium.KeyboardEventModifier.CTRL,
    };
    controller.zoomFactor = 3.0;
    controller.inertiaSpin = 0.9;
    controller.inertiaTranslate = 0.9;
    controller.inertiaZoom = 0.9;
}
controller.minimumZoomDistance = 100;
controller.maximumZoomDistance = 40000000;
controller.enableCollisionDetection = true;

const INITIAL_CAMERA_CONFIG = {
    longitude: -87.6298, // Chicago
    latitude: 36.0, // further south, to see Chicago with the tilt
    altitude: 550000,
    heading: 0,
    pitch: -45,
    roll: 0,
};

function initialView() {
    return {
        destination: Cesium.Cartesian3.fromDegrees(INITIAL_CAMERA_CONFIG.longitude, INITIAL_CAMERA_CONFIG.latitude, INITIAL_CAMERA_CONFIG.altitude),
        orientation: {
            heading: Cesium.Math.toRadians(INITIAL_CAMERA_CONFIG.heading),
            pitch: Cesium.Math.toRadians(INITIAL_CAMERA_CONFIG.pitch),
            roll: Cesium.Math.toRadians(INITIAL_CAMERA_CONFIG.roll),
        },
    };
}
viewer.camera.setView(initialView());

function setPitch(deg) {
    // keep looking at the same ground point while changing the tilt
    const target = screenCentreOnGlobe();
    if (!target) {
        viewer.camera.setView({ orientation: { heading: viewer.camera.heading, pitch: Cesium.Math.toRadians(deg), roll: 0 } });
        return;
    }
    const range = Cesium.Cartesian3.distance(viewer.camera.positionWC, target);
    viewer.camera.flyToBoundingSphere(new Cesium.BoundingSphere(target, 1), {
        offset: new Cesium.HeadingPitchRange(viewer.camera.heading, Cesium.Math.toRadians(deg), range),
        duration: 0.8,
    });
}

function rotateBy(deg) {
    const target = screenCentreOnGlobe();
    if (!target) return;
    const range = Cesium.Cartesian3.distance(viewer.camera.positionWC, target);
    viewer.camera.flyToBoundingSphere(new Cesium.BoundingSphere(target, 1), {
        offset: new Cesium.HeadingPitchRange(viewer.camera.heading + Cesium.Math.toRadians(deg), viewer.camera.pitch, range),
        duration: 0.8,
    });
}

function screenCentreOnGlobe() {
    const c = new Cesium.Cartesian2(scene.canvas.clientWidth / 2, scene.canvas.clientHeight / 2);
    const ray = viewer.camera.getPickRay(c);
    return (ray && globe.pick(ray, scene)) || viewer.camera.pickEllipsoid(c, globe.ellipsoid) || null;
}

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

const aircraftEntities = new Map();
const stemEntities = new Map();
const aircraftTrails = new Map();
let airportEntities = [];
let showAirports = false;
let showTrails = false;
let showTerrain = true;
let showStems = true;
let showLabels = true;
let followMode = false;
let followedAircraft = null;
let currentAircraftData = [];
let selectedAircraft = null;
let selectedCollision = null;
let updateInterval;
let collisionNote = null;

function isHelicopter(aircraft) {
    return aircraft.type === 6; // 6 = rotorcraft, from the server
}

// ---------------------------------------------------------------------------
// Aircraft: tinted model, data block, stem to the ground
// ---------------------------------------------------------------------------

function orientation(position, aircraft) {
    return Cesium.Transforms.headingPitchRollQuaternion(
        position,
        // the Cesium Air model needs a -90° offset to point its nose along the track
        new Cesium.HeadingPitchRoll(Cesium.Math.toRadians((aircraft.track || 0) - 90), 0, 0),
    );
}

function labelText(a) {
    return `${(a.flight || a.hex || '').trim()}\n${dataLine(a)}`;
}

function createAircraftEntity(aircraft) {
    const altitudeMeters = (aircraft.altitude || 0) * 0.3048;
    const position = Cesium.Cartesian3.fromDegrees(aircraft.lon, aircraft.lat, altitudeMeters);
    const isHeli = isHelicopter(aircraft);
    const band = altitudeBand(aircraft);
    const entity = viewer.entities.add({
        id: aircraft.hex,
        position,
        model: {
            uri: isHeli ? './helicopter.glb' : 'https://raw.githubusercontent.com/CesiumGS/cesium/main/Apps/SampleData/models/CesiumAir/Cesium_Air.glb',
            minimumPixelSize: isMobile ? 22 : 30,
            maximumScale: 20000,
            color: col(C.band[band]),
            colorBlendMode: Cesium.ColorBlendMode.MIX,
            colorBlendAmount: 0.72,
            silhouetteColor: col(C.bg, 0.9),
            silhouetteSize: 1.0,
        },
        orientation: orientation(position, aircraft),
        label: {
            text: labelText(aircraft),
            font: '500 11px "IBM Plex Mono", Menlo, monospace',
            fillColor: col(C.ink),
            showBackground: true,
            backgroundColor: col(C.bg, 0.74),
            backgroundPadding: new Cesium.Cartesian2(5, 3),
            horizontalOrigin: Cesium.HorizontalOrigin.LEFT,
            verticalOrigin: Cesium.VerticalOrigin.BOTTOM,
            pixelOffset: new Cesium.Cartesian2(16, -10),
            disableDepthTestDistance: Number.POSITIVE_INFINITY,
            // data blocks only near the camera: at overview distance they would pile up
            distanceDisplayCondition: LABEL_RANGE,
            translucencyByDistance: new Cesium.NearFarScalar(1.5e5, 1.0, 3.2e5, 0.35),
            show: showLabels,
        },
        properties: {},
    });
    entity._band = band;
    setProps(entity, aircraft);
    // positions through a callback: a polyline whose positions are replaced
    // every update would be rebuilt asynchronously each time and may never show
    const pts = [Cesium.Cartesian3.fromDegrees(aircraft.lon, aircraft.lat, 0), position];
    const stem = viewer.entities.add({
        position: pts[0],
        polyline: {
            positions: new Cesium.CallbackProperty(() => stem._pts, false),
            width: 1.25,
            material: col(C.band[band], STEM_ALPHA),
            arcType: Cesium.ArcType.NONE,
        },
        point: { pixelSize: 3, color: col(C.band[band], 0.7), heightReference: Cesium.HeightReference.CLAMP_TO_GROUND },
        show: showStems,
    });
    stem._pts = pts;
    stem._band = band;
    stemEntities.set(aircraft.hex, stem);
    return entity;
}

function setProps(entity, a) {
    entity.properties = {
        hex: a.hex, flight: a.flight, altitude: a.altitude, speed: a.speed, track: a.track,
        vertical_rate: a.vertical_rate, squawk: a.squawk ?? null, on_ground: !!a.on_ground,
        lat: a.lat, lon: a.lon, isHeli: isHelicopter(a),
    };
}

function updateAircraftEntity(entity, aircraft) {
    const altitudeMeters = (aircraft.altitude || 0) * 0.3048;
    const position = Cesium.Cartesian3.fromDegrees(aircraft.lon, aircraft.lat, altitudeMeters);
    entity.position = position;
    entity.orientation = orientation(position, aircraft);
    entity.label.text = labelText(aircraft);
    const band = altitudeBand(aircraft);
    if (band !== entity._band) {
        entity._band = band;
        entity.model.color = col(C.band[band]);
    }
    setProps(entity, aircraft);
    const stem = stemEntities.get(aircraft.hex);
    if (stem) {
        const ground = Cesium.Cartesian3.fromDegrees(aircraft.lon, aircraft.lat, 0);
        stem.position = ground;
        stem._pts = [ground, position];
        if (stem._band !== band) {
            stem._band = band;
            stem.polyline.material = col(C.band[band], STEM_ALPHA);
            stem.point.color = col(C.band[band], 0.7);
        }
    }
    if (showTrails) addToTrail(aircraft.hex, position, band);
}

function addToTrail(hex, position, band) {
    if (!aircraftTrails.has(hex)) {
        const trail = viewer.entities.add({
            polyline: { positions: [], width: 1.4, material: col(C.band[band] || C.ink2, 0.7), clampToGround: false },
        });
        aircraftTrails.set(hex, trail);
    }
    const trail = aircraftTrails.get(hex);
    const positions = trail.polyline.positions.getValue(Cesium.JulianDate.now());
    if (positions.length > 50) positions.shift(); // about 100 s at one update per 2 s
    positions.push(position);
    trail.polyline.positions = positions;
}

function clearTrails() {
    aircraftTrails.forEach((trail) => viewer.entities.remove(trail));
    aircraftTrails.clear();
}

// ---------------------------------------------------------------------------
// Data
// ---------------------------------------------------------------------------

async function fetchAircraftData() {
    try {
        const response = await fetch('/api/aircraft/enriched?window=0&trails=true&cones=true');
        if (!response.ok) throw new Error('Failed to fetch aircraft data');
        const data = await response.json();
        updateStatus(true);
        currentAircraftData = (data.aircraft || []).filter((a) => a.lat !== null && a.lon !== null && a.lat !== undefined);
        $('count-big').textContent = fmt(currentAircraftData.length);
        $('count-sub').textContent = `${fmt(currentAircraftData.filter((a) => !a.on_ground && (a.altitude ?? 0) >= 100).length)} airborne`;
        updateAircraftOn3DGlobe(currentAircraftData);
        renderIndex();
        renderAnnotation();
    } catch (error) {
        console.error('Error fetching aircraft data:', error);
        updateStatus(false, 'Connection Error');
    }
}

function updateAircraftOn3DGlobe(aircraftData) {
    const seen = new Set();
    for (const aircraft of aircraftData) {
        if (!aircraft.lat || !aircraft.lon) continue;
        seen.add(aircraft.hex);
        let entity = aircraftEntities.get(aircraft.hex);
        if (entity) updateAircraftEntity(entity, aircraft);
        else {
            entity = createAircraftEntity(aircraft);
            aircraftEntities.set(aircraft.hex, entity);
        }
        const grounded = (aircraft.altitude || 0) < 100;
        const visible = !(hideGroundedPlanes && grounded);
        entity.show = visible;
        const stem = stemEntities.get(aircraft.hex);
        if (stem) stem.show = visible && showStems && !grounded;
    }
    aircraftEntities.forEach((entity, hex) => {
        if (seen.has(hex)) return;
        viewer.entities.remove(entity);
        aircraftEntities.delete(hex);
        const stem = stemEntities.get(hex);
        if (stem) { viewer.entities.remove(stem); stemEntities.delete(hex); }
        if (aircraftTrails.has(hex)) { viewer.entities.remove(aircraftTrails.get(hex)); aircraftTrails.delete(hex); }
    });

    // Follow mode: keep the camera behind the followed aircraft
    if (followMode && followedAircraft && aircraftEntities.has(followedAircraft)) {
        const entity = aircraftEntities.get(followedAircraft);
        const position = entity.position.getValue(Cesium.JulianDate.now());
        const heading = entity.properties.track?.getValue() || 0;
        const headingRadians = Cesium.Math.toRadians(heading);
        const distance = 5000; // 5 km behind
        const heightOffset = 2000; // 2 km above
        const cartographic = Cesium.Cartographic.fromCartesian(position);
        const offsetPosition = Cesium.Cartesian3.fromRadians(
            cartographic.longitude - Math.sin(headingRadians) * (distance / 6371000),
            cartographic.latitude - Math.cos(headingRadians) * (distance / 6371000),
            cartographic.height + heightOffset,
        );
        viewer.camera.flyTo({
            destination: offsetPosition,
            orientation: { heading: headingRadians, pitch: Cesium.Math.toRadians(-20), roll: 0.0 },
            duration: 1.5,
            easingFunction: Cesium.EasingFunction.LINEAR_NONE,
        });
    }
}

// ---------------------------------------------------------------------------
// Collisions (Fall 2025 server; the gateway sends none, with a note)
// ---------------------------------------------------------------------------

let collisionLines = [];
let probabilityCones = [];
const probabilityConesByHex = new Map();
let showProbabilityCones = false;
let hideGroundedPlanes = false;
let collisionInterval;
let statisticsInterval;
let isUpdatingCones = false;
let lastConeUpdateTimestamp = 0;

async function fetchCollisions() {
    try {
        const response = await fetch('/api/collisions/kalman');
        const data = await response.json();
        if (data.error) { console.error('Collision detection error:', data.error); return; }
        collisionNote = data.note || null;
        displayCollisions3D(data.collisions);
    } catch (error) {
        console.error('Error fetching collisions:', error);
    }
}

function displayCollisions3D(collisions) {
    collisionLines.forEach((entity) => viewer.entities.remove(entity));
    collisionLines = [];
    const count = $('collision-count');
    const sub = $('ro-conflict-sub');
    const ro = $('ro-conflict');
    if (collisionNote) {
        count.textContent = '—';
        sub.textContent = 'not computed here';
        ro.title = collisionNote;
    } else {
        count.textContent = String(collisions?.length || 0);
        sub.textContent = 'next 5 min';
        ro.title = 'Kalman-filter conflict prediction (Fall 2025 server)';
    }
    ro.classList.toggle('is-alert', !collisionNote && (collisions?.length || 0) > 0);
    if (!collisions || collisions.length === 0) return;

    for (const collision of collisions) {
        const aircraft1 = currentAircraftData.find((ac) => ac.hex === collision.aircraft1);
        const aircraft2 = currentAircraftData.find((ac) => ac.hex === collision.aircraft2);
        if (!aircraft1 || !aircraft2) continue;
        const currentPos1 = Cesium.Cartesian3.fromDegrees(aircraft1.lon, aircraft1.lat, (aircraft1.altitude || 10000) * 0.3048);
        const currentPos2 = Cesium.Cartesian3.fromDegrees(aircraft2.lon, aircraft2.lat, (aircraft2.altitude || 10000) * 0.3048);
        const p1 = collision.position1, p2 = collision.position2;
        const collisionPointPos = Cesium.Cartesian3.fromDegrees(
            (parseFloat(p1.lon) + parseFloat(p2.lon)) / 2,
            (parseFloat(p1.lat) + parseFloat(p2.lat)) / 2,
            ((parseFloat(p1.alt) + parseFloat(p2.alt)) / 2) * 0.3048,
        );
        const probability = parseFloat(collision.probability);
        const time = collision.timeToCollision;
        const severe = probability > 15 || time < 60;
        const color = severe ? col(C.alert) : probability > 10 || time < 120 ? col(C.caution) : col(C.ochre);
        for (const from of [currentPos1, currentPos2]) {
            const mat = new Cesium.PolylineDashMaterialProperty({ color, dashLength: 12.0 });
            collisionLines.push(viewer.entities.add({ polyline: { positions: [from, collisionPointPos], width: 1.6, material: mat, depthFailMaterial: mat } }));
        }
        const a1 = collision.flight1 || collision.aircraft1;
        const a2 = collision.flight2 || collision.aircraft2;
        collisionLines.push(viewer.entities.add({
            name: `Conflict: ${a1} / ${a2}`,
            position: collisionPointPos,
            point: { pixelSize: 11, color: color.withAlpha(0.25), outlineColor: color, outlineWidth: 1.5, disableDepthTestDistance: Number.POSITIVE_INFINITY },
            properties: {
                isCollision: true, aircraft1: a1, aircraft2: a2, timeToCollision: collision.timeToCollision,
                probability: collision.probability, distance: collision.distance, verticalSeparation: collision.verticalSeparation,
                alt1: p1.alt, alt2: p2.alt, severe,
            },
        }));
    }
}

// ---------------------------------------------------------------------------
// Prediction cones (server-computed; unchanged geometry, atlas colours)
// ---------------------------------------------------------------------------

async function fetchAndDisplayProbabilityCones() {
    if (!showProbabilityCones) return;
    displayProbabilityCones(currentAircraftData);
}

async function displayProbabilityCones(aircraft) {
    if (!showProbabilityCones || !aircraft || aircraft.length === 0) return;
    if (isUpdatingCones) return;
    const dataTimestamp = Date.now();
    if (dataTimestamp - lastConeUpdateTimestamp < 2000 && probabilityCones.length > 0) return;
    isUpdatingCones = true;
    try {
        const numSegments = 16;
        const newCones = [];
        const processed = new Set();
        for (const ac of aircraft) {
            if (!showProbabilityCones) break;
            if (!ac.lat || !ac.lon || !ac.hex) continue;
            try {
                const predictions = ac.probabilityCone;
                if (!predictions || predictions.length < 2) continue;
                processed.add(ac.hex);
                const existing = probabilityConesByHex.get(ac.hex);
                if (existing && existing.predictions) {
                    const lastPred = existing.predictions[existing.predictions.length - 1];
                    const cur = predictions[predictions.length - 1];
                    if (Math.abs(lastPred.lat - cur.lat) < 0.001 && Math.abs(lastPred.lon - cur.lon) < 0.001 && Math.abs(lastPred.altitude - cur.altitude) < 100) continue;
                    existing.entities?.forEach((e) => viewer.entities.remove(e));
                }
                if (!ac.altitude) continue;
                const color = col(C.band[altitudeBand(ac)]);
                const apex = Cesium.Cartesian3.fromDegrees(ac.lon, ac.lat, ac.altitude * 0.3048);
                const futurePrediction = predictions[Math.min(5, predictions.length - 1)];
                if (!futurePrediction.lat || !futurePrediction.lon || !futurePrediction.altitude) continue;
                const futurePredPos = Cesium.Cartesian3.fromDegrees(futurePrediction.lon, futurePrediction.lat, futurePrediction.altitude * 0.3048);
                if (Cesium.Cartesian3.magnitude(Cesium.Cartesian3.subtract(futurePredPos, apex, new Cesium.Cartesian3())) < 100) continue;
                const last = predictions[predictions.length - 1];
                if (!last.lat || !last.lon || !last.altitude || !last.radius) continue;
                const futurePosition = Cesium.Cartesian3.fromDegrees(last.lon, last.lat, last.altitude * 0.3048);
                const dir = Cesium.Cartesian3.normalize(Cesium.Cartesian3.subtract(futurePosition, apex, new Cesium.Cartesian3()), new Cesium.Cartesian3());
                const up = Cesium.Cartesian3.normalize(apex, new Cesium.Cartesian3());
                let perp = Cesium.Cartesian3.cross(dir, up, new Cesium.Cartesian3());
                if (Cesium.Cartesian3.magnitude(perp) < 0.001) perp = new Cesium.Cartesian3(1, 0, 0);
                Cesium.Cartesian3.normalize(perp, perp);
                const perp2 = Cesium.Cartesian3.normalize(Cesium.Cartesian3.cross(dir, perp, new Cesium.Cartesian3()), new Cesium.Cartesian3());
                const edge = [];
                for (let i = 0; i <= numSegments; i++) {
                    const ang = (i / numSegments) * 2 * Math.PI;
                    const o1 = Cesium.Cartesian3.multiplyByScalar(perp, last.radius * Math.cos(ang), new Cesium.Cartesian3());
                    const o2 = Cesium.Cartesian3.multiplyByScalar(perp2, last.radius * Math.sin(ang), new Cesium.Cartesian3());
                    edge.push(Cesium.Cartesian3.add(futurePosition, Cesium.Cartesian3.add(o1, o2, o1), new Cesium.Cartesian3()));
                }
                const ents = [];
                for (let i = 0; i < numSegments; i++) {
                    ents.push(viewer.entities.add({
                        name: `Cone: ${ac.flight || ac.hex}`,
                        polygon: { hierarchy: new Cesium.PolygonHierarchy([apex, edge[i], edge[i + 1]]), material: color.withAlpha(0.07), outline: false, perPositionHeight: true },
                    }));
                }
                ents.push(viewer.entities.add({ polyline: { positions: [apex, futurePosition], width: 1, material: color.withAlpha(0.7) } }));
                ents.push(viewer.entities.add({ polyline: { positions: edge, width: 1, material: color.withAlpha(0.4) } }));
                newCones.push(...ents);
                probabilityConesByHex.set(ac.hex, { entities: ents, predictions });
            } catch (error) {
                console.error(`Error creating cone for ${ac.hex}:`, error);
            }
        }
        if (!showProbabilityCones) { newCones.forEach((e) => viewer.entities.remove(e)); return; }
        for (const [hex, data] of probabilityConesByHex.entries()) {
            if (processed.has(hex)) continue;
            data.entities?.forEach((e) => viewer.entities.remove(e));
            probabilityConesByHex.delete(hex);
        }
        probabilityCones = [...probabilityConesByHex.values()].flatMap((d) => d.entities);
        lastConeUpdateTimestamp = dataTimestamp;
    } finally {
        isUpdatingCones = false;
    }
}

function toggleProbabilityCones() {
    showProbabilityCones = !showProbabilityCones;
    if (showProbabilityCones) fetchAndDisplayProbabilityCones();
    else {
        for (const data of probabilityConesByHex.values()) data.entities?.forEach((e) => { try { viewer.entities.remove(e); } catch { /* gone */ } });
        probabilityConesByHex.clear();
        probabilityCones = [];
        viewer.entities.values.filter((e) => e.name?.startsWith('Cone:')).forEach((e) => viewer.entities.remove(e));
    }
    $('toggle-probability-cones').classList.toggle('active', showProbabilityCones);
}

function toggleHideGroundedPlanes() {
    hideGroundedPlanes = !hideGroundedPlanes;
    aircraftEntities.forEach((entity, hex) => {
        const grounded = (entity.properties.altitude?.getValue() || 0) < 100;
        entity.show = !(hideGroundedPlanes && grounded);
        const stem = stemEntities.get(hex);
        if (stem) stem.show = entity.show && showStems && !grounded;
    });
    $('toggle-grounded').classList.toggle('active', hideGroundedPlanes);
}

// ---------------------------------------------------------------------------
// Airports, drawn as chart symbols
// ---------------------------------------------------------------------------

let airportsData = null;

function airportIcon(isLarge) {
    const s = isLarge ? 22 : 12;
    const c = isLarge ? C.ink2 : C.stone;
    const svg = isLarge
        ? `<svg xmlns="http://www.w3.org/2000/svg" width="${s}" height="${s}" viewBox="0 0 22 22"><circle cx="11" cy="11" r="6" fill="none" stroke="${c}" stroke-width="1.4"/><path d="M11 2v4M11 16v4M2 11h4M16 11h4" stroke="${c}" stroke-width="1.4"/></svg>`
        : `<svg xmlns="http://www.w3.org/2000/svg" width="${s}" height="${s}" viewBox="0 0 12 12"><circle cx="6" cy="6" r="3.5" fill="none" stroke="${c}" stroke-width="1.2"/></svg>`;
    return `data:image/svg+xml;base64,${btoa(svg)}`;
}

async function loadAirports() {
    try {
        const response = await fetch('airports.json');
        if (!response.ok) return;
        airportsData = await response.json();
        for (const feature of airportsData.features) {
            const [lon, lat] = feature.geometry.coordinates;
            const props = feature.properties;
            const isLarge = props.type === 'large_airport';
            airportEntities.push(viewer.entities.add({
                position: Cesium.Cartesian3.fromDegrees(lon, lat, 100),
                billboard: {
                    image: airportIcon(isLarge),
                    width: isLarge ? 22 : 12,
                    height: isLarge ? 22 : 12,
                    disableDepthTestDistance: isLarge ? 500000 : 200000,
                    heightReference: Cesium.HeightReference.RELATIVE_TO_GROUND,
                },
                label: {
                    text: isLarge ? (props.name || '').replace(/ International Airport$| Airport$/, '') : (props.iata || props.icao || ''),
                    font: isLarge ? 'italic 16px "Instrument Serif", Georgia, serif' : '10px "IBM Plex Mono", monospace',
                    fillColor: col(isLarge ? C.ink2 : C.stone),
                    outlineColor: col(C.bg, 0.9),
                    outlineWidth: 3,
                    style: Cesium.LabelStyle.FILL_AND_OUTLINE,
                    horizontalOrigin: Cesium.HorizontalOrigin.LEFT,
                    verticalOrigin: Cesium.VerticalOrigin.CENTER,
                    pixelOffset: new Cesium.Cartesian2(isLarge ? 14 : 9, 0),
                    disableDepthTestDistance: isLarge ? 500000 : 200000,
                    translucencyByDistance: new Cesium.NearFarScalar(1e5, 1.0, isLarge ? 1.4e6 : 4e5, 0.0),
                },
                show: isLarge, // large airports are always named; the layer adds the rest
            }));
            airportEntities[airportEntities.length - 1]._large = isLarge;
        }
        updateMasthead();
    } catch (error) {
        console.error('Error loading airports:', error);
    }
}

function toggleAirports() {
    showAirports = !showAirports;
    airportEntities.forEach((entity) => { entity.show = showAirports || entity._large; });
    $('toggle-airports').classList.toggle('active', showAirports);
}

async function toggleTrails() {
    showTrails = !showTrails;
    if (!showTrails) clearTrails();
    else await Promise.all(currentAircraftData.map((a) => fetchAndDisplayHistoricalTrail(a.hex)));
    $('toggle-trails').classList.toggle('active', showTrails);
}

async function fetchAndDisplayHistoricalTrail(hex) {
    try {
        const response = await fetch(`/api/aircraft/${hex}/trail?minutes=120&limit=500`);
        if (!response.ok) return;
        const data = await response.json();
        if (!data.trail || data.trail.length === 0) return;
        const positions = data.trail.map((p) => Cesium.Cartesian3.fromDegrees(p.lon, p.lat, (p.altitude || 0) * 0.3048));
        if (aircraftTrails.has(hex)) viewer.entities.remove(aircraftTrails.get(hex));
        const a = currentAircraftData.find((ac) => ac.hex === hex);
        const band = a ? altitudeBand(a) : 'mid';
        aircraftTrails.set(hex, viewer.entities.add({
            polyline: { positions, width: 1.4, material: col(C.band[band], 0.7), clampToGround: false },
        }));
    } catch (error) {
        console.error(`Error fetching historical trail for ${hex}:`, error);
    }
}

function toggleTerrain() {
    showTerrain = !showTerrain;
    globe.show = showTerrain;
    $('toggle-terrain').classList.toggle('active', showTerrain);
}

function resetView() {
    stopFollow();
    viewer.camera.flyTo({ ...initialView(), duration: 2.0 });
}

// ---------------------------------------------------------------------------
// Status and readouts
// ---------------------------------------------------------------------------

let lastUpdateAt = 0;

function updateStatus(connected, text) {
    const indicator = $('status-indicator');
    const statusText = $('status-text');
    indicator.className = `status-dot ${connected ? 'connected' : 'disconnected'}`;
    statusText.textContent = text || (connected ? 'Connected' : 'Disconnected');
    $('ro-link-value').textContent = connected ? 'Live' : (text || 'Disconnected').replace('Connection Error', 'Error');
    $('ro-link').classList.toggle('is-down', !connected);
    if (connected) {
        lastUpdateAt = Date.now();
        $('last-update').textContent = new Date().toLocaleTimeString('en-GB', { hour12: false });
    }
}

setInterval(() => {
    if (!lastUpdateAt) return;
    const s = Math.round((Date.now() - lastUpdateAt) / 1000);
    $('ro-update-sub').textContent = s <= 1 ? 'just now' : `${s} s ago`;
}, 1000);

const live = { stats: null, statsT: 0, rate: null, statsAvailable: null };

async function pollStats() {
    if (live.statsAvailable === false) return;
    try {
        const r = await fetch('/gateway/stats', { cache: 'no-store' });
        if (!r.ok) throw new Error(String(r.status));
        const s = await r.json();
        const t = performance.now();
        if (live.stats) {
            const d = (s.subscriber?.applied ?? 0) - (live.stats.subscriber?.applied ?? 0);
            live.rate = d >= 0 ? d / ((t - live.statsT) / 1000) : null;
        }
        live.stats = s;
        live.statsT = t;
        live.statsAvailable = true;
        AtlasAnalysis.ingestStats(s);
        document.body.classList.add('has-feed-stats');
        $('ro-seq-value').textContent = fmt(s.status?.as_of_seq);
        $('ro-seq-sub').textContent = `session ${s.status?.session ?? '—'}`;
        $('ro-rate-value').textContent = live.rate === null ? '—' : fmt(live.rate);
        const lat = s.latency_us?.feed_to_ws;
        $('ro-lat-value').textContent = lat?.count ? fmt(lat.p50_us) : '—';
        $('ro-lat-sub').textContent = lat?.count ? `µs p50 · p99 ${fmt(lat.p99_us)}` : 'µs p50';
        $('ro-link-sub').textContent = 'gateway · polling 2 s';
    } catch {
        if (live.statsAvailable === null) {
            live.statsAvailable = false;
            document.body.classList.remove('has-feed-stats');
        }
    }
}

async function fetchStatistics() {
    try {
        const response = await fetch('/api/statistics?window=60');
        if (!response.ok) throw new Error('Failed to fetch statistics');
        AtlasAnalysis.setStatistics(await response.json());
    } catch (error) {
        console.error('Error fetching statistics:', error);
    }
}

// ---------------------------------------------------------------------------
// Masthead and compass: where the camera looks
// ---------------------------------------------------------------------------

function coords(lat, lon) {
    const dm = (v, p, n, w) => {
        const a = Math.abs(v);
        const d = Math.floor(a);
        return `${String(d).padStart(w, '0')}°${((a - d) * 60).toFixed(2).padStart(5, '0')}′${v >= 0 ? p : n}`;
    };
    return `${dm(lat, 'N', 'S', 2)} ${dm(lon, 'E', 'W', 3)}`;
}

function distanceKm(lat1, lon1, lat2, lon2) {
    const R = 6371;
    const r = Math.PI / 180;
    const a = Math.sin(((lat2 - lat1) * r) / 2) ** 2 + Math.cos(lat1 * r) * Math.cos(lat2 * r) * Math.sin(((lon2 - lon1) * r) / 2) ** 2;
    return 2 * R * Math.asin(Math.sqrt(a));
}

let mastLast = 0;
function updateMasthead() {
    const now = performance.now();
    if (now - mastLast < 250) return;
    mastLast = now;
    const cam = viewer.camera;
    const heading = (Cesium.Math.toDegrees(cam.heading) + 360) % 360;
    const pitch = Cesium.Math.toDegrees(cam.pitch);
    $('compass-rose').style.transform = `rotate(${-heading}deg)`;
    $('compass-heading').textContent = `${String(Math.round(heading) % 360).padStart(3, '0')}°`;
    $('compass-pitch').textContent = `pitch ${pitch < 0 ? '−' : ''}${Math.abs(Math.round(pitch))}°`;
    const eye = cam.positionCartographic.height;
    $('mast-eye').textContent = eye > 10000 ? `eye ${fmt(eye / 1000)} km` : `eye ${fmt(eye)} m`;
    const target = screenCentreOnGlobe();
    if (!target) { $('mast-coords').textContent = 'space'; return; }
    const c = Cesium.Cartographic.fromCartesian(target);
    const lat = Cesium.Math.toDegrees(c.latitude), lon = Cesium.Math.toDegrees(c.longitude);
    $('mast-coords').textContent = coords(lat, lon);
    if (!airportsData) return;
    let best = null, bd = Infinity;
    for (const f of airportsData.features) {
        const t = f.properties.type;
        if (t !== 'large_airport' && t !== 'medium_airport') continue;
        const [alon, alat] = f.geometry.coordinates;
        const d = distanceKm(lat, lon, alat, alon) * (t === 'large_airport' ? 0.55 : 1);
        if (d < bd) { bd = d; best = f; }
    }
    if (best && bd < 250) {
        $('mast-place').textContent = (best.properties.municipality || best.properties.name).split(' / ')[0];
        $('mast-near').textContent = `nearest ${best.properties.name.replace(/ International Airport$| Airport$| International$/, '').replace(/ \/.*$/, '')}`;
    } else {
        $('mast-place').textContent = 'Open airspace';
        $('mast-near').textContent = '';
    }
}
viewer.camera.changed.addEventListener(updateMasthead);
viewer.camera.percentageChanged = 0.01;
viewer.camera.moveEnd.addEventListener(() => { mastLast = 0; updateMasthead(); });

// ---------------------------------------------------------------------------
// Selection: a floating annotation tied to the aircraft by a leader
// ---------------------------------------------------------------------------

function selectedData() {
    return selectedAircraft ? currentAircraftData.find((a) => a.hex === selectedAircraft) || null : null;
}

function setSilhouette(hex, on) {
    const e = hex && aircraftEntities.get(hex);
    if (!e) return;
    e.model.silhouetteColor = on ? col(C.ochre) : col(C.bg, 0.9);
    e.model.silhouetteSize = on ? 2.0 : 1.0;
    // the selected aircraft keeps its data block at any distance
    e.label.distanceDisplayCondition = on ? undefined : LABEL_RANGE;
    e.label.translucencyByDistance = on ? undefined : new Cesium.NearFarScalar(1.5e5, 1.0, 3.2e5, 0.35);
}

function select(hex, { fly = false } = {}) {
    if (selectedAircraft && selectedAircraft !== hex) setSilhouette(selectedAircraft, false);
    selectedAircraft = hex;
    selectedCollision = null;
    document.body.classList.toggle('has-selection', !!hex);
    if (!hex) { hideAnnotation(); renderIndex(true); return; }
    setSilhouette(hex, true);
    renderAnnotation(true);
    renderIndex(true);
    if (fly) focusOnAircraft(hex);
}

function hideAnnotation() {
    $('annotation').hidden = true;
    $('annotation-leader').classList.remove('on');
}

function renderAnnotation(rebuild = false) {
    const box = $('annotation');
    if (selectedCollision) return;
    if (!selectedAircraft) return;
    const a = selectedData();
    if (!a) {
        box.classList.add('is-lost');
        const k = box.querySelector('.ann-kicker-state');
        if (k) k.textContent = 'Signal lost';
        return;
    }
    box.classList.remove('is-lost');
    const emergency = EMERGENCY[a.squawk];
    const band = altitudeBand(a);
    if (rebuild || !box.querySelector('.ann-callsign')) {
        box.innerHTML = `
            <button class="ann-close" type="button" aria-label="Close (Esc)">Esc&nbsp;&nbsp;×</button>
            <div class="ann-kicker"><span class="ann-kicker-state"></span><span class="ann-icao"></span></div>
            <div class="ann-callsign"></div>
            <div class="ann-primary">
                <div class="ann-fig"><span class="ann-big" data-k="alt"></span><span class="ann-unit">ft</span><label>Altitude</label></div>
                <div class="ann-fig"><span class="ann-big" data-k="spd"></span><span class="ann-unit">kt</span><label>Ground speed</label></div>
                <div class="ann-fig"><span class="ann-big" data-k="trk"></span><span class="ann-unit">°</span><label>Track</label></div>
            </div>
            <dl class="ann-rows">
                <dt>Vertical</dt><dd data-k="vr"></dd>
                <dt>Squawk</dt><dd data-k="sq"></dd>
                <dt>Position</dt><dd data-k="pos"></dd>
                <dt>Type</dt><dd data-k="type"></dd>
            </dl>
            <div class="ann-actions">
                <button type="button" data-act="fly">Fly to</button>
                <button type="button" data-act="follow">Follow</button>
                <button type="button" data-act="trail">Source trail</button>
            </div>`;
        box.querySelector('.ann-close').onclick = () => select(null);
        box.querySelector('[data-act="fly"]').onclick = () => focusOnAircraft(selectedAircraft);
        box.querySelector('[data-act="follow"]').onclick = () => (followMode && followedAircraft === selectedAircraft ? stopFollow() : startFollow(selectedAircraft));
        box.querySelector('[data-act="trail"]').onclick = async (e) => {
            const hex = selectedAircraft;
            if (aircraftTrails.has(hex) && !showTrails) {
                viewer.entities.remove(aircraftTrails.get(hex));
                aircraftTrails.delete(hex);
                e.currentTarget.classList.remove('on');
            } else {
                await fetchAndDisplayHistoricalTrail(hex);
                e.currentTarget.classList.add('on');
            }
        };
    }
    const q = (k) => box.querySelector(`[data-k="${k}"]`);
    box.dataset.band = band;
    box.classList.toggle('is-emergency', !!emergency);
    box.querySelector('.ann-kicker-state').textContent = emergency ? `Squawk ${a.squawk} · ${emergency}` : followMode && followedAircraft === a.hex ? 'Following' : 'Live';
    box.querySelector('.ann-icao').textContent = a.hex;
    box.querySelector('.ann-callsign').textContent = (a.flight || a.hex).trim();
    q('alt').textContent = a.on_ground ? 'GND' : fmt(a.altitude);
    q('spd').textContent = fmt(a.speed);
    q('trk').textContent = a.track === null || a.track === undefined ? '—' : String(Math.round(a.track)).padStart(3, '0');
    const vr = a.vertical_rate;
    q('vr').innerHTML = vr === null || vr === undefined ? '—' : `${vr > 0 ? '+' : vr < 0 ? '−' : ''}${fmt(Math.abs(vr))}<small>ft/min</small>`;
    q('sq').textContent = a.squawk || '—';
    q('pos').textContent = coords(a.lat, a.lon);
    q('type').textContent = isHelicopter(a) ? 'Rotorcraft' : 'Fixed wing';
    box.querySelector('[data-act="follow"]').classList.toggle('on', followMode && followedAircraft === a.hex);
    box.querySelector('[data-act="trail"]').classList.toggle('on', aircraftTrails.has(a.hex));
    box.hidden = false;
    positionAnnotation();
}

function renderCollisionAnnotation(entity) {
    const p = (k) => entity.properties[k]?.getValue();
    const box = $('annotation');
    selectedCollision = entity;
    if (selectedAircraft) setSilhouette(selectedAircraft, false);
    selectedAircraft = null;
    box.dataset.band = '';
    box.classList.remove('is-lost');
    box.classList.toggle('is-emergency', !!p('severe'));
    const t = p('timeToCollision');
    box.innerHTML = `
        <button class="ann-close" type="button" aria-label="Close (Esc)">Esc&nbsp;&nbsp;×</button>
        <div class="ann-kicker"><span class="ann-kicker-state">Predicted conflict</span></div>
        <div class="ann-callsign">${esc(p('aircraft1'))} / ${esc(p('aircraft2'))}</div>
        <div class="ann-primary">
            <div class="ann-fig"><span class="ann-big">${fmt(t)}</span><span class="ann-unit">s</span><label>Time to closest</label></div>
            <div class="ann-fig"><span class="ann-big">${esc(p('probability'))}</span><label>Probability</label></div>
        </div>
        <dl class="ann-rows">
            <dt>Separation</dt><dd>${(Number(p('distance')) / 1000).toFixed(2)}<small>km</small></dd>
            <dt>Vertical</dt><dd>${fmt(Number(p('verticalSeparation')))}<small>ft</small></dd>
            <dt>Altitudes</dt><dd>${fmt(Number(p('alt1')))} / ${fmt(Number(p('alt2')))}<small>ft</small></dd>
        </dl>`;
    box.querySelector('.ann-close').onclick = () => { selectedCollision = null; viewer.selectedEntity = undefined; hideAnnotation(); };
    box.hidden = false;
    positionAnnotation();
}

function annotationAnchor() {
    const e = selectedCollision || (selectedAircraft && aircraftEntities.get(selectedAircraft));
    if (!e) return null;
    const pos = e.position?.getValue(viewer.clock.currentTime);
    if (!pos) return null;
    return Cesium.SceneTransforms.wgs84ToWindowCoordinates(scene, pos) || null;
}

function positionAnnotation() {
    const box = $('annotation');
    const leader = $('annotation-leader');
    if (box.hidden) return;
    if (window.matchMedia('(max-width: 768px)').matches) { leader.classList.remove('on'); return; }
    const p = annotationAnchor();
    if (!p) { leader.classList.remove('on'); return; }
    const W = window.innerWidth, H = window.innerHeight;
    const bw = box.offsetWidth, bh = box.offsetHeight;
    const margin = 28, gap = 70;
    const right = p.x + gap + bw < W - 180;
    let x = right ? p.x + gap : p.x - gap - bw;
    x = Math.max(60, Math.min(W - bw - margin, x));
    let y = Math.max(140, Math.min(H - bh - margin, p.y - bh * 0.35));
    // keep clear of the aircraft count in the bottom-left corner
    const count = document.querySelector('.count')?.getBoundingClientRect();
    if (count && x < count.right + 16 && y + bh > count.top - 12) y = Math.max(140, count.top - 12 - bh);
    box.style.transform = `translate(${Math.round(x)}px, ${Math.round(y)}px)`;
    const ex = right ? x : x + bw;
    const ey = y + 18;
    const mx = right ? ex - 18 : ex + 18;
    leader.querySelector('polyline').setAttribute('points', `${p.x},${p.y} ${mx},${ey} ${ex},${ey}`);
    const dot = leader.querySelector('circle');
    dot.setAttribute('cx', p.x);
    dot.setAttribute('cy', p.y);
    leader.classList.add('on');
}
scene.postRender.addEventListener(positionAnnotation);

viewer.selectedEntityChanged.addEventListener((entity) => {
    if (!entity) return; // deselecting is done with Esc, the close button, or a click on empty globe
    if (entity.properties?.isCollision?.getValue()) { renderCollisionAnnotation(entity); return; }
    const hex = aircraftEntities.has(entity.id) ? entity.id : null;
    if (hex) select(hex);
});

// a click on empty globe clears the selection
new Cesium.ScreenSpaceEventHandler(scene.canvas).setInputAction((click) => {
    const picked = scene.pick(click.position);
    if (!Cesium.defined(picked)) {
        viewer.selectedEntity = undefined;
        select(null);
        selectedCollision = null;
        hideAnnotation();
    }
}, Cesium.ScreenSpaceEventType.LEFT_CLICK);

function focusOnAircraft(hex) {
    const aircraft = currentAircraftData.find((ac) => ac.hex === hex);
    if (!aircraft || !aircraft.lat || !aircraft.lon) return;
    const position = Cesium.Cartesian3.fromDegrees(aircraft.lon, aircraft.lat, (aircraft.altitude || 10000) * 0.3048);
    viewer.camera.flyToBoundingSphere(new Cesium.BoundingSphere(position, 1), {
        duration: 2,
        offset: new Cesium.HeadingPitchRange(Cesium.Math.toRadians(aircraft.track || 0), Cesium.Math.toRadians(-30), 100000),
    });
}

function toast(text, ms = 2600) {
    const t = $('toast');
    t.textContent = text;
    t.hidden = false;
    clearTimeout(toast._t);
    toast._t = setTimeout(() => { t.hidden = true; }, ms);
}

function startFollow(hex) {
    if (!hex || !aircraftEntities.has(hex)) {
        toast('Select an aircraft to follow it');
        return;
    }
    followMode = true;
    followedAircraft = hex;
    const a = currentAircraftData.find((x) => x.hex === hex);
    toast(`Following ${(a?.flight || hex).trim()} · Esc to stop`);
    renderAnnotation();
}

function stopFollow() {
    if (!followMode) return;
    followMode = false;
    followedAircraft = null;
    renderAnnotation();
}

// ---------------------------------------------------------------------------
// Rail, sheets, index
// ---------------------------------------------------------------------------

const sheet = { current: null, sort: 'callsign', query: '', last: 0 };

function openSheet(name) {
    if (sheet.current === name) name = null;
    sheet.current = name;
    for (const b of document.querySelectorAll('.rail-btn[data-sheet]')) b.classList.toggle('active', b.dataset.sheet === name);
    for (const s of document.querySelectorAll('.sheet')) s.hidden = s.dataset.sheet !== name;
    document.body.classList.toggle('has-sheet', !!name);
    AtlasAnalysis.show(name === 'analysis');
    if (name === 'index') { renderIndex(true); setTimeout(() => $('index-search').focus(), 0); }
}

function renderIndex(force = false) {
    if (sheet.current !== 'index') return;
    const t = performance.now();
    if (!force && t - sheet.last < 500) return;
    sheet.last = t;
    const q = sheet.query.trim().toUpperCase();
    let rows = currentAircraftData.filter((a) => !q || (a.flight || '').toUpperCase().includes(q) || a.hex.includes(q) || (a.squawk || '').includes(q));
    const by = {
        callsign: (a, b) => (a.flight || a.hex).localeCompare(b.flight || b.hex),
        alt: (a, b) => (b.altitude ?? -1) - (a.altitude ?? -1),
        spd: (a, b) => (b.speed ?? -1) - (a.speed ?? -1),
    }[sheet.sort];
    rows = rows.slice().sort(by);
    $('index-count').textContent = `${rows.length} of ${currentAircraftData.length}`;
    $('index-list').innerHTML = rows.map((a) => `<li class="index-row${a.hex === selectedAircraft ? ' is-selected' : ''}${EMERGENCY[a.squawk] ? ' is-emergency' : ''}" data-hex="${a.hex}" data-band="${altitudeBand(a)}">
        <i></i><b>${esc((a.flight || a.hex).trim())}</b><span>${a.on_ground ? 'GND' : fmt(a.altitude)}</span><span>${a.speed ?? '—'}</span><span>${a.track === null || a.track === undefined ? '—' : String(Math.round(a.track)).padStart(3, '0')}</span></li>`).join('')
        || '<li class="index-empty">No aircraft match</li>';
}

function initRail() {
    for (const b of document.querySelectorAll('.rail-btn[data-sheet]')) b.onclick = () => openSheet(b.dataset.sheet);
    for (const b of document.querySelectorAll('.sheet-close')) b.onclick = () => openSheet(null);
    $('view-top').onclick = () => setPitch(-90);
    $('view-angle').onclick = () => setPitch(-45);
    $('view-horizon').onclick = () => setPitch(-12);
    $('rotate-left').onclick = () => rotateBy(-45);
    $('reset-view').onclick = resetView;
    $('toggle-airports').onclick = toggleAirports;
    $('toggle-trails').onclick = toggleTrails;
    $('toggle-terrain').onclick = toggleTerrain;
    $('toggle-probability-cones').onclick = toggleProbabilityCones;
    $('toggle-grounded').onclick = toggleHideGroundedPlanes;
    $('toggle-stems').onclick = (e) => {
        showStems = !showStems;
        e.currentTarget.classList.toggle('active', showStems);
        stemEntities.forEach((stem, hex) => {
            const ent = aircraftEntities.get(hex);
            const grounded = (ent?.properties.altitude?.getValue() || 0) < 100;
            stem.show = showStems && !!ent?.show && !grounded;
        });
    };
    $('toggle-labels').onclick = (e) => {
        showLabels = !showLabels;
        e.currentTarget.classList.toggle('active', showLabels);
        aircraftEntities.forEach((ent) => { ent.label.show = showLabels; });
    };
    $('toggle-buildings').onclick = (e) => {
        const on = !e.currentTarget.classList.contains('active');
        e.currentTarget.classList.toggle('active', on);
        if (buildings) buildings.show = on;
    };
    $('index-search').addEventListener('input', (e) => { sheet.query = e.target.value; renderIndex(true); });
    for (const b of document.querySelectorAll('.index-sort button')) {
        b.onclick = () => {
            sheet.sort = b.dataset.sort;
            for (const o of document.querySelectorAll('.index-sort button')) o.classList.toggle('on', o === b);
            renderIndex(true);
        };
    }
    $('index-list').addEventListener('click', (e) => {
        const r = e.target.closest('.index-row');
        if (r) select(r.dataset.hex, { fly: true });
    });
    document.addEventListener('keydown', (e) => {
        if (e.target instanceof Element && e.target.matches('input, textarea')) {
            if (e.key === 'Escape') e.target.blur();
            return;
        }
        if (e.key === 'Escape') {
            if (followMode) stopFollow();
            else if (selectedAircraft || selectedCollision) { viewer.selectedEntity = undefined; select(null); selectedCollision = null; hideAnnotation(); }
            else if (sheet.current) openSheet(null);
        } else if (e.key === '/') {
            e.preventDefault();
            openSheet('index');
        } else if (e.key === 'f' || e.key === 'F') {
            if (selectedAircraft) (followMode ? stopFollow() : startFollow(selectedAircraft));
        }
    });
}

// ---------------------------------------------------------------------------
// Start
// ---------------------------------------------------------------------------

function startPolling() {
    loadAirports();
    fetchAircraftData();
    fetchCollisions();
    fetchStatistics();
    pollStats();
    updateInterval = setInterval(fetchAircraftData, 2000);
    collisionInterval = setInterval(fetchCollisions, 3000);
    setInterval(() => { if (showProbabilityCones) fetchAndDisplayProbabilityCones(); }, 5000);
    statisticsInterval = setInterval(fetchStatistics, 5000);
    setInterval(pollStats, 1000);
}

function stopPolling() {
    if (updateInterval) clearInterval(updateInterval);
}

AtlasAnalysis.init({
    getAircraft: () => currentAircraftData,
    receiver: RECEIVER,
    page: () => null, // the 3D view polls; no WebSocket counters to show
    onSelect: (hex) => { if (aircraftEntities.has(hex)) select(hex, { fly: true }); },
}, $('analysis-body'));

// kept for anything that still calls them by name
window.toggleProbabilityCones = toggleProbabilityCones;
window.toggleHideGroundedPlanes = toggleHideGroundedPlanes;
window.focusOnAircraft = focusOnAircraft;

initRail();
updateStatus(false, 'Connecting...');
updateMasthead();
startPolling();
window.addEventListener('beforeunload', stopPolling);
window.__atlas3d = { viewer, aircraftEntities, stemEntities, select, get selected() { return selectedAircraft; } };
