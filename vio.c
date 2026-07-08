/* SPDX-License-Identifier: MIT
 * vio.c  --  minimal screen I/O for full-screen editing
 *
 * Build (add to your Makefile):
 *   vio.o: vio.c vio.h vgaterm.h
 *       $(CC) $(CFLAGS) -c -o $@ $<
 */
#define _POSIX_C_SOURCE 199309L

#include "vio.h"

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>

#include <string.h>
#include <stdlib.h>

/* -----------------------------------------------------------------------
 * Module state (static singleton -- one terminal per process)
 * ----------------------------------------------------------------------- */

static VGATerm  *s_vt   = NULL;
static Display  *s_dpy  = NULL;
static Window    s_win  = 0;
static Atom      s_wmdel = 0;

static int       s_col  = 0;
static int       s_row  = 0;
static uint8_t   s_attr = VGA_ATTR_DEFAULT;

static int       s_alive    = 1;  /* 0 once window is closed */
static int       s_btn1_down = 0; /* 1 while left mouse button is held */
static int       s_scale    = 1;  /* current display scale factor */

/* -----------------------------------------------------------------------
 * Mouse event state (written before returning KEY_MOUSE_* / KEY_SCROLL_*)
 * ----------------------------------------------------------------------- */
VioMouseEvent vio_mouse;

/* ----------------------------------------------------------------------- */
/* Clipboard state                                                           */
/* ----------------------------------------------------------------------- */
#define CLIP_BUF_MAX  (1024 * 1024)   /* 1 MB paste cap */

static Atom s_atom_clipboard  = None;
static Atom s_atom_utf8       = None;
static Atom s_atom_targets    = None;
static Atom s_atom_xsel_data  = None;  /* property we ask data into */

/* Data we are advertising as our CLIPBOARD content */
static char *s_clip_owned     = NULL;  /* malloc'd copy, or NULL    */
static int   s_clip_owned_len = 0;
static int   s_clip_we_own    = 0;     /* 1 if we hold XA_CLIPBOARD */

/* Data received from a paste request */
static char  s_clip_in[CLIP_BUF_MAX + 1];
static int   s_clip_in_len    = 0;
static int   s_clip_in_ready  = 0;    /* 1 when data is available  */

/* -----------------------------------------------------------------------
 * Lifecycle
 * ----------------------------------------------------------------------- */

