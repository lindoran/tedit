/* SPDX-License-Identifier: MIT
 * spell.c  --  ispell/aspell/hunspell integration for tedit
 *
 * Protocol (ispell -a pipe mode)
 * --------------------------------
 * We write one word per line, prefixed with '^' to suppress any magic
 * interpretation of special leading chars.  ispell responds with:
 *   *          -- correctly spelled
 *   + root     -- correct via suffix stripping from "root"
 *   & w n o: s1, s2, ...  -- misspelled; n suggestions at offset o
 *   # w o      -- misspelled; no suggestions
 * Each response is followed by a blank line.  We read until the blank.
 *
 * Session commands:
 *   @word\n   -- accept word for this session (ignore)
 *   *word\n   -- add word to personal dictionary
 *   #\n       -- save personal dictionary
 *   Q\n       -- quit ispell cleanly
 */

#define _POSIX_C_SOURCE 200809L

#include "spell.h"
#include "editor.h"
#include "hl.h"
#include "vio.h"

#include <ctype.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/* Intentional fire-and-forget write to the ispell pipe.  Assigning the
 * return value to a local and casting it to void is the only portable way
 * to silence -Wunused-result on write(); a bare (void) cast is not enough
 * on GCC >= 10 when the function carries __attribute__((warn_unused_result)).
 * If the pipe is broken the next read will catch it and we abort then. */
#define PIPE_WRITE(fd, buf, n) \
    do { ssize_t _pw = write((fd), (buf), (size_t)(n)); (void)_pw; } while (0)

/* -----------------------------------------------------------------------
 * Helpers
 * ----------------------------------------------------------------------- */

static int is_word_char(unsigned char c)
{
    /* Only plain ASCII letters and the apostrophe (for contractions).
     * CP437 high bytes are non-English glyphs; skip them. */
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '\'';
}

/* Write `s` left-aligned into `w` columns, space-padded, no NUL written. */
static void spl_field(const char *s, int w)
{
    int n = (int)strlen(s);
    if (n > w) n = w;
    vio_puts_n(s, n);
    int pad = w - n;
    while (pad-- > 0) vio_putch(' ');
}

/* -----------------------------------------------------------------------
 * ispell subprocess
 * ----------------------------------------------------------------------- */

/* Try to exec `prog -a` as a child.  On success, fills ed->spell_{pid,wfd,rfp}
 * and returns 0.  Returns -1 on any failure. */
static int try_spawn(Editor *ed, const char *prog)
{
    int to_child[2], from_child[2];

    if (pipe(to_child) < 0)   return -1;
    if (pipe(from_child) < 0) { close(to_child[0]); close(to_child[1]); return -1; }

    pid_t pid = fork();
    if (pid < 0) {
        close(to_child[0]);  close(to_child[1]);
        close(from_child[0]); close(from_child[1]);
        return -1;
    }

    if (pid == 0) {
        /* child */
        dup2(to_child[0],   STDIN_FILENO);
        dup2(from_child[1], STDOUT_FILENO);
        close(to_child[0]);  close(to_child[1]);
        close(from_child[0]); close(from_child[1]);
        execlp(prog, prog, "-a", (char *)NULL);
        _exit(127);
    }

    /* parent */
    close(to_child[0]);
    close(from_child[1]);

    FILE *rfp = fdopen(from_child[0], "r");
    if (!rfp) {
        close(to_child[1]);
        close(from_child[0]);
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        return -1;
    }

    /* Expect the banner: "@(#) International Ispell ..." or equivalent */
    char line[512];
    if (!fgets(line, sizeof(line), rfp) || line[0] != '@') {
        fclose(rfp);
        close(to_child[1]);
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        return -1;
    }

    ed->spell_pid = (int)pid;
    ed->spell_wfd = to_child[1];
    ed->spell_rfp = rfp;
    return 0;
}

