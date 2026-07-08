/* SPDX-License-Identifier: MIT
 * main.c  --  tedit: a minimal CP437/VGA text editor
 *
 * Usage:  tedit [--scale N] [file]
 *         tedit [-s N] [file]
 *
 * Keys (editor):
 *   Arrows / PgUp / PgDn   move cursor
 *   Home / End             top / bottom of document
 *   Tab / Shift+Tab        indent / unindent
 *   Enter                  new line (auto-indent)
 *   Backspace / Delete     delete left / right
 *   Shift+Arrow            select text
 *   Ctrl+C                 copy selection
 *   Ctrl+X                 cut  selection
 *   Ctrl+V                 paste
 *   Insert                 toggle insert / overwrite
 *   Esc                    clear selection / status message
 *   Ctrl+Q                 quit immediately
 *   Ctrl+1 / Ctrl+2 / Ctrl+4   set display scale 1x / 2x / 4x
 *
 * Console (after Esc):
 *   help / ?               open the command palette
 *   new  /  new!           new empty buffer
 *   open <file>            load a file  (open! to discard changes)
 *   save [file]            save  (optionally with new name)
 *   save as <file>         save with a new name
 *   quit  /  quit!         quit
 *   tab <n>                set tab size
 *   numbers                toggle line numbers
 *   unix  /  dos           set line-ending mode
 *   scale <n>              set display scale (1, 2, or 4)
 *
 * Palette (after typing "help" + Enter in the console):
 *   Up / Down              move selection
 *   type to filter         narrows the list
 *   Enter                  fills the console with the selected command
 *   Esc                    cancel, return to editing
 */

#include "vgaterm.h"
#include "vio.h"
#include "editor.h"
#include "console.h"
#include "palette.h"
#include "charmap.h"
#include "render.h"
#include "spell.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static void update_title(Editor *ed, char *last, size_t sz)
{
    char title[320];
    snprintf(title, sizeof(title), "tedit - %s%s",
             ed->filename[0] ? ed->filename : "[No Name]",
             ed->modified ? " [+]" : "");
    if (strcmp(title, last) != 0) {
        vio_set_title(title);
        strncpy(last, title, sz - 1);
        last[sz - 1] = '\0';
    }
}

int main(int argc, char **argv)
{
    Editor  ed;
    VGATerm *vt;
    char    last_title[320] = "";
    int     k;
    int     init_scale = 1;   /* default: 1x */
    int     argi;

    /* ---- command-line parsing ---- */
    for (argi = 1; argi < argc; argi++) {
        if ((strcmp(argv[argi], "--scale") == 0 ||
             strcmp(argv[argi], "-s")      == 0 ||
             strcmp(argv[argi], "-scale")  == 0) && argi + 1 < argc) {
            init_scale = atoi(argv[++argi]);
            if (init_scale != 1 && init_scale != 2 && init_scale != 4) {
                fprintf(stderr, "tedit: invalid scale %d (use 1, 2, or 4)\n",
                        init_scale);
                return 1;
            }
        } else if (strcmp(argv[argi], "--") == 0) {
            argi++;
            break;
        } else if (argv[argi][0] != '-') {
            break;   /* first non-option arg = filename */
        } else {
            fprintf(stderr, "tedit: unknown option: %s\n", argv[argi]);
            fprintf(stderr, "Usage: tedit [--scale 1|2|4] [file]\n");
            return 1;
        }
    }

    ed_init(&ed);
    if (argi < argc)
        ed_open(&ed, argv[argi]);

    vt = vgaterm_open("tedit");
    if (!vt) { ed_free(&ed); return 1; }

    if (init_scale != 1)
        vgaterm_setup_scaling(vt, init_scale);

    vio_init(vt);
    /* keep vio's scale tracker in sync with whatever was set above */
    if (init_scale != 1)
        vio_set_scale(init_scale);   /* sync the vio-internal counter */
    vio_setattr(A_TEXT);
    vio_clrscr();

    update_title(&ed, last_title, sizeof(last_title));
    render_full(&ed);

    while (!ed.quit) {
        k = vio_getch();
        if (k == KEY_CLOSED) {
            /* Window close (X): require confirmation if modified. */
            if (ed.modified && !ed.pending_quit) {
                ed.pending_quit = 1;
                ed_set_message(&ed, "Unsaved changes, close again to force quit!");
                /* Render the warning immediately so the user sees it. */
                update_title(&ed, last_title, sizeof(last_title));
                render_full(&ed);
                continue;
            }
            break;
        }

        /* Any activity inside the window cancels a pending quit
         * (except the actual close event handled above).  Do NOT clear
         * pending_quit if the user is pressing Ctrl+Q — that key is used
         * to confirm/force quit and must be delivered with the state
         * intact. */
        if (k != KEY_CTRL('q') && k != KEY_CLOSED && ed.pending_quit) {
            ed.pending_quit = 0;
            ed.message[0] = '\0';
        }

        if (k == KEY_PASTE_READY) {
            /* Paste data arrives asynchronously; always route to the editor. */
            ed_handle_key(&ed, k);
        } else if (k == KEY_SCROLL_UP || k == KEY_SCROLL_DOWN) {
            /* Route scroll to whichever overlay is active, else editor. */
            if (ed.palette_active)
                palette_handle_key(&ed, k);
            else if (ed.spell_active)
                spell_handle_key(&ed, k);
            else
                ed_handle_key(&ed, k);
        } else if (k == KEY_SCALE_1 || k == KEY_SCALE_2 || k == KEY_SCALE_4) {
            /* Ctrl+1/2/4: set display scale -- always active, any mode. */
            int sc = (k == KEY_SCALE_1) ? 1 : (k == KEY_SCALE_2) ? 2 : 4;
            if (vio_set_scale(sc) == 0)
                ed_set_message(&ed, "Scale: %dx", sc);
            else
                ed_set_message(&ed, "Scale %dx failed", sc);
        } else if (k == KEY_MOUSE_PRESS || k == KEY_MOUSE_MOVE ||
                   k == KEY_MOUSE_RELEASE) {
            /* Route mouse to whichever overlay is active, else editor. */
            if (ed.charmap_active)
                charmap_handle_key(&ed, k);
            else if (ed.palette_active)
                palette_handle_key(&ed, k);
            else if (ed.spell_active)
                spell_handle_key(&ed, k);
            else if (ed.console_active) {
                /* click on the status bar while console is open = Enter */
                if (k == KEY_MOUSE_PRESS && vio_mouse.row == STATUS_ROW)
                    console_handle_key(&ed, KEY_ENTER);
            } else
                ed_handle_key(&ed, k);
        } else if (k == KEY_MOUSE_RIGHT) {
            /* right-click: open command palette when no dialog is open */
            if (!ed.charmap_active && !ed.palette_active &&
                !ed.spell_active   && !ed.console_active)
                palette_open(&ed);
        } else if (ed.charmap_active)
            charmap_handle_key(&ed, k);
        else if (ed.spell_active)
            spell_handle_key(&ed, k);
        else if (ed.palette_active)
            palette_handle_key(&ed, k);
        else if (ed.console_active)
            console_handle_key(&ed, k);
        else
            ed_handle_key(&ed, k);

        update_title(&ed, last_title, sizeof(last_title));
        render_full(&ed);
    }

    vio_fini();
    vgaterm_close(vt);
    ed_free(&ed);
    return 0;
}
