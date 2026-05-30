/**
 * Public surface of culpeostream-client.
 *
 * Re-exports the typed event emitter, reconnect utilities, and the main client
 * class. Types only needed internally are not exported.
 */

// Core client
export { CulpeoStreamClient } from "./client.js";
export type {
  ClientCloseReason,
  ClientEventMap,
  ConnectOptions,
  CulpeoStreamClientOptions,
} from "./client.js";

// Typed event emitter (useful if callers want to extend or compose it)
export { TypedEventEmitter } from "./events.js";
export type { Listener } from "./events.js";

// Reconnect utilities (exposed for callers who want to tune backoff)
export {
  computeBackoffDelayMs,
  defaultReconnectOptions,
  shouldRetry,
} from "./reconnect.js";
export type { ReconnectOptions } from "./reconnect.js";
