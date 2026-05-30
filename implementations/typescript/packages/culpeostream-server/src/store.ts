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
   * When the limit is reached the oldest entry is evicted.
   * Default: 1000.
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
}

export class InMemorySessionStore implements ISessionStore {
  private readonly sessions = new Map<string, StoredEntry>();
  private readonly maxSessions: number;
  private readonly ttlMs: number | undefined;

  public constructor(options?: InMemorySessionStoreOptions) {
    this.maxSessions = options?.maxSessions ?? 1_000;
    this.ttlMs = options?.ttlMs;
  }

  public async save(snapshot: SessionSnapshot): Promise<void> {
    if (
      !this.sessions.has(snapshot.sessionId) &&
      this.sessions.size >= this.maxSessions
    ) {
      // Evict the oldest entry (Map preserves insertion order).
      const oldest = this.sessions.keys().next().value;
      if (oldest !== undefined) {
        this.sessions.delete(oldest);
      }
    }

    const expiresAt =
      this.ttlMs !== undefined ? Date.now() + this.ttlMs : undefined;

    this.sessions.set(snapshot.sessionId, { snapshot, expiresAt });
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
    return entry.snapshot;
  }

  public async delete(sessionId: string): Promise<void> {
    this.sessions.delete(sessionId);
  }
}
