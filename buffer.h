/* SPDX-License-Identifier: MIT
 * buffer.h  --  dynamic line-oriented text buffer
 *
 * A document is an array of lines.  Each line is a growable byte
 * array (CP437 bytes, no NUL terminator required but one is kept
 * for convenience).  Lines never contain '\n' or '\r'.  Tab
 * characters ('\t') ARE stored as-is; the editor renders them
 * visually via its tabsize setting.
 */

#ifndef BUFFER_H
#define BUFFER_H

#include <stdint.h>
#include "enc.h"

typedef struct {
    char *data;   /* allocated, data[len] == '\0' always valid    */
    int   len;    /* number of characters in the line             */
    int   cap;    /* allocated size of data (>= len + 1)           */
} Line;

typedef struct {
    Line    *lines;
    uint8_t *hl_states;
    int      count;
    int      cap;
} TextBuffer;

/* Line ending style used for save/load */
typedef enum { LE_UNIX, LE_DOS } LineEnding;

/* ---- lifecycle ---------------------------------------------------- */

void tb_init(TextBuffer *b);
void tb_free(TextBuffer *b);

/* Reset to a single empty line (used for "new file"). */
void tb_clear(TextBuffer *b);

/* ---- line-level edits ---------------------------------------------- */

/* Insert a new line at index idx (0..count), with initial content
 * text[0..len-1].  text may be NULL/len==0 for a blank line. */
void tb_insert_line(TextBuffer *b, int idx, const char *text, int len);

/* Remove the line at idx. */
void tb_delete_line(TextBuffer *b, int idx);

/* Insert one character at column col within the line (shifts right). */
void line_insert_char(Line *l, int col, char ch);

/* Insert n space characters at column col. */
void line_insert_spaces(Line *l, int col, int n);

/* Delete the character at column col (shifts left).  No-op if
 * col >= l->len. */
void line_delete_char(Line *l, int col);

/* Append src[0..srclen-1] to the end of dst. */
void line_append(Line *dst, const char *src, int srclen);

/* Truncate the line to `col` characters (col <= len). */
void line_truncate(Line *l, int col);

/* ---- file I/O -------------------------------------------------------
 *
 * tb_load reads `filename` into b (which is cleared first).  Tab
 * characters are preserved as-is.  The entire file is slurped so that
 * encoding can be detected before line-splitting.
 *
 *   *ending   -- set to LE_DOS if any line ended with "\r\n", else LE_UNIX
 *   *enc      -- set to the detected FileEncoding (ENC_UTF8 or ENC_CP437)
 *   *replaced -- number of codepoints that had no CP437 mapping and were
 *                replaced with '?' (only non-zero when *enc == ENC_UTF8)
 *
 * Returns 0 on success, -1 if the file could not be opened or read.
 */
int tb_load(TextBuffer *b, const char *filename,
            LineEnding *ending, FileEncoding *enc, int *replaced);

/* tb_save writes b to `filename` using the given line-ending style and
 * encoding.  Every line (including the last) is terminated.
 *
 *   enc       -- ENC_UTF8:  encode CP437 -> UTF-8 before writing
 *                ENC_ASCII: replace non-ASCII bytes with '?'
 *                ENC_CP437: write raw bytes (no conversion)
 *   replaced  -- if non-NULL, receives the number of bytes replaced
 *                with '?' (only relevant for ENC_ASCII)
 *
 * Returns 0 on success, -1 on error. */
int tb_save(const TextBuffer *b, const char *filename,
            LineEnding ending, FileEncoding enc, int *replaced);

#endif /* BUFFER_H */
