/**
 * Kalman Filter for Aircraft Tracking
 *
 * Tracks aircraft state: [x, y, z, vx, vy, vz]
 * - Position: x, y, z (ECEF coordinates in meters)
 * - Velocity: vx, vy, vz (meters per second)
 *
 * Uses constant velocity model for prediction
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

export class KalmanFilter {
    constructor(initialState = null) {
        // State dimension: 6 (position + velocity in 3D)
        this.stateDim = 6;

        // State vector: [x, y, z, vx, vy, vz]
        this.state = initialState || math.zeros(6, 1);

        // State covariance matrix (6x6) - represents uncertainty in state estimate
        // Start with high uncertainty since we don't know initial velocity
        this.P = math.multiply(math.identity(6), 1000);

        // Process noise covariance (6x6) - models uncertainty in the prediction
        // Aircraft don't follow perfect constant velocity (they turn, accelerate)
        const processNoise = [
            [1, 0, 0, 0, 0, 0],
            [0, 1, 0, 0, 0, 0],
            [0, 0, 1, 0, 0, 0],
            [0, 0, 0, 10, 0, 0],  // Higher noise for velocity (less predictable)
            [0, 0, 0, 0, 10, 0],
            [0, 0, 0, 0, 0, 10]
        ];
        this.Q = math.matrix(processNoise);

        // Measurement noise covariance (3x3) - models GPS/ADS-B measurement uncertainty
        // Typical ADS-B position accuracy: ~50-100 meters
        const measurementNoise = [
            [50, 0, 0],
            [0, 50, 0],
            [0, 0, 100]  // Altitude slightly less accurate
        ];
        this.R = math.matrix(measurementNoise);

        // Measurement matrix H (3x6) - we only measure position, not velocity
        // Maps state [x,y,z,vx,vy,vz] to measurement [x,y,z]
        this.H = math.matrix([
            [1, 0, 0, 0, 0, 0],
            [0, 1, 0, 0, 0, 0],
            [0, 0, 1, 0, 0, 0]
        ]);
    }

    /**
     * Predict next state using constant velocity model
     * @param {number} dt - Time step in seconds
     * @returns {Array} Predicted state [x, y, z, vx, vy, vz]
     */
    predict(dt) {
        // State transition matrix F (6x6)
        // Constant velocity model:
        //   x_new = x_old + vx * dt
        //   vx_new = vx_old (constant velocity assumption)
        const F = math.matrix([
            [1, 0, 0, dt, 0,  0 ],
            [0, 1, 0, 0,  dt, 0 ],
            [0, 0, 1, 0,  0,  dt],
            [0, 0, 0, 1,  0,  0 ],
            [0, 0, 0, 0,  1,  0 ],
            [0, 0, 0, 0,  0,  1 ]
        ]);

        // Predict state: x_pred = F * x
        this.state = math.multiply(F, this.state);

        // Predict covariance: P_pred = F * P * F^T + Q
        const FT = math.transpose(F);
        this.P = math.add(
            math.multiply(math.multiply(F, this.P), FT),
            this.Q
        );

        return this.getStateArray();
    }

    /**
     * Update state with new measurement
     * @param {Array} measurement - [x, y, z] position measurement
     */
    update(measurement) {
        const z = math.matrix(measurement.map(v => [v])); // Convert to column vector

        // Innovation (residual): y = z - H * x
        const innovation = math.subtract(z, math.multiply(this.H, this.state));

        // Innovation covariance: S = H * P * H^T + R
        const HT = math.transpose(this.H);
        const S = math.add(
            math.multiply(math.multiply(this.H, this.P), HT),
            this.R
        );

        // Kalman gain: K = P * H^T * S^-1
        const K = math.multiply(
            math.multiply(this.P, HT),
            math.inv(S)
        );

        // Update state: x = x + K * y
        this.state = math.add(this.state, math.multiply(K, innovation));

        // Update covariance: P = (I - K * H) * P
        const I = math.identity(6);
        const IKH = math.subtract(I, math.multiply(K, this.H));
        this.P = math.multiply(IKH, this.P);
    }

    /**
     * Get current state as array
     * @returns {Array} [x, y, z, vx, vy, vz]
     */
    getStateArray() {
        const stateMatrix = math.flatten(this.state);
        return stateMatrix.toArray();
    }

    /**
     * Get position estimate
     * @returns {Object} {x, y, z}
     */
    getPosition() {
        const state = this.getStateArray();
        return {
            x: state[0],
            y: state[1],
            z: state[2]
        };
    }

    /**
     * Get velocity estimate
     * @returns {Object} {vx, vy, vz}
     */
    getVelocity() {
        const state = this.getStateArray();
        return {
            vx: state[3],
            vy: state[4],
            vz: state[5]
        };
    }

    /**
     * Get position uncertainty (standard deviation in meters)
     * @returns {Object} {x, y, z}
     */
    getPositionUncertainty() {
        return {
            x: Math.sqrt(this.P.get([0, 0])),
            y: Math.sqrt(this.P.get([1, 1])),
            z: Math.sqrt(this.P.get([2, 2]))
        };
    }

    /**
     * Get total position uncertainty (combined 3D)
     * @returns {number} Total uncertainty in meters
     */
    getTotalUncertainty() {
        const unc = this.getPositionUncertainty();
        return Math.sqrt(unc.x * unc.x + unc.y * unc.y + unc.z * unc.z);
    }
}

