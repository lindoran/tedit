/* SPDX-License-Identifier: MIT
 * editor.c  --  editor state and core editing/navigation operations
 */

/* opendir/readdir/closedir require POSIX.1-2008 under strict C99. */
#define _POSIX_C_SOURCE 200809L

#include "editor.h"
#include "enc.h"
#include "undo.h"
#include "vio.h"

#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* System data directory baked in at build time.  The Makefile passes
 * -DTEDIT_DATADIR="..." via CFLAGS; this fallback covers in-tree builds. */
#ifndef TEDIT_DATADIR
#define TEDIT_DATADIR "/usr/local/share/tedit/lang"
#endif

/* Load every *.csv file found in `dir` as a language definition.
 * A language whose name already exists in the table is replaced, which
 * lets a later directory silently override an earlier one. */
/* Upsert `lang` into ed->hl_langs, keyed on lang->name.  An existing
 * entry with the same name is replaced (allows later locations to
 * override earlier ones).  Returns the slot index, or -1 if the table
 * is full. */
static int lang_upsert(Editor *ed, const HLang *lang)
{
    int idx, slot = ed->n_hl_langs;
    for (idx = 0; idx < ed->n_hl_langs; idx++) {
        if (strcmp(ed->hl_langs[idx].name, lang->name) == 0) {
            slot = idx;
            break;
        }
    }
    if (slot >= HL_MAX_LANGS) return -1;
    if (slot == ed->n_hl_langs) ed->n_hl_langs++;
    ed->hl_langs[slot] = *lang;
    return slot;
}

/* Load a language definition from an explicit path and upsert it into
 * ed->hl_langs.  Returns the slot index (>= 0) on success, -1 on
 * failure (bad path or table full). */
int ed_load_lang_file(Editor *ed, const char *path)
{
    HLang lang;
    if (hl_load(&lang, path) != 0) return -1;
    return lang_upsert(ed, &lang);
}

/* Search the standard cascade for a bare filename (e.g. "ansi-c.csv").
 * Returns the slot index (>= 0) on success, -1 if not found anywhere. */
int ed_load_lang_search(Editor *ed, const char *filename)
{
    char        path[1024];
    const char *home;
    int         n, slot;

    /* 1. System data directory (baked in at build time). */
    n = snprintf(path, sizeof(path), "%s/%s", TEDIT_DATADIR, filename);
    if (n > 0 && n < (int)sizeof(path)) {
        slot = ed_load_lang_file(ed, path);
        if (slot >= 0) return slot;
    }

    /* 2. User config directory (~/.tedit/lang). */
    home = getenv("HOME");
    if (home && home[0]) {
        n = snprintf(path, sizeof(path), "%s/.tedit/lang/%s", home, filename);
        if (n > 0 && n < (int)sizeof(path)) {
            slot = ed_load_lang_file(ed, path);
            if (slot >= 0) return slot;
        }
    }

    /* 3. Local lang/ directory (relative to cwd, for in-tree development). */
    n = snprintf(path, sizeof(path), "lang/%s", filename);
    if (n > 0 && n < (int)sizeof(path)) {
        slot = ed_load_lang_file(ed, path);
        if (slot >= 0) return slot;
    }

    return -1;
}

/* Scan `dir` for every *.csv file and upsert each into ed->hl_langs. */
static void load_langs_from_dir(Editor *ed, const char *dir)
{
    DIR           *dp;
    struct dirent *ent;
    char           path[1024];
    size_t         dlen;

    if (!dir || !dir[0]) return;
    dp = opendir(dir);
    if (!dp) return;

    dlen = strlen(dir);

    while ((ent = readdir(dp)) != NULL) {
        const char *nm   = ent->d_name;
        size_t      nlen = strlen(nm);

        if (nlen < 5 || strcmp(nm + nlen - 4, ".csv") != 0)
            continue;
        if (dlen + 1 + nlen >= sizeof(path))
            continue;

        memcpy(path, dir, dlen);
        path[dlen] = '/';
        memcpy(path + dlen + 1, nm, nlen + 1);

        ed_load_lang_file(ed, path);   /* upsert; ignore individual failures */
    }
    closedir(dp);
}

#define ED_MARK_DIRTY(ed, row) do { \
    if ((ed)->hl_dirty_row < 0 || (row) < (ed)->hl_dirty_row) { \
        (ed)->hl_dirty_row = (row); \
    } \
} while(0)

/* -----------------------------------------------------------------------
 * Lifecycle
 * ----------------------------------------------------------------------- */

void ed_init(Editor *ed)
{
    memset(ed, 0, sizeof(*ed));

    tb_init(&ed->tb);

    ed->cur_row = ed->cur_col = 0;
    ed->top_row = ed->left_col = 0;

    ed->tabsize     = DEFAULT_TABSIZE;
    ed->insert_mode = 1;
    ed->show_linenum = 0;
    ed->ending      = LE_UNIX;

    ed->modified = 0;
    ed->filename[0] = '\0';
    ed->message[0]  = '\0';

    ed->console_active = 0;
    ed->console_len    = 0;
    ed->console_buf[0] = '\0';

    ed->quit = 0;
    ed->pending_quit = 0;

    ed->sel_active = 0;
    ed->sel_row    = 0;
    ed->sel_col    = 0;

    ed->clipboard     = NULL;
    ed->clipboard_len = 0;

    ed->use_hard_tabs = 0;

    /* Undo coalescing: default 1000 ms (1 second). 0 disables coalescing. */
    ed->coalesce_ms = 1000;

    ed->last_find[0] = '\0';
    ed->last_find_len = 0;
    ed->last_find_row = -1;
    ed->last_find_col = -1;

    ed->mouse_btn_down = 0;

    ed->palette_active     = 0;
    ed->palette_sel        = 0;
    ed->palette_scroll     = 0;
    ed->palette_filter[0]  = '\0';
    ed->palette_filter_len = 0;

    ed->n_hl_langs   = 0;
    ed->hl           = NULL;
    ed->hl_dirty_row = 0;

    /* Spell checker: no process running yet. */
    ed->spell_pid     = -1;
    ed->spell_wfd     = -1;
    ed->spell_sug_sel = -1;

    /* Auto-load language definitions from three locations in order:
     *   1. system data dir  (TEDIT_DATADIR, set at build time via Makefile)
     *   2. user config dir  (~/.tedit/lang)
     *   3. local lang/      (relative to cwd — useful for in-tree development)
     *
     * Each location is scanned for *.csv files.  If two locations provide
     * a language with the same name, the later one wins, so user files
     * override system defaults and local files override both. */
    {
        const char *home = getenv("HOME");

        load_langs_from_dir(ed, TEDIT_DATADIR);

        if (home && home[0]) {
            char hdir[512];
            int  n = snprintf(hdir, sizeof(hdir), "%s/.tedit/lang", home);
            if (n > 0 && n < (int)sizeof(hdir))
                load_langs_from_dir(ed, hdir);
        }

        load_langs_from_dir(ed, "lang");
    }

    undo_init(ed);
}

void ed_free(Editor *ed)
{
    undo_free(ed);
    tb_free(&ed->tb);
    free(ed->clipboard);
}

