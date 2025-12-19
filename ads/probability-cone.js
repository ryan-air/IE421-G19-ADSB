/**
 * Probability Cone Generator for Collision Detection
 *
 * Creates a 3D cone representing the probability distribution of future aircraft positions
 * Uses Kalman filter to predict position with uncertainty that grows over time
 */

// Import math - works in both browser (from CDN) and Node.js (from npm)
let math;
if (typeof window !== 'undefined' && window.math) {
    // Browser environment with CDN
    math = window.math;
} else {
    // Node.js environment - dynamic import
    const mathModule = await import('mathjs');
    math = mathModule.default || mathModule;
}

import { KalmanFilter, latLonAltToECEF, ecefToLatLonAlt, distance3D } from './kalman-filter.js';

/**
 * Represents a probability cone for an aircraft's predicted trajectory
 */
export class ProbabilityCone {
    /**
     * @param {Object} aircraft - Current aircraft state
     * @param {Array} positionHistory - Array of past positions {lat, lon, altitude, timestamp}
     * @param {number} predictionTimeHorizon - How far into future to predict (seconds)
     * @param {number} confidenceLevel - Confidence level for cone (e.g., 0.95 for 95%)
     */
    constructor(aircraft, positionHistory, predictionTimeHorizon = 300, confidenceLevel = 0.95) {
        this.aircraft = aircraft;
        this.positionHistory = positionHistory;
        this.predictionTimeHorizon = predictionTimeHorizon;
        this.confidenceLevel = confidenceLevel;

        // Chi-squared value for 3D confidence ellipsoid
        // For 95% confidence in 3D: chi2(3, 0.95) ≈ 7.815
        // For 99% confidence in 3D: chi2(3, 0.99) ≈ 11.345
        this.chiSquared = confidenceLevel >= 0.99 ? 11.345 : 7.815;

        // Initialize Kalman filter with historical data
        this.kalmanFilter = this.initializeKalmanFilter();

        // Generate predictions
        this.predictions = this.generatePredictions();
    }

