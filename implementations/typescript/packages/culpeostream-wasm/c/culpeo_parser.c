/**
 * culpeo_parser.c — CulpeoStream header parser and serializer.
 *
 * Compiled to WebAssembly via Emscripten.  All functions are pure and
 * stateless; the caller supplies and owns every buffer.
 */

#include "culpeo_parser.h"

#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* -------------------------------------------------------------------------
 * Internal helpers
 * ---------------------------------------------------------------------- */

/** ASCII lower-case a single byte (letters only). */
static inline uint8_t ascii_lower(uint8_t c) {
    if (c >= (uint8_t)'A' && c <= (uint8_t)'Z') {
        return (uint8_t)(c + 32u);
    }
    return c;
}

/** Is `c` an ASCII horizontal whitespace character? */
static inline int is_hspace(uint8_t c) {
    return c == (uint8_t)' ' || c == (uint8_t)'\t';
}

/* -------------------------------------------------------------------------
 * culpeo_parse_headers
 * ---------------------------------------------------------------------- */

int culpeo_parse_headers(uint8_t *buf, size_t len,
                         struct culpeo_header *headers_out, int max_headers,
                         size_t *body_offset_out) {
    /* Locate the mandatory \r\n\r\n terminator. */
    size_t term_pos = (size_t)-1;
    if (len >= 4) {
        for (size_t i = 0; i <= len - 4; i++) {
            if (buf[i]     == '\r' && buf[i + 1] == '\n' &&
                buf[i + 2] == '\r' && buf[i + 3] == '\n') {
                term_pos = i;
                break;
            }
        }
    }
    if (term_pos == (size_t)-1) {
        return -1; /* incomplete — terminator not found */
    }

    *body_offset_out = term_pos + 4;

    /* Parse header lines separated by \r\n within buf[0..term_pos). */
    int header_count = 0;
    size_t pos = 0;

    while (pos < term_pos) {
        /* Find end of line (\r\n or end of header block). */
        size_t line_end = pos;
        while (line_end < term_pos) {
            if (buf[line_end] == '\r' && line_end + 1 < term_pos &&
                buf[line_end + 1] == '\n') {
                break;
            }
            line_end++;
        }
        /* line: buf[pos..line_end) */

        /* Skip empty lines (shouldn't appear, but be defensive). */
        if (line_end == pos) {
            pos = line_end + 2;
            continue;
        }

        /* Find the first ':' separator. */
        size_t colon = pos;
        while (colon < line_end && buf[colon] != ':') {
            colon++;
        }
        if (colon == line_end) {
            return -2; /* missing ':' — malformed line */
        }
        if (colon == pos) {
            return -2; /* empty key — malformed */
        }

        /* Trim trailing whitespace from key. */
        size_t key_start = pos;
        size_t key_end   = colon;
        while (key_end > key_start && is_hspace(buf[key_end - 1])) {
            key_end--;
        }

        /* Lower-case the key in-place. */
        for (size_t k = key_start; k < key_end; k++) {
            buf[k] = ascii_lower(buf[k]);
        }

        /* Trim leading whitespace from value. */
        size_t val_start = colon + 1;
        while (val_start < line_end && is_hspace(buf[val_start])) {
            val_start++;
        }
        /* Trim trailing whitespace from value. */
        size_t val_end = line_end;
        while (val_end > val_start && is_hspace(buf[val_end - 1])) {
            val_end--;
        }

        /* Store result. */
        if (header_count >= max_headers) {
            return -3; /* too many headers */
        }
        headers_out[header_count].key_ptr = (uint32_t)key_start;
        headers_out[header_count].key_len = (uint32_t)(key_end - key_start);
        headers_out[header_count].val_ptr = (uint32_t)val_start;
        headers_out[header_count].val_len = (uint32_t)(val_end - val_start);
        header_count++;

        /* Advance past the \r\n (or to end if at last line without \r\n). */
        pos = (line_end < term_pos) ? line_end + 2 : line_end;
    }

    return header_count;
}

/* -------------------------------------------------------------------------
 * culpeo_serialize_frame
 * ---------------------------------------------------------------------- */

int culpeo_serialize_frame(const struct culpeo_header *headers,
                           int header_count,
                           const uint8_t *strings_buf,
                           const uint8_t *body, size_t body_len,
                           uint8_t *out_buf, size_t out_cap) {
    /* Calculate required capacity:
       For each header: key_len + 2 (": ") + val_len + 2 ("\r\n")
       Plus 2 bytes for the final "\r\n"
       Plus body_len */
    size_t required = 2; /* final \r\n */
    for (int i = 0; i < header_count; i++) {
        required += headers[i].key_len + 2 + headers[i].val_len + 2;
    }
    required += body_len;

    if (required > out_cap) {
        return -1;
    }

    uint8_t *p = out_buf;

    for (int i = 0; i < header_count; i++) {
        const uint8_t *key = strings_buf + headers[i].key_ptr;
        size_t         kl  = headers[i].key_len;
        const uint8_t *val = strings_buf + headers[i].val_ptr;
        size_t         vl  = headers[i].val_len;

        memcpy(p, key, kl);   p += kl;
        p[0] = ':';
        p[1] = ' ';           p += 2;
        memcpy(p, val, vl);   p += vl;
        p[0] = '\r';
        p[1] = '\n';          p += 2;
    }

    /* Header-body separator */
    p[0] = '\r';
    p[1] = '\n';
    p += 2;

    if (body_len > 0 && body != NULL) {
        memcpy(p, body, body_len);
    }

    return (int)required;
}
