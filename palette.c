/* SPDX-License-Identifier: MIT
 * palette.c  --  command palette
 */

#include "palette.h"
#include "console.h"
#include "vio.h"

#include <ctype.h>
#include <string.h>

/* -----------------------------------------------------------------------
 * Command table
 * ----------------------------------------------------------------------- */

typedef struct { const char *display; const char *fill; const char *hint; } Cmd;

static const Cmd CMDS[] = {
    { "new",                "new",       "New empty buffer"           },
    { "new!",               "new!",      "New, discard changes"       },
    { "open <filename>",    "open ",     "Load a file"                },
    { "save",               "save",      "Save in place"              },
    { "save as <filename>", "save as ",  "Save with a new name"       },
    { "quit",               "quit",      "Quit  (warns if unsaved)"   },
    { "quit!",              "quit!",     "Force quit, discard"        },
    { "wq",                 "wq",        "Save and quit"              },
    { "tab <n>",            "tab ",      "Set tab size  (1-16)"       },
    { "coalesce <ms>",     "coalesce ","Set undo coalesce ms (0 disables)"},
    { "tabs",               "tabs",      "Use hard tabs (\\t character)"},
    { "spaces",             "spaces",    "Use soft tabs (spaces)"     },
    { "numbers",            "numbers",   "Toggle line numbers"        },
    { "unix",               "unix",      "LF line endings on save"    },
    { "dos",                "dos",       "CRLF line endings on save"  },
    { "enc utf8",           "enc utf8",  "Save encoding: Unicode UTF-8"      },
    { "enc cp437",          "enc cp437", "Save encoding: raw CP437 bytes"    },
    { "enc ascii",          "enc ascii", "Save encoding: 7-bit ASCII (lossy)"},
    { "syntax <lang>",      "syntax ",   "Set syntax highlighting"           },
    { "syntax off",         "syntax off","Syntax highlighting off"           },
    { "spell",              "spell",     "Spell check all text"              },
    { "spell code",         "spell code","Spell check strings + comments"    },
    { "charmap",            "charmap",   "Browse and insert CP437 characters"},
    { "find <text>",        "find ",     "Search forward  (Ctrl+F)"         },
    { "find-next",          "next",      "Repeat last search  (F3)"         },
    { "replace <old> <new>","replace ",  "Replace next match"               },
    { "rall <old> <new>",   "replace-all ","Replace all occurrences"        },
    { "scale 1",            "scale 1",   "Display scale: 1x (640x400)"       },
    { "scale 2",            "scale 2",   "Display scale: 2x (1280x800)"      },
    { "scale 4",            "scale 4",   "Display scale: 4x (2560x1600)"     },
};
#define NCMDS (int)(sizeof(CMDS) / sizeof(CMDS[0]))

/* -----------------------------------------------------------------------
 * Filter  (prefix match, case-insensitive)
 * ----------------------------------------------------------------------- */

static int filt_idx[NCMDS];   /* indices into CMDS[] of matching entries */
static int filt_cnt;

static void build_filter(const char *filter, int flen)
{
    int i, j;
    filt_cnt = 0;
    for (i = 0; i < NCMDS; i++) {
        if (flen == 0) {
            filt_idx[filt_cnt++] = i;
            continue;
        }
        /* prefix match */
        for (j = 0; j < flen; j++) {
            if (tolower((unsigned char)filter[j]) !=
                tolower((unsigned char)CMDS[i].display[j]))
                break;
        }
        if (j == flen)
            filt_idx[filt_cnt++] = i;
    }
}

/* -----------------------------------------------------------------------
 * Public API
 * ----------------------------------------------------------------------- */

/* Commit a palette selection.
 * Commands whose fill string ends with a trailing space require further input:
 * copy to the console bar and leave it open so the user can complete the arg.
 * Commands with a complete fill string (no trailing space) run immediately. */
