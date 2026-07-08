/* SPDX-License-Identifier: MIT
 * vgaterm.c  --  VGA text-mode emulator for X11
 *
 * _POSIX_C_SOURCE 199309L is required for clock_gettime / struct timespec.
 */
#define _POSIX_C_SOURCE 199309L

/*
 * vgaterm.c  --  VGA text-mode emulator for X11
 *
 * Renders an 80x25 CP437 text buffer using the real 8x16 VGA bitmap font
 * into a 640x400 X11 window.  No font libraries required; everything is
 * done through Xlib with a single XImage that is blitted each frame.
 *
 * Build:
 *   cc -O2 -o vgaterm.o -c vgaterm.c $(pkg-config --cflags x11)
 */

#include "vgaterm.h"
#include "font_vga.h"

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

/* -------------------------------------------------------------------------
 * CGA / VGA 16-colour palette  (RGB values matching real VGA hardware)
 * ------------------------------------------------------------------------- */
static const uint32_t CGA_PALETTE[16] = {
    0x000000, /* 0  Black        */
    0x0000AA, /* 1  Blue         */
    0x00AA00, /* 2  Green        */
    0x00AAAA, /* 3  Cyan         */
    0xAA0000, /* 4  Red          */
    0xAA00AA, /* 5  Magenta      */
    0xAA5500, /* 6  Brown        */
    0xAAAAAA, /* 7  Light Grey   */
    0x555555, /* 8  Dark Grey    */
    0x5555FF, /* 9  Light Blue   */
    0x55FF55, /* 10 Light Green  */
    0x55FFFF, /* 11 Light Cyan   */
    0xFF5555, /* 12 Light Red    */
    0xFF55FF, /* 13 Light Magenta*/
    0xFFFF55, /* 14 Yellow       */
    0xFFFFFF, /* 15 White        */
};

/* -------------------------------------------------------------------------
 * Internal state
 * ------------------------------------------------------------------------- */
struct VGATerm {
    Display     *dpy;
    Window       win;
    GC           gc;
    XImage      *img;          /* backing image (size depends on scale)    */
    Visual      *vis;
    int          depth;
    Atom         wm_del;       /* WM_DELETE_WINDOW atom                    */

    uint8_t      mem[VGA_MEMSIZE]; /* the text buffer, like 0xB8000        */

    /* pixel colours pre-mapped to display format */
    unsigned long px[16];

    /* cursor */
    int          cur_col;
    int          cur_row;
    int          cur_visible;  /* 1 if cursor drawn in current img         */
    struct timespec cur_time;  /* for blinking                             */

    int          alive;        /* 0 once window is closed                  */

    /* Four character generator tables, matching real VGA plane-2 behaviour.
     *
     * font_table[0] is the default (equivalent to VGA page 0 / BIOS font).
     * NULL in any slot falls back to the compiled-in vga_font_8x16.
     * fplane[row*VGA_COLS+col] & 0x03 selects the table for each cell.
     * All slots zero-initialised by calloc (=> built-in font everywhere). */
    const unsigned char (*font_table[4])[16];
    uint8_t              fplane[VGA_ROWS * VGA_COLS];
    uint8_t              uplane[VGA_ROWS * VGA_COLS]; /* underline per cell */

    /* raw pixel buffer (32bpp BGRA or BGRX depending on display) */
    uint32_t    *pixels;       /* scaled pixel buffer                       */

    /* scaling configuration */
    int          scale_factor; /* 1x, 2x, or 4x scaling                   */
    int          current_px_w; /* current pixel width (scaled)             */
    int          current_px_h; /* current pixel height (scaled)            */
};

/* -------------------------------------------------------------------------
 * Colour mapping
 * ------------------------------------------------------------------------- */
static unsigned long map_colour(Display *dpy, Visual *vis, uint32_t rgb)
{
    XColor xc;
    (void)vis;
    Colormap cm = DefaultColormap(dpy, DefaultScreen(dpy));
    xc.red   = ((rgb >> 16) & 0xFF) * 257;
    xc.green = ((rgb >>  8) & 0xFF) * 257;
    xc.blue  = ((rgb      ) & 0xFF) * 257;
    xc.flags = DoRed | DoGreen | DoBlue;
    XAllocColor(dpy, cm, &xc);
    return xc.pixel;
}

