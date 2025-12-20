/**
 * Simple in-memory cache with TTL support
 * Stores computed probability cones, trails, and other expensive operations
 * Results are shared across all clients to reduce redundant computation
 */

class Cache {
    constructor() {
        this.store = new Map();
        this.stats = {
            hits: 0,
            misses: 0,
            sets: 0,
            evictions: 0
        };
    }

    /**
     * Get value from cache
     * @param {string} key - Cache key
     * @returns {any|null} Cached value or null if expired/missing
     */
    get(key) {
        const entry = this.store.get(key);

        if (!entry) {
            this.stats.misses++;
            return null;
        }

        // Check if expired
        if (Date.now() > entry.expiresAt) {
            this.store.delete(key);
            this.stats.evictions++;
            this.stats.misses++;
            return null;
        }

        this.stats.hits++;
        return entry.value;
    }

    /**
     * Set value in cache with TTL
     * @param {string} key - Cache key
     * @param {any} value - Value to cache
     * @param {number} ttlSeconds - Time to live in seconds
     */
    set(key, value, ttlSeconds = 5) {
        const expiresAt = Date.now() + (ttlSeconds * 1000);

        this.store.set(key, {
            value,
            expiresAt,
            createdAt: Date.now()
        });

        this.stats.sets++;
    }

    /**
     * Delete specific key from cache
     * @param {string} key - Cache key
     */
    delete(key) {
        return this.store.delete(key);
    }

    /**
     * Clear all entries from cache
     */
    clear() {
        this.store.clear();
        this.stats.evictions += this.store.size;
    }

    /**
     * Get cache statistics
     * @returns {Object} Cache statistics
     */
    getStats() {
        const hitRate = this.stats.hits + this.stats.misses > 0
            ? (this.stats.hits / (this.stats.hits + this.stats.misses) * 100).toFixed(1)
            : 0;

        return {
            ...this.stats,
            hitRate: `${hitRate}%`,
            size: this.store.size
        };
    }

    /**
     * Clean up expired entries
     */
    cleanup() {
        const now = Date.now();
        let removed = 0;

        for (const [key, entry] of this.store.entries()) {
            if (now > entry.expiresAt) {
                this.store.delete(key);
                removed++;
            }
        }

        this.stats.evictions += removed;
        return removed;
    }
}

// Create singleton cache instance
const cache = new Cache();

// Run cleanup every 30 seconds to prevent memory leaks
setInterval(() => {
    const removed = cache.cleanup();
    if (removed > 0) {
        console.log(`🧹 Cache cleanup: removed ${removed} expired entries`);
    }
}, 30000);

export default cache;