int ed_open(Editor *ed, const char *filename)
{
    LineEnding   detected_end;
    FileEncoding detected_enc;
    int          replaced = 0;

    if (tb_load(&ed->tb, filename, &detected_end, &detected_enc, &replaced) != 0) {
        if (errno == ENOENT) {
            /* File does not exist yet -- start a new empty buffer with this
             * name and mark it modified so the user must save or explicitly
             * discard before closing. */
            tb_clear(&ed->tb);
            strncpy(ed->filename, filename, MAX_FILENAME - 1);
            ed->filename[MAX_FILENAME - 1] = '\0';
            ed->cur_row = ed->cur_col = 0;
            ed->top_row = ed->left_col = 0;
            ed->modified      = 1;
            ed->ending        = LE_UNIX;
            ed->enc           = ENC_UTF8;
            ed->use_hard_tabs = 0;
            undo_clear(ed);
            ed->hl = hl_match_ext(ed->hl_langs, ed->n_hl_langs, filename);
            ed->hl_dirty_row = 0;
            ed_set_message(ed, "[New file] \"%s\"", filename);
            return 0;
        }
        ed_set_message(ed, "Cannot open \"%s\"", filename);
        return -1;
    }

    ed->ending   = detected_end;
    ed->enc      = detected_enc;
    ed->modified = 0;
    ed->cur_row = ed->cur_col = 0;
    ed->top_row = ed->left_col = 0;

    /* Discard undo history for the previous file. */
    undo_clear(ed);

    /* Auto-enable hard-tab mode if the file contains tab characters. */
    {
        int r, c;
        ed->use_hard_tabs = 0;
        for (r = 0; r < ed->tb.count && !ed->use_hard_tabs; r++)
            for (c = 0; c < ed->tb.lines[r].len && !ed->use_hard_tabs; c++)
                if ((unsigned char)ed->tb.lines[r].data[c] == '\t')
                    ed->use_hard_tabs = 1;
    }

    strncpy(ed->filename, filename, MAX_FILENAME - 1);
    ed->filename[MAX_FILENAME - 1] = '\0';

    ed->hl = hl_match_ext(ed->hl_langs, ed->n_hl_langs, filename);
    ed->hl_dirty_row = 0;

    ed_set_message(ed, "\"%s\" %d line%s, %s, %s%s",
                    ed->filename, ed->tb.count,
                    ed->tb.count == 1 ? "" : "s",
                    ed->ending == LE_DOS ? "DOS" : "Unix",
                    ed->enc == ENC_UTF8  ? "UTF-8"  :
                    ed->enc == ENC_ASCII ? "ASCII"  : "CP437",
                    replaced ? " [codepoints replaced with '?']" : "");

    ed_clamp_and_scroll(ed);
    return 0;
}

int ed_save(Editor *ed, const char *filename)
{
    const char *name = (filename && filename[0]) ? filename : ed->filename;

    if (!name || !name[0]) {
        ed_set_message(ed, "No filename");
        return -1;
    }

    int replaced = 0;
    if (tb_save(&ed->tb, name, ed->ending, ed->enc, &replaced) != 0) {
        ed_set_message(ed, "Cannot write \"%s\"", name);
        return -1;
    }

    if (filename && filename[0]) {
        strncpy(ed->filename, filename, MAX_FILENAME - 1);
        ed->filename[MAX_FILENAME - 1] = '\0';
    }

    ed->modified = 0;
    ed->pending_quit = 0;

    if (replaced > 0)
        ed_set_message(ed, "Wrote \"%s\", %d line%s -- %d byte%s replaced with '?' (not in %s)",
                        ed->filename, ed->tb.count,
                        ed->tb.count == 1 ? "" : "s",
                        replaced, replaced == 1 ? "" : "s",
                        ed->enc == ENC_ASCII ? "ASCII" : "CP437");
    else
        ed_set_message(ed, "Wrote \"%s\", %d line%s", ed->filename,
                        ed->tb.count, ed->tb.count == 1 ? "" : "s");
    return 0;
}

/* -----------------------------------------------------------------------
 * Layout helpers
 * ----------------------------------------------------------------------- */

int ed_gutter_width(const Editor *ed)
{
    return ed->show_linenum ? GUTTER_WIDTH : 0;
}

int ed_text_cols(const Editor *ed)
{
    return SCR_COLS - ed_gutter_width(ed);
}

int ed_line_len(const Editor *ed, int row)
{
    if (row < 0 || row >= ed->tb.count) return 0;
    return ed->tb.lines[row].len;
}

int ed_visual_col(const Editor *ed, int row, int char_col)
{
    const Line *l;
    int vcol = 0, i;

    if (row < 0 || row >= ed->tb.count) return char_col;
    l = &ed->tb.lines[row];

    for (i = 0; i < char_col && i < l->len; i++) {
        if ((unsigned char)l->data[i] == '\t')
            vcol = ((vcol / ed->tabsize) + 1) * ed->tabsize;
        else
            vcol++;
    }
    return vcol;
}

void ed_clamp_and_scroll(Editor *ed)
{
    int text_cols = ed_text_cols(ed);
    int len, vcol;

    if (ed->cur_row < 0) ed->cur_row = 0;
    if (ed->cur_row >= ed->tb.count) ed->cur_row = ed->tb.count - 1;

    len = ed_line_len(ed, ed->cur_row);
    if (ed->cur_col < 0) ed->cur_col = 0;
    if (ed->cur_col > len) ed->cur_col = len;

    /* vertical scroll */
    if (ed->cur_row < ed->top_row)
        ed->top_row = ed->cur_row;
    if (ed->cur_row >= ed->top_row + TEXT_ROWS)
        ed->top_row = ed->cur_row - TEXT_ROWS + 1;
    if (ed->top_row < 0) ed->top_row = 0;

    /* Horizontal scroll is in visual columns (tabs expand to tabsize). */
    vcol = ed_visual_col(ed, ed->cur_row, ed->cur_col);

    if (vcol < ed->left_col)
        ed->left_col = vcol;
    if (vcol >= ed->left_col + text_cols)
        ed->left_col = vcol - text_cols + 1;
    if (ed->left_col < 0) ed->left_col = 0;

    /* When left_col > 0 the « indicator steals one column from the text
     * area.  Compensate so the cursor is never hidden behind it. */
    if (ed->left_col > 0 &&
        vcol >= ed->left_col + text_cols - 1)
        ed->left_col = vcol - (text_cols - 2);
}

void ed_set_message(Editor *ed, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(ed->message, MAX_MESSAGE, fmt, ap);
    va_end(ap);
}

/* -----------------------------------------------------------------------
 * Simple literal find/replace (single-line only)
 * --------------------------------------------------------------------- */

/* Scan forward from (start_row, start_col) to end of buffer.
 * On match: updates all editor state (selection, cursor, last_find*) and
 * returns 1.  On failure: returns 0 and leaves state unchanged.
 * Does NOT set any status message — callers do that. */
static int find_forward(Editor *ed, const char *pattern, size_t plen,
                        int start_row, int start_col)
{
    int r;
    for (r = start_row; r < ed->tb.count; r++) {
        Line *l = &ed->tb.lines[r];
        const char *s = l->data;
        if (!s) continue;
        int off = (r == start_row) ? start_col : 0;
        if (off < 0) off = 0;
        if (off > l->len) off = l->len;
        const char *found = NULL;
        if (off < l->len)
            found = strstr(s + off, pattern);
        if (found) {
            int col = (int)(found - s);
            ed->sel_active = 1;
            ed->sel_row = r;
            ed->sel_col = col + (int)plen; /* anchor at END of match   */
            ed->cur_row = r;
            ed->cur_col = col;             /* cursor at START — blinks
                                            * on first matched char and
                                            * is exempted from A_SEL   */
            ed->last_find_len = (int)plen;
            ed->last_find_row = r;
            ed->last_find_col = col;
            strncpy(ed->last_find, pattern, CONSOLE_BUFSZ - 1);
            ed->last_find[CONSOLE_BUFSZ - 1] = '\0';
            ed_clamp_and_scroll(ed);
            return 1;
        }
    }
    return 0;
}

/* Public entry point.  Searches forward from (start_row, start_col); if
 * nothing is found before the end of the buffer, wraps back to (0, 0) and
 * tries again so the user is never left stranded at the last match. */
int ed_find_from(Editor *ed, const char *pattern, int start_row, int start_col)
{
    if (!pattern || !*pattern) return 0;
    size_t plen = strlen(pattern);

    if (find_forward(ed, pattern, plen, start_row, start_col)) {
        ed_set_message(ed, "Found");
        return 1;
    }
    /* Nothing from here to EOF — wrap around to the top. */
    if (start_row == 0 && start_col == 0) {
        /* Already started from the very beginning; pattern doesn't exist. */
        ed_set_message(ed, "Not found");
        return 0;
    }
    if (find_forward(ed, pattern, plen, 0, 0)) {
        ed_set_message(ed, "Search wrapped");
        return 1;
    }
    ed_set_message(ed, "Not found");
    return 0;
}