/* -------------------------------------------------------------------------
 * Render one cell into the pixel buffer
 * -------------------------------------------------------------------------
 * Supports 1x, 2x, and 4x scaling with pixel and line replication.
 */
static void render_cell(VGATerm *vt, int col, int row, int cursor_here)
{
    int offset = (row * VGA_COLS + col) * 2;
    uint8_t ch   = vt->mem[offset];
    uint8_t attr = vt->mem[offset + 1];

    int fg_idx = attr & 0x0F;
    int bg_idx = (attr >> 4) & 0x0F;

    unsigned long fg_px = vt->px[fg_idx];
    unsigned long bg_px = vt->px[bg_idx];

    const unsigned char (*f)[16] = vt->font_table[vt->fplane[row*VGA_COLS+col] & 0x03];
    const unsigned char  *glyph  = f ? f[ch] : vga_font_8x16[ch];

    int base_x = col * 8 * vt->scale_factor;
    int base_y = row * 16 * vt->scale_factor;
    int scale = vt->scale_factor;

    int r, b, yr, xr;
    int underline = vt->uplane[row * VGA_COLS + col];
    for (r = 0; r < 16; r++) {
        unsigned char row_bits = glyph[r];
        if (underline && r == 14)
            row_bits = 0xFF;
        
        /* Render row 'r' scaled vertically */
        for (yr = 0; yr < scale; yr++) {
            int pixel_row = base_y + r * scale + yr;
            uint32_t *scanline = vt->pixels + pixel_row * vt->current_px_w + base_x;
            
            for (b = 0; b < 8; b++) {
                int set = (row_bits >> (7 - b)) & 1;
                /* cursor inverts the cell */
                if (cursor_here) set = !set;
                uint32_t pixel_value = (uint32_t)(set ? fg_px : bg_px);
                
                /* Render pixel 'b' scaled horizontally */
                for (xr = 0; xr < scale; xr++) {
                    scanline[b * scale + xr] = pixel_value;
                }
            }
        }
    }
}

/* -------------------------------------------------------------------------
 * Full render of all 2000 cells
 * ------------------------------------------------------------------------- */
static void render_all(VGATerm *vt, int draw_cursor)
{
    int col, row;
    for (row = 0; row < VGA_ROWS; row++) {
        for (col = 0; col < VGA_COLS; col++) {
            int here = draw_cursor && (col == vt->cur_col) &&
                       (row == vt->cur_row) && (vt->cur_col >= 0);
            render_cell(vt, col, row, here);
        }
    }
}

/* -------------------------------------------------------------------------
 * Compute whether the cursor should be visible right now (500 ms blink)
 * ------------------------------------------------------------------------- */
static int cursor_blink_on(VGATerm *vt)
{
    struct timespec now;
    long ms;
    if (vt->cur_col < 0) return 0;
    clock_gettime(CLOCK_MONOTONIC, &now);
    ms = (long)((now.tv_sec  - vt->cur_time.tv_sec) * 1000 +
                (now.tv_nsec - vt->cur_time.tv_nsec) / 1000000);
    return (ms / 500) % 2 == 0;
}

/* =========================================================================
 * Public API
 * ========================================================================= */

