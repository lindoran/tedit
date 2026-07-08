/* SPDX-License-Identifier: MIT
 * render.c  --  screen drawing
 */

#include "render.h"
#include "enc.h"
#include "palette.h"
#include "charmap.h"
#include "spell.h"
#include "vio.h"
#include "hl.h"

#include <stdio.h>
#include <string.h>

/* -----------------------------------------------------------------------
 * Text rows
 * ----------------------------------------------------------------------- */

static void draw_text_row(Editor *ed, int screen_row, int buf_row)
{
    int gw        = ed_gutter_width(ed);
    int text_cols = ed_text_cols(ed);
    Line *l       = &ed->tb.lines[buf_row];
    int avail     = text_cols;
    int vcol      = 0;
    int i;
    HLCursor hlc;

    if (ed->hl) {
        hl_begin(&hlc, ed->hl, ed->tb.hl_states[buf_row]);
    }

    /* ---- selection bounds for this row --------------------------------
     * sel_from : first selected buffer column  (-1 = no selection here)
     * sel_to   : first non-selected column     (-1 = selected to EOL)
     *
     * The cursor cell is NEVER highlighted: the hardware cursor inverts
     * whatever is beneath it, so leaving it as A_TEXT means the cursor
     * always looks identical regardless of selection state.
     * ------------------------------------------------------------------ */
    int sel_from = -1, sel_to = -1;

    if (ed->sel_active) {
        int sr, sc, er, ec;

        /* normalise anchor vs cursor */
        if (ed->cur_row < ed->sel_row ||
            (ed->cur_row == ed->sel_row && ed->cur_col <= ed->sel_col)) {
            sr = ed->cur_row;  sc = ed->cur_col;
            er = ed->sel_row;  ec = ed->sel_col;
        } else {
            sr = ed->sel_row;  sc = ed->sel_col;
            er = ed->cur_row;  ec = ed->cur_col;
        }

        if (buf_row >= sr && buf_row <= er) {
            sel_from = (buf_row == sr) ? sc : 0;
            sel_to   = (buf_row == er) ? ec : -1;
        }
    }

    /* ---- gutter ------------------------------------------------------- */
    if (gw > 0) {
        vio_gotoxy(0, screen_row);
        vio_setattr(A_LINENO);
        vio_int(buf_row + 1, GUTTER_WIDTH - 1);
        vio_putch(' ');
    }

    vio_gotoxy(gw, screen_row);

    /* ---- « left-scroll indicator -------------------------------------- */
    if (ed->left_col > 0) {
        vio_setattr(A_LINENO);
        vio_putch(0xAE); /* « */
        avail--;
    }

    /* ---- text: walk characters tracking visual column -----------------
     *
     * left_col is a visual-column offset, so we must iterate chars and
     * expand tabs to know which cells are visible.  For a tab character:
     *   - first visible cell renders as » (\xbb), rest as spaces
     *   - if the tab straddles the left-scroll boundary the » is already
     *     scrolled off; only the trailing spaces are visible
     * Selection attributes are applied per character-index (same as
     * before), so the full visual extent of a tab shares one attribute.
     * ------------------------------------------------------------------ */
    for (i = 0; i < l->len && avail > 0; i++) {
        unsigned char ch = (unsigned char)l->data[i];
        uint8_t base_attr = A_TEXT;

        if (ed->hl) {
            base_attr = hl_step(&hlc, l->data, l->len, i);
        }

        int tab_end = 0, ch_width;

        if (ch == '\t') {
            tab_end  = ((vcol / ed->tabsize) + 1) * ed->tabsize;
            ch_width = tab_end - vcol;
        } else {
            ch_width = 1;
        }

        /* Entirely before the scroll window — advance vcol, no draw. */
        if (vcol + ch_width <= ed->left_col) {
            vcol += ch_width;
            continue;
        }

        /* Selection / cursor attribute for this character (char-index). */
        {
            int in_sel = (sel_from >= 0) &&
                         (i >= sel_from) &&
                         (sel_to < 0 || i < sel_to);
            int is_cur = (buf_row == ed->cur_row && i == ed->cur_col);
            uint8_t attr = (in_sel && !is_cur) ? A_SEL : base_attr;

            if (ch == '\t') {
                /* Render each visible cell of the tab stop. */
                int v;
                for (v = vcol; v < tab_end && avail > 0; v++) {
                    if (v < ed->left_col) continue; /* partially scrolled */
                    vio_setattr(attr);
                    /* » in first cell of tab, spaces for the padding */
                    vio_putch((v == vcol) ? (uint8_t)0xAF : (uint8_t)' ');
                    avail--;
                }
            } else {
                vio_setattr(attr);
                vio_putch(ch);
                avail--;
            }
        }

        vcol += ch_width;
    }

    /* ---- pad the rest of the text area with spaces -------------------- */
    vio_setattr(A_TEXT);
    while (avail-- > 0)
        vio_putch(' ');
}

/* -----------------------------------------------------------------------
 * Status / HUD and console row
 * ----------------------------------------------------------------------- */