int ed_find(Editor *ed, const char *pattern)
{
    /* start searching at current cursor column on current row */
    return ed_find_from(ed, pattern, ed->cur_row, ed->cur_col);
}

int ed_find_next(Editor *ed)
{
    if (ed->last_find[0] == '\0') {
        ed_set_message(ed, "No previous search");
        return 0;
    }
    /* start after the previous match if the cursor still at end */
    int sr = ed->last_find_row;
    int sc = ed->last_find_col + ed->last_find_len;
    /* If cursor moved past that, use current cursor */
    if (ed->cur_row > sr || (ed->cur_row == sr && ed->cur_col > sc)) {
        sr = ed->cur_row;
        sc = ed->cur_col;
    }
    return ed_find_from(ed, ed->last_find, sr, sc);
}

/* Replace the last found match (if any) with `newtext`. Returns 1 on success. */
int ed_replace_current(Editor *ed, const char *newtext)
{
    if (ed->last_find[0] == '\0') {
        ed_set_message(ed, "No active match to replace");
        return 0;
    }

    int row = ed->last_find_row;
    int col = ed->last_find_col;
    int oldlen = ed->last_find_len;
    int newlen = newtext ? (int)strlen(newtext) : 0;

    if (row < 0 || row >= ed->tb.count) return 0;

    /* Capture old text */
    char *oldbuf = NULL;
    if (oldlen > 0) {
        Line *l = &ed->tb.lines[row];
        oldbuf = (char *)malloc((size_t)oldlen);
        if (oldbuf) memcpy(oldbuf, l->data + col, (size_t)oldlen);
    }

    /* Record inverse: OP_INS_CHARS to restore old text */
    if (oldlen > 0) {
        UndoOp op;
        memset(&op, 0, sizeof(op));
        op.type = OP_INS_CHARS;
        op.row = row; op.col = col; op.len = oldlen; op.data = oldbuf;
        op.cur_row = ed->cur_row; op.cur_col = ed->cur_col;
        undo_record(ed, &op);
    } else {
        free(oldbuf);
        oldbuf = NULL;
    }

    /* Delete the old text (no undo recorded; we've already recorded inverse) */
    if (oldlen > 0)
        ed_delete_range(ed, row, col, row, col + oldlen);

    /* Insert new text */
    if (newlen > 0) {
        ed->cur_row = row; ed->cur_col = col;
        ed_paste_blob(ed, newtext, newlen);
    }

    /* Record undo for the insertion so undo removes the new text */
    if (newlen > 0) {
        UndoOp op2;
        memset(&op2, 0, sizeof(op2));
        op2.type = OP_DEL_CHARS;
        op2.row = row; op2.col = col; op2.len = newlen; op2.data = NULL;
        op2.cur_row = ed->cur_row; op2.cur_col = ed->cur_col;
        undo_record(ed, &op2);
    }

    /* Move cursor to end of inserted text and set selection to it */
    ed->cur_row = row;
    ed->cur_col = col + newlen;
    ed->sel_active = (newlen > 0);
    ed->sel_row = row;
    ed->sel_col = col;
    ed->modified = 1;
    ed_clamp_and_scroll(ed);
    ed_set_message(ed, "Replaced");
    return 1;
}

/* Replace all occurrences on all lines, returns count replaced */
int ed_replace_all(Editor *ed, const char *oldpat, const char *newpat)
{
    if (!oldpat || !*oldpat) return 0;
    int replaced = 0;
    int r;
    int oldlen = (int)strlen(oldpat);
    int newlen = (int)strlen(newpat);

    /* Iterate each line, performing replacements left-to-right. */
    for (r = 0; r < ed->tb.count; r++) {
        Line *l = &ed->tb.lines[r];
        int start = 0;
        while (start <= l->len - oldlen) {
            char *found = strstr(l->data + start, oldpat);
            if (!found) break;
            int col = (int)(found - l->data);
            /* Use ed_find/replace logic: record inverse then perform change */
            /* Capture old */
            char *oldbuf = (char *)malloc((size_t)oldlen);
            if (!oldbuf) return replaced;
            memcpy(oldbuf, l->data + col, (size_t)oldlen);
            UndoOp op;
            memset(&op, 0, sizeof(op));
            op.type = OP_INS_CHARS;
            op.row = r; op.col = col; op.len = oldlen; op.data = oldbuf;
            op.cur_row = ed->cur_row; op.cur_col = ed->cur_col;
            undo_record(ed, &op);

            /* Delete and insert */
            ed_delete_range(ed, r, col, r, col + oldlen);
            if (newlen > 0) {
                ed->cur_row = r; ed->cur_col = col;
                ed_paste_blob(ed, newpat, newlen);
                /* record deletion undo for new text */
                UndoOp op2;
                memset(&op2, 0, sizeof(op2));
                op2.type = OP_DEL_CHARS;
                op2.row = r; op2.col = col; op2.len = newlen; op2.data = NULL;
                op2.cur_row = ed->cur_row; op2.cur_col = ed->cur_col;
                undo_record(ed, &op2);
            }
            replaced++;
            /* Advance start after the inserted text to avoid infinite loop */
            start = col + newlen;
            /* refresh line pointer since underlying buffer may have changed */
            l = &ed->tb.lines[r];
        }
    }
    if (replaced > 0) {
        ed->modified = 1;
        ed_set_message(ed, "Replaced %d occurrence%s", replaced, replaced == 1 ? "" : "s");
    } else {
        ed_set_message(ed, "No matches");
    }
    ed_clamp_and_scroll(ed);
    return replaced;
}

/* -----------------------------------------------------------------------
 * Editing
 * ----------------------------------------------------------------------- */

void ed_insert_char(Editor *ed, char ch)
{
    Line *l = &ed->tb.lines[ed->cur_row];

    if (ed->insert_mode || ed->cur_col >= l->len) {
        line_insert_char(l, ed->cur_col, ch);
    } else {
        l->data[ed->cur_col] = ch;
    }

    ED_MARK_DIRTY(ed, ed->cur_row);
    ed->cur_col++;
    ed->modified = 1;
    ed_clamp_and_scroll(ed);
}

void ed_insert_tab(Editor *ed)
{
    if (ed->use_hard_tabs) {
        int r = ed->cur_row, c = ed->cur_col;
        UndoOp op;
        ed_insert_char(ed, '\t');
        memset(&op, 0, sizeof(op));
        op.type = OP_DEL_CHARS;
        op.row = r;  op.col = c;  op.len = 1;
        op.cur_row = r;  op.cur_col = c;
        undo_record(ed, &op);
    } else {
        int r      = ed->cur_row, c = ed->cur_col;
        int vcol   = ed_visual_col(ed, r, c);
        int spaces = ed->tabsize - (vcol % ed->tabsize);
        Line *l    = &ed->tb.lines[r];
        UndoOp op;

        line_insert_spaces(l, c, spaces);
        ED_MARK_DIRTY(ed, r);
        ed->cur_col += spaces;
        ed->modified = 1;
        ed_clamp_and_scroll(ed);

        memset(&op, 0, sizeof(op));
        op.type = OP_DEL_CHARS;
        op.row = r;  op.col = c;  op.len = spaces;
        op.cur_row = r;  op.cur_col = c;
        undo_record(ed, &op);
    }
}