VGATerm *vgaterm_open(const char *title)
{
    VGATerm *vt;
    XSetWindowAttributes xswa;
    XSizeHints hints;
    int screen;
    int i;

    vt = (VGATerm *)calloc(1, sizeof(VGATerm));
    if (!vt) return NULL;

    vt->cur_col = 0;
    vt->cur_row = 0;
    vt->alive   = 1;
    
    /* Default: 1x scaling */
    vt->scale_factor = 1;
    vt->current_px_w = VGA_PX_W;
    vt->current_px_h = VGA_PX_H;
    
    clock_gettime(CLOCK_MONOTONIC, &vt->cur_time);

    /* Allocate pixel buffer (32bpp, width * height) */
    vt->pixels = (uint32_t *)malloc(vt->current_px_w * vt->current_px_h * sizeof(uint32_t));
    if (!vt->pixels) { free(vt); return NULL; }
    memset(vt->pixels, 0, vt->current_px_w * vt->current_px_h * sizeof(uint32_t));

    /* Open display */
    vt->dpy = XOpenDisplay(NULL);
    if (!vt->dpy) {
        fprintf(stderr, "vgaterm: XOpenDisplay failed\n");
        free(vt->pixels); free(vt); return NULL;
    }

    screen   = DefaultScreen(vt->dpy);
    vt->vis  = DefaultVisual(vt->dpy, screen);
    vt->depth= DefaultDepth(vt->dpy, screen);

    /* Pre-map the 16 palette colours */
    for (i = 0; i < 16; i++)
        vt->px[i] = map_colour(vt->dpy, vt->vis, CGA_PALETTE[i]);

    /* Create window: fixed size, no resize */
    xswa.background_pixel = vt->px[VGA_BLACK];
    xswa.border_pixel      = vt->px[VGA_BLACK];
    xswa.event_mask        = ExposureMask | StructureNotifyMask;

    vt->win = XCreateWindow(
        vt->dpy, RootWindow(vt->dpy, screen),
        0, 0, vt->current_px_w, vt->current_px_h, 0,
        vt->depth, InputOutput, vt->vis,
        CWBackPixel | CWBorderPixel | CWEventMask, &xswa
    );

    /* Lock the size */
    memset(&hints, 0, sizeof(hints));
    hints.flags      = PMinSize | PMaxSize | PBaseSize;
    hints.min_width  = hints.max_width  = hints.base_width  = vt->current_px_w;
    hints.min_height = hints.max_height = hints.base_height = vt->current_px_h;
    XSetWMNormalHints(vt->dpy, vt->win, &hints);

    /* Window title */
    XStoreName(vt->dpy, vt->win, title ? title : "VGA Terminal");

    /* WM_DELETE_WINDOW so we can intercept the close button */
    vt->wm_del = XInternAtom(vt->dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(vt->dpy, vt->win, &vt->wm_del, 1);

    /* GC */
    vt->gc = XCreateGC(vt->dpy, vt->win, 0, NULL);

    /* XImage wrapping our pixel buffer */
    vt->img = XCreateImage(
        vt->dpy, vt->vis, vt->depth,
        ZPixmap, 0,
        (char *)vt->pixels,
        vt->current_px_w, vt->current_px_h,
        32,          /* bitmap_pad */
        vt->current_px_w * 4 /* bytes_per_line */
    );
    if (!vt->img) {
        fprintf(stderr, "vgaterm: XCreateImage failed\n");
        XDestroyWindow(vt->dpy, vt->win);
        XCloseDisplay(vt->dpy);
        free(vt->pixels); free(vt); return NULL;
    }

    XMapWindow(vt->dpy, vt->win);
    XFlush(vt->dpy);

    return vt;
}

/* =========================================================================
 * Setup scaling
 * =========================================================================
 * Configures the window for 1x, 2x, or 4x scaling.
 * Must be called after vgaterm_open() and before rendering begins.
 * When scaling is changed, the window is resized and the pixel buffer is
 * reallocated. To minimize artifacts, change scaling before writing to the
 * text buffer or call vgaterm_cls() after scaling to clear the display.
 *
 * Parameters:
 *   scale_factor  -- 1, 2, or 4 for 1x, 2x, or 4x scaling
 * Returns 0 on success, -1 on failure (e.g., invalid scale factor).
 */
int vgaterm_setup_scaling(VGATerm *vt, int scale_factor)
{
    int new_width, new_height;
    uint32_t *new_pixels;
    XImage *new_img;
    XSizeHints hints;

    if (!vt || vt->scale_factor == 0) return -1;

    /* Validate scale factor */
    if (scale_factor != 1 && scale_factor != 2 && scale_factor != 4) {
        fprintf(stderr, "vgaterm_setup_scaling: invalid scale factor %d (must be 1, 2, or 4)\n",
                scale_factor);
        return -1;
    }

    /* Calculate new dimensions */
    new_width  = VGA_PX_W * scale_factor;
    new_height = VGA_PX_H * scale_factor;

    /* If scaling hasn't actually changed, no-op */
    if (scale_factor == vt->scale_factor) {
        return 0;
    }

    /* Allocate new pixel buffer */
    new_pixels = (uint32_t *)malloc(new_width * new_height * sizeof(uint32_t));
    if (!new_pixels) {
        fprintf(stderr, "vgaterm_setup_scaling: failed to allocate pixel buffer\n");
        return -1;
    }
    memset(new_pixels, 0, new_width * new_height * sizeof(uint32_t));

    /* Create new XImage with new dimensions */
    new_img = XCreateImage(
        vt->dpy, vt->vis, vt->depth,
        ZPixmap, 0,
        (char *)new_pixels,
        new_width, new_height,
        32,          /* bitmap_pad */
        new_width * 4 /* bytes_per_line */
    );
    if (!new_img) {
        fprintf(stderr, "vgaterm_setup_scaling: XCreateImage failed\n");
        free(new_pixels);
        return -1;
    }

    /* Destroy old image (but don't free pixels, we'll do that ourselves) */
    vt->img->data = NULL;
    XDestroyImage(vt->img);
    free(vt->pixels);

    /* Update VGATerm state */
    vt->pixels       = new_pixels;
    vt->img          = new_img;
    vt->scale_factor = scale_factor;
    vt->current_px_w = new_width;
    vt->current_px_h = new_height;

    /* Resize the X11 window */
    XResizeWindow(vt->dpy, vt->win, new_width, new_height);

    /* Update size hints to lock new dimensions */
    memset(&hints, 0, sizeof(hints));
    hints.flags      = PMinSize | PMaxSize | PBaseSize;
    hints.min_width  = hints.max_width  = hints.base_width  = new_width;
    hints.min_height = hints.max_height = hints.base_height = new_height;
    XSetWMNormalHints(vt->dpy, vt->win, &hints);

    XFlush(vt->dpy);

    return 0;
}

void vgaterm_close(VGATerm *vt)
{
    if (!vt) return;
    /* Don't call XDestroyImage: it would try to free our pixels */
    vt->img->data = NULL;
    XDestroyImage(vt->img);
    XFreeGC(vt->dpy, vt->gc);
    XDestroyWindow(vt->dpy, vt->win);
    XCloseDisplay(vt->dpy);
    free(vt->pixels);
    free(vt);
}

uint8_t *vgaterm_mem(VGATerm *vt)
{
    return vt ? vt->mem : NULL;
}

void vgaterm_blit(VGATerm *vt)
{
    int draw_cur;
    if (!vt || !vt->alive) return;
    draw_cur = cursor_blink_on(vt);
    render_all(vt, draw_cur);
    XPutImage(vt->dpy, vt->win, vt->gc, vt->img,
              0, 0, 0, 0, vt->current_px_w, vt->current_px_h);
    XFlush(vt->dpy);
    vt->cur_visible = draw_cur;
}

int vgaterm_events(VGATerm *vt)
{
    XEvent ev;
    if (!vt) return 0;
    while (XPending(vt->dpy)) {
        XNextEvent(vt->dpy, &ev);
        switch (ev.type) {
        case Expose:
            if (ev.xexpose.count == 0) {
                /* Re-blit after expose without re-rendering */
                XPutImage(vt->dpy, vt->win, vt->gc, vt->img,
                          0, 0, 0, 0, vt->current_px_w, vt->current_px_h);
                XFlush(vt->dpy);
            }
            break;
        case ClientMessage:
            if ((Atom)ev.xclient.data.l[0] == vt->wm_del) {
                vt->alive = 0;
                return 0;
            }
            break;
        default:
            break;
        }
    }
    return vt->alive;
}

void vgaterm_set_cursor(VGATerm *vt, int col, int row)
{
    if (!vt) return;
    vt->cur_col = col;
    vt->cur_row = row;
    clock_gettime(CLOCK_MONOTONIC, &vt->cur_time);
}

int vgaterm_fd(VGATerm *vt)
{
    return vt ? ConnectionNumber(vt->dpy) : -1;
}

/* -------------------------------------------------------------------------
 * Convenience helpers
 * ------------------------------------------------------------------------- */

void vgaterm_putc(VGATerm *vt, int col, int row, uint8_t ch, uint8_t attr)
{
    int off;
    if (!vt || col < 0 || col >= VGA_COLS || row < 0 || row >= VGA_ROWS) return;
    off = (row * VGA_COLS + col) * 2;
    vt->mem[off]     = ch;
    vt->mem[off + 1] = attr;
}

void vgaterm_puts(VGATerm *vt, int col, int row, const char *s, uint8_t attr)
{
    if (!vt || !s) return;
    while (*s && col < VGA_COLS) {
        vgaterm_putc(vt, col++, row, (uint8_t)*s++, attr);
    }
}

void vgaterm_cls(VGATerm *vt, uint8_t attr)
{
    int i;
    if (!vt) return;
    for (i = 0; i < VGA_COLS * VGA_ROWS; i++) {
        vt->mem[i * 2]     = 0x20; /* space */
        vt->mem[i * 2 + 1] = attr;
    }
}

void vgaterm_scroll(VGATerm *vt, int n, uint8_t attr)
{
    int i;
    int row_bytes;
    if (!vt || n <= 0) return;
    if (n >= VGA_ROWS) { vgaterm_cls(vt, attr); return; }
    row_bytes = VGA_COLS * 2;
    memmove(vt->mem, vt->mem + n * row_bytes,
            (VGA_ROWS - n) * row_bytes);
    /* blank the scrolled-in lines */
    for (i = (VGA_ROWS - n) * VGA_COLS; i < VGA_ROWS * VGA_COLS; i++) {
        vt->mem[i * 2]     = 0x20;
        vt->mem[i * 2 + 1] = attr;
    }
}

/* ---------- X11 accessors ----------------------------------------- */
Display *vgaterm_display(VGATerm *vt) { return vt ? vt->dpy : NULL; }
Window   vgaterm_window (VGATerm *vt) { return vt ? vt->win : 0; }

/* -------------------------------------------------------------------------
 * Character generator table API  (4 slots, matching real VGA plane 2)
 * ------------------------------------------------------------------------- */

/* Set one of the four font slots (slot 0-3).
 * NULL restores the slot to the compiled-in vga_font_8x16 fallback.
 * The pointer is stored, not copied; modifications to the pointed-at
 * array are reflected on the very next vgaterm_blit() call.            */
void vgaterm_set_font_slot(VGATerm *vt, int slot,
                            const unsigned char (*font)[16])
{
    if (vt && slot >= 0 && slot <= 3)
        vt->font_table[slot] = font;
}

/* Backward-compatible helpers */
void vgaterm_set_font  (VGATerm *vt, const unsigned char (*font)[16])
{ vgaterm_set_font_slot(vt, 0, font); }
void vgaterm_set_font_a(VGATerm *vt, const unsigned char (*font)[16])
{ vgaterm_set_font_slot(vt, 0, font); }
void vgaterm_set_font_b(VGATerm *vt, const unsigned char (*font)[16])
{ vgaterm_set_font_slot(vt, 1, font); }

/* Returns a pointer to the raw per-cell font-select array
 * [VGA_ROWS * VGA_COLS].  Index as [row * VGA_COLS + col].
 * Values 0-3 select the corresponding font_table slot.
 * Write directly; changes take effect on the next vgaterm_blit().      */
uint8_t *vgaterm_fplane(VGATerm *vt)
{
    return vt ? vt->fplane : NULL;
}

uint8_t *vgaterm_uplane(VGATerm *vt)
{
    return vt ? vt->uplane : NULL;
}

/* Fill a rectangle of cells with the given slot value (0-3).           */
void vgaterm_set_fplane_rect(VGATerm *vt,
                              int col, int row, int w, int h,
                              uint8_t slot)
{
    int r, c;
    if (!vt) return;
    if (col < 0) { w += col; col = 0; }
    if (row < 0) { h += row; row = 0; }
    if (col + w > VGA_COLS) w = VGA_COLS - col;
    if (row + h > VGA_ROWS) h = VGA_ROWS - row;
    if (w <= 0 || h <= 0) return;
    slot &= 0x03;
    for (r = row; r < row + h; r++)
        for (c = col; c < col + w; c++)
            vt->fplane[r * VGA_COLS + c] = slot;
}
