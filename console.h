/* SPDX-License-Identifier: MIT
 * console.h  --  Esc-activated command console
 *
 * The console occupies the same screen row as the status/HUD bar.
 * Pressing Esc from the editor opens it; pressing Esc again (with an
 * empty prompt) or Enter closes it.
 *
 * Commands (whitespace separated, optional trailing '!' forces an
 * action that would otherwise be blocked by unsaved changes):
 *
 *   s | save | w  [file]     save (uses current filename if omitted)
 *   o | open | load [file]   open file, replacing the buffer
 *   wq | x [file]            save then quit
 *   q | quit                 quit
 *   tab | ts <n>              set tab size (1..16)
 *   ln | linenum              toggle the line-number gutter
 *   lf | unix                 use Unix (LF) line endings on save
 *   crlf | dos                use DOS (CRLF) line endings on save
 *   ? | help                  show a short command summary
 */

#ifndef CONSOLE_H
#define CONSOLE_H

#include "editor.h"

/* Handle one key while the console has focus. */
void console_handle_key(Editor *ed, int key);

/* Parse and run a command line (without leading ':' or trailing
 * newline).  Sets ed->message with the result. */
void console_execute(Editor *ed, const char *cmdline);

#endif /* CONSOLE_H */
