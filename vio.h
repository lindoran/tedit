/* SPDX-License-Identifier: MIT
 * vio.h  --  minimal screen I/O for full-screen editing
 *
 * Sits directly on top of vgaterm.  Adds:
 *   - keyboard input (blocking + non-blocking)
 *   - cursor-tracked character output
 *   - clear-screen / clear-to-EOL
 *
 * There is no scrolling, no line wrapping, no echo, no line buffering.
 * The caller owns all of that.
 *
 * Typical editor loop:
 *
 *   VGATerm *vt = vgaterm_open("myeditor");
 *   vio_init(vt);
 *   vio_clrscr();
 *   vio_flush();
 *
 *   for (;;) {
 *       int k = vio_getch();            // blocks
 *       if (k == KEY_CLOSED) break;
 *       if (k == KEY_CTRL('q')) break;
 *       // ... handle k, update vgaterm_mem(vt) directly ...
 *       vio_flush();
 *   }
 *
 *   vio_fini();
 *   vgaterm_close(vt);
 */

#ifndef VIO_H
#define VIO_H

#include "vgaterm.h"

/* -----------------------------------------------------------------------
 * Key codes
 *
 * Printable ASCII and C0 control codes (0x00-0x7F) are returned as-is,
 * which means Ctrl+A == 0x01, Ctrl+Z == 0x1A, etc.
 *
 * The named aliases below are provided for common control codes.
 * Special (non-ASCII) keys are returned as values >= KEY_SPECIAL.
 * ----------------------------------------------------------------------- */

/* Common control-code aliases (their actual ASCII values) */
#define KEY_CTRL(c)    ((c) & 0x1F)     /* KEY_CTRL('q') == 0x11 */
#define KEY_BS         0x08             /* Backspace (^H)         */
#define KEY_TAB        0x0130           /* Tab — extended so Ctrl+I (0x09) is free */
#define KEY_ENTER      0x0D             /* Enter     (^M)         */
#define KEY_ESC        0x1B             /* Escape                 */

/* Special keys (above the ASCII range) */
#define KEY_SPECIAL    0x0100

#define KEY_DEL        0x0100           /* Delete (forward)       */
#define KEY_UP         0x0101
#define KEY_DOWN       0x0102
#define KEY_LEFT       0x0103
#define KEY_RIGHT      0x0104
#define KEY_HOME       0x0105
#define KEY_END        0x0106
#define KEY_PGUP       0x0107
#define KEY_PGDN       0x0108
#define KEY_INS        0x0109

#define KEY_F1         0x0110           /* F1  .. F12: 0x0110..0x011B */
#define KEY_F2         0x0111
#define KEY_F3         0x0112
#define KEY_F4         0x0113
#define KEY_F5         0x0114
#define KEY_F6         0x0115
#define KEY_F7         0x0116
#define KEY_F8         0x0117
#define KEY_F9         0x0118
#define KEY_F10        0x0119
#define KEY_F11        0x011A
#define KEY_F12        0x011B

#define KEY_SHIFT_TAB  0x011C           /* Shift+Tab (back-tab)   */

/* Shift + arrow (useful for selection) */
#define KEY_SHIFT_UP   0x0120
#define KEY_SHIFT_DOWN 0x0121
#define KEY_SHIFT_LEFT 0x0122
#define KEY_SHIFT_RIGHT 0x0123

/* Ctrl + arrow */
#define KEY_CTRL_UP    0x0128
#define KEY_CTRL_DOWN  0x0129
#define KEY_CTRL_LEFT  0x012A
#define KEY_CTRL_RIGHT 0x012B
#define KEY_CTRL_HOME  0x012C
#define KEY_CTRL_END   0x012D

/* Ctrl + Backspace / Delete (word-kill keys) */
#define KEY_CTRL_BS    0x012E
#define KEY_CTRL_DEL   0x012F

/* Ctrl+1 / Ctrl+2 / Ctrl+4 -- set display scale */
#define KEY_SCALE_1    0x0140
#define KEY_SCALE_2    0x0141
#define KEY_SCALE_4    0x0142

/* Sentinel values */
#define KEY_CLOSED        -1              /* window was closed      */
#define KEY_NONE          -2              /* vio_kbhit() returned nothing */
#define KEY_PASTE_READY   -3             /* SelectionNotify arrived; call vio_clipboard_take() */
#define KEY_SHIFT_RELEASE -4             /* Shift key released (not auto-repeat) */

/* Mouse events (negative, like sentinels) */
#define KEY_MOUSE_PRESS   -5   /* left button pressed; see vio_mouse for position  */
#define KEY_MOUSE_RELEASE -6   /* left button released; see vio_mouse              */
#define KEY_MOUSE_MOVE    -7   /* mouse moved with button 1 held (drag)            */
#define KEY_SCROLL_UP     -8   /* wheel scrolled up                                */
#define KEY_SCROLL_DOWN   -9   /* wheel scrolled down                              */
#define KEY_MOUSE_RIGHT  -10   /* right button pressed; see vio_mouse for position */

