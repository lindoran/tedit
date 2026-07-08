/* SPDX-License-Identifier: MIT
 * console.c  --  Esc-activated command console
 */

#include "console.h"
#include "enc.h"
#include "palette.h"
#include "charmap.h"
#include "spell.h"
#include "vio.h"
#include "hl.h"

#include <stdlib.h>
#include <string.h>

void console_handle_key(Editor *ed, int key)
{
    switch (key) {

    case KEY_ESC:
        /* Close the console.  Leave the status message intact so the user
         * can still read the result of whatever ran before they opened it. */
        ed->console_active = 0;
        ed->console_len    = 0;
        ed->console_buf[0] = '\0';
        break;

    case KEY_ENTER:
        ed->console_active = 0;
        console_execute(ed, ed->console_buf);
        ed->console_len    = 0;
        ed->console_buf[0] = '\0';
        break;

    case KEY_BS:
        if (ed->console_len > 0)
            ed->console_buf[--ed->console_len] = '\0';
        break;

    default:
        if (((key >= 0x20 && key < 0x7F) || (key >= 0x80 && key <= 0xFF)) &&
            ed->console_len < CONSOLE_BUFSZ - 1) {
            ed->console_buf[ed->console_len++] = (char)key;
            ed->console_buf[ed->console_len]   = '\0';
        }
        break;
    }
}