static void palette_commit(Editor *ed, const char *fill)
{
    int n = (int)strlen(fill);
    int needs_input = (n > 0 && fill[n - 1] == ' ');

    if (needs_input) {
        /* Copy into console bar, leave console open for user to type arg. */
        if (n > CONSOLE_BUFSZ - 1) n = CONSOLE_BUFSZ - 1;
        memcpy(ed->console_buf, fill, (size_t)n);
        ed->console_buf[n] = '\0';
        ed->console_len    = n;
        ed->console_active = 1;
    } else {
        /* Complete command -- execute immediately, close everything. */
        ed->console_active = 0;
        ed->console_buf[0] = '\0';
        ed->console_len    = 0;
        console_execute(ed, fill);
    }
}

void palette_open(Editor *ed)
{
    ed->palette_active     = 1;
    ed->palette_sel        = 0;
    ed->palette_scroll     = 0;
    ed->palette_filter[0]  = '\0';
    ed->palette_filter_len = 0;
    /* keep console active so the status row becomes the filter prompt */
    ed->console_active = 1;
    ed->console_buf[0] = '\0';
    ed->console_len    = 0;
}

void palette_handle_key(Editor *ed, int key)
{
    int fc;

    build_filter(ed->palette_filter, ed->palette_filter_len);
    fc = filt_cnt;

    switch (key) {

    case KEY_ESC:
        ed->palette_active  = 0;
        ed->console_active  = 0;
        ed->console_buf[0]  = '\0';
        ed->console_len     = 0;
        ed->message[0]      = '\0';
        break;

    case KEY_ENTER:
        if (fc > 0 && ed->palette_sel < fc) {
            const char *fill = CMDS[filt_idx[ed->palette_sel]].fill;
            ed->palette_active = 0;
            palette_commit(ed, fill);
        } else {
            ed->palette_active = 0;
        }
        break;

    case KEY_UP:
    case KEY_SCROLL_UP:
        if (ed->palette_sel > 0) {
            ed->palette_sel--;
            if (ed->palette_sel < ed->palette_scroll)
                ed->palette_scroll = ed->palette_sel;
        }
        break;

    case KEY_DOWN:
    case KEY_SCROLL_DOWN:
        if (ed->palette_sel < fc - 1) {
            ed->palette_sel++;
            if (ed->palette_sel >= ed->palette_scroll + PAL_VISIBLE)
                ed->palette_scroll = ed->palette_sel - PAL_VISIBLE + 1;
        }
        break;

    case KEY_MOUSE_PRESS:
        {
            int mcol = vio_mouse.col;
            int mrow = vio_mouse.row;

            if (mrow == STATUS_ROW) {
                /* click on the console bar: confirm current selection */
                ed->palette_active = 0;
                if (fc > 0 && ed->palette_sel < fc) {
                    const char *fill = CMDS[filt_idx[ed->palette_sel]].fill;
                    palette_commit(ed, fill);
                }
            } else if (mcol < PAL_COL || mcol >= PAL_COL + PAL_W ||
                       mrow < PAL_ROW || mrow >= PAL_ROW + PAL_H) {
                /* click outside box: cancel */
                ed->palette_active = 0;
                ed->console_active = 0;
                ed->console_buf[0] = '\0';
                ed->console_len    = 0;
                ed->message[0]     = '\0';
            } else if (mrow > PAL_ROW && mrow < PAL_ROW + PAL_H - 1) {
                /* click on an entry row: move highlight there and select it */
                int row_idx = ed->palette_scroll + (mrow - PAL_ROW - 1);
                if (row_idx < fc) {
                    ed->palette_sel = row_idx;
                    if (ed->palette_sel < ed->palette_scroll)
                        ed->palette_scroll = ed->palette_sel;
                    else if (ed->palette_sel >= ed->palette_scroll + PAL_VISIBLE)
                        ed->palette_scroll = ed->palette_sel - PAL_VISIBLE + 1;
                    const char *fill = CMDS[filt_idx[row_idx]].fill;
                    ed->palette_active = 0;
                    palette_commit(ed, fill);
                }
            }
        }
        break;

    case KEY_BS:
        if (ed->palette_filter_len > 0) {
            ed->palette_filter[--ed->palette_filter_len] = '\0';
            ed->palette_sel    = 0;
            ed->palette_scroll = 0;
        }
        break;

    default:
        if (key >= 0x20 && key < 0x7F &&
            ed->palette_filter_len < (int)sizeof(ed->palette_filter) - 1) {
            ed->palette_filter[ed->palette_filter_len++] = (char)key;
            ed->palette_filter[ed->palette_filter_len]   = '\0';
            ed->palette_sel    = 0;
            ed->palette_scroll = 0;
        }
        break;
    }
}