static void ed_unindent(Editor *ed)
{
    Line *l = &ed->tb.lines[ed->cur_row];
    int remove = 0;
    int old_col = ed->cur_col;
    UndoOp op;

    /* Hard-tab: remove one leading tab character. */
    if (l->len > 0 && (unsigned char)l->data[0] == '\t') {
        memset(&op, 0, sizeof(op));
        op.type = OP_INS_CHARS;
        op.row = ed->cur_row;  op.col = 0;
        op.len = 1;
        op.data = (char *)malloc(1);
        if (op.data) op.data[0] = '\t';
        op.cur_row = ed->cur_row;  op.cur_col = old_col;

        line_delete_char(l, 0);
        if (ed->cur_col > 0) ed->cur_col--;
        ed->modified = 1;
        ED_MARK_DIRTY(ed, ed->cur_row);
        ed_clamp_and_scroll(ed);

        undo_record(ed, &op);
        return;
    }

    /* Soft-tab: remove up to one tab-stop worth of leading spaces. */
    while (remove < ed->tabsize && remove < l->len &&
           l->data[remove] == ' ')
        remove++;

    if (remove == 0) return;

    memset(&op, 0, sizeof(op));
    op.type = OP_INS_CHARS;
    op.row = ed->cur_row;  op.col = 0;
    op.len = remove;
    op.data = (char *)malloc((size_t)remove);
    if (op.data) memcpy(op.data, l->data, (size_t)remove);
    op.cur_row = ed->cur_row;  op.cur_col = old_col;

    while (remove > 0) {
        line_delete_char(l, 0);
        remove--;
        ed->cur_col--;
        if (ed->cur_col < 0) ed->cur_col = 0;
    }

    ed->modified = 1;
    ED_MARK_DIRTY(ed, ed->cur_row);
    ed_clamp_and_scroll(ed);

    undo_record(ed, &op);
}

void ed_enter(Editor *ed)
{
    Line *l = &ed->tb.lines[ed->cur_row];
    int leading_ws = 0;
    int indent, carried_len;
    char *carried   = NULL;
    char *ws_prefix = NULL; /* copy of leading whitespace to replicate */

    /* Scan leading whitespace (spaces AND tabs). */
    while (leading_ws < l->len &&
           (l->data[leading_ws] == ' ' || l->data[leading_ws] == '\t'))
        leading_ws++;

    /* New line inherits the same indentation as this one, but never
     * more than the column the cursor is currently sitting at. */
    indent = (leading_ws < ed->cur_col) ? leading_ws : ed->cur_col;

    /* Save a copy of the whitespace before tb_insert_line can reallocate
     * the lines array (which would invalidate `l`). */
    if (indent > 0) {
        ws_prefix = (char *)malloc((size_t)indent);
        memcpy(ws_prefix, l->data, (size_t)indent);
    }

    carried_len = l->len - ed->cur_col;
    if (carried_len > 0) {
        carried = (char *)malloc((size_t)carried_len);
        memcpy(carried, l->data + ed->cur_col, (size_t)carried_len);
    }

    line_truncate(l, ed->cur_col);

    tb_insert_line(&ed->tb, ed->cur_row + 1, NULL, 0);
    {
        Line *nl = &ed->tb.lines[ed->cur_row + 1];
        /* Reproduce the exact whitespace characters (preserves \t). */
        if (indent > 0 && ws_prefix)
            line_append(nl, ws_prefix, indent);
        if (carried_len > 0)
            line_append(nl, carried, carried_len);
    }

    free(ws_prefix);
    free(carried);

    /* Record undo BEFORE updating cur_row/cur_col.
     * The new line already contains [ws_prefix | carried] = [indent | B].
     * OP_JOIN_LINES will append data[col2:] = B to line[cur_row] to undo. */
    {
        Line  *nl = &ed->tb.lines[ed->cur_row + 1];
        UndoOp op;
        memset(&op, 0, sizeof(op));
        op.type    = OP_JOIN_LINES;
        op.row     = ed->cur_row + 1;   /* new line's index */
        op.col2    = indent;             /* bytes to skip (indent prefix) */
        op.len     = nl->len;
        op.data    = op.len > 0 ? (char *)malloc((size_t)op.len) : NULL;
        if (op.data) memcpy(op.data, nl->data, (size_t)op.len);
        op.cur_row = ed->cur_row;
        op.cur_col = ed->cur_col;       /* = split point C (not yet updated) */
        undo_record(ed, &op);
    }

    ed->cur_row++;
    ed->cur_col = indent;
    ed->modified = 1;
    ED_MARK_DIRTY(ed, ed->cur_row - 1);
    ed_clamp_and_scroll(ed);
}

void ed_backspace(Editor *ed)
{
    Line *l = &ed->tb.lines[ed->cur_row];

    if (ed->cur_col > 0) {
        UndoOp op;
        memset(&op, 0, sizeof(op));

        /* Indent-snap: if the cursor sits right after a run of leading
         * whitespace (and only whitespace), remove the whole prefix at once
         * so one BS snaps back to column 0. */
        int is_all_ws = 1, i;
        for (i = 0; i < ed->cur_col; i++) {
            if (l->data[i] != ' ' && l->data[i] != '\t') {
                is_all_ws = 0;
                break;
            }
        }

        if (is_all_ws && ed->cur_col > 0) {
            /* Backspace to previous tab stop when prefix is all whitespace.
             * Compute current visual column and the target visual column
             * (previous tab stop). Then delete the characters between the
             * target char column and the cursor. */
            int r = ed->cur_row;
            int vcol = ed_visual_col(ed, r, ed->cur_col);
            int tab = ed->tabsize > 0 ? ed->tabsize : DEFAULT_TABSIZE;
            int target_vcol;

            if (vcol <= 0)
                target_vcol = 0;
            else
                target_vcol = ((vcol - 1) / tab) * tab;

            /* Find character column that corresponds to target_vcol. */
            int cur_v = 0;
            int target_char_col = -1;

            /* If target visual column is 0, delete back to column 0. */
            if (target_vcol == 0) {
                target_char_col = 0;
            } else {
                for (i = 0; i < ed->cur_col; i++) {
                    if ((unsigned char)l->data[i] == '\t')
                        cur_v = ((cur_v / tab) + 1) * tab;
                    else
                        cur_v++;

                    /* Use >= to handle cases where a tab expands past the
                     * desired visual column. */
                    if (cur_v >= target_vcol) {
                        target_char_col = i + 1; /* char after this yields target_vcol */
                        break;
                    }
                }
            }

            /* If we didn't find a matching char column, fall back to removing
             * a single character before the cursor. */
            int start_col = ed->cur_col - 1;
            int count = 1;
            if (target_char_col >= 0 && target_char_col < ed->cur_col) {
                start_col = target_char_col;
                count = ed->cur_col - target_char_col;
                if (count <= 0) { start_col = ed->cur_col - 1; count = 1; }
            }

            op.type    = OP_INS_CHARS;
            op.row     = ed->cur_row;
            op.col     = start_col;
            op.len     = count;
            op.data    = (char *)malloc((size_t)count);
            if (op.data) memcpy(op.data, l->data + start_col, (size_t)count);
            op.cur_row = ed->cur_row;
            op.cur_col = ed->cur_col;
            undo_record(ed, &op);

            /* Delete the selected characters starting at start_col. */
            for (i = 0; i < count; i++)
                line_delete_char(l, start_col);
            ed->cur_col = start_col;

        } else {
            /* Normal single-char backspace. */
            op.type    = OP_INS_CHARS;
            op.row     = ed->cur_row;
            op.col     = ed->cur_col - 1;
            op.len     = 1;
            op.data    = (char *)malloc(1);
            if (op.data) op.data[0] = l->data[ed->cur_col - 1];
            op.cur_row = ed->cur_row;
            op.cur_col = ed->cur_col;
            undo_record(ed, &op);

            line_delete_char(l, ed->cur_col - 1);
            ed->cur_col--;
        }

    } else if (ed->cur_row > 0) {
        /* Line-join: record inverse split. */
        Line  *prev = &ed->tb.lines[ed->cur_row - 1];
        int    new_col = prev->len;
        UndoOp op;

        memset(&op, 0, sizeof(op));
        op.type    = OP_SPLIT_LINE;
        op.row     = ed->cur_row;   /* line to restore */
        op.col     = prev->len;     /* split point = current prev length */
        op.col2    = 0;             /* no indent in restored line */
        op.len     = l->len;
        op.data    = op.len > 0 ? (char *)malloc((size_t)op.len) : NULL;
        if (op.data) memcpy(op.data, l->data, (size_t)op.len);
        op.cur_row = ed->cur_row;
        op.cur_col = 0;
        undo_record(ed, &op);

        if (l->len > 0)
            line_append(prev, l->data, l->len);
        tb_delete_line(&ed->tb, ed->cur_row);
        ed->cur_row--;
        ed->cur_col = new_col;
    }

    ed->modified = 1;
    ED_MARK_DIRTY(ed, ed->cur_row);
    ed_clamp_and_scroll(ed);
}

