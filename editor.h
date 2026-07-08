/* SPDX-License-Identifier: MIT
 * editor.h  --  editor state and core editing/navigation operations
 */

#ifndef EDITOR_H
#define EDITOR_H

#include <stdio.h>   /* FILE* used by spell checker fields */
#include "vgaterm.h"
#include "vio.h"
#include "buffer.h"
#include "hl.h"

/* -----------------------------------------------------------------------
 * Screen layout
 *
 * The full 80x25 screen is used.  The bottom row (24) is shared by the
 * status / HUD line and the command console -- whichever is active is
 * drawn there, overwriting the other.  The remaining 24 rows are the
 * text area.  When line numbers are enabled, a gutter on the left of
 * the text area shows them.
 * ----------------------------------------------------------------------- */

#define SCR_COLS    VGA_COLS              /* 80 */
#define SCR_ROWS    VGA_ROWS              /* 25 */
#define STATUS_ROW  (SCR_ROWS - 1)        /* row 24 */
#define TEXT_ROWS   (SCR_ROWS - 1)        /* rows 0..23 */

#define DEFAULT_TABSIZE 3
#define MAX_TABSIZE     16
#define MAX_FILENAME    240
#define MAX_MESSAGE     80
#define CONSOLE_BUFSZ   240

/* Line-number gutter: always 6 columns when enabled.
 * 5 digits (max 99999) + 1 separator space.  Fixed width means
 * the text area never shifts as the document grows. */
#define GUTTER_WIDTH    6

/* Colour scheme */
#define A_TEXT      VGA_ATTR(VGA_BLACK, VGA_LGRAY)
#define A_LINENO    VGA_ATTR(VGA_BLACK, VGA_DGRAY)
#define A_STATUS    VGA_ATTR(VGA_CYAN,  VGA_BLACK)
#define A_CONSOLE   VGA_ATTR(VGA_LGRAY, VGA_BLACK)
#define A_SEL       VGA_ATTR(VGA_LGRAY, VGA_BLACK)  /* selection: inverted A_TEXT */

/* -----------------------------------------------------------------------
 * Editor state
 * ----------------------------------------------------------------------- */

typedef struct {
    TextBuffer tb;

    int cur_row;        /* cursor line, 0..tb.count-1 */
    int cur_col;        /* cursor column (character index), 0..line.len */

    int top_row;        /* first visible buffer line */
    int left_col;       /* horizontal scroll offset (text columns) */

    int tabsize;        /* 1..MAX_TABSIZE, default DEFAULT_TABSIZE */
    int insert_mode;    /* 1 = insert (default), 0 = overwrite */
    int show_linenum;   /* 0/1, line-number gutter */
    LineEnding  ending;  /* line ending used on save            */
    FileEncoding enc;    /* character encoding used on save      */

    int  modified;
    char filename[MAX_FILENAME];   /* empty string = no file yet */

    char message[MAX_MESSAGE];     /* transient status message */

    /* command console (activated by Esc) */
    int  console_active;
    char console_buf[CONSOLE_BUFSZ];
    int  console_len;

    int quit;           /* set to 1 to exit the main loop */
    int pending_quit;   /* set when a quit-confirmation is pending */

    /* selection (Shift+Arrow) */
    int  sel_active;    /* 1 = selection live          */
    int  sel_row;       /* anchor row                  */
    int  sel_col;       /* anchor col                  */

    /* command palette (opened by the "help" console command) */
    int  palette_active;
    int  palette_sel;            /* selected entry in filtered list  */
    int  palette_scroll;         /* index of first visible entry     */
    char palette_filter[64];     /* current filter string            */
    int  palette_filter_len;

    /* ---- Character map (charmap.c) ----------------------------------- */
    int  charmap_active;         /* 1 = dialog visible                */
    int  charmap_cur;            /* selected CP437 code point 0..255  */

    /* internal clipboard (mirrors what was last set on the X11 CLIPBOARD) */
    char *clipboard;             /* malloc'd; NULL if empty          */
    int   clipboard_len;         /* byte length of clipboard data    */

    /* tab insertion mode: 0 = soft (spaces), 1 = hard (\t characters) */
    int   use_hard_tabs;

    /* undo/redo subsystem (opaque; managed by undo.c) */
    void *undo_state;
    /* Coalescing timeout in milliseconds for undo (0 disables). */
    int  coalesce_ms;
    /* Last-find state for simple literal search/replace */
    char last_find[CONSOLE_BUFSZ];
    int  last_find_len;
    int  last_find_row;
    int  last_find_col;

    /* Mouse state */
    int  mouse_btn_down;   /* 1 while left button is held (drag selection) */

    /* Syntax highlighting */
    HLang        hl_langs[HL_MAX_LANGS];
    int          n_hl_langs;
    const HLang *hl;
    int          hl_dirty_row;

    /* ---- Spell checker (spell.c) ------------------------------------- */
    int   spell_active;         /* 1 = dialog visible                    */
    int   spell_mode;           /* SPELL_ALL / SPELL_TEXT_ONLY           */
    /* current misspelling */
    int   spell_row;            /* buffer row                            */
    int   spell_col;            /* start column                          */
    int   spell_len;            /* word byte-length                      */
    char  spell_word[64];       /* the misspelled word (NUL-terminated)  */
    char  spell_sug[30][64];    /* up to 30 ispell suggestions           */
    int   spell_nsug;           /* number of valid suggestions           */
    int   spell_sug_top;        /* first suggestion index shown (paging) */
    int   spell_sug_sel;        /* index of clicked/selected suggestion (-1=none) */
    /* forward scan state */
    int   spell_scan_row;
    int   spell_scan_col;
    /* ispell subprocess */
    int   spell_pid;            /* child PID; -1 = none                  */
    int   spell_wfd;            /* write end of pipe to ispell stdin     */
    FILE *spell_rfp;            /* FILE* reading ispell stdout           */
    /* replace-mode (user types a custom replacement) */
    int   spell_replace_mode;
    char  spell_replace_buf[64];
    int   spell_replace_len;
} Editor;