/**
 * Coordinate Conversion Utilities
 */

// WGS84 ellipsoid constants
const WGS84_A = 6378137.0;          // Semi-major axis (meters)
const WGS84_E2 = 0.00669437999014;  // First eccentricity squared

/**
 * Convert lat/lon/alt to ECEF (Earth-Centered Earth-Fixed) Cartesian coordinates
 * @param {number} lat - Latitude in degrees
 * @param {number} lon - Longitude in degrees
 * @param {number} alt - Altitude in feet
 * @returns {Object} {x, y, z} in meters
 */
export function latLonAltToECEF(lat, lon, alt) {
    const latRad = lat * Math.PI / 180;
    const lonRad = lon * Math.PI / 180;
    const altMeters = alt * 0.3048; // feet to meters

    // Radius of curvature in prime vertical
    const N = WGS84_A / Math.sqrt(1 - WGS84_E2 * Math.sin(latRad) * Math.sin(latRad));

    const x = (N + altMeters) * Math.cos(latRad) * Math.cos(lonRad);
    const y = (N + altMeters) * Math.cos(latRad) * Math.sin(lonRad);
    const z = (N * (1 - WGS84_E2) + altMeters) * Math.sin(latRad);

    return { x, y, z };
}

/**
 * Convert ECEF to lat/lon/alt
 * @param {number} x - X coordinate in meters
 * @param {number} y - Y coordinate in meters
 * @param {number} z - Z coordinate in meters
 * @returns {Object} {lat, lon, alt} - lat/lon in degrees, alt in feet
 */
export function ecefToLatLonAlt(x, y, z) {
    const lon = Math.atan2(y, x);

    const p = Math.sqrt(x * x + y * y);
    let lat = Math.atan2(z, p * (1 - WGS84_E2));

    // Iterative method for latitude (usually converges in 2-3 iterations)
    for (let i = 0; i < 5; i++) {
        const N = WGS84_A / Math.sqrt(1 - WGS84_E2 * Math.sin(lat) * Math.sin(lat));
        const h = p / Math.cos(lat) - N;
        lat = Math.atan2(z, p * (1 - WGS84_E2 * N / (N + h)));
    }

    const N = WGS84_A / Math.sqrt(1 - WGS84_E2 * Math.sin(lat) * Math.sin(lat));
    const alt = p / Math.cos(lat) - N;

    return {
        lat: lat * 180 / Math.PI,
        lon: lon * 180 / Math.PI,
        alt: alt / 0.3048  // meters to feet
    };
}

/**
 * Calculate 3D distance between two points in ECEF
 * @param {Object} p1 - {x, y, z}
 * @param {Object} p2 - {x, y, z}
 * @returns {number} Distance in meters
 */
export function distance3D(p1, p2) {
    const dx = p2.x - p1.x;
    const dy = p2.y - p1.y;
    const dz = p2.z - p1.z;
    return Math.sqrt(dx * dx + dy * dy + dz * dz);
}

/**
 * Calculate horizontal distance (ignoring altitude)
 * @param {Object} p1 - {x, y, z}
 * @param {Object} p2 - {x, y, z}
 * @returns {number} Horizontal distance in nautical miles
 */
export function horizontalDistance(p1, p2) {
    const dx = p2.x - p1.x;
    const dy = p2.y - p1.y;
    const distMeters = Math.sqrt(dx * dx + dy * dy);
    return distMeters / 1852; // meters to nautical miles
}

/**
 * Calculate vertical distance
 * @param {Object} p1 - {x, y, z}
 * @param {Object} p2 - {x, y, z}
 * @returns {number} Vertical distance in feet
 */
export function verticalDistance(p1, p2) {
    const dz = Math.abs(p2.z - p1.z);
    return dz / 0.3048; // meters to feet
}
