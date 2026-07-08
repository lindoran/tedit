/* SPDX-License-Identifier: MIT
 * enc.h  --  file encoding: detection and conversion (UTF-8 / CP437 / ASCII)
 *
 * The editor's internal representation is always CP437.  These functions sit
 * at the I/O boundary (file load/save, clipboard) and nowhere else.
 *
 * ASCII control bytes (0x00-0x1F, 0x7F) are always treated as their text
 * meanings (tab, LF, CR, ...) rather than as CP437 graphic glyphs.
 */

#ifndef ENC_H
#define ENC_H

#include <stdint.h>

typedef enum {
    ENC_UTF8  = 0,  /* Unicode UTF-8                        */
    ENC_CP437 = 1,  /* IBM Code Page 437 (raw bytes)        */
    ENC_ASCII = 2   /* 7-bit ASCII (tab / LF / CR / 0x20-0x7E) */
} FileEncoding;

/* enc_detect -- scan raw file bytes and return the most likely encoding.
 *
 * Returns ENC_UTF8  if valid UTF-8 multi-byte sequences are found.
 * Returns ENC_CP437 if an invalid UTF-8 byte (bare continuation, overlong,
 *                   truncated sequence) is encountered.
 * Pure-ASCII files (no byte >= 0x80) return ENC_UTF8 -- the two encodings
 * are identical for that range and UTF-8 is the safer modern assumption. */
FileEncoding enc_detect(const char *raw, int len);

/* enc_utf8_decode -- decode one UTF-8 sequence starting at buf[*pos].
 * Advances *pos past the consumed bytes.
 * Returns the UCS-4 codepoint, or UINT32_MAX on invalid / truncated data. */
uint32_t enc_utf8_decode(const char *buf, int len, int *pos);

/* enc_utf8_to_cp437 -- convert a UTF-8 buffer to CP437.
 *
 *  - ASCII control bytes (U+0000-001F, U+007F) pass through unchanged so
 *    that LF, CR, and tab survive without hitting the CP437 graphic table.
 *  - Unmapped codepoints become '?'; *replaced is incremented for each one.
 *  - Invalid UTF-8 bytes are treated as raw CP437 and passed through.
 *
 * Output length <= input length (multi-byte sequences collapse to one byte).
 * Returns a malloc'd buffer of *dstlen bytes, or NULL on allocation failure. */
char *enc_utf8_to_cp437(const char *src, int srclen,
                         int *dstlen, int *replaced);

/* enc_cp437_to_utf8 -- convert a CP437 buffer to UTF-8.
 *
 *  - ASCII control bytes (< 0x20, 0x7F) pass through unchanged (valid
 *    single-byte UTF-8; tab/LF/CR keep their text meaning).
 *  - 0x20-0xFF are encoded via the CP437 → Unicode table.
 *
 * No information loss.  Worst-case output is 3× input (all BMP, no 4-byte).
 * Returns a malloc'd buffer of *dstlen bytes, or NULL on allocation failure. */
char *enc_cp437_to_utf8(const char *src, int srclen, int *dstlen);

/* enc_cp437_to_ascii -- convert a CP437 buffer to 7-bit ASCII.
 *
 * Bytes outside { 0x09, 0x0A, 0x0D, 0x20-0x7E } become '?'; *replaced is
 * incremented for each replacement.  Output is always the same length as
 * input (1-to-1 substitution).
 * Returns a malloc'd buffer of *dstlen bytes, or NULL on allocation failure. */
char *enc_cp437_to_ascii(const char *src, int srclen,
                          int *dstlen, int *replaced);

#endif /* ENC_H */