static void draw_console(Editor *ed)
{
    /* vio_clrline already called by render_full */
    vio_gotoxy(0, STATUS_ROW);
    vio_setattr(A_CONSOLE);

    if (ed->palette_active) {
        /* "? " prompt followed by the live filter string */
        vio_putch('?');
        vio_putch(' ');
        vio_puts(ed->palette_filter);
    } else {
        vio_putch(':');
        vio_puts(ed->console_buf);
    }
}

static void draw_status(Editor *ed)
{
    char left[MAX_FILENAME + MAX_MESSAGE + 8];
    char right[72];
    int  rlen;

    snprintf(left, sizeof(left), " %s%s%s%s",
             ed->filename[0] ? ed->filename : "[No Name]",
             ed->modified  ? " [+]" : "",
             ed->message[0] ? "  "  : "",
             ed->message);

    snprintf(right, sizeof(right), "%d/%d:%d %s %s %s T%d%s%s ",
             ed->cur_row + 1, ed->tb.count,
             ed_visual_col(ed, ed->cur_row, ed->cur_col) + 1,
             ed->insert_mode ? "INS" : "OVR",
             ed->ending == LE_DOS ? "CRLF" : "LF",
             ed->enc == ENC_UTF8  ? "UTF8"  :
             ed->enc == ENC_ASCII ? "ASCII" : "CP437",
             ed->tabsize,
             ed->use_hard_tabs ? " TAB" : " SPC",
             ed->show_linenum ? " #" : "");

    rlen = (int)strlen(right);
    if (rlen > SCR_COLS) rlen = SCR_COLS;

    vio_gotoxy(0, STATUS_ROW);
    vio_setattr(A_STATUS);
    vio_puts(left);
    vio_gotoxy(SCR_COLS - rlen, STATUS_ROW);
    vio_puts(right);
}

/* -----------------------------------------------------------------------
 * Cursor
 * ----------------------------------------------------------------------- */

static void place_cursor(const Editor *ed)
{
    int scr_col, scr_row;

    /* Spell replace-prompt overrides all other cursor positions. */
    if (ed->spell_active && ed->spell_replace_mode) {
        int sx, sy;
        if (spell_cursor_pos(ed, &sx, &sy)) {
            vio_gotoxy(sx, sy);
            vio_show_cursor();
            return;
        }
    }

    /* Character-map: place cursor on the selected glyph in the grid. */
    if (ed->charmap_active) {
        int gr = ed->charmap_cur / CMP_COLS;
        int gc = ed->charmap_cur % CMP_COLS;
        vio_gotoxy(CMP_COL + 1 + gc, CMP_ROW + 1 + gr);
        vio_show_cursor();
        return;
    }

    if (ed->console_active) {
        if (ed->palette_active) {
            /* "? " is a 2-char prefix; cursor follows the filter text */
            scr_col = 2 + ed->palette_filter_len;
        } else {
            /* ":" is a 1-char prefix */
            scr_col = 1 + ed->console_len;
        }
        if (scr_col >= SCR_COLS) scr_col = SCR_COLS - 1;
        scr_row = STATUS_ROW;
    } else {
        /* +1 when the « indicator is occupying the first text column */
        int vcol   = ed_visual_col(ed, ed->cur_row, ed->cur_col);
        int ind    = (ed->left_col > 0) ? 1 : 0;
        scr_col = ed_gutter_width(ed) + ind + (vcol - ed->left_col);
        scr_row = ed->cur_row - ed->top_row;
    }

    vio_gotoxy(scr_col, scr_row);
    vio_show_cursor();
}

/* -----------------------------------------------------------------------
 * Full redraw
 * ----------------------------------------------------------------------- */

void render_full(Editor *ed)
{
    if (ed->hl && ed->hl_dirty_row >= 0) {
        hl_update_states(ed->hl, &ed->tb, ed->hl_dirty_row);
        ed->hl_dirty_row = -1;
    }

    int gw        = ed_gutter_width(ed);
    int text_cols = ed_text_cols(ed);
    int visible   = ed->tb.count - ed->top_row;
    int r;

    if (visible < 0)         visible = 0;
    if (visible > TEXT_ROWS) visible = TEXT_ROWS;

    /* content rows */
    for (r = 0; r < visible; r++)
        draw_text_row(ed, r, ed->top_row + r);

    /* empty rows below the buffer */
    if (visible < TEXT_ROWS) {
        int empty = TEXT_ROWS - visible;
        if (gw > 0)
            vio_fill(0,  visible, gw,        empty, ' ', A_LINENO);
        vio_fill(gw, visible, text_cols, empty, ' ', A_TEXT);
    }

    /* status / console row */
    vio_clrline(STATUS_ROW,
                ed->console_active ? A_CONSOLE : A_STATUS);

    if (ed->console_active)
        draw_console(ed);
    else
        draw_status(ed);

    /* palette overlay (drawn on top of text rows if active) */
    if (ed->palette_active)
        palette_render(ed);

    /* spell-check dialog overlay */
    if (ed->spell_active)
        spell_render(ed);

    /* character-map dialog overlay */
    if (ed->charmap_active)
        charmap_render(ed);

    place_cursor(ed);
    vio_flush();
}