    /**
     * Initialize Kalman filter from position history
     * @returns {KalmanFilter}
     */
    initializeKalmanFilter() {
        if (!this.positionHistory || this.positionHistory.length < 2) {
            // Not enough history - use current position and velocity
            const currentECEF = latLonAltToECEF(
                this.aircraft.lat,
                this.aircraft.lon,
                this.aircraft.altitude
            );

            // Estimate velocity by computing position delta over 1 second
            // This avoids ENU→ECEF transformation issues
            const speedMS = (this.aircraft.speed || 0) * 0.514444; // knots to m/s
            const trackRad = ((this.aircraft.track || 0) * Math.PI) / 180;
            const verticalRateFtPerSec = (this.aircraft.vertical_rate || 0) / 60; // ft/min to ft/sec
            
            // Compute lat/lon change per second
            // North velocity component changes latitude
            // East velocity component changes longitude (scaled by cos(lat))
            const latRad = this.aircraft.lat * Math.PI / 180;
            const metersPerDegreeLat = 111320; // meters per degree latitude
            const metersPerDegreeLon = 111320 * Math.cos(latRad); // meters per degree longitude
            
            const deltaLatPerSec = (speedMS * Math.cos(trackRad)) / metersPerDegreeLat;
            const deltaLonPerSec = (speedMS * Math.sin(trackRad)) / metersPerDegreeLon;
            
            // Compute position after 1 second
            const futureLat = this.aircraft.lat + deltaLatPerSec;
            const futureLon = this.aircraft.lon + deltaLonPerSec;
            const futureAlt = this.aircraft.altitude + verticalRateFtPerSec;
            
            // Convert both positions to ECEF and compute velocity as the delta
            const futureECEF = latLonAltToECEF(futureLat, futureLon, futureAlt);
            
            const vx = futureECEF.x - currentECEF.x;
            const vy = futureECEF.y - currentECEF.y;
            const vz = futureECEF.z - currentECEF.z;

            const initialState = math.matrix([[currentECEF.x], [currentECEF.y], [currentECEF.z],
                                              [vx], [vy], [vz]]);

            return new KalmanFilter(initialState);
        }

        // Sort history by timestamp (oldest first)
        const sorted = [...this.positionHistory].sort((a, b) =>
            new Date(a.timestamp) - new Date(b.timestamp)
        );

        // Initialize with first position
        const first = sorted[0];
        const firstECEF = latLonAltToECEF(first.lat, first.lon, first.altitude);

        // Estimate initial velocity from first two positions
        let vx = 0, vy = 0, vz = 0;
        if (sorted.length >= 2) {
            const second = sorted[1];
            const secondECEF = latLonAltToECEF(second.lat, second.lon, second.altitude);
            const dt = (new Date(second.timestamp) - new Date(first.timestamp)) / 1000;

            if (dt > 0) {
                vx = (secondECEF.x - firstECEF.x) / dt;
                vy = (secondECEF.y - firstECEF.y) / dt;
                vz = (secondECEF.z - firstECEF.z) / dt;
            }
        }

        const initialState = math.matrix([[firstECEF.x], [firstECEF.y], [firstECEF.z],
                                          [vx], [vy], [vz]]);
        const kf = new KalmanFilter(initialState);

        // Train Kalman filter with historical positions
        let prevTime = new Date(first.timestamp);
        let lastHistoricalTime = prevTime;

        for (let i = 1; i < sorted.length; i++) {
            const pos = sorted[i];
            const currentTime = new Date(pos.timestamp);
            const dt = (currentTime - prevTime) / 1000; // seconds

            if (dt > 0) {
                // Predict
                kf.predict(dt);

                // Update with measurement
                const ecef = latLonAltToECEF(pos.lat, pos.lon, pos.altitude);
                kf.update([ecef.x, ecef.y, ecef.z]);

                prevTime = currentTime;
                lastHistoricalTime = currentTime;
            }
        }

        // CRITICAL: Predict forward from last historical position to current time
        // Without this, predictions start from the past!
        const now = Date.now();
        const timeSinceLastUpdate = (now - lastHistoricalTime.getTime()) / 1000; // seconds
        if (timeSinceLastUpdate > 0 && timeSinceLastUpdate < 300) {  // Only predict forward up to 5 minutes
            kf.predict(timeSinceLastUpdate);
        } else if (timeSinceLastUpdate >= 300) {
            // History is too old (>5min), fall back to velocity-from-track method
            console.warn(`[${this.aircraft.hex}] History too old (${timeSinceLastUpdate.toFixed(0)}s), using track/speed instead`);
            return this.initializeKalmanFilterFromVelocity();
        }

        return kf;
    }

    /**
     * Initialize Kalman filter using current position and velocity from track/speed
     * Used as fallback when position history is too old
     */
    initializeKalmanFilterFromVelocity() {
        const currentECEF = latLonAltToECEF(
            this.aircraft.lat,
            this.aircraft.lon,
            this.aircraft.altitude
        );

        // Estimate velocity from speed and track
        const speedMS = (this.aircraft.speed || 0) * 0.514444; // knots to m/s
        const trackRad = ((this.aircraft.track || 0) * Math.PI) / 180;
        const verticalRateMS = (this.aircraft.vertical_rate || 0) * 0.00508; // feet/min to m/s

        // Estimate velocity by computing position delta over 1 second
        // This avoids ENU→ECEF transformation issues
        const verticalRateFtPerSec = (this.aircraft.vertical_rate || 0) / 60; // ft/min to ft/sec
        
        // Compute lat/lon change per second
        const latRad = this.aircraft.lat * Math.PI / 180;
        const metersPerDegreeLat = 111320;
        const metersPerDegreeLon = 111320 * Math.cos(latRad);
        
        const deltaLatPerSec = (speedMS * Math.cos(trackRad)) / metersPerDegreeLat;
        const deltaLonPerSec = (speedMS * Math.sin(trackRad)) / metersPerDegreeLon;
        
        // Compute position after 1 second
        const futureLat = this.aircraft.lat + deltaLatPerSec;
        const futureLon = this.aircraft.lon + deltaLonPerSec;
        const futureAlt = this.aircraft.altitude + verticalRateFtPerSec;
        
        // Convert both positions to ECEF and compute velocity as the delta
        const futureECEF = latLonAltToECEF(futureLat, futureLon, futureAlt);
        
        const vx = futureECEF.x - currentECEF.x;
        const vy = futureECEF.y - currentECEF.y;
        const vz = futureECEF.z - currentECEF.z;

        const initialState = math.matrix([[currentECEF.x], [currentECEF.y], [currentECEF.z],
                                          [vx], [vy], [vz]]);

        return new KalmanFilter(initialState);
    }

