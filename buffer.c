/* SPDX-License-Identifier: MIT
 * buffer.c  --  dynamic line-oriented text buffer
 */

#include "buffer.h"
#include "enc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LINE_INIT_CAP 32
#define BUF_INIT_CAP  64

/* -----------------------------------------------------------------------
 * Line helpers
 * ----------------------------------------------------------------------- */

static void line_init(Line *l, const char *text, int len)
{
    int cap = LINE_INIT_CAP;
    while (cap < len + 1)
        cap *= 2;

    l->data = (char *)malloc((size_t)cap);
    l->cap  = cap;
    l->len  = len;

    if (len > 0 && text)
        memcpy(l->data, text, (size_t)len);
    l->data[len] = '\0';
}

static void line_free(Line *l)
{
    free(l->data);
    l->data = NULL;
    l->len  = l->cap = 0;
}

static void line_reserve(Line *l, int needed)
{
    if (needed + 1 <= l->cap)
        return;

    int cap = l->cap ? l->cap : LINE_INIT_CAP;
    while (cap < needed + 1)
        cap *= 2;

    l->data = (char *)realloc(l->data, (size_t)cap);
    l->cap  = cap;
}

void line_insert_char(Line *l, int col, char ch)
{
    if (col < 0) col = 0;
    if (col > l->len) col = l->len;

    line_reserve(l, l->len + 1);
    memmove(l->data + col + 1, l->data + col, (size_t)(l->len - col));
    l->data[col] = ch;
    l->len++;
    l->data[l->len] = '\0';
}

void line_insert_spaces(Line *l, int col, int n)
{
    int i;
    if (n <= 0) return;
    if (col < 0) col = 0;
    if (col > l->len) col = l->len;

    line_reserve(l, l->len + n);
    memmove(l->data + col + n, l->data + col, (size_t)(l->len - col));
    for (i = 0; i < n; i++)
        l->data[col + i] = ' ';
    l->len += n;
    l->data[l->len] = '\0';
}

void line_delete_char(Line *l, int col)
{
    if (col < 0 || col >= l->len) return;

    memmove(l->data + col, l->data + col + 1, (size_t)(l->len - col - 1));
    l->len--;
    l->data[l->len] = '\0';
}

void line_append(Line *dst, const char *src, int srclen)
{
    if (srclen <= 0) return;

    line_reserve(dst, dst->len + srclen);
    memcpy(dst->data + dst->len, src, (size_t)srclen);
    dst->len += srclen;
    dst->data[dst->len] = '\0';
}

void line_truncate(Line *l, int col)
{
    if (col < 0) col = 0;
    if (col > l->len) col = l->len;
    l->len = col;
    l->data[l->len] = '\0';
}

/* -----------------------------------------------------------------------
 * TextBuffer
 * ----------------------------------------------------------------------- */

void tb_init(TextBuffer *b)
{
    b->cap   = BUF_INIT_CAP;
    b->count = 0;
    b->lines = (Line *)malloc(sizeof(Line) * (size_t)b->cap);
    b->hl_states = (uint8_t *)malloc(sizeof(uint8_t) * (size_t)b->cap);
    /* always start with one empty line */
    tb_insert_line(b, 0, NULL, 0);
}

void tb_free(TextBuffer *b)
{
    int i;
    for (i = 0; i < b->count; i++)
        line_free(&b->lines[i]);
    free(b->lines);
    free(b->hl_states);
    b->lines = NULL;
    b->hl_states = NULL;
    b->count = b->cap = 0;
}

void tb_clear(TextBuffer *b)
{
    int i;
    for (i = 0; i < b->count; i++)
        line_free(&b->lines[i]);
    b->count = 0;
    tb_insert_line(b, 0, NULL, 0);
}

static void tb_reserve(TextBuffer *b, int needed)
{
    if (needed <= b->cap) return;

    int cap = b->cap ? b->cap : BUF_INIT_CAP;
    while (cap < needed)
        cap *= 2;

    b->lines = (Line *)realloc(b->lines, sizeof(Line) * (size_t)cap);
    b->hl_states = (uint8_t *)realloc(b->hl_states, sizeof(uint8_t) * (size_t)cap);
    b->cap   = cap;
}

void tb_insert_line(TextBuffer *b, int idx, const char *text, int len)
{
    if (idx < 0) idx = 0;
    if (idx > b->count) idx = b->count;

    tb_reserve(b, b->count + 1);
    memmove(&b->lines[idx + 1], &b->lines[idx],
            sizeof(Line) * (size_t)(b->count - idx));
    memmove(&b->hl_states[idx + 1], &b->hl_states[idx],
            sizeof(uint8_t) * (size_t)(b->count - idx));
    line_init(&b->lines[idx], text, len);
    /* 0xFF is an intentional "not yet computed" sentinel.  HL_ST_NORMAL==0
     * and HL_ST_BLOCK_COMMENT==1 are the only valid states, so 0xFF never
     * equals a computed next_state.  This prevents hl_update_states() from
     * terminating early against an uninitialised row, which would leave
     * block-comment state un-propagated through the rest of the file. */
    b->hl_states[idx] = 0xFF;
    b->count++;
}

void tb_delete_line(TextBuffer *b, int idx)
{
    if (idx < 0 || idx >= b->count) return;

    line_free(&b->lines[idx]);
    memmove(&b->lines[idx], &b->lines[idx + 1],
            sizeof(Line) * (size_t)(b->count - idx - 1));
    memmove(&b->hl_states[idx], &b->hl_states[idx + 1],
            sizeof(uint8_t) * (size_t)(b->count - idx - 1));
    b->count--;
}