static int ispell_spawn(Editor *ed)
{
    if (try_spawn(ed, "ispell")   == 0) return 0;
    if (try_spawn(ed, "aspell")   == 0) return 0;
    if (try_spawn(ed, "hunspell") == 0) return 0;
    return -1;
}

/* Check one word.  Returns:
 *   0  = correct
 *   1  = misspelled (ed->spell_sug[] / ed->spell_nsug filled)
 *  -1  = I/O error
 */
static int ispell_check_word(Editor *ed, const char *word)
{
    /* Prefix with ^ so ispell never interprets the first char as a command. */
    char buf[64 + 4];
    int  n = snprintf(buf, sizeof(buf), "^%s\n", word);
    if (write(ed->spell_wfd, buf, (size_t)n) != n) return -1;

    ed->spell_nsug = 0;
    int misspelled = 0;
    char line[1024];

    while (fgets(line, sizeof(line), ed->spell_rfp)) {
        /* strip trailing CR/LF */
        int len = (int)strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'))
            line[--len] = '\0';

        if (len == 0) break;   /* blank line = end of this word's response */

        if (line[0] == '*' || line[0] == '+') {
            misspelled = 0;    /* correct */
        } else if (line[0] == '&') {
            /* & word count offset: sug1, sug2, ...
             * Everything after the colon+space is a comma-separated list. */
            misspelled = 1;
            char *colon = strchr(line, ':');
            if (colon) {
                char *p = colon + 2;  /* skip ": " */
                int ns = 0;
                while (*p && ns < SPELL_MAX_SUG) {
                    char *comma = strstr(p, ", ");
                    int   wlen  = comma ? (int)(comma - p) : (int)strlen(p);
                    if (wlen > 0 && wlen < 64) {
                        memcpy(ed->spell_sug[ns], p, (size_t)wlen);
                        ed->spell_sug[ns][wlen] = '\0';
                        ns++;
                    }
                    if (!comma) break;
                    p = comma + 2;
                }
                ed->spell_nsug = ns;
            }
        } else if (line[0] == '#') {
            misspelled = 1;    /* no suggestions */
            ed->spell_nsug = 0;
        }
        /* '@' informational lines: silently ignored */
    }
    return misspelled;
}

/* Tell ispell to accept the current word for this session. */
static void ispell_accept(Editor *ed)
{
    char buf[64 + 4];
    int  n = snprintf(buf, sizeof(buf), "@%s\n", ed->spell_word);
    PIPE_WRITE(ed->spell_wfd, buf, n);
}

/* Add the current word to the personal dictionary and save it. */
static void ispell_add_word(Editor *ed)
{
    char buf[64 + 4];
    int  n = snprintf(buf, sizeof(buf), "*%s\n#\n", ed->spell_word);
    PIPE_WRITE(ed->spell_wfd, buf, n);
    /* '#' has no pipe response, so nothing to drain */
}

/* -----------------------------------------------------------------------
 * Token classification  (one HLTok per character on a line)
 * ----------------------------------------------------------------------- */

/* Fills toks[0..len-1] with the HLTok for each character.
 * When no language is loaded, every character gets TOK_TEXT. */
static void classify_line_toks(const Editor *ed, int row,
                                HLTok *toks, int len)
{
    if (!ed->hl || len <= 0) {
        int i;
        for (i = 0; i < len; i++) toks[i] = TOK_TEXT;
        return;
    }
    HLCursor hlc;
    hl_begin(&hlc, ed->hl, ed->tb.hl_states[row]);
    const Line *l = &ed->tb.lines[row];
    int i;
    for (i = 0; i < len; i++) {
        hl_step(&hlc, l->data, l->len, i);
        toks[i] = hl_tok(&hlc);
    }
}

/* -----------------------------------------------------------------------
 * Viewport adjustment
 *
 * The spell dialog is centred on screen, covering rows SPL_ROW ..
 * SPL_ROW+SPL_H-1.  If the misspelled word would land behind the dialog
 * after a normal ed_clamp_and_scroll(), adjust top_row so the word
 * appears in the visible strip above (preferred) or below the dialog.
 * ----------------------------------------------------------------------- */

