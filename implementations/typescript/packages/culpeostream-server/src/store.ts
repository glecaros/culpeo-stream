/**
 * ISessionStore — interface for persisting session snapshots across reconnects.
 * InMemorySessionStore — default in-memory implementation with optional TTL and capacity limits.
 *
 * Security invariants:
 *  - Session IDs are treated as opaque secrets; this module never logs them.
 */

import type { SessionSnapshot } from "culpeostream";

// ---------------------------------------------------------------------------
// Public interface
// ---------------------------------------------------------------------------

export interface ISessionStore {
  /** Persist a session snapshot so it can be resumed after reconnect. */
  save(snapshot: SessionSnapshot): Promise<void>;

  /**
   * Load a previously saved snapshot by session ID.
   * Returns null if the session is unknown or has expired.
   */
  load(sessionId: string): Promise<SessionSnapshot | null>;

  /** Remove a session from the store (e.g., after clean close). */
  delete(sessionId: string): Promise<void>;
}

// ---------------------------------------------------------------------------
// InMemorySessionStore
// ---------------------------------------------------------------------------

export interface InMemorySessionStoreOptions {
  /**
   * Maximum number of sessions to hold simultaneously.
   * When the limit is reached the LRU (least-recently-used) entry is evicted.
   * Default: 1000.
   *
   * SECURITY (SEC-022): To prevent session-eviction DoS attacks from high-volume
   * clients, set this to at least (expected_peak_concurrent_sessions × safety_factor).
   * Per-identity session quotas require a custom ISessionStore implementation that
   * tracks identity → session mappings; this built-in store does not implement them.
   */
  maxSessions?: number;

  /**
   * Time-to-live in milliseconds for each entry.
   * Expired entries are removed lazily on `load`.
   * Default: no expiry.
   */
  ttlMs?: number;
}

interface StoredEntry {
  snapshot: SessionSnapshot;
  /** Absolute epoch-ms after which this entry is considered stale, or undefined for no expiry. */
  expiresAt: number | undefined;
  /** Monotonically increasing access counter — used for LRU eviction (avoids Date.now() precision issues). */
  lastAccessedAt: number;
}

export class InMemorySessionStore implements ISessionStore {
  private readonly sessions = new Map<string, StoredEntry>();
  private readonly maxSessions: number;
  private readonly ttlMs: number | undefined;
  /** Monotonically increasing counter; incremented on every save/load to track LRU order. */
  private accessCounter = 0;

  public constructor(options?: InMemorySessionStoreOptions) {
    this.maxSessions = options?.maxSessions ?? 1_000;
    this.ttlMs = options?.ttlMs;
  }

  public async save(snapshot: SessionSnapshot): Promise<void> {
    if (
      !this.sessions.has(snapshot.sessionId) &&
      this.sessions.size >= this.maxSessions
    ) {
      // Evict the LRU (least-recently-used) entry instead of the oldest-inserted.
      let lruKey: string | undefined;
      let lruTime = Infinity;
      for (const [k, v] of this.sessions) {
        if (v.lastAccessedAt < lruTime) {
          lruTime = v.lastAccessedAt;
          lruKey = k;
        }
      }
      if (lruKey !== undefined) {
        this.sessions.delete(lruKey);
        console.warn(
          "[culpeostream-server] InMemorySessionStore: evicted LRU session to stay within maxSessions=%d. " +
            "If active sessions are being evicted, increase maxSessions or implement a custom ISessionStore " +
            "with per-identity quotas.",
          this.maxSessions,
        );
      }
    }

    const expiresAt =
      this.ttlMs !== undefined ? Date.now() + this.ttlMs : undefined;

    this.sessions.set(snapshot.sessionId, {
      snapshot,
      expiresAt,
      lastAccessedAt: ++this.accessCounter,
    });
  }

  public async load(sessionId: string): Promise<SessionSnapshot | null> {
    const entry = this.sessions.get(sessionId);
    if (entry === undefined) {
      return null;
    }
    if (entry.expiresAt !== undefined && Date.now() > entry.expiresAt) {
      this.sessions.delete(sessionId);
      return null;
    }
    // Update lastAccessedAt so this session is not the next LRU victim.
    entry.lastAccessedAt = ++this.accessCounter;
    return entry.snapshot;
  }

  public async delete(sessionId: string): Promise<void> {
    this.sessions.delete(sessionId);
  }
}
