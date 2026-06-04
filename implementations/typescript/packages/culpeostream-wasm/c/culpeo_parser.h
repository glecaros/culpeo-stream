/**
 * culpeo_parser.h — Public C API for the CulpeoStream header parser and
 * serializer, compiled to WebAssembly via Emscripten.
 *
 * This header mirrors `libculpeo-message/include/culpeo/c_api.h`.
 * When building with Emscripten the implementation comes from `c_api.cpp`
 * in that library (backed by the C++20 `culpeo::message` parser).
 *
 * All functions are pure (no global state) and safe to call concurrently.
 * The caller is responsible for allocating and freeing all buffers.
 */
#ifndef CULPEO_PARSER_H
#define CULPEO_PARSER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Represents a single parsed header key/value pair.
 *
 * `key_ptr` and `val_ptr` are byte offsets into the input buffer passed to
 * `culpeo_parse_headers` — they are NOT pointers to a copy.  The caller must
 * read the data from the input buffer while it is still live.
 *
 * Keys are returned in their original case — the input buffer is NOT
 * modified.  Callers should normalise key case on their side (e.g.,
 * `key.toLowerCase()` in JavaScript).
 * Values are whitespace-trimmed by the parser.
 */
struct culpeo_header {
    uint32_t key_ptr; /**< Byte offset of key start within the input buffer */
    uint32_t key_len; /**< Length of key in bytes */
    uint32_t val_ptr; /**< Byte offset of value start within the input buffer */
    uint32_t val_len; /**< Length of value in bytes */
};

/**
 * Parse a CulpeoStream header block from a byte buffer.
 *
 * Scans `buf[0..len)` for the mandatory `\r\n\r\n` terminator.  For each
 * `\r\n`-delimited line before the terminator the first `:` separates the key
 * from the value; both sides are whitespace-trimmed.
 *
 * IMPORTANT: `buf` is `const` — the input buffer is NOT modified.  Keys are
 * returned in their original case (no in-place lowercasing).  Callers should
 * normalise key case on their side (e.g. `key.toLowerCase()` in JS).
 *
 * @param buf            Input buffer.  Read-only; not modified by the parser.
 *                       Must remain live until the caller has finished reading
 *                       the returned `culpeo_header` offsets.
 * @param len            Length of `buf` in bytes.
 * @param headers_out    Caller-allocated array of at least `max_headers`
 *                       `culpeo_header` structs.  Filled on success.
 * @param max_headers    Capacity of `headers_out`.
 * @param body_offset_out  Set to the byte offset immediately after `\r\n\r\n`
 *                         on success.
 *
 * @return  Number of headers parsed (>= 0) on success.
 *          -1  if the `\r\n\r\n` terminator is not present in `buf`
 *              (incomplete frame — try again when more data arrives).
 *          -2  if a header line is missing a `:` separator (malformed).
 *          -3  if the number of header lines exceeds `max_headers`.
 */
int culpeo_parse_headers(const uint8_t *buf, size_t len,
                         struct culpeo_header *headers_out, int max_headers,
                         size_t *body_offset_out);

/**
 * Serialize headers and body into a complete CulpeoStream frame byte sequence.
 *
 * Produces:
 *   `Key: Value\r\n`  (repeated for each header, keys written as-is)
 *   `\r\n`
 *   body bytes
 *
 * @param headers       Array of `header_count` header descriptors.  `key_ptr`
 *                      and `val_ptr` are byte offsets relative to `strings_buf`
 *                      (see below).
 * @param header_count  Number of headers.
 * @param strings_buf   Buffer from which key/value byte ranges are read.
 * @param body          Body bytes (may be NULL when `body_len` is 0).
 * @param body_len      Length of `body` in bytes.
 * @param out_buf       Caller-allocated output buffer.
 * @param out_cap       Capacity of `out_buf` in bytes.
 *
 * @return  Number of bytes written on success, or -1 if `out_buf` is too small.
 */
int culpeo_serialize_frame(const struct culpeo_header *headers,
                           int header_count,
                           const uint8_t *strings_buf,
                           const uint8_t *body, size_t body_len,
                           uint8_t *out_buf, size_t out_cap);

#ifdef __cplusplus
}
#endif

#endif /* CULPEO_PARSER_H */