    /**
     * Generate future position predictions with uncertainty
     * @returns {Array} Array of predictions {time, position, uncertainty}
     */
    generatePredictions() {
        const predictions = [];
        const timeSteps = 20; // Number of prediction points
        const dt = this.predictionTimeHorizon / timeSteps;

        for (let i = 1; i <= timeSteps; i++) {
            const time = i * dt;

            // Predict next state
            this.kalmanFilter.predict(dt);

            // Get predicted position and uncertainty
            const state = this.kalmanFilter.getStateArray();
            const position = { x: state[0], y: state[1], z: state[2] };
            const velocity = { vx: state[3], vy: state[4], vz: state[5] };

            // Get uncertainty (standard deviation in each dimension)
            const uncertainty = this.kalmanFilter.getPositionUncertainty();

            // Convert to lat/lon/alt
            const latLonAlt = ecefToLatLonAlt(position.x, position.y, position.z);

            predictions.push({
                time: time, // seconds from now
                position: position, // ECEF coordinates
                latLonAlt: latLonAlt,
                velocity: velocity,
                uncertainty: uncertainty, // {x, y, z} standard deviations in meters
                radius: Math.sqrt(uncertainty.x * uncertainty.x +
                                 uncertainty.y * uncertainty.y +
                                 uncertainty.z * uncertainty.z) // 3D uncertainty
            });
        }

        return predictions;
    }

    /**
     * Get cone radius at specific time
     * @param {number} time - Time in seconds from now
     * @returns {number} Cone radius in meters
     */
    getConeRadiusAtTime(time) {
        // Find closest prediction point
        const prediction = this.predictions.reduce((prev, curr) =>
            Math.abs(curr.time - time) < Math.abs(prev.time - time) ? curr : prev
        );

        // Return radius scaled by chi-squared value for confidence level
        return prediction.radius * Math.sqrt(this.chiSquared);
    }

    /**
     * Check if a point is inside the probability cone at given time
     * @param {Object} point - {x, y, z} in ECEF
     * @param {number} time - Time in seconds from now
     * @returns {boolean}
     */
    containsPoint(point, time) {
        const prediction = this.predictions.reduce((prev, curr) =>
            Math.abs(curr.time - time) < Math.abs(prev.time - time) ? curr : prev
        );

        const dist = distance3D(point, prediction.position);
        const threshold = prediction.radius * Math.sqrt(this.chiSquared);

        return dist <= threshold;
    }

    /**
     * Get all prediction points for visualization
     * @returns {Array}
     */
    getPredictionPoints() {
        return this.predictions.map(p => ({
            time: p.time,
            lat: p.latLonAlt.lat,
            lon: p.latLonAlt.lon,
            altitude: p.latLonAlt.alt,
            radius: p.radius * Math.sqrt(this.chiSquared)
        }));
    }
}

/**
 * Check if two probability cones intersect
 * @param {ProbabilityCone} cone1
 * @param {ProbabilityCone} cone2
 * @param {number} minVerticalSeparation - Minimum vertical separation in feet (default 1000)
 * @returns {Object|null} Collision info or null if no intersection
 */