void ed_delete(Editor *ed)
{
    Line *l = &ed->tb.lines[ed->cur_row];

    if (ed->cur_col < l->len) {
        /* Single-char forward delete: record inverse insert. */
        UndoOp op;
        memset(&op, 0, sizeof(op));
        op.type    = OP_INS_CHARS;
        op.row     = ed->cur_row;
        op.col     = ed->cur_col;
        op.len     = 1;
        op.data    = (char *)malloc(1);
        if (op.data) op.data[0] = l->data[ed->cur_col];
        op.cur_row = ed->cur_row;
        op.cur_col = ed->cur_col;
        undo_record(ed, &op);

        line_delete_char(l, ed->cur_col);

    } else if (ed->cur_row < ed->tb.count - 1) {
        /* Line-join: record inverse split. */
        Line  *next = &ed->tb.lines[ed->cur_row + 1];
        UndoOp op;

        memset(&op, 0, sizeof(op));
        op.type    = OP_SPLIT_LINE;
        op.row     = ed->cur_row + 1;
        op.col     = ed->cur_col;   /* = l->len = end of current line */
        op.col2    = 0;
        op.len     = next->len;
        op.data    = op.len > 0 ? (char *)malloc((size_t)op.len) : NULL;
        if (op.data) memcpy(op.data, next->data, (size_t)op.len);
        op.cur_row = ed->cur_row;
        op.cur_col = ed->cur_col;
        undo_record(ed, &op);

        if (next->len > 0)
            line_append(l, next->data, next->len);
        tb_delete_line(&ed->tb, ed->cur_row + 1);
    }

    ed->modified = 1;
    ED_MARK_DIRTY(ed, ed->cur_row);
    ed_clamp_and_scroll(ed);
}

/* Forward declarations — defined below in the Clipboard section. */
static char *ed_selection_text(const Editor *ed, int *len_out);
static void  ed_selection_bounds(const Editor *ed, int *sr, int *sc,
                                  int *er, int *ec);

void ed_delete_range(Editor *ed, int sr, int sc, int er, int ec)
{
    int i;

    if (sr == er) {
        int count = ec - sc;
        for (i = 0; i < count; i++)
            line_delete_char(&ed->tb.lines[sr], sc);
    } else {
        Line *first = &ed->tb.lines[sr];
        Line *last  = &ed->tb.lines[er];

        line_truncate(first, sc);
        if (ec < last->len)
            line_append(first, last->data + ec, last->len - ec);

        for (i = er; i > sr; i--)
            tb_delete_line(&ed->tb, i);
    }

    ed->modified = 1;
    ED_MARK_DIRTY(ed, sr);
}

void ed_delete_selection(Editor *ed)
{
    int sr, sc, er, ec;
    char *sel_text;
    int   sel_len;

    if (!ed->sel_active) return;

    /* Must match ed_selection_text's bounds exactly, or cut would copy
     * one character but delete a different amount, corrupting undo. */
    ed_selection_bounds(ed, &sr, &sc, &er, &ec);

    /* Capture selection text for undo before deleting. */
    sel_text = ed_selection_text(ed, &sel_len);
    if (sel_text && sel_len > 0) {
        UndoOp op;
        memset(&op, 0, sizeof(op));
        op.type    = OP_INS_BLOB;
        op.row     = sr;  op.col = sc;
        op.len     = sel_len;
        op.data    = sel_text;   /* transferred to undo stack */
        op.cur_row = er;  op.cur_col = ec;
        undo_record(ed, &op);
    } else {
        free(sel_text);
    }

    ed_delete_range(ed, sr, sc, er, ec);

    ed->cur_row    = sr;
    ed->cur_col    = sc;
    ed->sel_active = 0;
    ed->modified   = 1;
    ed_clamp_and_scroll(ed);
}

/* -----------------------------------------------------------------------
 * Clipboard  (Ctrl+C copy, Ctrl+X cut, Ctrl+V paste)
 * ----------------------------------------------------------------------- */

/* Compute the effective (sr,sc)-(er,ec) bounds of the active selection for
 * data operations (copy / cut / delete).
 *
 * The raw anchor-to-cursor range is half-open: (er,ec) is one column past
 * the last selected character.  That matches the usual "caret between two
 * characters" model, but it does NOT match what this editor actually shows
 * onscreen: the block cursor is drawn by inverting the cell it sits on
 * (see vgaterm.c / render_cell), and A_SEL is defined as exactly the
 * colour-inverse of A_TEXT.  So a plain, unselected character sitting under
 * the cursor renders pixel-identical to a selected one.  When the live
 * cursor is the later endpoint of the selection, the character it rests on
 * visually looks selected even though the half-open range excludes it --
 * which is why selecting through the last letter of a word and copying it
 * silently drops that letter.
 *
 * To make data operations match what the user actually sees highlighted,
 * fold that trailing character into the range whenever the live cursor
 * (not the anchor) is the endpoint resting on a real character. */
static void ed_selection_bounds(const Editor *ed, int *sr, int *sc,
                                 int *er, int *ec)
{
    if (ed->cur_row < ed->sel_row ||
        (ed->cur_row == ed->sel_row && ed->cur_col <= ed->sel_col)) {
        *sr = ed->cur_row; *sc = ed->cur_col;
        *er = ed->sel_row; *ec = ed->sel_col;
    } else {
        *sr = ed->sel_row; *sc = ed->sel_col;
        *er = ed->cur_row; *ec = ed->cur_col;
    }

    if (*er == ed->cur_row && *ec == ed->cur_col &&
        *ec < ed->tb.lines[*er].len) {
        (*ec)++;
    }
}

/* Build a flat string (newlines between lines) from the current selection.
 * Returns a malloc'd, NUL-terminated buffer and sets *len_out to its byte
 * length.  Returns NULL if there is no active selection or on alloc failure. */
static char *ed_selection_text(const Editor *ed, int *len_out)
{
    int sr, sc, er, ec;
    int total_len, row;
    char *buf;
    int pos;

    *len_out = 0;
    if (!ed->sel_active) return NULL;

    ed_selection_bounds(ed, &sr, &sc, &er, &ec);

    /* Calculate required size */
    if (sr == er) {
        total_len = ec - sc;
    } else {
        total_len = ed->tb.lines[sr].len - sc + 1; /* +1 for \n */
        for (row = sr + 1; row < er; row++)
            total_len += ed->tb.lines[row].len + 1; /* +1 for \n */
        total_len += ec;
    }

    if (total_len <= 0) return NULL;

    buf = (char *)malloc((size_t)(total_len + 1));
    if (!buf) return NULL;

    pos = 0;
    if (sr == er) {
        memcpy(buf + pos, ed->tb.lines[sr].data + sc, ec - sc);
        pos += ec - sc;
    } else {
        int first_len = ed->tb.lines[sr].len - sc;
        memcpy(buf + pos, ed->tb.lines[sr].data + sc, (size_t)first_len);
        pos += first_len;
        buf[pos++] = '\n';
        for (row = sr + 1; row < er; row++) {
            int rlen = ed->tb.lines[row].len;
            if (rlen > 0) {
                memcpy(buf + pos, ed->tb.lines[row].data, (size_t)rlen);
                pos += rlen;
            }
            buf[pos++] = '\n';
        }
        if (ec > 0) {
            memcpy(buf + pos, ed->tb.lines[er].data, (size_t)ec);
            pos += ec;
        }
    }
    buf[pos] = '\0';
    *len_out = pos;
    return buf;
}

