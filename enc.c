/* SPDX-License-Identifier: MIT
 * enc.c  --  file encoding detection and conversion
 *
 * See enc.h for the contract.  cp437.h is included here for its three
 * helpers (cp437_to_ucs4[], unicode_to_cp437(), cp437_to_utf8()); all are
 * declared static so there is no conflict with other translation units that
 * also include cp437.h.
 */

#include "enc.h"
#include "cp437.h"
#include <stdlib.h>
#include <string.h>

/* =========================================================================
 * enc_detect
 * =========================================================================
 * One pass over the raw bytes.  A single valid multi-byte UTF-8 sequence
 * is enough to conclude UTF-8, provided no invalid byte appears first.
 * ========================================================================= */
FileEncoding enc_detect(const char *raw, int len)
{
    int i = 0, found_mb = 0;

    while (i < len) {
        unsigned char b = (unsigned char)raw[i];

        if (b < 0x80) { i++; continue; }   /* ASCII -- valid in both */

        /* 0x80-0xBF: bare continuation byte (invalid UTF-8 lead)
         * 0xC0-0xC1: overlong encoding (also invalid UTF-8)          */
        if (b < 0xC2) return ENC_CP437;

        int seq;
        if      (b < 0xE0) seq = 2;
        else if (b < 0xF0) seq = 3;
        else if (b < 0xF8) seq = 4;
        else               return ENC_CP437;   /* > U+10FFFF */

        if (i + seq > len) return ENC_CP437;   /* truncated sequence */

        int j;
        for (j = 1; j < seq; j++) {
            if (((unsigned char)raw[i + j] & 0xC0) != 0x80)
                return ENC_CP437;
        }

        found_mb = 1;
        i += seq;
    }

    /* Pure ASCII (found_mb == 0): UTF-8 and CP437 are identical for bytes
     * 0x20-0x7E so ENC_UTF8 is the safer, more modern default. */
    (void)found_mb;
    return ENC_UTF8;
}

/* =========================================================================
 * enc_utf8_decode
 * =========================================================================
 * Decodes one codepoint and advances *pos.  Returns UINT32_MAX on any
 * error (invalid lead byte, bad continuation, truncated, surrogate, OOB).
 * ========================================================================= */
uint32_t enc_utf8_decode(const char *buf, int len, int *pos)
{
    if (*pos >= len) return UINT32_MAX;

    unsigned char b = (unsigned char)buf[(*pos)++];

    if (b < 0x80) return (uint32_t)b;     /* ASCII fast path */
    if (b < 0xC2) return UINT32_MAX;      /* bare continuation / overlong */

    int      seq;
    uint32_t cp;

    if      (b < 0xE0) { seq = 2; cp = b & 0x1F; }
    else if (b < 0xF0) { seq = 3; cp = b & 0x0F; }
    else if (b < 0xF8) { seq = 4; cp = b & 0x07; }
    else               return UINT32_MAX;

    int j;
    for (j = 1; j < seq; j++) {
        if (*pos >= len)                              { return UINT32_MAX; }
        unsigned char c = (unsigned char)buf[(*pos)++];
        if ((c & 0xC0) != 0x80)                      { return UINT32_MAX; }
        cp = (cp << 6) | (c & 0x3F);
    }

    if (cp >= 0xD800 && cp <= 0xDFFF) return UINT32_MAX;   /* surrogate */
    if (cp > 0x10FFFF)                return UINT32_MAX;

    return cp;
}

/* =========================================================================
 * Typographic approximation table
 * =========================================================================
 * Common Unicode characters that have no direct CP437 equivalent but have
 * an acceptable visual substitute.  Applied by enc_utf8_to_cp437() AFTER
 * the primary unicode_to_cp437() lookup returns '?'.  Matches here are NOT
 * counted as replacements -- they are considered reasonable equivalents,
 * not information loss.  Keep sorted by ucs4 for readability; the table is
 * small enough that a linear scan is faster than bsearch overhead.
 * ========================================================================= */
static const struct { uint32_t ucs4; uint8_t cp437; } approx_map[] = {
    { 0x00A0, 0x20 },   /* NO-BREAK SPACE              -> space             */
    { 0x2013, 0x2D },   /* EN DASH                     -> hyphen-minus      */
    { 0x2014, 0x2D },   /* EM DASH                     -> hyphen-minus      */
    { 0x2018, 0x27 },   /* LEFT SINGLE QUOTATION MARK  -> apostrophe        */
    { 0x2019, 0x27 },   /* RIGHT SINGLE QUOTATION MARK -> apostrophe        */
    { 0x201A, 0x27 },   /* SINGLE LOW-9 QUOTATION MARK -> apostrophe        */
    { 0x201C, 0x22 },   /* LEFT DOUBLE QUOTATION MARK  -> double quote      */
    { 0x201D, 0x22 },   /* RIGHT DOUBLE QUOTATION MARK -> double quote      */
    { 0x201E, 0x22 },   /* DOUBLE LOW-9 QUOTATION MARK -> double quote      */
    { 0x2026, 0x2E },   /* HORIZONTAL ELLIPSIS         -> period            */
    { 0x2212, 0x2D },   /* MINUS SIGN                  -> hyphen-minus      */
};
#define APPROX_COUNT ((int)(sizeof(approx_map) / sizeof(approx_map[0])))

