/**
 * framing.ts — HTTP/2 frame encoding/decoding for the CulpeoStream protocol.
 *
 * Addendum C of the CulpeoStream spec defines a length-prefixed envelope for
 * carrying CulpeoStream frames over HTTP/2 DATA frames.
 *
 * Envelope format (per implementation contract):
 *   [type: 1 byte][length: 4 bytes big-endian][payload: N bytes]
 *
 * Note on spec vs. implementation order:
 *   The spec diagram in section C.4 shows [length][type+payload], but this
 *   implementation uses [type][length][payload] as specified in the task
 *   interface. Both sides agree on the format so interoperability is preserved.
 *   See DECISIONS.md § "HTTP/2 Frame Envelope Byte Order" for full rationale.
 *
 * Security: no secrets are processed here; this module is safe to log
 * frame metadata (typeOctet, bytesConsumed) but MUST NOT log payload bytes.
 */

/** Type octet for control / event frames (text: headers + JSON body). */
export const CONTROL_FRAME = 0x01 as const;

/** Type octet for media frames (binary: headers + raw bytes). */
export const MEDIA_FRAME = 0x02 as const;

/** Minimum number of bytes needed to read the header (type + 4-byte length). */
const HEADER_SIZE = 5;

/**
 * Encode a CulpeoStream frame for transmission over HTTP/2.
 *
 * Layout: [typeOctet: 1][length: 4 big-endian][payload: N]
 *
 * @param typeOctet  CONTROL_FRAME (0x01) or MEDIA_FRAME (0x02).
 * @param payload    Raw serialized CulpeoStream frame bytes (header block + body).
 * @returns          A Node.js Buffer ready to write to an HTTP/2 stream.
 */
export function encodeFrame(typeOctet: number, payload: Uint8Array): Buffer {
  const buf = Buffer.allocUnsafe(HEADER_SIZE + payload.length);
  buf.writeUInt8(typeOctet, 0);
  buf.writeUInt32BE(payload.length, 1);
  buf.set(payload, HEADER_SIZE);
  return buf;
}

/**
 * Attempt to decode one frame from the front of `buf`.
 *
 * Returns the decoded frame and the number of bytes consumed, or `null` if
 * `buf` does not yet contain a complete frame (caller should accumulate more
 * data and retry).
 *
 * Throws `RangeError` if the encoded length would exceed `maxPayloadBytes`.
 *
 * @param buf             Buffer that may contain one or more frames.
 * @param maxPayloadBytes Upper bound on the payload length (default: 16 MiB).
 */
export function decodeFrame(
  buf: Buffer,
  maxPayloadBytes = 16 * 1024 * 1024,
): { typeOctet: number; payload: Buffer; bytesConsumed: number } | null {
  if (buf.length < HEADER_SIZE) {
    return null;
  }

  const typeOctet = buf.readUInt8(0);
  const payloadLength = buf.readUInt32BE(1);

  if (payloadLength > maxPayloadBytes) {
    throw new RangeError(
      `CulpeoStream HTTP/2: frame payload length ${payloadLength} exceeds maximum ${maxPayloadBytes}`,
    );
  }

  const bytesConsumed = HEADER_SIZE + payloadLength;
  if (buf.length < bytesConsumed) {
    return null;
  }

  const payload = buf.subarray(HEADER_SIZE, bytesConsumed);
  return { typeOctet, payload, bytesConsumed };
}
