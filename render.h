/* SPDX-License-Identifier: MIT
 * render.h  --  screen drawing
 */

#ifndef RENDER_H
#define RENDER_H

#include "editor.h"

/* Redraw the whole screen (text area, gutter, status/console row,
 * cursor) and flush to the window.  Called once per main-loop
 * iteration; the buffer is small enough that a full redraw is cheap
 * and avoids partial-update bugs around scrolling. */
void render_full(Editor *ed);

#endif /* RENDER_H */