static uint8_t enc_approx(uint32_t ucs4)
{
    int i;
    for (i = 0; i < APPROX_COUNT; i++)
        if (approx_map[i].ucs4 == ucs4) return approx_map[i].cp437;
    return 0;   /* no approximation available */
}

/* enc_utf8_to_cp437 -- see enc.h for full contract.
 * Used at load time and on paste (X11 clipboard is always UTF-8).
 * Output length <= input length: every multi-byte sequence collapses to a
 * single CP437 byte, so srclen is a safe upper bound for the allocation. */
char *enc_utf8_to_cp437(const char *src, int srclen,
                         int *dstlen, int *replaced)
{
    char *dst = (char *)malloc((size_t)(srclen > 0 ? srclen : 1));
    if (!dst) return NULL;

    int si = 0, di = 0, rep = 0;

    while (si < srclen) {
        int      old_si = si;
        uint32_t ucs4   = enc_utf8_decode(src, srclen, &si);
        uint8_t  cp;

        if (ucs4 == UINT32_MAX) {
            /* Invalid UTF-8: treat the single offending byte as raw CP437.
             * If that raw byte is itself a control code, apply the same
             * drop rule as below. */
            cp = (uint8_t)(unsigned char)src[old_si];
            si = old_si + 1;
            if (cp < 0x20 && cp != 0x09 && cp != 0x0A && cp != 0x0D)
                continue;           /* drop -- do not advance di */
            dst[di++] = (char)cp;
        } else if (ucs4 < 0x20 || ucs4 == 0x7F) {
            /* Control code: only tab (0x09), LF (0x0A) and CR (0x0D) have
             * structural meaning in a text document.  Everything else --
             * including U+001A (DOS EOF / Ctrl-Z) -- is silently dropped.
             * It must not land in the buffer as a control byte. */
            if (ucs4 == 0x09 || ucs4 == 0x0A || ucs4 == 0x0D)
                dst[di++] = (char)(uint8_t)ucs4;
            /* else: drop -- do not advance di */
        } else {
            cp = unicode_to_cp437(ucs4);
            if (cp == 0x3F && ucs4 != (uint32_t)'?') {
                /* Not in primary CP437 table; try typographic approximation. */
                uint8_t approx = enc_approx(ucs4);
                if (approx != 0)
                    cp = approx;        /* reasonable substitute -- no warning */
                else
                    rep++;              /* genuine loss -- count it */
            }
            /* Defensive: the CP437 table maps some Unicode symbols (e.g.
             * U+263A SMILEY -> 0x01) to the control-code range.  Those
             * must not enter the buffer either -- drop and count them. */
            if (cp < 0x20 && cp != 0x09) {
                rep++;
                continue;           /* drop -- do not advance di */
            }
            dst[di++] = (char)cp;
        }
    }

    *dstlen  = di;
    *replaced = rep;
    return dst;
}

/* =========================================================================
 * enc_cp437_to_utf8
 * =========================================================================
 * Used at save time.  All CP437 non-ASCII code points are in the BMP
 * (U+0000-FFFF), so UTF-8 encoding is at most 3 bytes; allocating 3×srclen
 * is always sufficient.
 * ========================================================================= */
char *enc_cp437_to_utf8(const char *src, int srclen, int *dstlen)
{
    char *dst = (char *)malloc((size_t)(srclen * 3 + 1));
    if (!dst) return NULL;

    int si, di = 0;
    for (si = 0; si < srclen; si++) {
        unsigned char b = (unsigned char)src[si];

        if (b < 0x20 || b == 0x7F) {
            /* ASCII control: valid single-byte UTF-8, pass through. */
            dst[di++] = (char)b;
        } else {
            /* cp437_to_utf8() handles 0x20-0x7E as ASCII passthrough and
             * encodes 0x80-0xFF as 2- or 3-byte UTF-8 sequences. */
            char utf8[4];
            int  n = cp437_to_utf8(b, utf8);
            memcpy(dst + di, utf8, (size_t)n);
            di += n;
        }
    }

    *dstlen = di;
    return dst;
}

/* =========================================================================
 * enc_cp437_to_ascii
 * =========================================================================
 * Used at save time when the user has chosen ENC_ASCII.  Output is always
 * the same length as input (1-to-1: passthrough or '?').
 * ========================================================================= */
char *enc_cp437_to_ascii(const char *src, int srclen,
                          int *dstlen, int *replaced)
{
    char *dst = (char *)malloc((size_t)(srclen > 0 ? srclen : 1));
    if (!dst) return NULL;

    int si, rep = 0;
    for (si = 0; si < srclen; si++) {
        unsigned char b = (unsigned char)src[si];

        if (b == 0x09 || b == 0x0A || b == 0x0D ||
            (b >= 0x20 && b <= 0x7E)) {
            dst[si] = (char)b;
        } else {
            dst[si] = '?';
            rep++;
        }
    }

    *dstlen  = srclen;
    *replaced = rep;
    return dst;
}