static void spell_ensure_visible(Editor *ed)
{
    int scr = ed->cur_row - ed->top_row;   /* current screen row of word */

    if (scr >= SPL_ROW && scr < SPL_ROW + SPL_H) {
        /* Word is hidden.  Try to show it a few rows above the dialog. */
        int target = SPL_ROW - 3;
        if (target < 0) target = 0;
        int new_top = ed->cur_row - target;

        if (new_top < 0) {
            /* Word is too near the top of the buffer; put it below instead. */
            target  = SPL_ROW + SPL_H + 2;
            new_top = ed->cur_row - target;
            if (new_top < 0) new_top = 0;
        }

        /* Clamp: word must remain on-screen. */
        if (new_top > ed->cur_row)
            new_top = ed->cur_row;
        if (ed->cur_row - new_top >= TEXT_ROWS)
            new_top = ed->cur_row - (TEXT_ROWS - 1);

        ed->top_row = new_top;
    }
}

/* -----------------------------------------------------------------------
 * Forward scan: find the next misspelled word
 * ----------------------------------------------------------------------- */

/* Largest line we'll classify in one pass (longer lines are scanned but
 * token classification is skipped -- every char is treated as TOK_TEXT). */
#define SPELL_LINE_MAX  4096

static HLTok g_toks[SPELL_LINE_MAX];  /* reused across calls */

/* Advance ed->spell_scan_row/col until we find a misspelled word.
 * Returns 1 and fills ed->spell_{row,col,len,word,sug[],nsug} if found.
 * Returns 0 when the end of the buffer has been reached.
 * Returns -1 on ispell I/O error. */
static int spell_next(Editor *ed)
{
    while (ed->spell_scan_row < ed->tb.count) {
        int         row = ed->spell_scan_row;
        const Line *l   = &ed->tb.lines[row];
        int         len = l->len > SPELL_LINE_MAX ? SPELL_LINE_MAX : l->len;

        classify_line_toks(ed, row, g_toks, len);

        int col = ed->spell_scan_col;
        while (col < len) {
            unsigned char ch = (unsigned char)l->data[col];

            /* Non-word character: skip. */
            if (!is_word_char(ch) || ch == '\'') { col++; continue; }

            /* Decide whether this token class is spellable. */
            HLTok tok = (col < len) ? g_toks[col] : TOK_TEXT;
            int spellable;
            if (ed->spell_mode == SPELL_ALL) {
                /* Check everything except pure-code token classes. */
                spellable = (tok != TOK_KEYWORD  &&
                             tok != TOK_REGISTER &&
                             tok != TOK_NUMBER   &&
                             tok != TOK_OPERATOR &&
                             tok != TOK_BRACKET  &&
                             tok != TOK_LABEL    &&
                             tok != TOK_PREPROC);
            } else {
                /* SPELL_TEXT_ONLY: only strings and comments. */
                spellable = (tok == TOK_COMMENT || tok == TOK_STRING);
            }

            if (!spellable) { col++; continue; }

            /* Inside a string literal, skip the opening quote character. */
            if (tok == TOK_STRING && ed->hl) {
                const char *quotes = ed->hl->string_quotes;
                int q;
                for (q = 0; quotes[q]; q++) {
                    if (ch == (unsigned char)quotes[q]) { col++; goto next_char; }
                }
            }

            /* Extract the word (run of letters + embedded apostrophes). */
            {
                int wstart = col;
                int wend   = col;
                while (wend < len) {
                    unsigned char wc = (unsigned char)l->data[wend];
                    if (!is_word_char(wc)) break;
                    /* Apostrophe only valid inside the word (contractions). */
                    if (wc == '\'' && (wend + 1 >= len ||
                        !isalpha((unsigned char)l->data[wend + 1]))) break;
                    wend++;
                }
                col = wend;   /* advance past the word */

                int wlen = wend - wstart;
                if (wlen < 2 || wlen >= 64) continue;  /* too short or long */

                /* Copy and strip trailing apostrophe if any. */
                char word[64];
                memcpy(word, l->data + wstart, (size_t)wlen);
                word[wlen] = '\0';
                while (wlen > 0 && word[wlen - 1] == '\'') word[--wlen] = '\0';
                if (wlen < 2) continue;

                int res = ispell_check_word(ed, word);
                if (res < 0) return -1;   /* I/O error */
                if (res == 1) {
                    /* Found a misspelling. */
                    ed->spell_row     = row;
                    ed->spell_col     = wstart;
                    ed->spell_len     = wlen;
                    ed->spell_sug_top = 0;     /* reset page on each new word */
                    ed->spell_sug_sel = -1;    /* clear mouse selection       */
                    memcpy(ed->spell_word, word, (size_t)(wlen + 1));
                    /* Resume scan after this word next time. */
                    ed->spell_scan_row = row;
                    ed->spell_scan_col = wend;
                    /* Jump the editor cursor to the misspelled word. */
                    ed->cur_row = row;
                    ed->cur_col = wstart;
                    ed_clamp_and_scroll(ed);
                    spell_ensure_visible(ed);  /* keep word outside dialog */
                    return 1;
                }
            }
            continue;
next_char:;
        }

        /* End of this row: move to the next. */
        ed->spell_scan_row++;
        ed->spell_scan_col = 0;
    }

    return 0;  /* end of buffer */
}

