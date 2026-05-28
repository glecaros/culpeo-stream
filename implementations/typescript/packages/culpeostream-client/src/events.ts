/**
 * A minimal, zero-dependency typed event emitter.
 *
 * EventMap is a plain object whose keys are event names and whose values are
 * the payload types. Use `void` for events that carry no data.
 *
 * @example
 * interface MyEvents {
 *   connected: void;
 *   data: Uint8Array;
 *   error: Error;
 * }
 * const emitter = new TypedEventEmitter<MyEvents>();
 * emitter.on('data', (buf) => console.log(buf));
 * emitter.emit('data', new Uint8Array([1, 2, 3]));
 */

/** Listener type: void-payload events take no argument; all others take exactly one. */
export type Listener<T> = [T] extends [void] ? () => void : (payload: T) => void;

// Internal alias used to store heterogeneous listeners in a single collection.
type AnyListener = (...args: readonly unknown[]) => void;

// EventMap is constrained to object (not Record<string, unknown>) so that
// callers can pass plain interfaces without needing an index signature.
// Type safety is still enforced via `K extends keyof EventMap` in each method.
export class TypedEventEmitter<EventMap extends object> {
  private readonly _listeners = new Map<keyof EventMap, Set<AnyListener>>();

  /** Subscribe to an event. Returns `this` for chaining. */
  public on<K extends keyof EventMap>(
    event: K,
    listener: Listener<EventMap[K]>,
  ): this {
    let set = this._listeners.get(event);
    if (set === undefined) {
      set = new Set<AnyListener>();
      this._listeners.set(event, set);
    }
    set.add(listener as AnyListener);
    return this;
  }

  /** Unsubscribe a previously registered listener. Returns `this` for chaining. */
  public off<K extends keyof EventMap>(
    event: K,
    listener: Listener<EventMap[K]>,
  ): this {
    this._listeners.get(event)?.delete(listener as AnyListener);
    return this;
  }

  /**
   * Subscribe for a single emission, then auto-unsubscribe.
   *
   * Note: the stored reference is a wrapper, not the original listener.
   * Calling `off(event, listener)` after `once` will not remove the wrapper.
   * If you need cancellation, capture the return value and call `off` with the
   * wrapper — or just call `removeAllListeners`.
   */
  public once<K extends keyof EventMap>(
    event: K,
    listener: Listener<EventMap[K]>,
  ): this {
    const wrapper: AnyListener = (...args: readonly unknown[]) => {
      this._listeners.get(event)?.delete(wrapper);
      (listener as AnyListener)(...args);
    };
    return this.on(event, wrapper as unknown as Listener<EventMap[K]>);
  }

  /**
   * Emit an event. If EventMap[K] is `void`, no payload argument is needed;
   * otherwise exactly one payload argument is required.
   */
  public emit<K extends keyof EventMap>(
    event: K,
    ...args: [EventMap[K]] extends [void] ? [] : [EventMap[K]]
  ): void {
    const set = this._listeners.get(event);
    if (set === undefined) return;
    // Snapshot to avoid mutations during iteration (e.g. once() removing itself).
    for (const listener of [...set]) {
      (listener as AnyListener)(...(args as readonly unknown[]));
    }
  }

  /** Remove all listeners for the given event, or every listener if omitted. */
  public removeAllListeners(event?: keyof EventMap): this {
    if (event !== undefined) {
      this._listeners.delete(event);
    } else {
      this._listeners.clear();
    }
    return this;
  }
}