/* Modifier bits in VioMouseEvent.mods (X11-independent) */
#define VIO_MOD_SHIFT  0x01
#define VIO_MOD_CTRL   0x04
#define VIO_MOD_ALT    0x08

/*
 * VioMouseEvent -- populated before vio_getch() returns any KEY_MOUSE_*
 * or KEY_SCROLL_* value.  col/row are VGA cell coordinates (0-based).
 */
typedef struct {
    int col;    /* 0..VGA_COLS-1 */
    int row;    /* 0..VGA_ROWS-1 */
    int mods;   /* VIO_MOD_* flags */
} VioMouseEvent;

/*
 * Last mouse event.  Updated immediately before vio_getch() /
 * vio_kbhit() returns a KEY_MOUSE_* or KEY_SCROLL_* code.
 */
extern VioMouseEvent vio_mouse;

/* -----------------------------------------------------------------------
 * Lifecycle
 * ----------------------------------------------------------------------- */

/*
 * vio_init  --  attach vio to an existing VGATerm window.
 * Adds KeyPress events to the window's event mask.
 * Must be called before any other vio function.
 */
void vio_init(VGATerm *vt);

/*
 * vio_fini  --  release vio resources.
 * Does NOT close the VGATerm; call vgaterm_close() separately.
 */
void vio_fini(void);

/* -----------------------------------------------------------------------
 * Drawing state
 *
 * vio tracks a current cursor position and a current attribute, used by
 * vio_putch() / vio_puts().  These do NOT affect the vgaterm cursor shown
 * on screen; call vio_show_cursor() to sync it.
 * ----------------------------------------------------------------------- */

void    vio_gotoxy  (int col, int row);
int     vio_col     (void);
int     vio_row     (void);

void    vio_setattr (uint8_t attr);
uint8_t vio_attr    (void);

/* -----------------------------------------------------------------------
 * Output
 *
 * All output writes to the vgaterm memory buffer.
 * Call vio_flush() to push it to the screen.
 * ----------------------------------------------------------------------- */

/*
 * vio_putch  --  write one CP437 character at (col, row), advance col.
 * Does NOT wrap; stops at column 79.
 */
void vio_putch(uint8_t ch);

/*
 * vio_putch_at  --  write one character at an explicit position.
 * Does not move the tracked cursor.
 */
void vio_putch_at(int col, int row, uint8_t ch, uint8_t attr);

/*
 * vio_puts  --  write a NUL-terminated string at (col, row), advance col.
 * Does NOT wrap or scroll.
 */
void vio_puts(const char *s);

/*
 * vio_puts_n  --  write exactly n characters (space-padded if s is shorter).
 * Useful for fixed-width fields in status bars etc.
 */
void vio_puts_n(const char *s, int n);

/*
 * vio_clrscr  --  fill entire screen with space + current attribute.
 * Resets cursor to (0, 0).
 */
void vio_clrscr(void);

/*
 * vio_clreol  --  fill from current col to end of current row with
 * space + current attribute.  Does not move the cursor.
 */
void vio_clreol(void);

/*
 * vio_clrline  --  clear an entire row to space + attr.
 * Does not move the tracked cursor.
 */
void vio_clrline(int row, uint8_t attr);

/*
 * vio_show_cursor  --  sync the vgaterm on-screen cursor to vio's tracked
 * position.  Call after your render pass before vio_flush().
 */
void vio_show_cursor(void);

/*
 * vio_hide_cursor  --  remove the on-screen cursor.
 */
void vio_hide_cursor(void);

/*
 * vio_flush  --  blit the vgaterm buffer to the X11 window.
 * Equivalent to vgaterm_blit().
 */
void vio_flush(void);

/* -----------------------------------------------------------------------
 * Input
 * ----------------------------------------------------------------------- */

/*
 * vio_getch  --  wait for the next keypress and return its code.
 * Also handles expose events (reblits the screen transparently).
 * Returns KEY_CLOSED if the window is closed.
 */
int vio_getch(void);

/*
 * vio_kbhit  --  return the next pending key code without blocking,
 * or KEY_NONE if nothing is queued.
 * Also handles expose events.
 * Returns KEY_CLOSED if the window is closed.
 */
int vio_kbhit(void);

/* -----------------------------------------------------------------------
 * Numeric output
 *
 * Hand-rolled; no snprintf, no allocation.  Safe to call per-line in a
 * render loop.  For anything more complex (mixed strings + numbers),
 * snprintf into a stack buffer and call vio_puts -- the cost is visible
 * at the call site.
 * ----------------------------------------------------------------------- */