/* -----------------------------------------------------------------------
 * Replace the current misspelled word
 * ----------------------------------------------------------------------- */

static void spell_do_replace(Editor *ed, const char *replacement)
{
    int row  = ed->spell_row;
    int col  = ed->spell_col;
    int len  = ed->spell_len;
    int rlen = (int)strlen(replacement);

    /* Delete the old word in-place, then paste the replacement. */
    ed_delete_range(ed, row, col, row, col + len);

    ed->cur_row = row;
    ed->cur_col = col;
    ed_paste_blob(ed, replacement, rlen);

    /* Advance the scan past the replacement so we don't re-check it. */
    ed->spell_scan_row = row;
    ed->spell_scan_col = col + rlen;

    ed_clamp_and_scroll(ed);
}

/* -----------------------------------------------------------------------
 * After a replacement or ignore, find the next misspelling.
 * Handles the "done" case and any I/O error.
 * ----------------------------------------------------------------------- */

static void advance_or_done(Editor *ed)
{
    int r = spell_next(ed);
    if (r == 0) {
        spell_stop(ed);
        ed_set_message(ed, "Spell: finished -- no more misspellings");
    } else if (r < 0) {
        /* I/O error already set message in spell_next; stop cleanly. */
        spell_stop(ed);
    }
    /* r == 1: dialog stays visible with new word */
}

/* -----------------------------------------------------------------------
 * Public API
 * ----------------------------------------------------------------------- */

void spell_start(Editor *ed, int mode)
{
    /* Clean up any previous run. */
    if (ed->spell_active || ed->spell_pid >= 0)
        spell_stop(ed);

    ed->spell_mode         = mode;
    ed->spell_active       = 0;
    ed->spell_scan_row     = 0;
    ed->spell_scan_col     = 0;
    ed->spell_replace_mode = 0;
    ed->spell_replace_len  = 0;
    ed->spell_replace_buf[0] = '\0';

    if (mode == SPELL_TEXT_ONLY && !ed->hl) {
        ed_set_message(ed,
            "spell code: load a syntax file first (e.g. syntax ansi-c.csv)");
        return;
    }

    /* Ensure HL states are current (we use them in classify_line_toks). */
    if (ed->hl && ed->hl_dirty_row >= 0) {
        hl_update_states(ed->hl, &ed->tb, ed->hl_dirty_row);
        ed->hl_dirty_row = -1;
    }

    if (ispell_spawn(ed) < 0) {
        ed_set_message(ed,
            "Spell: ispell/aspell/hunspell not found  (try: apt install ispell)");
        return;
    }

    int r = spell_next(ed);
    if (r == 0) {
        spell_stop(ed);
        ed_set_message(ed, "Spell: no misspellings found!");
        return;
    }
    if (r < 0) {
        spell_stop(ed);
        return;
    }

    ed->spell_active = 1;
    ed_set_message(ed, "");   /* clear any previous status */
}

