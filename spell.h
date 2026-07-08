/* SPDX-License-Identifier: MIT
 * spell.h  --  ispell integration for tedit
 *
 * Activated via the console commands:
 *   spell          check all text in the buffer
 *   spell code     check only strings and comments (requires syntax HL)
 *
 * While the dialog is shown the following keys are active:
 *   1-7      replace with the Nth suggestion
 *   r        replace with custom text (type then Enter)
 *   i        ignore this occurrence (accepted for the session)
 *   a        add word to your personal dictionary
 *   n / ]    skip to the next misspelling
 *   Esc      stop the spell check
 *
 * When ispell/aspell is not installed, a helpful error message is shown.
 * The feature gracefully degrades to no-op if no spell checker is found.
 */

#ifndef SPELL_H
#define SPELL_H

#include "editor.h"

/* ---- modes ----------------------------------------------------------- */
#define SPELL_ALL        0   /* check every word in the buffer            */
#define SPELL_TEXT_ONLY  1   /* only strings + comments (needs HL active) */

/* ---- dialog geometry ------------------------------------------------- */
/* The box sits centred in the text area (rows 0..TEXT_ROWS-1).
 *
 * Interior row map (relative to top border, height = SPL_H):
 *   1  word info     "Word: "xxxx"  line:col"
 *   2  ═ rule ═      (shows page indicator when suggestions overflow)
 *   3..3+SPL_MAX_DISP-1   suggestion rows
 *   3+SPL_MAX_DISP         hint / replace-prompt row
 *   SPL_H-1                bottom border
 *
 * SPL_H = 1(top) + 1(word) + 1(rule) + SPL_MAX_DISP + 1(hint) + 1(bot)
 *       = SPL_MAX_DISP + 5
 */
#define SPL_W          54                        /* total width inc. border */
#define SPL_INNER      (SPL_W - 2)               /* usable interior width   */
#define SPL_MAX_DISP   7                         /* suggestion rows per page */
#define SPL_H          (SPL_MAX_DISP + 5)        /* total height = 12       */
#define SPL_COL        ((SCR_COLS - SPL_W) / 2)  /* centred horizontally    */
#define SPL_ROW        ((TEXT_ROWS - SPL_H) / 2) /* centred vertically      */

/* ---- storage (must match Editor struct in editor.h) ------------------ */
#define SPELL_MAX_SUG  30         /* max suggestions stored from ispell    */

/* ---- colours --------------------------------------------------------- */
#define A_SPL_BDR      VGA_ATTR(VGA_DGRAY, VGA_LCYAN)  /* border            */
#define A_SPL_BODY     VGA_ATTR(VGA_DGRAY, VGA_LGRAY)  /* background        */
#define A_SPL_WORDLBL  VGA_ATTR(VGA_DGRAY, VGA_LGRAY)  /* "Word:" label     */
#define A_SPL_WORDVAL  VGA_ATTR(VGA_WHITE, VGA_LGRAY)  /* the actual word   */
#define A_SPL_POS      VGA_ATTR(VGA_DGRAY, VGA_LGRAY)  /* line:col info     */
#define A_SPL_SUGNUM   VGA_ATTR(VGA_CYAN,  VGA_LGRAY)  /* "1" "2" ... key   */
#define A_SPL_SUGTEXT  VGA_ATTR(VGA_BLACK, VGA_LGRAY)  /* suggestion text   */
#define A_SPL_NOSUG    VGA_ATTR(VGA_DGRAY, VGA_LGRAY)  /* "(no suggestions)"*/
#define A_SPL_HINT     VGA_ATTR(VGA_DGRAY, VGA_CYAN)   /* hint / prompt row */
#define A_SPL_RPLVAL   VGA_ATTR(VGA_WHITE, VGA_CYAN)   /* typed replacement */
/* Selected (first-click) suggestion row */
#define A_SPL_SELNUM   VGA_ATTR(VGA_LCYAN, VGA_BLACK)  /* number key        */
#define A_SPL_SELTEXT  VGA_ATTR(VGA_WHITE, VGA_BLACK)  /* suggestion text   */
#define A_SPL_SELBODY  VGA_ATTR(VGA_DGRAY, VGA_BLACK)  /* row padding       */

/* ---- public API ------------------------------------------------------ */

/* Start a spell check from the top of the buffer.
 * Spawns ispell/aspell/hunspell (tried in order) and advances to the
 * first misspelled word.  Sets ed->spell_active = 1 on success.
 * On failure (no spell checker found, no errors, HL missing for code mode)
 * sets a status message and returns without activating the dialog. */
void spell_start(Editor *ed, int mode);

/* Clean up: close the ispell pipe, reap the child, clear spell_active. */
void spell_stop(Editor *ed);

/* Route one keypress to the spell-check dialog.
 * Must only be called when ed->spell_active == 1. */
void spell_handle_key(Editor *ed, int key);

/* Draw the spell-check dialog overlay.  Called from render_full() after
 * the main text has been drawn and the palette overlay (if any). */
void spell_render(const Editor *ed);

/* If in replace-mode, return the screen position where the cursor should
 * be placed (inside the replace-prompt).  Returns 1 and sets *x and *y.
 * Returns 0 if no special cursor placement is needed. */
int  spell_cursor_pos(const Editor *ed, int *x, int *y);

#endif /* SPELL_H */