/*
 * vio_int  --  print a signed decimal integer, right-justified in a
 * field of `width` columns (0 = minimum width).  Fills with spaces on
 * the left.  Writes at most width chars; clamps if the number is wider
 * than the field.
 */
void vio_int(int n, int width);

/*
 * vio_uint  --  unsigned decimal, same rules.
 */
void vio_uint(unsigned int n, int width);

/*
 * vio_hex  --  lowercase hexadecimal, zero-padded to `width` digits
 * (0 = minimum width).  Useful for address display.
 */
void vio_hex(unsigned int n, int width);

/* -----------------------------------------------------------------------
 * Window title
 * ----------------------------------------------------------------------- */

/*
 * vio_set_title  --  update the X11 window title.
 * Cheap: one XStoreName call.  Suitable for calling on file-save or
 * dirty-state change, not in the render loop.
 */
void vio_set_title(const char *title);

/* -----------------------------------------------------------------------
 * Alt+key codes
 *
 * Alt+letter returns KEY_ALT('a')..KEY_ALT('z') regardless of case.
 * Alt+digit  returns KEY_ALT('0')..KEY_ALT('9').
 * Other Alt combos return KEY_NONE (ignored).
 *
 * KEY_ALT(c) packs c into the 0x0200 range:
 *   KEY_ALT('q') == 0x0271,  KEY_ALT('1') == 0x0231, etc.
 * ----------------------------------------------------------------------- */

#define KEY_ALT(c)   (0x0200 | ((unsigned char)(c)))

/* -----------------------------------------------------------------------
 * X11 Clipboard (CLIPBOARD selection)
 *
 * Copy/paste goes through the standard X11 CLIPBOARD atom so it
 * interoperates with other applications (terminals, browsers, etc.).
 *
 * COPY side:
 *   Call vio_clipboard_set(buf, len) with the UTF-8 text to advertise.
 *   vio owns the buffer for as long as it holds the selection; the caller
 *   must not free it until vio_clipboard_owns() returns 0.
 *
 * PASTE side:
 *   Call vio_clipboard_request() to ask the current owner to convert to
 *   UTF-8.  The owner will send a SelectionNotify event which pump_one()
 *   will handle, storing the result internally.  Retrieve it (and consume
 *   it) with vio_clipboard_take().  Returns NULL if no data is ready yet.
 *   The returned pointer is valid until the next call to clipboard_request
 *   or clipboard_take.
 * ----------------------------------------------------------------------- */

/*
 * Advertise buf[0..len-1] as our CLIPBOARD content.
 * Takes ownership of the X11 CLIPBOARD selection.
 */
void vio_clipboard_set(const char *buf, int len);

/*
 * Ask the current CLIPBOARD owner to send us UTF-8 text.
 * The result arrives asynchronously; poll vio_clipboard_take().
 */
void vio_clipboard_request(void);

/*
 * Returns 1 if this process owns the CLIPBOARD selection.
 */
int vio_clipboard_owns(void);

/*
 * If a paste response has arrived, return a pointer to the NUL-terminated
 * UTF-8 string and set *len to its byte length.  Returns NULL if nothing
 * has arrived yet.  The data is consumed (cleared) on return.
 */
const char *vio_clipboard_take(int *len);

/* -----------------------------------------------------------------------
 * Rectangular fill
 * ----------------------------------------------------------------------- */

/*
 * vio_fill  --  fill a w×h rectangle with ch+attr.
 * Clips silently to screen bounds.
 * Does not move the tracked cursor.
 */
void vio_fill(int col, int row, int w, int h, uint8_t ch, uint8_t attr);

/*
 * vio_box  --  draw a single-line box using CP437 border characters.
 * Interior is NOT cleared; call vio_fill on the interior if needed.
 * Does not move the tracked cursor.
 *
 * CP437 corners/edges:  ┌─┐│└┘
 */
void vio_box(int col, int row, int w, int h, uint8_t attr);

/*
 * vio_dbox  --  same with double-line borders:  ╔═╗║╚╝
 */
void vio_dbox(int col, int row, int w, int h, uint8_t attr);

/* -----------------------------------------------------------------------
 * Scaling
 *
 * vio_set_scale  --  change window pixel scaling.
 *   scale  1 = 1x (640x400), 2 = 2x (1280x800), 4 = 4x (2560x1600)
 *   Returns 0 on success, -1 on failure (bad value or vt not initialised).
 *
 * vio_get_scale  --  return the current scale factor (1, 2, or 4).
 * ----------------------------------------------------------------------- */
int vio_set_scale(int scale);
int vio_get_scale(void);

#endif /* VIO_H */