void spell_stop(Editor *ed)
{
    /* Close the write end first so ispell sees EOF and exits cleanly. */
    if (ed->spell_wfd >= 0) {
        /* Send 'Q' for a graceful quit before closing. */
        PIPE_WRITE(ed->spell_wfd, "Q\n", 2);
        close(ed->spell_wfd);
        ed->spell_wfd = -1;
    }
    if (ed->spell_rfp) {
        fclose(ed->spell_rfp);
        ed->spell_rfp = NULL;
    }
    if (ed->spell_pid >= 0) {
        waitpid(ed->spell_pid, NULL, 0);
        ed->spell_pid = -1;
    }
    ed->spell_active       = 0;
    ed->spell_replace_mode = 0;
}

void spell_handle_key(Editor *ed, int key)
{
    if (!ed->spell_active) return;

    /* ---- Replace-prompt mode: user is typing a custom replacement ----- */
    if (ed->spell_replace_mode) {
        switch (key) {
        case KEY_ESC:
            ed->spell_replace_mode   = 0;
            ed->spell_replace_len    = 0;
            ed->spell_replace_buf[0] = '\0';
            break;

        case KEY_ENTER:
            if (ed->spell_replace_len > 0) {
                spell_do_replace(ed, ed->spell_replace_buf);
                ed->spell_replace_mode   = 0;
                ed->spell_replace_len    = 0;
                ed->spell_replace_buf[0] = '\0';
                advance_or_done(ed);
            } else {
                /* Empty input: cancel replace mode. */
                ed->spell_replace_mode = 0;
            }
            break;

        case KEY_BS:
            if (ed->spell_replace_len > 0)
                ed->spell_replace_buf[--ed->spell_replace_len] = '\0';
            break;

        default:
            if (key >= 0x20 && key < 0x7F &&
                ed->spell_replace_len < 63) {
                ed->spell_replace_buf[ed->spell_replace_len++] = (char)key;
                ed->spell_replace_buf[ed->spell_replace_len]   = '\0';
            }
            break;
        }
        return;
    }

    /* ---- Normal dialog keys ------------------------------------------ */
    switch (key) {

    case KEY_ESC:
        spell_stop(ed);
        ed_set_message(ed, "Spell: stopped");
        break;

    /* Ignore / skip */
    case 'i':
    case ' ':
    case 'n':
        ispell_accept(ed);
        advance_or_done(ed);
        break;

    /* Add to personal dictionary */
    case 'a':
        ispell_add_word(ed);
        advance_or_done(ed);
        break;

    /* Custom replacement */
    case 'r':
        ed->spell_replace_mode   = 1;
        ed->spell_replace_len    = 0;
        ed->spell_replace_buf[0] = '\0';
        break;

    /* Next (without ignoring) */
    case KEY_ENTER:
        advance_or_done(ed);
        break;

    /* Page up through suggestions */
    case '[':
    case KEY_SCROLL_UP:
        if (ed->spell_sug_top > 0) {
            ed->spell_sug_top -= SPL_MAX_DISP;
            if (ed->spell_sug_top < 0) ed->spell_sug_top = 0;
        }
        break;

    /* Page down through suggestions */
    case ']':
    case KEY_SCROLL_DOWN:
        if (ed->spell_sug_top + SPL_MAX_DISP < ed->spell_nsug) {
            ed->spell_sug_top += SPL_MAX_DISP;
            /* clamp so we don't show an empty page */
            if (ed->spell_sug_top >= ed->spell_nsug)
                ed->spell_sug_top = ed->spell_nsug - 1;
        }
        break;

    case KEY_MOUSE_PRESS:
        {
            int mcol = vio_mouse.col;
            int mrow = vio_mouse.row;

            if (mcol < SPL_COL || mcol >= SPL_COL + SPL_W ||
                mrow < SPL_ROW || mrow >= SPL_ROW + SPL_H) {
                /* click outside dialog: stop */
                spell_stop(ed);
                ed_set_message(ed, "Spell: stopped");
            } else if (mrow >= SPL_ROW + 3 &&
                       mrow <  SPL_ROW + 3 + SPL_MAX_DISP) {
                /* click on a suggestion row */
                int i   = mrow - (SPL_ROW + 3);
                int idx = ed->spell_sug_top + i;
                if (idx < ed->spell_nsug) {
                    if (idx == ed->spell_sug_sel) {
                        /* second click on already-selected: apply */
                        spell_do_replace(ed, ed->spell_sug[idx]);
                        advance_or_done(ed);
                    } else {
                        /* first click: select/highlight */
                        ed->spell_sug_sel = idx;
                    }
                }
            }
        }
        break;

    default:
        /* 1-7: replace with Nth suggestion on the current page */
        if (key >= '1' && key <= '7') {
            int idx = ed->spell_sug_top + (key - '1');
            if (idx < ed->spell_nsug) {
                spell_do_replace(ed, ed->spell_sug[idx]);
                advance_or_done(ed);
            }
        }
        break;
    }
}