/* -----------------------------------------------------------------------
 * File I/O
 *
 * The file is slurped into a temporary buffer so enc_detect() can scan
 * it before line-splitting begins.  If UTF-8 is detected the buffer is
 * converted to CP437 in-place; after that the split logic is the same
 * regardless of the original encoding.
 *
 * Line ending detection: the style of the FIRST newline in the file
 * wins.  A lone \r (not followed by \n) is preserved as a regular byte.
 * Files with no newlines default to LE_UNIX.
 * ----------------------------------------------------------------------- */

/* Remove every control byte (< 0x20) except tab (0x09) from a line,
 * in-place.  Called after loading raw CP437 files, which bypass the
 * enc_utf8_to_cp437 filter.  O(n) single pass, no allocation. */
static void line_strip_controls(Line *l)
{
    int i, j = 0;
    for (i = 0; i < l->len; i++) {
        unsigned char b = (unsigned char)l->data[i];
        if (b >= 0x20 || b == 0x09)
            l->data[j++] = l->data[i];
    }
    l->data[j] = '\0';
    l->len = j;
}

int tb_load(TextBuffer *b, const char *filename,
            LineEnding *ending, FileEncoding *enc, int *replaced)
{
    FILE *fp;
    char *raw;
    long  raw_len;

    *ending   = LE_UNIX;
    *enc      = ENC_UTF8;
    *replaced = 0;

    fp = fopen(filename, "rb");
    if (!fp) return -1;

    /* Slurp whole file so enc_detect() can inspect it before line split. */
    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return -1; }
    raw_len = ftell(fp);
    if (raw_len < 0)                  { fclose(fp); return -1; }
    rewind(fp);

    raw = (char *)malloc((size_t)(raw_len + 1));
    if (!raw) { fclose(fp); return -1; }

    if (raw_len > 0 &&
        (long)fread(raw, 1, (size_t)raw_len, fp) != raw_len) {
        free(raw); fclose(fp); return -1;
    }
    raw[raw_len] = '\0';
    fclose(fp);

    /* Detect encoding; convert UTF-8 -> CP437 if needed. */
    *enc = enc_detect(raw, (int)raw_len);
    if (*enc == ENC_UTF8 && raw_len > 0) {
        int   conv_len;
        char *conv = enc_utf8_to_cp437(raw, (int)raw_len, &conv_len, replaced);
        free(raw);
        if (!conv) return -1;
        raw     = conv;
        raw_len = (long)conv_len;
    }

    /* Split the (now CP437) buffer into lines. */
    tb_clear(b);
    tb_delete_line(b, 0);

    if (raw_len > 0) {
        char *p     = raw;
        char *endp  = raw + raw_len;
        int   first = 1;

        while (p < endp) {
            char *sol = p;

            /* Scan to next LF or end of buffer. */
            while (p < endp && *p != '\n') p++;

            int at_end = (p >= endp);
            int len    = (int)(p - sol);

            /* Strip a preceding \r if this \r was immediately before \n. */
            int saw_cr = (!at_end && len > 0 && sol[len - 1] == '\r');
            if (saw_cr) len--;

            /* Record line-ending style from the first terminated line. */
            if (first && !at_end) {
                *ending = saw_cr ? LE_DOS : LE_UNIX;
                first   = 0;
            }

            tb_insert_line(b, b->count, sol, len);

            if (at_end) break;
            p++; /* skip the LF */

            /* A file that ends exactly on a \n should not produce a
             * spurious trailing empty line (mirrors original behaviour). */
            if (p >= endp) break;
        }
    }

    /* Sanitize CP437 lines: strip control bytes that the raw-byte load path
     * does not filter (unlike enc_utf8_to_cp437, which now drops them).
     * UTF-8 files are already clean after conversion. */
    if (*enc == ENC_CP437) {
        int r;
        for (r = 0; r < b->count; r++)
            line_strip_controls(&b->lines[r]);
    }

    free(raw);

    if (b->count == 0)
        tb_insert_line(b, 0, NULL, 0);

    return 0;
}

int tb_save(const TextBuffer *b, const char *filename,
            LineEnding ending, FileEncoding enc, int *replaced)
{
    FILE       *fp;
    int         i;
    const char *eol    = (ending == LE_DOS) ? "\r\n" : "\n";
    size_t      eollen = (ending == LE_DOS) ? 2 : 1;

    if (replaced) *replaced = 0;

    fp = fopen(filename, "wb");
    if (!fp) return -1;

    for (i = 0; i < b->count; i++) {
        const Line *l = &b->lines[i];

        if (l->len > 0) {
            if (enc == ENC_UTF8) {
                int   dstlen;
                char *out = enc_cp437_to_utf8(l->data, l->len, &dstlen);
                if (!out) { fclose(fp); return -1; }
                fwrite(out, 1, (size_t)dstlen, fp);
                free(out);
            } else if (enc == ENC_ASCII) {
                int   dstlen, rep;
                char *out = enc_cp437_to_ascii(l->data, l->len, &dstlen, &rep);
                if (!out) { fclose(fp); return -1; }
                if (replaced) *replaced += rep;
                fwrite(out, 1, (size_t)dstlen, fp);
                free(out);
            } else {
                /* ENC_CP437: raw bytes, no conversion. */
                fwrite(l->data, 1, (size_t)l->len, fp);
            }
        }

        fwrite(eol, 1, eollen, fp);
    }

    fclose(fp);
    return 0;
}