/* ---- lifecycle ------------------------------------------------------ */

void ed_init(Editor *ed);
void ed_free(Editor *ed);

/* Load a language definition from an explicit path and upsert it into
 * ed->hl_langs by name.  Returns the slot index (>= 0) on success, -1
 * on failure (file missing / malformed / table full). */
int  ed_load_lang_file(Editor *ed, const char *path);

/* Search the standard location cascade for a bare filename such as
 * "ansi-c.csv" and load the first one found.  Cascade order:
 *   1. TEDIT_DATADIR (system, set at build time)
 *   2. ~/.tedit/lang (user overrides)
 *   3. ./lang        (in-tree / development)
 * Returns the slot index (>= 0) on success, -1 if not found anywhere. */
int  ed_load_lang_search(Editor *ed, const char *filename);

/* Load `filename` (replacing the buffer).  On failure, leaves the
 * buffer untouched and sets a status message.  Returns 0/-1. */
int  ed_open(Editor *ed, const char *filename);

/* Save to `filename`, or ed->filename if filename is NULL/empty.
 * Returns 0/-1; sets a status message either way. */
int  ed_save(Editor *ed, const char *filename);

/* ---- layout helpers --------------------------------------------------- */

/* Width of the line-number gutter, 0 if show_linenum is off. */
int ed_gutter_width(const Editor *ed);

/* Width of the text area in columns (SCR_COLS - gutter). */
int ed_text_cols(const Editor *ed);

/* Length (in characters) of line `row`, or 0 if out of range. */
int ed_line_len(const Editor *ed, int row);

/* Visual (screen) column corresponding to character index char_col on
 * buf row `row`, expanding tab characters to tab stops.  This is the
 * value that should be used for horizontal-scroll and cursor-placement
 * arithmetic.  Cheap: O(line length). */
int ed_visual_col(const Editor *ed, int row, int char_col);

/* Clamp cur_col/cur_row to valid positions and adjust top_row/left_col
 * so the cursor is visible.  Call after any edit or move. */
void ed_clamp_and_scroll(Editor *ed);

/* Set the transient status-bar message (printf-style). */
void ed_set_message(Editor *ed, const char *fmt, ...);

/* ---- editing ----------------------------------------------------------- */

void ed_insert_char(Editor *ed, char ch);
void ed_insert_tab(Editor *ed);
void ed_enter(Editor *ed);
void ed_backspace(Editor *ed);
void ed_delete(Editor *ed);
void ed_delete_selection(Editor *ed);  /* delete the selected region */
void ed_copy(Editor *ed);              /* copy selection to clipboard */
void ed_cut(Editor *ed);               /* cut  selection to clipboard */

/* Paste arbitrary text at the cursor (splits on \n, ignores \r).
 * Does NOT record undo — callers that need undo should wrap this. */
void ed_paste_blob(Editor *ed, const char *text, int len);

/* Delete the rectangle (sr,sc)-(er,ec) from the buffer.
 * Caller is responsible for cursor update and undo recording. */
void ed_delete_range(Editor *ed, int sr, int sc, int er, int ec);

/* ---- navigation ---------------------------------------------------------*/

void ed_move_up(Editor *ed);
void ed_move_down(Editor *ed);
void ed_move_left(Editor *ed);
void ed_move_right(Editor *ed);
void ed_move_line_start(Editor *ed);   /* Home -> start of line */
void ed_move_line_end(Editor *ed);     /* End  -> end of line */
void ed_move_doc_start(Editor *ed);   /* Home -> top of document   */
void ed_move_doc_end(Editor *ed);     /* End  -> bottom of document */
void ed_page_up(Editor *ed);
void ed_page_down(Editor *ed);

/* Simple find/replace (single-line, literal): */
int ed_find(Editor *ed, const char *pattern);
int ed_find_next(Editor *ed);
int ed_replace_current(Editor *ed, const char *newtext);
int ed_replace_all(Editor *ed, const char *oldpat, const char *newpat);

/* ---- dispatch ------------------------------------------------------------
 *
 * Handle one key while the editor (not the console) has focus.
 * Returns nothing; updates ed in place.  KEY_ESC opens the console;
 * KEY_CTRL('q') sets ed->quit.
 */
void ed_handle_key(Editor *ed, int key);

#endif /* EDITOR_H */