/* Split the current line at the cursor with no auto-indent.
 * Used by ed_paste_text so pasted indentation is not compounded. */
static void break_line_raw(Editor *ed)
{
    Line *l = &ed->tb.lines[ed->cur_row];
    int   carried_len = l->len - ed->cur_col;
    char *carried = NULL;

    if (carried_len > 0) {
        carried = (char *)malloc((size_t)carried_len);
        memcpy(carried, l->data + ed->cur_col, (size_t)carried_len);
    }
    line_truncate(l, ed->cur_col);
    tb_insert_line(&ed->tb, ed->cur_row + 1, carried, carried_len);
    free(carried);

    ed->cur_row++;
    ed->cur_col = 0;
    ed->modified = 1;
    ED_MARK_DIRTY(ed, ed->cur_row - 1);
    ed_clamp_and_scroll(ed);
}

/* Insert arbitrary text at the cursor, splitting on \n (\r is ignored).
 * Does NOT perform auto-indent on newlines, so pasted indentation is
 * preserved exactly as stored.  Does NOT record undo — callers wrap this. */
void ed_paste_blob(Editor *ed, const char *text, int len)
{
    int i;

    /* If the pasted content contains tab characters, automatically switch
     * to hard-tab mode so they are preserved as \t in the buffer. */
    if (!ed->use_hard_tabs) {
        for (i = 0; i < len; i++) {
            if (text[i] == '\t') {
                ed->use_hard_tabs = 1;
                break;
            }
        }
    }

    for (i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)text[i];
        if (ch == '\n') {
            break_line_raw(ed);
        } else if (ch == '\r' &&
                   i + 1 < len && (unsigned char)text[i + 1] == '\n') {
            /* CR of a DOS \r\n pair: skip it; the \n handles the break.
             * A bare 0x0D is the CP437 ♪ glyph (U+266A) and falls through
             * to ed_insert_char below. */
        } else {
            ed_insert_char(ed, (char)ch);
        }
    }
    ed->modified = 1;
}

void ed_copy(Editor *ed)
{
    /* Copy is non-typing: stop coalescing so undo groups don't include prior typing. */
    undo_reset_coalesce(ed);
    char *extract;
    int   len;

    if (!ed->sel_active) {
        /* No explicit selection: fall back to copying the single character
         * the cursor is currently resting on, since that's the character
         * the block cursor visually highlights (it inverts the cell it
         * sits on -- see ed_selection_bounds for why that matters). */
        Line *l = &ed->tb.lines[ed->cur_row];
        if (ed->cur_col >= l->len) {
            ed_set_message(ed, "No selection -- use Shift+Arrow to select");
            return;
        }
        extract = (char *)malloc(2);
        if (!extract) return;
        extract[0] = l->data[ed->cur_col];
        extract[1] = '\0';
        len = 1;
    } else {
        extract = ed_selection_text(ed, &len);
        if (!extract || len == 0) {
            free(extract);
            return;
        }
    }

    /* X11 clipboard always expects UTF-8; encode from our internal CP437.
     * vio_clipboard_set takes ownership of the buffer it is given. */
    {
        int   utf8len;
        char *utf8buf = enc_cp437_to_utf8(extract, len, &utf8len);
        if (utf8buf)
            vio_clipboard_set(utf8buf, utf8len);
    }

    /* Keep our own CP437 copy for fast self-paste while we own the selection. */
    free(ed->clipboard);
    ed->clipboard     = extract;
    ed->clipboard_len = len;

    ed_set_message(ed, "Copied %d char%s", len, len == 1 ? "" : "s");
    /* Leave sel_active so the highlight stays visible after copy. */
}

void ed_cut(Editor *ed)
{
    /* Cut is non-typing: stop coalescing. */
    undo_reset_coalesce(ed);
    if (!ed->sel_active) {
        ed_set_message(ed, "No selection -- use Shift+Arrow to select");
        return;
    }
    ed_copy(ed);
    ed_delete_selection(ed);
    ed_set_message(ed, "Cut");
}

/* -----------------------------------------------------------------------
 * Navigation
 * ----------------------------------------------------------------------- */

void ed_move_up(Editor *ed)
{
    if (ed->cur_row > 0) ed->cur_row--;
    ed_clamp_and_scroll(ed);
}

void ed_move_down(Editor *ed)
{
    if (ed->cur_row < ed->tb.count - 1) ed->cur_row++;
    ed_clamp_and_scroll(ed);
}

void ed_move_left(Editor *ed)
{
    if (ed->cur_col > 0) {
        ed->cur_col--;
    } else if (ed->cur_row > 0) {
        ed->cur_row--;
        ed->cur_col = ed_line_len(ed, ed->cur_row);
    }
    ed_clamp_and_scroll(ed);
}

void ed_move_right(Editor *ed)
{
    int len = ed_line_len(ed, ed->cur_row);

    if (ed->cur_col < len) {
        ed->cur_col++;
    } else if (ed->cur_row < ed->tb.count - 1) {
        ed->cur_row++;
        ed->cur_col = 0;
    }
    ed_clamp_and_scroll(ed);
}

void ed_move_doc_start(Editor *ed)
{
    ed->cur_row = 0;
    ed->cur_col = 0;
    ed_clamp_and_scroll(ed);
}
void ed_move_line_start(Editor *ed)
{
    ed->cur_col = 0;
    ed_clamp_and_scroll(ed);
}

void ed_move_line_end(Editor *ed)
{
    ed->cur_col = ed_line_len(ed, ed->cur_row);
    ed_clamp_and_scroll(ed);
}

void ed_move_doc_end(Editor *ed)
{
    ed->cur_row = ed->tb.count - 1;
    ed->cur_col = ed_line_len(ed, ed->cur_row);
    ed_clamp_and_scroll(ed);
}

void ed_page_up(Editor *ed)
{
    ed->cur_row -= TEXT_ROWS;
    if (ed->cur_row < 0) ed->cur_row = 0;
    ed_clamp_and_scroll(ed);
}

void ed_page_down(Editor *ed)
{
    ed->cur_row += TEXT_ROWS;
    if (ed->cur_row > ed->tb.count - 1) ed->cur_row = ed->tb.count - 1;
    ed_clamp_and_scroll(ed);
}

/* -----------------------------------------------------------------------
 * Mouse helpers
 * ----------------------------------------------------------------------- */

/*
 * screen_to_buf_pos  --  convert screen cell (scr_col, scr_row) to buffer
 * (buf_row, buf_col), accounting for gutter, left_col offset, « indicator,
 * and tab expansion.
 *
 * Returns 1 and fills *out_row / *out_col if the click is in the text area.
 * Returns 0 for status row or positions fully outside the buffer.
 * Gutter clicks are clamped to column 0 of that row.
 */
static int screen_to_buf_pos(const Editor *ed, int scr_col, int scr_row,
                              int *out_row, int *out_col)
{
    int gw, buf_row, ind, text_scr_col, target_vcol, vcol, i;
    Line *l;

    /* Status row is not editable text. */
    if (scr_row >= TEXT_ROWS) return 0;

    buf_row = ed->top_row + scr_row;
    if (buf_row < 0) buf_row = 0;
    if (buf_row >= ed->tb.count) buf_row = ed->tb.count > 0 ? ed->tb.count - 1 : 0;

    gw = ed_gutter_width(ed);

    /* Click in gutter → column 0. */
    if (scr_col < gw) {
        *out_row = buf_row;
        *out_col = 0;
        return 1;
    }

    /* « indicator steals one text column when left_col > 0. */
    ind = (ed->left_col > 0) ? 1 : 0;
    text_scr_col = scr_col - gw - ind;
    if (text_scr_col < 0) text_scr_col = 0;

    target_vcol = ed->left_col + text_scr_col;

    /* Walk the line expanding tabs to find the char index. */
    l = &ed->tb.lines[buf_row];
    vcol = 0;
    for (i = 0; i < l->len; i++) {
        int cw;
        if ((unsigned char)l->data[i] == '\t') {
            cw = ((vcol / ed->tabsize) + 1) * ed->tabsize - vcol;
        } else {
            cw = 1;
        }
        /* Click inside this character's visual span → snap to it. */
        if (target_vcol < vcol + cw) {
            *out_row = buf_row;
            *out_col = i;
            return 1;
        }
        vcol += cw;
    }

    /* Past end of line. */
    *out_row = buf_row;
    *out_col = l->len;
    return 1;
}

