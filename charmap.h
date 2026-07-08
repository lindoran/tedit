/* SPDX-License-Identifier: MIT
 * charmap.h  --  CP437 character-map palette for tedit
 *
 * Activated via the console command:
 *   charmap          (alias: chars)
 *
 * While the dialog is shown the following keys are active:
 *   Arrow keys       move the selection cursor
 *   Home / End       jump to start / end of the current row
 *   Ctrl+Home/End    jump to char 0x00 / 0xFF
 *   PgUp / PgDn      move 4 rows up / down
 *   type a char      jump the cursor to that character's code point
 *   Enter / Space    insert the selected character and close
 *   Esc              close without inserting
 *   Mouse click      insert the clicked character (click outside box closes)
 *
 * Dialog interior row map (relative to top border row, total height CMP_H):
 *   0               top border  ╔══ Character Map ══╗
 *   1 .. CMP_ROWS   character grid (8 rows × 32 cols = 256 glyphs)
 *   CMP_ROWS+1      divider     ╠══════════════════╣
 *   CMP_ROWS+2      hint row    code + key hints
 *   CMP_H-1         bottom border ╚═════════════════╝
 */

#ifndef CHARMAP_H
#define CHARMAP_H

#include "editor.h"

/* ---- dialog geometry ----------------------------------------------------- */

#define CMP_COLS   32                           /* characters per grid row    */
#define CMP_ROWS   8                            /* character grid rows        */
#define CMP_W      (CMP_COLS + 2)              /* 34: content + 2 border cols */
#define CMP_INNER  CMP_COLS                    /* 32: usable interior width   */
#define CMP_H      (CMP_ROWS + 4)             /* 12: borders+grid+div+hint   */
#define CMP_COL    ((SCR_COLS  - CMP_W) / 2)  /* centred horizontally        */
#define CMP_ROW    ((TEXT_ROWS - CMP_H) / 2)  /* centred vertically          */

/* ---- colours (match palette.h / spell.h palette) ------------------------- */

#define A_CMP_BDR    VGA_ATTR(VGA_DGRAY, VGA_LCYAN)  /* border chars         */
#define A_CMP_BODY   VGA_ATTR(VGA_DGRAY, VGA_LGRAY)  /* unselected char cell */
#define A_CMP_SEL    VGA_ATTR(VGA_CYAN,  VGA_BLACK)  /* selected char cell   */
#define A_CMP_DIM    VGA_ATTR(VGA_BLACK, VGA_LGRAY)  /* 0x00-0x1F: not insertable */
#define A_CMP_HINT   VGA_ATTR(VGA_DGRAY, VGA_CYAN)   /* hint row background  */
#define A_CMP_CODE   VGA_ATTR(VGA_WHITE, VGA_CYAN)   /* char code in hint    */

/* ---- API ----------------------------------------------------------------- */

/* Activate the character-map dialog, preserving charmap_cur. */
void charmap_open(Editor *ed);

/* Handle one key while the dialog is visible. */
void charmap_handle_key(Editor *ed, int key);

/* Draw the dialog overlay.  Call after render_full(). */
void charmap_render(const Editor *ed);

#endif /* CHARMAP_H */