void console_execute(Editor *ed, const char *cmdline)
{
    char   buf[CONSOLE_BUFSZ];
    char  *p, *cmd, *arg;
    int    force = 0;
    size_t cl;

    strncpy(buf, cmdline, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    p = buf;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '\0') { ed->message[0] = '\0'; return; }

    cmd = strtok(p, " \t");
    arg = strtok(NULL, "");
    if (arg) {
        while (*arg == ' ' || *arg == '\t') arg++;
        if (*arg == '\0') arg = NULL;
    }

    cl = strlen(cmd);
    if (cl > 0 && cmd[cl - 1] == '!') { force = 1; cmd[--cl] = '\0'; }

    /* ---- "save as <file>" convenience --------------------------------- */
    if (arg && strncmp(arg, "as ", 3) == 0 &&
        (!strcmp(cmd, "s") || !strcmp(cmd, "save") ||
         !strcmp(cmd, "w") || !strcmp(cmd, "write"))) {
        arg += 3;
        while (*arg == ' ') arg++;
        if (*arg == '\0') arg = NULL;
    }

    /* ---- dispatch ----------------------------------------------------- */

    if (!strcmp(cmd, "s") || !strcmp(cmd, "save") ||
        !strcmp(cmd, "w") || !strcmp(cmd, "write")) {

        ed_save(ed, arg);

    } else if (!strcmp(cmd, "o") || !strcmp(cmd, "open") ||
               !strcmp(cmd, "load") || !strcmp(cmd, "e") ||
               !strcmp(cmd, "edit")) {

        if (!arg) {
            ed_set_message(ed, "Usage: open <filename>  (open! to discard)");
        } else if (ed->modified && !force) {
            ed_set_message(ed, "Unsaved changes -- use open! to discard");
        } else {
            ed_open(ed, arg);
        }

    } else if (!strcmp(cmd, "new")) {

        if (ed->modified && !force) {
            ed_set_message(ed, "Unsaved changes -- use new! to discard");
        } else {
            tb_clear(&ed->tb);
            ed->filename[0] = '\0';
            ed->cur_row = ed->cur_col = 0;
            ed->top_row = ed->left_col = 0;
            ed->modified = 0;
            ed->ending   = LE_UNIX;
            ed->enc      = ENC_UTF8;
            ed_set_message(ed, "[New file]");
        }

    } else if (!strcmp(cmd, "wq") || !strcmp(cmd, "x")) {

        if (ed_save(ed, arg) == 0) ed->quit = 1;

    } else if (!strcmp(cmd, "q") || !strcmp(cmd, "quit") ||
               !strcmp(cmd, "exit")) {

        if (ed->modified && !force)
            ed_set_message(ed,
                "Unsaved changes -- use quit! to discard");
        else
            ed->quit = 1;

    } else if (!strcmp(cmd, "tab") || !strcmp(cmd, "ts") ||
               !strcmp(cmd, "tabsize")) {

        if (!arg) {
            ed_set_message(ed, "Tab size is %d", ed->tabsize);
        } else {
            int n = atoi(arg);
            if (n < 1)          n = 1;
            if (n > MAX_TABSIZE) n = MAX_TABSIZE;
            ed->tabsize = n;
            ed_set_message(ed, "Tab size set to %d", n);
        }

    } else if (!strcmp(cmd, "coalesce") || !strcmp(cmd, "undo_coalesce")) {

        if (!arg) {
            ed_set_message(ed, "Coalesce timeout is %d ms", ed->coalesce_ms);
        } else {
            int n = atoi(arg);
            if (n < 0) n = 0;
            if (n > 60000) n = 60000;
            ed->coalesce_ms = n;
            ed_set_message(ed, "Coalesce timeout set to %d ms", n);
        }

    } else if (!strcmp(cmd, "numbers") || !strcmp(cmd, "ln") ||
               !strcmp(cmd, "nu")      || !strcmp(cmd, "linenum")) {

        ed->show_linenum = !ed->show_linenum;
        ed_set_message(ed, "Line numbers %s",
                        ed->show_linenum ? "on" : "off");

    } else if (!strcmp(cmd, "tabs") || !strcmp(cmd, "hardtabs")) {

        ed->use_hard_tabs = 1;
        ed_set_message(ed, "Tab mode: hard tabs (\\t character, shown as »)");

    } else if (!strcmp(cmd, "spaces") || !strcmp(cmd, "softtabs")) {

        ed->use_hard_tabs = 0;
        ed_set_message(ed, "Tab mode: soft tabs (spaces)");

    } else if (!strcmp(cmd, "lf") || !strcmp(cmd, "unix")) {

        ed->ending = LE_UNIX;
        ed_set_message(ed, "Line endings: Unix (LF)");

    } else if (!strcmp(cmd, "crlf") || !strcmp(cmd, "dos")) {

        ed->ending = LE_DOS;
        ed_set_message(ed, "Line endings: DOS (CRLF)");

    } else if (!strcmp(cmd, "enc") || !strcmp(cmd, "encoding")) {

        if (!arg) {
            ed_set_message(ed, "Encoding: %s",
                            ed->enc == ENC_UTF8  ? "UTF-8"         :
                            ed->enc == ENC_ASCII ? "ASCII (7-bit)" : "CP437");
        } else if (!strcmp(arg, "utf8")    || !strcmp(arg, "utf-8") ||
                   !strcmp(arg, "unicode")) {
            ed->enc = ENC_UTF8;
            ed_set_message(ed, "Encoding: UTF-8");
        } else if (!strcmp(arg, "cp437") || !strcmp(arg, "cp-437")) {
            ed->enc = ENC_CP437;
            ed_set_message(ed, "Encoding: CP437");
        } else if (!strcmp(arg, "ascii") || !strcmp(arg, "7bit") ||
                   !strcmp(arg, "7-bit")) {
            ed->enc = ENC_ASCII;
            ed_set_message(ed, "Encoding: ASCII (7-bit)");
        } else {
            ed_set_message(ed,
                "Unknown encoding \"%s\"  --  try: utf8  cp437  ascii", arg);
        }

    } else if (!strcmp(cmd, "syntax")) {

        if (!arg) {
            if (ed->hl) {
                ed_set_message(ed, "Syntax: %s", ed->hl->name);
            } else {
                ed_set_message(ed, "Syntax: off");
            }
        } else if (!strcmp(arg, "off")) {
            ed->hl = NULL;
            ed_set_message(ed, "Syntax highlighting off");
        } else if (strchr(arg, '/') || strchr(arg, '\\')) {
            /* Explicit path: load directly, no search. */
            int slot = ed_load_lang_file(ed, arg);
            if (slot >= 0) {
                ed_set_message(ed, "Loaded syntax \"%s\"", ed->hl_langs[slot].name);
                ed->hl = hl_match_ext(ed->hl_langs, ed->n_hl_langs, ed->filename);
                if (ed->hl) ed->hl_dirty_row = 0;
            } else {
                ed_set_message(ed, "Failed to load syntax \"%s\"", arg);
            }
        } else if (strstr(arg, ".csv")) {
            /* Bare filename: search system dir, ~/.tedit/lang, ./lang. */
            int slot = ed_load_lang_search(ed, arg);
            if (slot >= 0) {
                ed_set_message(ed, "Loaded syntax \"%s\"", ed->hl_langs[slot].name);
                ed->hl = hl_match_ext(ed->hl_langs, ed->n_hl_langs, ed->filename);
                if (ed->hl) ed->hl_dirty_row = 0;
            } else {
                ed_set_message(ed, "Syntax file \"%s\" not found", arg);
            }
        } else {
            int idx;
            int found = 0;
            for (idx = 0; idx < ed->n_hl_langs; idx++) {
                if (strcmp(ed->hl_langs[idx].name, arg) == 0) {
                    ed->hl = &ed->hl_langs[idx];
                    ed->hl_dirty_row = 0;
                    ed_set_message(ed, "Syntax set to: %s", ed->hl->name);
                    found = 1;
                    break;
                }
            }
            if (!found) {
                ed_set_message(ed, "Syntax \"%s\" not loaded or found", arg);
            }
        }

    } else if (!strcmp(cmd, "help") || !strcmp(cmd, "?")) {

        palette_open(ed);   /* open command palette instead of text help */
        return;             /* palette_open sets console_active; don't clear */

    } else if (!strcmp(cmd, "spell") || !strcmp(cmd, "spellcheck")) {

        /* "spell"         -- check all text
         * "spell code"    -- check only strings and comments */
        if (arg && (!strcmp(arg, "code") || !strcmp(arg, "src"))) {
            spell_start(ed, SPELL_TEXT_ONLY);
        } else {
            spell_start(ed, SPELL_ALL);
        }
        return;  /* spell_start sets message; don't overwrite it */

    } else if (!strcmp(cmd, "charmap") || !strcmp(cmd, "chars")) {

        charmap_open(ed);
        return;  /* dialog takes over; no message needed */

    } else if (!strcmp(cmd, "scale") || !strcmp(cmd, "zoom") ||
               !strcmp(cmd, "sz")) {

        if (!arg) {
            ed_set_message(ed, "Scale: %dx  (use: scale 1 | 2 | 4)", vio_get_scale());
        } else {
            int n = atoi(arg);
            if (n != 1 && n != 2 && n != 4) {
                ed_set_message(ed, "Scale must be 1, 2, or 4");
            } else if (vio_set_scale(n) == 0) {
                ed_set_message(ed, "Scale: %dx", n);
            } else {
                ed_set_message(ed, "Failed to set scale %d", n);
            }
        }

    } else {
        ed_set_message(ed, "Unknown: \"%s\"  --  type help for commands", cmd);
    }

    /* ---- find / replace commands ------------------------------------ */
    if (!strcmp(cmd, "find")) {
        if (!arg) {
            ed_set_message(ed, "Usage: find <text>");
        } else {
            ed_find(ed, arg);
        }
    } else if (!strcmp(cmd, "next") || !strcmp(cmd, "find-next")) {
        ed_find_next(ed);
    } else if (!strcmp(cmd, "replace")) {
        if (!arg) {
            ed_set_message(ed, "Usage: replace <old> <new>");
        } else {
            char *old = strtok(arg, " \t");
            char *newp = strtok(NULL, "");
            if (!old || !newp) {
                ed_set_message(ed, "Usage: replace <old> <new>");
            } else {
                /* If old matches the last-find, replace current match */
                /* If arg exactly provided as two parts, perform replace-all? */
                /* We implement replace to replace current match when possible,
                 * otherwise do a single find & replace of the next occurrence. */
                if (ed->last_find[0] && strcmp(old, ed->last_find) == 0) {
                    ed_replace_current(ed, newp);
                } else {
                    /* find the next occurrence of old and replace it */
                    if (ed_find(ed, old))
                        ed_replace_current(ed, newp);
                }
            }
        }
    } else if (!strcmp(cmd, "replace-all") || !strcmp(cmd, "rall")) {
        if (!arg) {
            ed_set_message(ed, "Usage: replace-all <old> <new>");
        } else {
            char *old = strtok(arg, " \t");
            char *newp = strtok(NULL, "");
            if (!old || !newp) {
                ed_set_message(ed, "Usage: replace-all <old> <new>");
            } else {
                ed_replace_all(ed, old, newp);
            }
        }
    }

    ed_clamp_and_scroll(ed);
}