/* -----------------------------------------------------------------------
 * Rendering
 * ----------------------------------------------------------------------- */

/* Write `src` left-aligned into a field of `w` columns, padded with spaces.
 * Truncates if src is longer than w. */
static void put_field(const char *src, int w)
{
    int n = (int)strlen(src);
    if (n > w) n = w;
    vio_puts_n(src, n);
    /* pad remainder */
    if (n < w) vio_puts_n("", w - n);
}

void palette_render(const Editor *ed)
{
    const char *title  = " Command Palette ";
    const char *footer = " \x1b Enter select  Esc cancel ";
    /* CP437 0x1B = left arrow, used as a visual bullet */
    int   tlen = (int)strlen(title);
    int   flen_f = (int)strlen(footer);
    int   i, ci;
    uint8_t ea, ha;

    build_filter(ed->palette_filter, ed->palette_filter_len);

    /* --- box + flood-fill interior --- */
    vio_dbox(PAL_COL, PAL_ROW, PAL_W, PAL_H, A_PAL_BDR);
    vio_fill(PAL_COL + 1, PAL_ROW + 1,
             PAL_INNER, PAL_VISIBLE, ' ', A_PAL_ENTRY);

    /* --- title on top border --- */
    vio_gotoxy(PAL_COL + (PAL_W - tlen) / 2, PAL_ROW);
    vio_setattr(A_PAL_BDR);
    vio_puts(title);

    /* --- footer on bottom border --- */
    if (flen_f <= PAL_INNER) {
        vio_gotoxy(PAL_COL + (PAL_W - flen_f) / 2, PAL_ROW + PAL_H - 1);
        vio_setattr(A_PAL_BDR);
        vio_puts(footer);
    }

    /* --- scroll arrows on right border (replace corner chars if needed) --- */
    if (ed->palette_scroll > 0) {
        vio_setattr(A_PAL_BDR);
        vio_gotoxy(PAL_COL + PAL_W - 1, PAL_ROW + 1);
        vio_putch('\x1e');   /* CP437 ▲ */
    }
    if (ed->palette_scroll + PAL_VISIBLE < filt_cnt) {
        vio_setattr(A_PAL_BDR);
        vio_gotoxy(PAL_COL + PAL_W - 1, PAL_ROW + PAL_H - 2);
        vio_putch('\x1f');   /* CP437 ▼ */
    }

    /* --- entries --- */
    if (filt_cnt == 0) {
        /* no matches */
        vio_setattr(A_PAL_EHINT);
        vio_gotoxy(PAL_COL + 1 + (PAL_INNER - 8) / 2,
                   PAL_ROW + 1 + PAL_VISIBLE / 2 - 1);
        vio_puts(" no match ");
    } else {
        for (i = 0; i < PAL_VISIBLE; i++) {
            int row_idx = ed->palette_scroll + i;
            int scr_row = PAL_ROW + 1 + i;

            if (row_idx >= filt_cnt) break;
            ci = filt_idx[row_idx];

            if (row_idx == ed->palette_sel) {
                ea = A_PAL_SEL;
                ha = A_PAL_SHINT;
            } else {
                ea = A_PAL_ENTRY;
                ha = A_PAL_EHINT;
            }

            /* left pad */
            vio_gotoxy(PAL_COL + 1, scr_row);
            vio_setattr(ea);
            vio_putch(' ');

            /* command */
            vio_setattr(ea);
            put_field(CMDS[ci].display, PAL_CMD_W);

            /* gap */
            vio_setattr(ha);
            vio_putch(' ');
            vio_putch(' ');

            /* hint */
            vio_setattr(ha);
            put_field(CMDS[ci].hint, PAL_HINT_W);

            /* right pad (one cell before border) */
            vio_setattr(ea);
            vio_putch(' ');
        }
    }
}