void vio_init(VGATerm *vt)
{
    s_vt    = vt;
    s_dpy   = vgaterm_display(vt);
    s_win   = vgaterm_window(vt);
    s_col   = 0;
    s_row   = 0;
    s_attr  = VGA_ATTR_DEFAULT;
    s_alive = 1;

    if (!s_dpy || !s_win) {
        s_alive = 0;
        return;
    }

    /* Add keyboard + mouse events to the window's existing event mask */
    XSelectInput(s_dpy, s_win,
                 ExposureMask        |
                 KeyPressMask        |
                 KeyReleaseMask      |
                 ButtonPressMask     |
                 ButtonReleaseMask   |
                 Button1MotionMask   |
                 StructureNotifyMask);

    s_wmdel = XInternAtom(s_dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(s_dpy, s_win, &s_wmdel, 1);

    /* Clipboard atoms */
    s_atom_clipboard = XInternAtom(s_dpy, "CLIPBOARD",        False);
    s_atom_utf8      = XInternAtom(s_dpy, "UTF8_STRING",      False);
    s_atom_targets   = XInternAtom(s_dpy, "TARGETS",          False);
    s_atom_xsel_data = XInternAtom(s_dpy, "VIO_XSEL_DATA",    False);

    XFlush(s_dpy);
}

void vio_fini(void)
{
    if (s_clip_owned) {
        free(s_clip_owned);
        s_clip_owned     = NULL;
        s_clip_owned_len = 0;
    }
    s_clip_we_own   = 0;
    s_clip_in_ready = 0;
    s_vt  = NULL;
    s_dpy = NULL;
    s_win = 0;
}

/* -----------------------------------------------------------------------
 * Drawing state
 * ----------------------------------------------------------------------- */

void vio_gotoxy(int col, int row)
{
    if (col < 0) col = 0;
    if (col >= VGA_COLS) col = VGA_COLS - 1;
    if (row < 0) row = 0;
    if (row >= VGA_ROWS) row = VGA_ROWS - 1;
    s_col = col;
    s_row = row;
}

int     vio_col  (void) { return s_col;  }
int     vio_row  (void) { return s_row;  }
void    vio_setattr(uint8_t attr) { s_attr = attr; }
uint8_t vio_attr (void) { return s_attr; }

/* -----------------------------------------------------------------------
 * Output helpers
 * ----------------------------------------------------------------------- */

void vio_putch(uint8_t ch)
{
    if (s_col >= VGA_COLS) return;
    vgaterm_putc(s_vt, s_col, s_row, ch, s_attr);
    s_col++;
}

void vio_putch_at(int col, int row, uint8_t ch, uint8_t attr)
{
    vgaterm_putc(s_vt, col, row, ch, attr);
}

void vio_puts(const char *s)
{
    if (!s) return;
    while (*s && s_col < VGA_COLS)
        vio_putch((uint8_t)*s++);
}

void vio_puts_n(const char *s, int n)
{
    int i;
    if (!s) s = "";
    for (i = 0; i < n && s_col < VGA_COLS; i++) {
        if (*s) {
            vio_putch((uint8_t)*s++);  /* advance pointer through content */
        } else {
            vio_putch(' ');            /* s stays at null; all remaining = spaces */
        }
    }
}

void vio_clrscr(void)
{
    vgaterm_cls(s_vt, s_attr);
    s_col = 0;
    s_row = 0;
}

void vio_clreol(void)
{
    int c;
    for (c = s_col; c < VGA_COLS; c++)
        vgaterm_putc(s_vt, c, s_row, 0x20, s_attr);
}

void vio_clrline(int row, uint8_t attr)
{
    int c;
    for (c = 0; c < VGA_COLS; c++)
        vgaterm_putc(s_vt, c, row, 0x20, attr);
}

void vio_show_cursor(void)
{
    vgaterm_set_cursor(s_vt, s_col, s_row);
}

void vio_hide_cursor(void)
{
    vgaterm_set_cursor(s_vt, -1, 0);
}

void vio_flush(void)
{
    vgaterm_blit(s_vt);
}

int vio_set_scale(int scale)
{
    int rc;
    if (!s_vt) return -1;
    rc = vgaterm_setup_scaling(s_vt, scale);
    if (rc == 0) s_scale = scale;
    return rc;
}

int vio_get_scale(void)
{
    return s_scale;
}

/* -----------------------------------------------------------------------
 * Key translation
 * ----------------------------------------------------------------------- */

/*
 * VIO_FREE_CTRL_KEYS
 *
 * When defined at compile time, certain physical keys that X11 normally
 * maps exclusively (Tab → KEY_TAB, Return → KEY_ENTER, Escape → KEY_ESC)
 * will also honour their Ctrl+letter equivalents when ControlMask is set:
 *
 *   Ctrl+Tab (XK_Tab)    → KEY_CTRL('i')  = 0x09
 *   Ctrl+Return          → KEY_CTRL('m')  = 0x0D
 *   Ctrl+Escape          → KEY_CTRL('[')  = 0x1B
 *
 * Without this flag the special-key path intercepts these before
 * XLookupString can return the control byte, so Ctrl+I always acts as
 * Tab regardless of what the application binds to it.
 *
 * Define with: cc -DVIO_FREE_CTRL_KEYS ...
 */

/*
 * Translate an XKeyEvent into a vio KEY_* value.
 * Returns KEY_NONE if the event should be ignored (e.g. lone modifier press).
 */
static int translate_key(XKeyEvent *ev)
{
    char     buf[8];
    KeySym   sym;
    int      mod;
    int      n;

    n   = XLookupString(ev, buf, sizeof(buf) - 1, &sym, NULL);
    buf[n] = '\0';
    mod = ev->state;

    /* ---- modifier-only keys: ignore ---- */
    switch (sym) {
    case XK_Shift_L:   case XK_Shift_R:
    case XK_Control_L: case XK_Control_R:
    case XK_Alt_L:     case XK_Alt_R:
    case XK_Meta_L:    case XK_Meta_R:
    case XK_Super_L:   case XK_Super_R:
    case XK_Caps_Lock: case XK_Num_Lock:
        return KEY_NONE;
    default:
        break;
    }

    /* ---- special keys ---- */
    switch (sym) {
    case XK_BackSpace:
        return (mod & ControlMask) ? KEY_CTRL_BS : KEY_BS;

    case XK_Tab:
#ifdef VIO_FREE_CTRL_KEYS
        /* Ctrl+Tab → KEY_CTRL('i') = 0x09 via XLookupString fallthrough */
        if (mod & ControlMask) break;
#endif
        return (mod & ShiftMask) ? KEY_SHIFT_TAB : KEY_TAB;

    case XK_Return:  case XK_KP_Enter:
#ifdef VIO_FREE_CTRL_KEYS
        if (mod & ControlMask) break;   /* fall to XLookupString → 0x0D */
#endif
        return KEY_ENTER;

    case XK_Escape:
#ifdef VIO_FREE_CTRL_KEYS
        if (mod & ControlMask) break;   /* fall to XLookupString → 0x1B */
#endif
        return KEY_ESC;

    case XK_Delete:  case XK_KP_Delete:
        return (mod & ControlMask) ? KEY_CTRL_DEL : KEY_DEL;
    case XK_Insert:  case XK_KP_Insert: return KEY_INS;

    case XK_Up:    case XK_KP_Up:
        if (mod & ShiftMask)   return KEY_SHIFT_UP;
        if (mod & ControlMask) return KEY_CTRL_UP;
        return KEY_UP;
    case XK_Down:  case XK_KP_Down:
        if (mod & ShiftMask)   return KEY_SHIFT_DOWN;
        if (mod & ControlMask) return KEY_CTRL_DOWN;
        return KEY_DOWN;
    case XK_Left:  case XK_KP_Left:
        if (mod & ShiftMask)   return KEY_SHIFT_LEFT;
        if (mod & ControlMask) return KEY_CTRL_LEFT;
        return KEY_LEFT;
    case XK_Right: case XK_KP_Right:
        if (mod & ShiftMask)   return KEY_SHIFT_RIGHT;
        if (mod & ControlMask) return KEY_CTRL_RIGHT;
        return KEY_RIGHT;

    case XK_Home:  case XK_KP_Home:
        return (mod & ControlMask) ? KEY_CTRL_HOME : KEY_HOME;
    case XK_End:   case XK_KP_End:
        return (mod & ControlMask) ? KEY_CTRL_END  : KEY_END;
    case XK_Page_Up:   case XK_KP_Page_Up:   return KEY_PGUP;
    case XK_Page_Down: case XK_KP_Page_Down: return KEY_PGDN;

    case XK_F1:  return KEY_F1;  case XK_F2:  return KEY_F2;
    case XK_F3:  return KEY_F3;  case XK_F4:  return KEY_F4;
    case XK_F5:  return KEY_F5;  case XK_F6:  return KEY_F6;
    case XK_F7:  return KEY_F7;  case XK_F8:  return KEY_F8;
    case XK_F9:  return KEY_F9;  case XK_F10: return KEY_F10;
    case XK_F11: return KEY_F11; case XK_F12: return KEY_F12;

    /* Ctrl+1 / Ctrl+2 / Ctrl+4 -- display scaling */
    case XK_1: case XK_KP_1:
        if (mod & ControlMask) return KEY_SCALE_1;
        break;
    case XK_2: case XK_KP_2:
        if (mod & ControlMask) return KEY_SCALE_2;
        break;
    case XK_4: case XK_KP_4:
        if (mod & ControlMask) return KEY_SCALE_4;
        break;

    default:
        break;
    }

    /* ---- Alt+key:  Mod1Mask + a printable char ---- */
    if ((mod & Mod1Mask) && n == 1) {
        unsigned char c = (unsigned char)buf[0];
        /* normalise to lowercase so Alt+A and Alt+a are the same */
        if (c >= 'A' && c <= 'Z') c = (unsigned char)(c + 32);
        return KEY_ALT(c);
    }

    /* ---- printable / control from XLookupString ---- */
    if (n == 1) {
        unsigned char c = (unsigned char)buf[0];
        /* XLookupString gives us Ctrl+letter as 0x01-0x1A */
        return (int)c;
    }

    return KEY_NONE;
}

/* -----------------------------------------------------------------------
 * Core event pump
 *
 * Processes one X event.  Returns:
 *   KEY_NONE   -- event handled internally (expose, etc.), no key
 *   KEY_CLOSED -- window was closed
 *   >= 0       -- a key code
 * ----------------------------------------------------------------------- */
static int pump_one(XEvent *ev)
{
    switch (ev->type) {

    case Expose:
        if (ev->xexpose.count == 0)
            vgaterm_blit(s_vt);
        return KEY_NONE;

    case KeyPress:
        return translate_key(&ev->xkey);

    case KeyRelease: {
        /* Only care about Shift key releases.
         * Filter auto-repeat: X11 generates KeyRelease+KeyPress pairs
         * for held keys. If the next queued event is a KeyPress for
         * the same key at the same time, it's auto-repeat — discard.  */
        KeySym sym = XLookupKeysym(&ev->xkey, 0);
        if (sym == XK_Shift_L || sym == XK_Shift_R) {
            if (XPending(s_dpy)) {
                XEvent next;
                XPeekEvent(s_dpy, &next);
                if (next.type == KeyPress &&
                    next.xkey.keycode == ev->xkey.keycode &&
                    next.xkey.time    == ev->xkey.time) {
                    /* Auto-repeat pair — consume the press and ignore  */
                    XNextEvent(s_dpy, &next);
                    return KEY_NONE;
                }
            }
            return KEY_SHIFT_RELEASE;
        }
        return KEY_NONE;
    }

    case ClientMessage:
        if ((Atom)ev->xclient.data.l[0] == s_wmdel) {
            /* Do NOT mark the display dead here; notify the app that the
             * user requested window close (WM_DELETE_WINDOW) and let the
             * application decide whether to actually exit.  Setting
             * s_alive=0 here causes subsequent vio_getch() calls to
             * immediately return KEY_CLOSED, preventing the app from
             * confirming or cancelling the close. */
            return KEY_CLOSED;
        }
        return KEY_NONE;

    case DestroyNotify:
        s_alive = 0;
        return KEY_CLOSED;

    /* --- mouse button press -------------------------------------------- */
    case ButtonPress: {
        XButtonEvent *bev = &ev->xbutton;
        /* Round to the nearest character boundary (not just floor into the
         * cell) so that clicking/dragging past the midpoint of a glyph
         * resolves to the gap *after* it. Without this, a press or release
         * anywhere over a character's cell snaps to its left edge, which
         * makes mouse-drag selections always fall one character short of
         * wherever the pointer visually appears to be. */
        int cellw = 8 * s_scale, cellh = 16 * s_scale;
        int col = (bev->x + cellw / 2) / cellw;
        int row = bev->y / cellh;
        if (col < 0) col = 0;
        if (col >= VGA_COLS) col = VGA_COLS - 1;
        if (row < 0) row = 0;
        if (row >= VGA_ROWS) row = VGA_ROWS - 1;
        vio_mouse.col  = col;
        vio_mouse.row  = row;
        vio_mouse.mods = ((bev->state & ShiftMask)   ? VIO_MOD_SHIFT : 0) |
                         ((bev->state & ControlMask)  ? VIO_MOD_CTRL  : 0) |
                         ((bev->state & Mod1Mask)      ? VIO_MOD_ALT   : 0);
        if (bev->button == 4) return KEY_SCROLL_UP;
        if (bev->button == 5) return KEY_SCROLL_DOWN;
        if (bev->button == 1) { s_btn1_down = 1; return KEY_MOUSE_PRESS; }
        if (bev->button == 3) return KEY_MOUSE_RIGHT;
        return KEY_NONE;
    }

    /* --- mouse button release ------------------------------------------ */
    case ButtonRelease: {
        XButtonEvent *bev = &ev->xbutton;
        if (bev->button == 1) {
            s_btn1_down = 0;
            int cellw = 8 * s_scale, cellh = 16 * s_scale;
            int col = (bev->x + cellw / 2) / cellw;
            int row = bev->y / cellh;
            if (col < 0) col = 0;
            if (col >= VGA_COLS) col = VGA_COLS - 1;
            if (row < 0) row = 0;
            if (row >= VGA_ROWS) row = VGA_ROWS - 1;
            vio_mouse.col  = col;
            vio_mouse.row  = row;
            vio_mouse.mods = ((bev->state & ShiftMask)   ? VIO_MOD_SHIFT : 0) |
                             ((bev->state & ControlMask)  ? VIO_MOD_CTRL  : 0) |
                             ((bev->state & Mod1Mask)      ? VIO_MOD_ALT   : 0);
            return KEY_MOUSE_RELEASE;
        }
        return KEY_NONE;
    }

    /* --- mouse drag (button 1 held + motion) --------------------------- */
    case MotionNotify: {
        /* Only report if button 1 is actually down (belt-and-suspenders). */
        if (!s_btn1_down) return KEY_NONE;

        /* Coalesce: drain any further MotionNotify events already sitting
         * in the queue and keep only the most recent one.
         *
         * Why this matters at high scale factors:
         *   vgaterm_blit() calls render_all(), which redraws every one of
         *   the 80x25 cells unconditionally, then pushes the full frame
         *   through XPutImage.  At 4x scale each cell is 32x64 = 2048 px
         *   vs 128 px at 1x, so the blit is ~16x slower.  X11 can generate
         *   MotionNotify events much faster than those blits complete, so
         *   without coalescing the event queue backs up: every render cycle
         *   consumes one stale motion event while more arrive, and the
         *   selection highlight visibly trails the mouse by hundreds of ms.
         *
         * XCheckMaskEvent is non-blocking and leaves all other event types
         * (key presses, button events, expose, etc.) completely untouched. */
        {
            XEvent next;
            while (XCheckMaskEvent(s_dpy,
                                   PointerMotionMask | Button1MotionMask,
                                   &next))
                *ev = next;   /* keep the freshest position */
        }

        XMotionEvent *mev = &ev->xmotion;
        int cellw = 8 * s_scale, cellh = 16 * s_scale;
        int col = (mev->x + cellw / 2) / cellw;
        int row = mev->y / cellh;
        if (col < 0) col = 0;
        if (col >= VGA_COLS) col = VGA_COLS - 1;
        if (row < 0) row = 0;
        if (row >= VGA_ROWS) row = VGA_ROWS - 1;
        vio_mouse.col  = col;
        vio_mouse.row  = row;
        vio_mouse.mods = ((mev->state & ShiftMask)   ? VIO_MOD_SHIFT : 0) |
                         ((mev->state & ControlMask)  ? VIO_MOD_CTRL  : 0) |
                         ((mev->state & Mod1Mask)      ? VIO_MOD_ALT   : 0);
        return KEY_MOUSE_MOVE;
    }

    /* --- X11 clipboard: we are being asked to hand over our data --- */
    case SelectionRequest: {
        XSelectionRequestEvent *req = &ev->xselectionrequest;
        XSelectionEvent         reply;

        memset(&reply, 0, sizeof(reply));
        reply.type      = SelectionNotify;
        reply.display   = req->display;
        reply.requestor = req->requestor;
        reply.selection = req->selection;
        reply.target    = req->target;
        reply.property  = None;   /* denied by default */
        reply.time      = req->time;

        if (s_clip_we_own && s_clip_owned && req->property != None) {
            if (req->target == s_atom_targets) {
                /* Advertise what we can convert to */
                Atom supported[2];
                supported[0] = s_atom_utf8;
                supported[1] = XA_STRING;
                XChangeProperty(req->display, req->requestor,
                                req->property, XA_ATOM, 32,
                                PropModeReplace,
                                (unsigned char *)supported, 2);
                reply.property = req->property;
            } else if (req->target == s_atom_utf8 ||
                       req->target == XA_STRING) {
                /* Send as UTF-8 (or Latin-1 fallback for XA_STRING) */
                XChangeProperty(req->display, req->requestor,
                                req->property,
                                req->target == s_atom_utf8
                                    ? s_atom_utf8 : XA_STRING,
                                8, PropModeReplace,
                                (unsigned char *)s_clip_owned,
                                s_clip_owned_len);
                reply.property = req->property;
            }
        }
        XSendEvent(req->display, req->requestor, False, 0,
                   (XEvent *)&reply);
        XFlush(req->display);
        return KEY_NONE;
    }

    /* --- X11 clipboard: we lost ownership (another app copied) --- */
    case SelectionClear:
        if (ev->xselectionclear.selection == s_atom_clipboard) {
            s_clip_we_own = 0;
            /* keep s_clip_owned in case undelete still refs it;
               it will be freed on next vio_clipboard_set or vio_fini */
        }
        return KEY_NONE;

    /* --- X11 clipboard: paste data has arrived --- */
    case SelectionNotify: {
        XSelectionEvent *sev = &ev->xselection;
        if (sev->property == None) {
            /* Conversion failed — try XA_STRING fallback? Already done. */
            s_clip_in_ready = 0;
            return KEY_PASTE_READY;   /* signal editor to check anyway */
        }
        if (sev->selection == s_atom_clipboard &&
            sev->property  == s_atom_xsel_data) {
            Atom   actual_type;
            int    actual_format;
            unsigned long n_items, bytes_after;
            unsigned char *prop_data = NULL;

            XGetWindowProperty(s_dpy, s_win, s_atom_xsel_data,
                               0, (CLIP_BUF_MAX / 4) + 1, True,
                               AnyPropertyType,
                               &actual_type, &actual_format,
                               &n_items, &bytes_after,
                               &prop_data);

            if (prop_data && n_items > 0) {
                int bytes = (int)n_items * (actual_format / 8);
                if (bytes > CLIP_BUF_MAX) bytes = CLIP_BUF_MAX;
                memcpy(s_clip_in, prop_data, (size_t)bytes);
                s_clip_in[bytes] = '\0';
                s_clip_in_len    = bytes;
                s_clip_in_ready  = 1;
            }
            if (prop_data) XFree(prop_data);
        }
        return KEY_PASTE_READY;
    }

    default:
        return KEY_NONE;
    }
}

/* -----------------------------------------------------------------------
 * Public input
 * ----------------------------------------------------------------------- */

int vio_getch(void)
{
    XEvent ev;
    int    k;

    if (!s_alive || !s_dpy) return KEY_CLOSED;

    for (;;) {
        XNextEvent(s_dpy, &ev);
        k = pump_one(&ev);
        if (k == KEY_CLOSED) return KEY_CLOSED;
        if (k != KEY_NONE)   return k;
        /* KEY_NONE = event handled, wait for next */
    }
}

int vio_kbhit(void)
{
    XEvent ev;
    int    k;

    if (!s_alive || !s_dpy) return KEY_CLOSED;

    while (XPending(s_dpy)) {
        XNextEvent(s_dpy, &ev);
        k = pump_one(&ev);
        if (k == KEY_CLOSED) return KEY_CLOSED;
        if (k != KEY_NONE)   return k;
    }
    return KEY_NONE;
}

/* -----------------------------------------------------------------------
 * Numeric output -- hand-rolled, no snprintf
 * ----------------------------------------------------------------------- */

/* Render digits of n into tmp[], return count. n must be >= 0. */
static int uint_digits(unsigned int n, char tmp[12])
{
    int len = 0;
    if (n == 0) { tmp[len++] = '0'; return len; }
    while (n > 0) { tmp[len++] = (char)('0' + n % 10); n /= 10; }
    /* digits are reversed; caller reverses on output */
    return len;
}

void vio_uint(unsigned int n, int width)
{
    char tmp[12];
    int  len = uint_digits(n, tmp);
    int  pad = width - len;
    while (pad-- > 0) vio_putch(' ');
    while (len-- > 0) vio_putch((uint8_t)tmp[len]);   /* reverse */
}

void vio_int(int n, int width)
{
    if (n < 0) {
        vio_putch('-');
        if (width > 1) width--;
        /* avoid UB on INT_MIN: cast through unsigned */
        vio_uint((unsigned int)(-(n + 1)) + 1u, width);
    } else {
        vio_uint((unsigned int)n, width);
    }
}

void vio_hex(unsigned int n, int width)
{
    static const char hex[] = "0123456789abcdef";
    char tmp[8];
    int  len = 0;
    if (n == 0) { tmp[len++] = '0'; }
    else { while (n) { tmp[len++] = hex[n & 0xF]; n >>= 4; } }
    int pad = width - len;
    while (pad-- > 0) vio_putch('0');
    while (len-- > 0) vio_putch((uint8_t)tmp[len]);
}

/* -----------------------------------------------------------------------
 * Window title
 * ----------------------------------------------------------------------- */

void vio_set_title(const char *title)
{
    if (s_dpy && s_win)
        XStoreName(s_dpy, s_win, title ? title : "");
}

/* -----------------------------------------------------------------------
 * Clipboard public API
 * ----------------------------------------------------------------------- */

void vio_clipboard_set(const char *buf, int len)
{
    if (!s_dpy || !s_win) return;
    if (len < 0) len = 0;
    if (len > CLIP_BUF_MAX) len = CLIP_BUF_MAX;

    free(s_clip_owned);
    s_clip_owned = (char *)malloc((size_t)(len + 1));
    if (!s_clip_owned) { s_clip_owned_len = 0; s_clip_we_own = 0; return; }
    memcpy(s_clip_owned, buf, (size_t)len);
    s_clip_owned[len] = '\0';
    s_clip_owned_len  = len;

    XSetSelectionOwner(s_dpy, s_atom_clipboard, s_win, CurrentTime);
    s_clip_we_own = (XGetSelectionOwner(s_dpy, s_atom_clipboard) == s_win);
    XFlush(s_dpy);
}

void vio_clipboard_request(void)
{
    if (!s_dpy || !s_win) return;
    s_clip_in_ready = 0;
    XConvertSelection(s_dpy, s_atom_clipboard, s_atom_utf8,
                      s_atom_xsel_data, s_win, CurrentTime);
    XFlush(s_dpy);
}

int vio_clipboard_owns(void)
{
    return s_clip_we_own;
}

const char *vio_clipboard_take(int *len)
{
    if (!s_clip_in_ready) { if (len) *len = 0; return NULL; }
    s_clip_in_ready = 0;
    if (len) *len = s_clip_in_len;
    return s_clip_in;
}

/* -----------------------------------------------------------------------
 * Rectangular fill and box drawing
 * ----------------------------------------------------------------------- */

void vio_fill(int col, int row, int w, int h, uint8_t ch, uint8_t attr)
{
    int r, c;
    /* clamp to screen */
    if (col < 0) { w += col; col = 0; }
    if (row < 0) { h += row; row = 0; }
    if (col + w > VGA_COLS) w = VGA_COLS - col;
    if (row + h > VGA_ROWS) h = VGA_ROWS - row;
    if (w <= 0 || h <= 0) return;

    for (r = row; r < row + h; r++)
        for (c = col; c < col + w; c++)
            vgaterm_putc(s_vt, c, r, ch, attr);
}

/* generic box draw given the 6 border chars */
static void draw_box_chars(int col, int row, int w, int h, uint8_t attr,
                            uint8_t tl, uint8_t tr, uint8_t bl, uint8_t br,
                            uint8_t hz, uint8_t vt_ch)
{
    int i;
    if (w < 2 || h < 2) return;

    vgaterm_putc(s_vt, col,         row,         tl,    attr);
    vgaterm_putc(s_vt, col + w - 1, row,         tr,    attr);
    vgaterm_putc(s_vt, col,         row + h - 1, bl,    attr);
    vgaterm_putc(s_vt, col + w - 1, row + h - 1, br,    attr);

    for (i = 1; i < w - 1; i++) {
        vgaterm_putc(s_vt, col + i, row,         hz,    attr);
        vgaterm_putc(s_vt, col + i, row + h - 1, hz,    attr);
    }
    for (i = 1; i < h - 1; i++) {
        vgaterm_putc(s_vt, col,         row + i,  vt_ch, attr);
        vgaterm_putc(s_vt, col + w - 1, row + i,  vt_ch, attr);
    }
}

void vio_box(int col, int row, int w, int h, uint8_t attr)
{
    /* CP437: ┌0xDA ┐0xBF └0xC0 ┘0xD9 ─0xC4 │0xB3 */
    draw_box_chars(col, row, w, h, attr,
                   0xDA, 0xBF, 0xC0, 0xD9, 0xC4, 0xB3);
}

void vio_dbox(int col, int row, int w, int h, uint8_t attr)
{
    /* CP437: ╔0xC9 ╗0xBB ╚0xC8 ╝0xBC ═0xCD ║0xBA */
    draw_box_chars(col, row, w, h, attr,
                   0xC9, 0xBB, 0xC8, 0xBC, 0xCD, 0xBA);
}
