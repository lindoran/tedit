/* SPDX-License-Identifier: MIT
 * charmap.c  --  CP437 character-map palette
 *
 * Displays all 256 CP437 glyphs in a 32×8 grid dialog.  The user can
 * navigate with arrow keys (or click with the mouse) and press Enter to
 * insert the selected character at the current cursor position.
 */

#include "charmap.h"
#include "editor.h"
#include "vio.h"

#include <stdio.h>
#include <string.h>

/* -----------------------------------------------------------------------
 * Public API
 * ----------------------------------------------------------------------- */

void charmap_open(Editor *ed)
{
    ed->charmap_active = 1;
    /* charmap_cur persists between opens so the user finds the dialog
     * in the same position as they left it. */
}

/* -----------------------------------------------------------------------
 * Key handling
 * ----------------------------------------------------------------------- */

void charmap_handle_key(Editor *ed, int key)
{
    int c = (unsigned char)ed->charmap_cur;

    switch (key) {

    /* ------ dismiss without inserting ---------------------------------- */
    case KEY_ESC:
        ed->charmap_active = 0;
        break;

    /* ------ insert selected char and close ----------------------------- */
    case KEY_ENTER:
    case ' ':
        ed->charmap_active = 0;
        if (c >= 0x20)              /* 0x00-0x1F are control codes: not insertable */
            ed_insert_char(ed, (char)(unsigned char)c);
        break;

    /* ------ grid navigation -------------------------------------------- */
    case KEY_LEFT:
        ed->charmap_cur = (c == 0) ? 255 : c - 1;
        break;

    case KEY_RIGHT:
        ed->charmap_cur = (c == 255) ? 0 : c + 1;
        break;

    case KEY_UP:
        /* move one row up, wrapping from row 0 to row 7 */
        ed->charmap_cur = (c < CMP_COLS) ? (c + 256 - CMP_COLS) : (c - CMP_COLS);
        break;

    case KEY_DOWN:
        /* move one row down, wrapping from row 7 to row 0 */
        ed->charmap_cur = (c + CMP_COLS > 255) ? (c + CMP_COLS - 256) : (c + CMP_COLS);
        break;

    case KEY_HOME:
        /* jump to first column of the current row */
        ed->charmap_cur = c & ~(CMP_COLS - 1);
        break;

    case KEY_END:
        /* jump to last column of the current row */
        ed->charmap_cur = c | (CMP_COLS - 1);
        break;

    case KEY_CTRL_HOME:
        ed->charmap_cur = 0;
        break;

    case KEY_CTRL_END:
        ed->charmap_cur = 255;
        break;

    case KEY_PGUP:
        c -= CMP_COLS * 4;   /* 4 rows up */
        ed->charmap_cur = (c < 0) ? 0 : c;
        break;

    case KEY_PGDN:
        c += CMP_COLS * 4;   /* 4 rows down */
        ed->charmap_cur = (c > 255) ? 255 : c;
        break;

    /* ------ mouse: click in grid inserts; click outside closes -------- */
    case KEY_MOUSE_PRESS:
        {
            int mcol = vio_mouse.col;
            int mrow = vio_mouse.row;

            if (mcol >= CMP_COL + 1 && mcol <= CMP_COL + CMP_INNER &&
                mrow >= CMP_ROW + 1 && mrow <= CMP_ROW + CMP_ROWS) {
                /* click inside the character grid: select and insert */
                int clicked = (mrow - CMP_ROW - 1) * CMP_COLS
                            + (mcol - CMP_COL - 1);
                ed->charmap_cur    = clicked;
                ed->charmap_active = 0;
                if (clicked >= 0x20)    /* 0x00-0x1F are control codes: not insertable */
                    ed_insert_char(ed, (char)(unsigned char)clicked);
            } else if (mcol < CMP_COL || mcol >= CMP_COL + CMP_W ||
                       mrow < CMP_ROW || mrow >= CMP_ROW + CMP_H) {
                /* outside the whole box: close without inserting */
                ed->charmap_active = 0;
            }
        }
        break;

    default:
        /* Typing any character jumps the selection to that code point. */
        if (key >= 0x20 && key <= 0xFF)
            ed->charmap_cur = key;
        break;
    }
}

