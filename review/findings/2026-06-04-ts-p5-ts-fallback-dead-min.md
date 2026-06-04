# TS-fallback `parseHeadersTs`: `Math.min(buf.length, buf.length)` is a no-op — missing scan limit

**Severity:** Low

**Location:** `implementations/typescript/packages/culpeostream-wasm/src/index.ts`, line 57

## Description

The pure-TypeScript fallback `parseHeadersTs` computes the search bound as:

```typescript
const searchLen = Math.min(buf.length, buf.length);
```

Both arguments to `Math.min` are identical, so `searchLen === buf.length` unconditionally. The call is a no-op and provides no real bound. The intent was almost certainly to cap the header-block scan depth at some maximum (e.g., a constant like `MAX_HEADER_BLOCK_SIZE` or a configurable limit), but the second argument was accidentally left as `buf.length` instead of the intended bound.

As written, the function scans the *entire* input buffer looking for `\r\n\r\n`. This means:

1. A caller that passes a multi-megabyte streaming buffer (before framing has extracted a single frame) will scan every byte of it on every call, turning an O(frame) operation into O(accumulated-buffer).
2. There is no defence against a malicious or buggy peer sending a header block that never contains `\r\n\r\n`; the function walks the entire buffer before returning `null`, every time new data arrives.

Note that the WASM path (`parseHeadersWasm`) has no equivalent scan limit at all — it passes `buf.length` directly to the C function. The asymmetry means the two paths would diverge in behaviour if the TS fallback were fixed to add a real limit.

## Impact

In isolation (no unbounded streaming buffer at this layer) this is benign. However, if the caller is a streaming reassembly layer that passes an ever-growing buffer before a full frame arrives, each call is O(n) in the total bytes received, turning overall frame parsing into O(n²). Under a malicious peer that sends headers without a terminator this degrades to sustained CPU usage for the duration of the connection.

The no-op `Math.min` also misleads future maintainers into believing a bound is enforced when none is.

## Suggested Fix

Either replace the second argument with an appropriate limit constant:

```typescript
const MAX_HEADER_SCAN = 65_536; // 64 KiB — generous upper bound for any real header block
const searchLen = Math.min(buf.length, MAX_HEADER_SCAN);
```

Or, if no limit was intended and the call was simply left over from an earlier draft, remove it entirely and document the absence of a limit:

```typescript
// No scan limit: the caller is expected to pass a single frame worth of data.
const searchLen = buf.length;
```

If a limit is added here, apply the same limit in `parseHeadersWasm` (by capping `buf.length` before passing it to `_culpeo_parse_headers`) so both paths remain byte-for-byte compatible.