export function checkConeIntersection(cone1, cone2, minVerticalSeparation = 1000) {
    const predictions1 = cone1.predictions;
    const predictions2 = cone2.predictions;

    // Check each time step for intersection
    for (let i = 0; i < Math.min(predictions1.length, predictions2.length); i++) {
        const p1 = predictions1[i];
        const p2 = predictions2[i];

        // Check vertical separation first (altitude filtering)
        const altitudeDiff = Math.abs(p1.latLonAlt.alt - p2.latLonAlt.alt);

        // Calculate vertical separation requirement based on altitude
        // Above 29,000 ft: use 2000 ft separation, below: use 1000 ft
        const maxAlt = Math.max(p1.latLonAlt.alt, p2.latLonAlt.alt);
        const requiredVerticalSep = maxAlt > 29000 ? 2000 : minVerticalSeparation;

        // If aircraft are vertically separated beyond the threshold, skip this pair
        if (altitudeDiff > requiredVerticalSep) {
            continue; // Safely separated vertically, check next time step
        }

        // Calculate distance between predicted positions
        const dist = distance3D(p1.position, p2.position);

        // Calculate combined uncertainty (sum of radii for confidence ellipsoids)
        const radius1 = p1.radius * Math.sqrt(cone1.chiSquared);
        const radius2 = p2.radius * Math.sqrt(cone2.chiSquared);
        const combinedRadius = radius1 + radius2;

        // If distance is less than combined radii, cones intersect
        if (dist <= combinedRadius) {
            // Calculate collision probability based on overlap
            // Simple model: probability decreases as distance increases
            const overlapRatio = Math.max(0, 1 - (dist / combinedRadius));
            const baseProbability = cone1.confidenceLevel * cone2.confidenceLevel;
            const probability = baseProbability * overlapRatio;

            return {
                time: p1.time, // seconds from now
                probability: probability,
                distance: dist, // meters
                position1: p1.latLonAlt,
                position2: p2.latLonAlt,
                verticalSeparation: altitudeDiff, // feet
                uncertainty1: radius1,
                uncertainty2: radius2,
                aircraft1: cone1.aircraft.hex,
                aircraft2: cone2.aircraft.hex
            };
        }
    }

    return null; // No intersection found
}

/**
 * Find all collisions between a set of aircraft
 * @param {Array} aircraftList - Array of aircraft with position history
 * @param {number} predictionTimeHorizon - Prediction time in seconds
 * @param {number} confidenceLevel - Confidence level (0.95 or 0.99)
 * @returns {Array} Array of potential collisions
 */
export function detectCollisions(aircraftList, predictionTimeHorizon = 300, confidenceLevel = 0.95) {
    const collisions = [];

    // Generate probability cones for all aircraft
    const cones = aircraftList.map(aircraft => {
        try {
            return new ProbabilityCone(
                aircraft,
                aircraft.positionHistory || [],
                predictionTimeHorizon,
                confidenceLevel
            );
        } catch (error) {
            console.error(`Error creating cone for ${aircraft.hex}:`, error);
            return null;
        }
    }).filter(cone => cone !== null);

    // Check all pairs for intersections
    for (let i = 0; i < cones.length; i++) {
        for (let j = i + 1; j < cones.length; j++) {
            const collision = checkConeIntersection(cones[i], cones[j]);

            if (collision) {
                collisions.push(collision);
            }
        }
    }

    // Sort by time (earliest collisions first)
    collisions.sort((a, b) => a.time - b.time);

    return collisions;
}

/**
 * Format collision for display
 * @param {Object} collision
 * @returns {Object}
 */
export function formatCollision(collision) {
    return {
        aircraft1: collision.aircraft1,
        aircraft2: collision.aircraft2,
        timeToCollision: Math.round(collision.time),
        probability: (collision.probability * 100).toFixed(1) + '%',
        distance: Math.round(collision.distance),
        verticalSeparation: Math.round(collision.verticalSeparation),
        position1: {
            lat: collision.position1.lat.toFixed(4),
            lon: collision.position1.lon.toFixed(4),
            alt: Math.round(collision.position1.alt)
        },
        position2: {
            lat: collision.position2.lat.toFixed(4),
            lon: collision.position2.lon.toFixed(4),
            alt: Math.round(collision.position2.alt)
        }
    };
}