/* -----------------------------------------------------------------------
 * Rendering
 * ----------------------------------------------------------------------- */

void charmap_render(const Editor *ed)
{
    static const char title[]  = " Character Map ";
    int tlen = (int)(sizeof(title) - 1);
    int i, code_c;

    if (!ed->charmap_active) return;

    code_c = (unsigned char)ed->charmap_cur;

    /* ---- outer box ---------------------------------------------------- */
    vio_dbox(CMP_COL, CMP_ROW, CMP_W, CMP_H, A_CMP_BDR);

    /* flood-fill the full interior (grid + divider row + hint row) */
    vio_fill(CMP_COL + 1, CMP_ROW + 1, CMP_INNER, CMP_H - 2, ' ', A_CMP_BODY);

    /* ---- title on the top border -------------------------------------- */
    vio_gotoxy(CMP_COL + (CMP_W - tlen) / 2, CMP_ROW);
    vio_setattr(A_CMP_BDR);
    vio_puts(title);

    /* ---- 8 × 32 character grid ---------------------------------------- */
    for (i = 0; i < 256; i++) {
        int gr   = i / CMP_COLS;          /* grid row 0..7  */
        int gc   = i % CMP_COLS;          /* grid col 0..31 */
        int sx   = CMP_COL + 1 + gc;
        int sy   = CMP_ROW + 1 + gr;
        uint8_t attr;
        if ((unsigned)i == (unsigned)code_c)
            attr = A_CMP_SEL;
        else if (i < 0x20)
            attr = A_CMP_DIM;           /* control range: dimmed, not insertable */
        else
            attr = A_CMP_BODY;
        vio_putch_at(sx, sy, (uint8_t)i, attr);
    }

    /* ---- divider row  ╠════════════╣ ---------------------------------- */
    vio_gotoxy(CMP_COL, CMP_ROW + CMP_ROWS + 1);
    vio_setattr(A_CMP_BDR);
    vio_putch(0xCC);                       /* ╠ */
    for (i = 0; i < CMP_INNER; i++) vio_putch(0xCD); /* ═ */
    vio_putch(0xB9);                       /* ╣ */

    /* ---- hint row  (9-char code field  +  23-char key legend) --------- *
     *
     * Total = CMP_INNER = 32 columns:
     *   printable:     " 0x41 'A'"  (9)  +  "  ↑↓←→:move  Enter:ins "  (23)
     *   non-printable: " 0x01    "  (9)  +  same 23-char suffix
     *
     * CP437 glyphs used in the legend:
     *   0x18 ↑   0x19 ↓   0x1B ←   0x1A →
     * -------------------------------------------------------------------- */
    {
        char codebuf[10];   /* exactly 9 printable chars + NUL */

        if (code_c >= 0x20 && code_c <= 0x7E)
            snprintf(codebuf, sizeof(codebuf), " 0x%02X '%c'", code_c, code_c);
        else if (code_c < 0x20)
            snprintf(codebuf, sizeof(codebuf), " 0x%02X ctl", code_c);
        else
            snprintf(codebuf, sizeof(codebuf), " 0x%02X    ", code_c);

        vio_gotoxy(CMP_COL + 1, CMP_ROW + CMP_ROWS + 2);

        vio_setattr(A_CMP_CODE);
        vio_puts(codebuf);                                  /* 9 chars */

        vio_setattr(A_CMP_HINT);
        if (code_c < 0x20)
            vio_puts("  \x18\x19\x1b\x1a:move  [no insert] ");  /* 23 chars */
        else
            vio_puts("  \x18\x19\x1b\x1a:move  Enter:ins ");    /* 23 chars */
    }
}
