/**
 * culpeo/c_api.h — C API for the CulpeoStream message parser.
 *
 * Implemented by c_api.cpp, which wraps the C++20 culpeo::message API and can
 * be compiled to WebAssembly via Emscripten or used directly from any C project.
 *
 * All functions are pure (no global state) and safe to call concurrently.
 * The caller is responsible for allocating and freeing all buffers.
 * Input buffers are NEVER mutated — keys are NOT lowercased in-place;
 * callers should normalise case in their own layer if required.
 */
#ifndef CULPEO_C_API_H
#define CULPEO_C_API_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Represents a single parsed header key/value pair as byte offsets into the
 * original input buffer supplied to culpeo_parse_headers().
 *
 * key_ptr/val_ptr are NOT pointers; they are offsets relative to buf[0].
 * Both key and value are whitespace-trimmed; keys retain their original case.
 */
struct culpeo_header {
    uint32_t key_ptr; /**< Byte offset of key start within the input buffer  */
    uint32_t key_len; /**< Length of key in bytes                            */
    uint32_t val_ptr; /**< Byte offset of value start within the input buffer */
    uint32_t val_len; /**< Length of value in bytes                          */
};

/**
 * Parse a CulpeoStream header block from a byte buffer.
 *
 * Scans buf[0..len) for the mandatory \r\n\r\n terminator and splits each
 * \r\n-delimited line at the first ':'.  The input buffer is not modified.
 *
 * @param buf             Input buffer (read-only).
 * @param len             Length of buf in bytes.
 * @param headers_out     Caller-allocated array of at least max_headers entries.
 * @param max_headers     Capacity of headers_out.
 * @param body_offset_out Set to the byte offset immediately after \r\n\r\n.
 *
 * @return  >= 0   Number of headers parsed.
 *          -1     \r\n\r\n terminator absent (incomplete frame — buffer more data).
 *          -2     Malformed header line (missing ':', invalid chars).
 *          -3     Header block too large (byte limit) or header count exceeds max_headers.
 */
int culpeo_parse_headers(const uint8_t *buf, size_t len,
                         struct culpeo_header *headers_out, int max_headers,
                         size_t *body_offset_out);

/**
 * Serialize headers and an optional body into a complete CulpeoStream frame.
 *
 * Output format:
 *   "Key: Value\r\n"  (repeated for each header)
 *   "\r\n"
 *   <body bytes>
 *
 * @param headers          Array of header_count descriptors; key_ptr/val_ptr are
 *                         byte offsets relative to strings_buf.
 * @param header_count     Number of headers (0 is valid).
 * @param strings_buf      Buffer from which key/value byte ranges are read.
 * @param strings_buf_len  Length of strings_buf in bytes. Each header's
 *                         key_offset+key_len and val_offset+val_len must not
 *                         exceed this value; violation returns -2.
 * @param body             Body bytes (may be NULL when body_len is 0).
 * @param body_len         Length of body in bytes.
 * @param out_buf          Caller-allocated output buffer.
 * @param out_cap          Capacity of out_buf in bytes.
 *
 * @return  >= 0   Number of bytes written.
 *          -1     out_buf too small, NULL body with non-zero body_len, or other
 *                 invalid argument.
 *          -2     Header key/value offset+length exceeds strings_buf_len
 *                 (out-of-bounds read guard).
 *          -4     Output too large to represent as int (> INT_MAX bytes).
 */
int culpeo_serialize_frame(const struct culpeo_header *headers,
                           int header_count,
                           const uint8_t *strings_buf,
                           size_t strings_buf_len,
                           const uint8_t *body, size_t body_len,
                           uint8_t *out_buf, size_t out_cap);

#ifdef __cplusplus
}
#endif

#endif /* CULPEO_C_API_H */
