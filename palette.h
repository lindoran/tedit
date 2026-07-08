/* SPDX-License-Identifier: MIT
 * palette.h  --  command palette (opened by the "help" console command)
 *
 * Pressing Esc opens the console; typing "help" + Enter opens the palette.
 * Up / Down navigate the list; typing filters it.  Enter fills the console
 * with the selected command template ready to edit or execute.  Esc cancels.
 */

#ifndef PALETTE_H
#define PALETTE_H

#include "editor.h"

/* ---- layout -------------------------------------------------------------- */

#define PAL_VISIBLE  8           /* entry rows shown at once          */
#define PAL_W        52          /* total panel width (incl. border)  */
#define PAL_INNER    (PAL_W - 2) /* usable columns inside border      */
#define PAL_H        (PAL_VISIBLE + 2)       /* total height           */
#define PAL_COL      ((SCR_COLS - PAL_W) / 2)/* centre horizontally   */
#define PAL_ROW      (STATUS_ROW - PAL_H)    /* sit above status bar   */

/* column widths of the two fields inside each row */
#define PAL_CMD_W    20
#define PAL_HINT_W   (PAL_INNER - 1 - PAL_CMD_W - 2 - 1)  /* = 26 */

/* ---- colours ------------------------------------------------------------- */

#define A_PAL_BDR    VGA_ATTR(VGA_DGRAY, VGA_LCYAN)  /* border chars      */
#define A_PAL_ENTRY  VGA_ATTR(VGA_DGRAY, VGA_LGRAY)  /* normal command    */
#define A_PAL_EHINT  VGA_ATTR(VGA_DGRAY, VGA_CYAN)   /* normal hint text  */
#define A_PAL_SEL    VGA_ATTR(VGA_CYAN,  VGA_BLACK)  /* selected command  */
#define A_PAL_SHINT  VGA_ATTR(VGA_CYAN,  VGA_DGRAY)  /* selected hint     */

/* ---- API ----------------------------------------------------------------- */

/* Open the palette (clears filter, resets selection). */
void palette_open(Editor *ed);

/* Handle one key while the palette is visible. */
void palette_handle_key(Editor *ed, int key);

/* Draw the palette overlay.  Must be called after render_full(). */
void palette_render(const Editor *ed);

#endif /* PALETTE_H */