/* -----------------------------------------------------------------------
 * Key dispatch
 * ----------------------------------------------------------------------- */

/* Macro: set the anchor if not already selecting, then move. */
#define SEL_ANCHOR(ed)  do {                      \
    if (!(ed)->sel_active) {                      \
        (ed)->sel_active = 1;                     \
        (ed)->sel_row    = (ed)->cur_row;         \
        (ed)->sel_col    = (ed)->cur_col;         \
        undo_reset_coalesce(ed);                 \
    }                                             \
} while (0)

void ed_handle_key(Editor *ed, int key)
{
    switch (key) {

    case KEY_ESC:
        /* Open the command console.  Any active selection is dropped first
         * so the console has a clean context.  Status messages are left
         * alone; they clear naturally when the user starts typing. */
        ed->sel_active = 0;
        undo_reset_coalesce(ed);
        ed->console_active = 1;
        ed->console_len    = 0;
        ed->console_buf[0] = '\0';
        break;

    case KEY_CTRL('s'):
        ed_save(ed, NULL);
        break;

    case KEY_CTRL('q'):
        /* Require confirmation if buffer modified to avoid accidental quit. */
        if (ed->modified && !ed->pending_quit) {
            ed->pending_quit = 1;
            ed_set_message(ed, "Unsaved,press [Ctrl+Q] again to force quit!");
        } else {
            ed->quit = 1;
        }
        break;

    /* ---- clipboard ---------------------------------------------------- */
    case KEY_CTRL('c'):
        ed_copy(ed);
        break;

    case KEY_CTRL('x'):
        ed_cut(ed);
        break;

    case KEY_CTRL('v'):
        /* Pastes are non-typing actions: stop coalescing previous typing.
         * Also cancel any pending quit since the user acted. */
        undo_reset_coalesce(ed);
        ed->pending_quit = 0;
        /* Delete any active selection first (replace-on-paste). */
        if (ed->sel_active)
            ed_delete_selection(ed);
        if (vio_clipboard_owns()) {
            /* Fast path: we own the X11 selection, paste our internal copy. */
            if (ed->clipboard && ed->clipboard_len > 0) {
                int    sr = ed->cur_row, sc = ed->cur_col;
                int    plen = ed->clipboard_len;
                char  *dcopy = (char *)malloc((size_t)plen);
                if (dcopy) memcpy(dcopy, ed->clipboard, (size_t)plen);
                ed_paste_blob(ed, ed->clipboard, plen);
                if (dcopy) {
                    UndoOp op;
                    memset(&op, 0, sizeof(op));
                    op.type    = OP_DEL_RANGE;
                    op.row     = sr;           op.col  = sc;
                    op.row2    = ed->cur_row;  op.col2 = ed->cur_col;
                    op.data    = dcopy;        op.len  = plen;
                    op.cur_row = sr;           op.cur_col = sc;
                    undo_record(ed, &op);
                }
            } else {
                ed_set_message(ed, "Nothing to paste");
            }
        } else {
            /* Ask the X11 selection owner; KEY_PASTE_READY will follow. */
            vio_clipboard_request();
        }
        break;

    case KEY_PASTE_READY:
        {
            /* Paste completes a non-typing action: reset coalesce.
             * Also cancel pending quit. */
            undo_reset_coalesce(ed);
            ed->pending_quit = 0;
            int         plen;
            const char *ptext = vio_clipboard_take(&plen);
            if (ptext && plen > 0) {
                /* X11 delivers UTF-8; convert to CP437 before inserting. */
                int   cplen, replaced;
                char *cpbuf = enc_utf8_to_cp437(ptext, plen, &cplen, &replaced);
                if (cpbuf && cplen > 0) {
                    int   sr = ed->cur_row, sc = ed->cur_col;
                    char *dcopy = (char *)malloc((size_t)cplen);
                    if (dcopy) memcpy(dcopy, cpbuf, (size_t)cplen);
                    ed_paste_blob(ed, cpbuf, cplen);
                    free(cpbuf);
                    if (dcopy) {
                        UndoOp op;
                        memset(&op, 0, sizeof(op));
                        op.type    = OP_DEL_RANGE;
                        op.row     = sr;           op.col  = sc;
                        op.row2    = ed->cur_row;  op.col2 = ed->cur_col;
                        op.data    = dcopy;        op.len  = cplen;
                        op.cur_row = sr;           op.cur_col = sc;
                        undo_record(ed, &op);
                    }
                    if (replaced > 0)
                        ed_set_message(ed,
                            "Pasted -- %d codepoint%s replaced with '?'",
                            replaced, replaced == 1 ? "" : "s");
                } else {
                    free(cpbuf);
                    ed_set_message(ed, "Nothing to paste");
                }
            } else {
                ed_set_message(ed, "Nothing to paste");
            }
        }
        break;

    /* ---- undo / redo -------------------------------------------------- */
    case KEY_CTRL('z'):
        ed->sel_active = 0;
        if (!undo_do(ed))
            ed_set_message(ed, "Nothing to undo");
        else
            ed->hl_dirty_row = 0;
        break;

    case KEY_CTRL('y'):
        ed->sel_active = 0;
        if (!redo_do(ed))
            ed_set_message(ed, "Nothing to redo");
        else
            ed->hl_dirty_row = 0;
        break;

    /* ---- find --------------------------------------------------------- */
    case KEY_CTRL('f'):
        /* Open the console pre-filled with "find <last pattern>" if a
         * previous search exists, or plain "find " if not.  The user types
         * (or edits) the term and presses Enter to run. */
        ed->sel_active = 0;
        ed->console_active = 1;
        if (ed->last_find[0] != '\0') {
            int n = snprintf(ed->console_buf, CONSOLE_BUFSZ,
                             "find %s", ed->last_find);
            ed->console_len = (n >= 0 && n < CONSOLE_BUFSZ)
                              ? n : CONSOLE_BUFSZ - 1;
        } else {
            memcpy(ed->console_buf, "find ", 5);
            ed->console_buf[5] = '\0';
            ed->console_len    = 5;
        }
        break;

    case KEY_F3:
        /* Find next — repeat the last search from after the current match.
         * The result message ("Found" / "Search wrapped" / "Not found")
         * appears in the status bar as a hint, same as any other message. */
        ed_find_next(ed);
        break;

    /* ---- plain navigation — cancels any active selection -------------- */
    case KEY_UP:
        ed->sel_active = 0;
        undo_reset_coalesce(ed);
        ed_move_up(ed);
        break;
    case KEY_DOWN:
        ed->sel_active = 0;
        undo_reset_coalesce(ed);
        ed_move_down(ed);
        break;
    case KEY_LEFT:
        ed->sel_active = 0;
        undo_reset_coalesce(ed);
        ed_move_left(ed);
        break;
    case KEY_RIGHT:
        ed->sel_active = 0;
        undo_reset_coalesce(ed);
        ed_move_right(ed);
        break;
    case KEY_HOME:
        ed->sel_active = 0;
        undo_reset_coalesce(ed);
        ed_move_line_start(ed);
        break;
    case KEY_CTRL_HOME:
        ed->sel_active = 0;
        undo_reset_coalesce(ed);
        ed_move_doc_start(ed);
        break;
    case KEY_END:
        ed->sel_active = 0;
        undo_reset_coalesce(ed);
        ed_move_line_end(ed);
        break;
    case KEY_CTRL_END:
        ed->sel_active = 0;
        undo_reset_coalesce(ed);
        ed_move_doc_end(ed);
        break;
    case KEY_PGUP:
        ed->sel_active = 0;
        undo_reset_coalesce(ed);
        ed_page_up(ed);
        break;
    case KEY_PGDN:
        ed->sel_active = 0;
        undo_reset_coalesce(ed);
        ed_page_down(ed);
        break;

    /* ---- Shift+Arrow — extend (or start) selection ------------------- */
    case KEY_SHIFT_LEFT:
        SEL_ANCHOR(ed);
        ed_move_left(ed);
        break;
    case KEY_SHIFT_RIGHT:
        SEL_ANCHOR(ed);
        ed_move_right(ed);
        break;
    case KEY_SHIFT_UP:
        SEL_ANCHOR(ed);
        ed_move_up(ed);
        break;
    case KEY_SHIFT_DOWN:
        SEL_ANCHOR(ed);
        ed_move_down(ed);
        break;

    /* ---- insert / overwrite toggle ------------------------------------ */
    case KEY_INS:
        ed->sel_active = 0;
        undo_reset_coalesce(ed);
        ed->insert_mode = !ed->insert_mode;
        ed_set_message(ed, ed->insert_mode ? "-- INSERT --" : "-- OVERWRITE --");
        break;

    /* ---- editing ------------------------------------------------------ */
    case KEY_TAB:
        ed->sel_active = 0;
        undo_reset_coalesce(ed);
        ed->pending_quit = 0;
        ed_insert_tab(ed);
        break;
    case KEY_SHIFT_TAB:
        ed->sel_active = 0;
        undo_reset_coalesce(ed);
        ed->pending_quit = 0;
        ed_unindent(ed);
        break;
    case KEY_ENTER:
        ed->sel_active = 0;
        undo_reset_coalesce(ed);
        ed->pending_quit = 0;
        ed_enter(ed);
        break;

    case KEY_BS:
        /* User edited: cancel pending quit */
        ed->pending_quit = 0;
        if (ed->sel_active)
            ed_delete_selection(ed);
        else
            ed_backspace(ed);
        break;

    case KEY_DEL:
        /* Deleting is structural: stop coalescing typing and cancel pending quit. */
        undo_reset_coalesce(ed);
        ed->pending_quit = 0;
        if (ed->sel_active)
            ed_delete_selection(ed);
        else
            ed_delete(ed);
        break;

    /* ---- ignored events ----------------------------------------------- */
    case KEY_SHIFT_RELEASE:
        break;

    /* ---- scroll wheel ------------------------------------------------- */
    case KEY_SCROLL_UP: {
        /* Scroll the view up (toward the top of the document) by 3 lines.
         * The cursor follows only if it would go off the bottom of the
         * visible area; we deliberately do NOT call ed_clamp_and_scroll
         * because that would snap top_row back to the cursor position. */
        int n = 3;
        if (ed->top_row > 0) {
            ed->top_row -= n;
            if (ed->top_row < 0) ed->top_row = 0;
            /* Keep cursor inside the now-visible range. */
            if (ed->cur_row >= ed->top_row + TEXT_ROWS)
                ed->cur_row = ed->top_row + TEXT_ROWS - 1;
            if (ed->cur_row < 0) ed->cur_row = 0;
            /* Re-clamp left_col for the (possibly adjusted) cursor line. */
            {
                int vcol = ed_visual_col(ed, ed->cur_row, ed->cur_col);
                int tc   = ed_text_cols(ed);
                if (vcol < ed->left_col) ed->left_col = vcol;
                if (vcol >= ed->left_col + tc) ed->left_col = vcol - tc + 1;
                if (ed->left_col < 0) ed->left_col = 0;
            }
        }
        break;
    }

    case KEY_SCROLL_DOWN: {
        int n = 3;
        int max_top = ed->tb.count - 1;
        if (max_top < 0) max_top = 0;
        if (ed->top_row < max_top) {
            ed->top_row += n;
            if (ed->top_row > max_top) ed->top_row = max_top;
            /* Keep cursor inside the now-visible range. */
            if (ed->cur_row < ed->top_row)
                ed->cur_row = ed->top_row;
            if (ed->cur_row >= ed->tb.count)
                ed->cur_row = ed->tb.count > 0 ? ed->tb.count - 1 : 0;
            {
                int vcol = ed_visual_col(ed, ed->cur_row, ed->cur_col);
                int tc   = ed_text_cols(ed);
                if (vcol < ed->left_col) ed->left_col = vcol;
                if (vcol >= ed->left_col + tc) ed->left_col = vcol - tc + 1;
                if (ed->left_col < 0) ed->left_col = 0;
            }
        }
        break;
    }

    /* ---- mouse click / drag ------------------------------------------- */
    case KEY_MOUSE_PRESS: {
        int brow, bcol;
        if (!screen_to_buf_pos(ed, vio_mouse.col, vio_mouse.row, &brow, &bcol))
            break;
        undo_reset_coalesce(ed);
        if (vio_mouse.mods & VIO_MOD_SHIFT) {
            /* Shift+click: extend (or start) selection from current pos. */
            if (!ed->sel_active) {
                ed->sel_active = 1;
                ed->sel_row    = ed->cur_row;
                ed->sel_col    = ed->cur_col;
            }
        } else {
            ed->sel_active = 0;
        }
        ed->cur_row        = brow;
        ed->cur_col        = bcol;
        ed->mouse_btn_down = 1;
        ed_clamp_and_scroll(ed);
        break;
    }

    case KEY_MOUSE_MOVE: {
        /* Drag: extend selection from where button was pressed. */
        if (!ed->mouse_btn_down) break;
        int brow, bcol;
        if (!screen_to_buf_pos(ed, vio_mouse.col, vio_mouse.row, &brow, &bcol))
            break;
        if (!ed->sel_active) {
            /* Anchor at the cursor position captured on KEY_MOUSE_PRESS. */
            ed->sel_active = 1;
            ed->sel_row    = ed->cur_row;
            ed->sel_col    = ed->cur_col;
        }
        ed->cur_row = brow;
        ed->cur_col = bcol;
        ed_clamp_and_scroll(ed);
        break;
    }

    case KEY_MOUSE_RELEASE:
        /* Button released: stop drag, keep any selection that was built. */
        ed->mouse_btn_down = 0;
        /* Collapse zero-width selection (single click with no drag). */
        if (ed->sel_active &&
            ed->cur_row == ed->sel_row && ed->cur_col == ed->sel_col)
            ed->sel_active = 0;
        break;

    default:
        /* Printable CP437: cancel selection, then insert character. */
        ed->sel_active  = 0;
        ed->message[0]  = '\0';   /* typing clears the last status message */
        /* Typing cancels pending quit */
        ed->pending_quit = 0;
        if ((key >= 0x20 && key < 0x7F) ||
            (key >= 0x80 && key <= 0xFF)) {
            int    r  = ed->cur_row, c = ed->cur_col;
            Line  *ll = &ed->tb.lines[r];
            UndoOp op;
            memset(&op, 0, sizeof(op));

            if (!ed->insert_mode && c < ll->len) {
                /* Overwrite mode: capture old char for undo. */
                char old_ch = ll->data[c];
                ed_insert_char(ed, (char)key);
                op.type    = OP_OVERWRITE;
                op.row     = r;   op.col = c;
                op.len     = 1;
                op.data    = (char *)malloc(1);
                if (op.data) op.data[0] = old_ch;
                op.cur_row = r;   op.cur_col = c;   /* cursor before overwrite */
            } else {
                /* Insert mode (or at end of line in overwrite mode). */
                ed_insert_char(ed, (char)key);
                op.type    = OP_DEL_CHARS;
                op.row     = r;   op.col = c;  op.len = 1;
                op.cur_row = r;   op.cur_col = c;
            }
            undo_record(ed, &op);
        }
        break;
    }
}