/* -----------------------------------------------------------------------
 * Cursor placement
 * ----------------------------------------------------------------------- */

int spell_cursor_pos(const Editor *ed, int *x, int *y)
{
    if (!ed->spell_active || !ed->spell_replace_mode) return 0;
    /* Replace prompt is at the hint row: SPL_ROW + 3 + SPL_MAX_DISP.
     * Prompt prefix is " Replace: " = 10 chars; cursor after typed text. */
    *x = SPL_COL + 1 + 10 + ed->spell_replace_len;
    *y = SPL_ROW + 3 + SPL_MAX_DISP;  /* hint row */
    if (*x >= SPL_COL + SPL_W - 1)
        *x = SPL_COL + SPL_W - 2;
    return 1;
}

/* -----------------------------------------------------------------------
 * Rendering
 * ----------------------------------------------------------------------- */

void spell_render(const Editor *ed)
{
    if (!ed->spell_active) return;

    int x = SPL_COL;
    int y = SPL_ROW;

    /* ---- Outer box + interior fill ----------------------------------- */
    vio_dbox(x, y, SPL_W, SPL_H, A_SPL_BDR);
    vio_fill(x + 1, y + 1, SPL_INNER, SPL_H - 2, ' ', A_SPL_BODY);

    /* ---- Title on top border ----------------------------------------- */
    {
        const char *title = " Spell Check ";
        int tlen = (int)strlen(title);
        vio_gotoxy(x + (SPL_W - tlen) / 2, y);
        vio_setattr(A_SPL_BDR);
        vio_puts(title);
    }

    /* ---- Row y+1: word info ------------------------------------------ */
    {
        char rbuf[32];
        snprintf(rbuf, sizeof(rbuf), " %d:%d",
                 ed->spell_row + 1, ed->spell_col + 1);
        int rlen = (int)strlen(rbuf);
        int lw   = SPL_INNER - rlen;

        char lbuf[SPL_INNER + 1];
        snprintf(lbuf, sizeof(lbuf), " Word: \"%.*s\"",
                 lw - 10 > 1 ? lw - 10 : 1, ed->spell_word);

        vio_gotoxy(x + 1, y + 1);
        vio_setattr(A_SPL_WORDLBL);
        spl_field(lbuf, lw);
        vio_setattr(A_SPL_POS);
        vio_puts(rbuf);
    }

    /* ---- Row y+2: rule (with page indicator when suggestions overflow) */
    {
        vio_gotoxy(x, y + 2);
        vio_setattr(A_SPL_BDR);

        int total = ed->spell_nsug;
        int top   = ed->spell_sug_top;
        int bot   = top + SPL_MAX_DISP;
        if (bot > total) bot = total;

        if (total > SPL_MAX_DISP) {
            char label[48];
            int i;
            snprintf(label, sizeof(label), "  %d-%d of %d  ", top + 1, bot, total);
            int llen         = (int)strlen(label);
            int left_dashes  = (SPL_INNER - llen) / 2;
            int right_dashes = SPL_INNER - llen - left_dashes;

            vio_putch(0xCC);
            for (i = 0; i < left_dashes; i++)  vio_putch(0xCD);
            vio_setattr(A_SPL_HINT);
            vio_puts(label);
            vio_setattr(A_SPL_BDR);
            for (i = 0; i < right_dashes; i++) vio_putch(0xCD);
            vio_putch(0xB9);
        } else {
            int i;
            vio_putch(0xCC);
            for (i = 0; i < SPL_INNER; i++) vio_putch(0xCD);
            vio_putch(0xB9);
        }
    }

    /* ---- Rows y+3 .. y+3+SPL_MAX_DISP-1: suggestions --------------- */
    {
        int i;
        for (i = 0; i < SPL_MAX_DISP; i++) {
            int sy  = y + 3 + i;
            int idx = ed->spell_sug_top + i;

            vio_gotoxy(x + 1, sy);

            if (idx < ed->spell_nsug) {
                int sel = (idx == ed->spell_sug_sel);
                vio_setattr(sel ? A_SPL_SELBODY : A_SPL_BODY);   vio_putch(' ');
                vio_setattr(sel ? A_SPL_SELNUM  : A_SPL_SUGNUM); vio_putch('1' + i);
                vio_setattr(sel ? A_SPL_SELBODY : A_SPL_BODY);
                vio_putch(' ');  vio_putch(' ');
                vio_setattr(sel ? A_SPL_SELTEXT : A_SPL_SUGTEXT);
                spl_field(ed->spell_sug[idx], SPL_INNER - 4);
            } else if (i == 0 && ed->spell_nsug == 0) {
                vio_setattr(A_SPL_BODY);  vio_putch(' ');
                vio_setattr(A_SPL_NOSUG);
                spl_field("(no suggestions)", SPL_INNER - 1);
            } else {
                vio_setattr(A_SPL_BODY);
                spl_field("", SPL_INNER);
            }
        }
    }

    /* ---- Row y+3+SPL_MAX_DISP: hint or replace-prompt --------------- */
    {
        int hr = y + 3 + SPL_MAX_DISP;   /* one row below last suggestion */
        vio_gotoxy(x + 1, hr);

        if (ed->spell_replace_mode) {
            int avail = SPL_INNER - 11;
            if (avail < 1) avail = 1;
            char pbuf[SPL_INNER + 1];
            vio_setattr(A_SPL_HINT);
            vio_puts(" Replace: ");
            snprintf(pbuf, sizeof(pbuf), "%-*.*s",
                     avail, avail, ed->spell_replace_buf);
            vio_setattr(A_SPL_RPLVAL);
            vio_puts(pbuf);
            vio_setattr(A_SPL_HINT);
            vio_putch(' ');
        } else {
            const char *hint = (ed->spell_nsug > SPL_MAX_DISP)
                ? " 1-7:replace  []:page  i:skip  a:add  r:retype  Esc:stop"
                : " 1-7:replace  i:skip  a:add  r:retype  Esc:stop";
            vio_setattr(A_SPL_HINT);
            spl_field(hint, SPL_INNER);
        }
    }
}
