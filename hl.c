/* SPDX-License-Identifier: MIT
 * hl.c  --  syntax highlighting: CSV language loader + line lexer
 *
 * ---------------------------------------------------------------------
 * CSV definition file format (one directive per line, comma-separated;
 * leading/trailing whitespace on each field is trimmed; blank lines and
 * lines starting with '#' are ignored):
 *
 *   name,<display name>
 *   ext,<.ext1>,<.ext2>,...        repeatable; extensions accumulate
 *   line_comment,<marker>          e.g. // or ;
 *   block_open,<marker>            opening block-comment marker (C: slash-star)
 *   block_close,<marker>           closing block-comment marker (C: star-slash)
 *   preproc,<0|1>                  '#' at column 0 -> preprocessor line
 *   labels,<0|1>                   column-0 identifiers -> labels
 *   case_insensitive,<0|1>
 *   keyword,<kw1>,<kw2>,...        repeatable; keywords accumulate
 *   register,<r1>,<r2>,...         repeatable; registers accumulate
 *   color,<class>,<bg>,<fg>        class: text keyword register string
 *                                  comment number preproc label
 *                                  bg/fg: 0-15 VGA palette index
 *
 * Unknown directives are ignored (forward compatible).  Example for
 * ANSI C is in lang/ansi-c.csv; for Z80 in lang/z80.csv.
 * ---------------------------------------------------------------------
 *
 * All scanning here is byte-oriented and ASCII-only by design -- see
 * the note in hl.h.  No locale-dependent ctype.h calls are used so
 * behaviour can't shift under a different LANG/LC_CTYPE.
 */

#include "hl.h"
#include "vgaterm.h"   /* VGA_ATTR, VGA_BLACK, VGA_LGRAY (fallback color) */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ----------------------------------------------------------------------
 * Small ASCII-only helpers (no ctype.h -- see file header note)
 * ------------------------------------------------------------------- */

static int is_digit(unsigned char ch)
{
    return ch >= '0' && ch <= '9';
}

static int is_operator(unsigned char ch)
{
    return strchr("+-*/=<>&|^~?:%", ch) != NULL;
}

static int is_bracket(unsigned char ch)
{
    return strchr("()[]{}",  ch) != NULL;
}

static int is_xdigit(unsigned char ch)
{
    return is_digit(ch) || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F');
}

static int is_ident_start(unsigned char ch)
{
    return ch == '_' || (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z');
}

static int is_ident_char(unsigned char ch)
{
    return is_ident_start(ch) || is_digit(ch);
}

static unsigned char to_lower_ascii(unsigned char ch)
{
    if (ch >= 'A' && ch <= 'Z') return (unsigned char)(ch - 'A' + 'a');
    return ch;
}

static int stricmp_ascii(const char *a, const char *b)
{
    for (;;) {
        unsigned char ca = to_lower_ascii((unsigned char)*a);
        unsigned char cb = to_lower_ascii((unsigned char)*b);
        if (ca != cb) return (int)ca - (int)cb;
        if (ca == '\0') return 0;
        a++; b++;
    }
}

/* qsort/bsearch comparators over fixed-width, NUL-terminated char blocks */
static int cmp_cs(const void *a, const void *b)
{
    return strcmp((const char *)a, (const char *)b);
}

static int cmp_ci(const void *a, const void *b)
{
    return stricmp_ascii((const char *)a, (const char *)b);
}

static char *trim(char *s)
{
    char *end;
    while (*s == ' ' || *s == '\t') s++;
    end = s + strlen(s);
    while (end > s &&
           (end[-1] == '\n' || end[-1] == '\r' ||
            end[-1] == ' '  || end[-1] == '\t'))
        *--end = '\0';
    return s;
}

/* ----------------------------------------------------------------------
 * CSV loading
 * ------------------------------------------------------------------- */

static void copy_trunc(char *dst, size_t dstsz, const char *src)
{
    size_t n = strlen(src);
    if (n > dstsz - 1) n = dstsz - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static HLTok tok_class_from_name(const char *name)
{
    if (!strcmp(name, "text"))     return TOK_TEXT;
    if (!strcmp(name, "keyword"))  return TOK_KEYWORD;
    if (!strcmp(name, "register")) return TOK_REGISTER;
    if (!strcmp(name, "string"))   return TOK_STRING;
    if (!strcmp(name, "comment"))  return TOK_COMMENT;
    if (!strcmp(name, "number"))   return TOK_NUMBER;
    if (!strcmp(name, "preproc"))  return TOK_PREPROC;
    if (!strcmp(name, "label"))    return TOK_LABEL;
    if (!strcmp(name, "operator")) return TOK_OPERATOR;
    if (!strcmp(name, "bracket"))  return TOK_BRACKET;
    return TOK_COUNT;  /* sentinel: unrecognised */
}

static int clamp16(const char *s)
{
    int v = atoi(s);
    if (v < 0)  v = 0;
    if (v > 15) v = 15;
    return v;
}

int hl_load(HLang *lang, const char *path)
{
    FILE *f;
    char  raw[512];
    int   i;

    memset(lang, 0, sizeof(*lang));

    f = fopen(path, "r");
    if (!f) return -1;

    /* Sane default quote characters and preprocessor trigger. */
    strcpy(lang->string_quotes, "\"'");
    lang->preproc_char = '\0';

    /* Sane fallback colors in case the file never sets one of them. */
    for (i = 0; i < TOK_COUNT; i++)
        lang->color[i] = VGA_ATTR(VGA_BLACK, VGA_LGRAY);

    while (fgets(raw, (int)sizeof(raw), f)) {
        char *line = trim(raw);
        char *directive, *field;

        if (line[0] == '\0' || line[0] == '#') continue;

        directive = trim(strtok(line, ","));
        if (!directive) continue;

        if (!strcmp(directive, "name")) {

            field = strtok(NULL, ",");
            if (field) copy_trunc(lang->name, sizeof(lang->name), trim(field));

        } else if (!strcmp(directive, "ext")) {

            while (lang->n_ext < HL_MAX_EXTS &&
                   (field = strtok(NULL, ",")) != NULL) {
                 field = trim(field);
                 if (*field)
                     copy_trunc(lang->ext[lang->n_ext++], HL_MAX_EXT_LEN, field);
            }

        } else if (!strcmp(directive, "line_comment")) {

            field = strtok(NULL, ",");
            if (field) copy_trunc(lang->line_comment, HL_MAX_COMMENT_LEN, trim(field));

        } else if (!strcmp(directive, "block_open")) {

            field = strtok(NULL, ",");
            if (field) copy_trunc(lang->block_open, HL_MAX_COMMENT_LEN, trim(field));

        } else if (!strcmp(directive, "block_close")) {

            field = strtok(NULL, ",");
            if (field) copy_trunc(lang->block_close, HL_MAX_COMMENT_LEN, trim(field));

        } else if (!strcmp(directive, "preproc")) {

            field = strtok(NULL, ",");
            if (field) {
                char *fld = trim(field);
                if (fld[0] == '1') {
                    lang->preproc_char = '#';
                } else if (fld[0] == '0' || fld[0] == '\0') {
                    lang->preproc_char = '\0';
                } else {
                    lang->preproc_char = fld[0];
                }
            } else {
                lang->preproc_char = '\0';
            }

        } else if (!strcmp(directive, "hex_suffix")) {

            field = strtok(NULL, ",");
            if (field) {
                char *fld = trim(field);
                lang->hex_suffix = fld[0];
            }

        } else if (!strcmp(directive, "bin_suffix")) {

            field = strtok(NULL, ",");
            if (field) {
                char *fld = trim(field);
                lang->bin_suffix = fld[0];
            }

        } else if (!strcmp(directive, "string_quotes")) {

            field = strtok(NULL, ",");
            if (field) {
                copy_trunc(lang->string_quotes, sizeof(lang->string_quotes), trim(field));
            }

        } else if (!strcmp(directive, "labels")) {

            field = strtok(NULL, ",");
            lang->has_labels = field ? atoi(trim(field)) != 0 : 0;

        } else if (!strcmp(directive, "case_insensitive")) {

            field = strtok(NULL, ",");
            lang->case_insensitive = field ? atoi(trim(field)) != 0 : 0;

        } else if (!strcmp(directive, "keyword")) {

            while (lang->n_keywords < HL_MAX_KEYWORDS &&
                   (field = strtok(NULL, ",")) != NULL) {
                field = trim(field);
                if (*field)
                    copy_trunc(lang->keywords[lang->n_keywords++],
                               HL_MAX_KW_LEN, field);
            }

        } else if (!strcmp(directive, "register")) {

            while (lang->n_registers < HL_MAX_REGISTERS &&
                   (field = strtok(NULL, ",")) != NULL) {
                field = trim(field);
                if (*field)
                    copy_trunc(lang->registers[lang->n_registers++],
                               HL_MAX_REG_LEN, field);
            }

        } else if (!strcmp(directive, "color")) {

            char *cls_s = strtok(NULL, ",");
            char *bg_s  = strtok(NULL, ",");
            char *fg_s  = strtok(NULL, ",");
            if (cls_s && bg_s && fg_s) {
                HLTok cls = tok_class_from_name(trim(cls_s));
                if (cls != TOK_COUNT)
                    lang->color[cls] = VGA_ATTR(clamp16(trim(bg_s)),
                                                 clamp16(trim(fg_s)));
            }
        }
        /* unrecognised directives are silently skipped */
    }

    fclose(f);

    if (lang->name[0] == '\0')
        copy_trunc(lang->name, sizeof(lang->name), path);

    if (lang->n_keywords > 1)
        qsort(lang->keywords, (size_t)lang->n_keywords, HL_MAX_KW_LEN,
              lang->case_insensitive ? cmp_ci : cmp_cs);

    if (lang->n_registers > 1)
        qsort(lang->registers, (size_t)lang->n_registers, HL_MAX_REG_LEN,
              lang->case_insensitive ? cmp_ci : cmp_cs);

    return 0;
}

const HLang *hl_match_ext(const HLang *table, int n, const char *filename)
{
    const char *dot;
    int i, j;

    if (!filename) return NULL;
    dot = strrchr(filename, '.');
    if (!dot) return NULL;

    for (i = 0; i < n; i++) {
        const HLang *lang = &table[i];
        for (j = 0; j < lang->n_ext; j++) {
            if (stricmp_ascii(dot, lang->ext[j]) == 0)
                return lang;
        }
    }
    return NULL;
}

/* ----------------------------------------------------------------------
 * Lookup against the sorted keyword / register tables
 * ------------------------------------------------------------------- */

static int kw_lookup(const HLang *lang, const char *word, int wlen)
{
    char key[HL_MAX_KW_LEN];
    int  n = wlen < HL_MAX_KW_LEN - 1 ? wlen : HL_MAX_KW_LEN - 1;

    if (lang->n_keywords == 0) return 0;
    memcpy(key, word, (size_t)n);
    key[n] = '\0';
    return bsearch(key, lang->keywords, (size_t)lang->n_keywords,
                    HL_MAX_KW_LEN,
                    lang->case_insensitive ? cmp_ci : cmp_cs) != NULL;
}

static int reg_lookup(const HLang *lang, const char *word, int wlen)
{
    char key[HL_MAX_REG_LEN];
    int  n = wlen < HL_MAX_REG_LEN - 1 ? wlen : HL_MAX_REG_LEN - 1;

    if (lang->n_registers == 0) return 0;
    memcpy(key, word, (size_t)n);
    key[n] = '\0';
    return bsearch(key, lang->registers, (size_t)lang->n_registers,
                    HL_MAX_REG_LEN,
                    lang->case_insensitive ? cmp_ci : cmp_cs) != NULL;
}

/* ----------------------------------------------------------------------
 * Per-character lexer
 * ------------------------------------------------------------------- */

/* Returns the marker's length if `line` matches it starting at `col`,
 * else 0.  An empty marker never matches. */
static int starts_with(const char *line, int len, int col, const char *marker)
{
    int mlen = (int)strlen(marker);
    int i;
    if (mlen == 0 || col + mlen > len) return 0;
    for (i = 0; i < mlen; i++)
        if (line[col + i] != marker[i]) return 0;
    return mlen;
}

/* Scans forward from `from` for `marker`; returns the index just past
 * the marker if found before `len`, else -1. */
static int find_marker(const char *line, int len, int from, const char *marker)
{
    int p;
    for (p = from; p < len; p++) {
        int mlen = starts_with(line, len, p, marker);
        if (mlen) return p + mlen;
    }
    return -1;
}

/* Classifies the run starting at `col` (run_remaining == 0 on entry),
 * filling c->run_remaining / c->run_attr and updating c->state. */
static void classify_run(HLCursor *c, const char *line, int len, int col)
{
    const HLang *lang = c->lang;
    unsigned char ch;
    int mlen, end, wlen;

    if (col >= len) {
        c->run_remaining = 1;
        c->run_tok  = TOK_TEXT;
        c->run_attr = lang->color[TOK_TEXT];
        return;
    }
    ch = (unsigned char)line[col];

    /* Continuing a block comment carried over from a previous line. */
    if (c->state == HL_ST_BLOCK_COMMENT) {
        end = find_marker(line, len, col, lang->block_close);
        c->run_remaining = (end >= 0 ? end : len) - col;
        c->run_tok  = TOK_COMMENT;
        c->run_attr = lang->color[TOK_COMMENT];
        if (end >= 0) c->state = HL_ST_NORMAL;
        return;
    }

    /* Block comment opening on this line. */
    if (lang->block_open[0] &&
        (mlen = starts_with(line, len, col, lang->block_open)) != 0) {
        end = find_marker(line, len, col + mlen, lang->block_close);
        c->run_remaining = (end >= 0 ? end : len) - col;
        c->run_tok  = TOK_COMMENT;
        c->run_attr = lang->color[TOK_COMMENT];
        c->state = (end >= 0) ? HL_ST_NORMAL : HL_ST_BLOCK_COMMENT;
        return;
    }

    /* Line comment: rest of the line. */
    if (lang->line_comment[0] && starts_with(line, len, col, lang->line_comment)) {
        c->run_remaining = len - col;
        c->run_tok  = TOK_COMMENT;
        c->run_attr = lang->color[TOK_COMMENT];
        return;
    }

    /* Preprocessor line: whole remainder, vintage-editor style. */
    if (lang->preproc_char && col == 0 && ch == (unsigned char)lang->preproc_char) {
        c->run_remaining = len - col;
        c->run_tok  = TOK_PREPROC;
        c->run_attr = lang->color[TOK_PREPROC];
        return;
    }

    /* String / char literal, with backslash-escape skipping. */
    {
        int is_quote = 0;
        int qidx;
        for (qidx = 0; lang->string_quotes[qidx]; qidx++) {
            if (ch == (unsigned char)lang->string_quotes[qidx]) {
                is_quote = 1;
                break;
            }
        }
        if (is_quote) {
            unsigned char quote = ch;
            int p = col + 1;
            while (p < len) {
                if ((unsigned char)line[p] == '\\' && p + 1 < len) { p += 2; continue; }
                if ((unsigned char)line[p] == quote) { p++; break; }
                p++;
            }
            c->run_remaining = p - col;
            c->run_tok  = TOK_STRING;
            c->run_attr = lang->color[TOK_STRING];
            return;
        }
    }

    /* Numeric literal: decimal, 0x.., $hex (asm), %binary (asm). */
    if (is_digit(ch) ||
        (ch == '$' && col + 1 < len && is_xdigit((unsigned char)line[col + 1])) ||
        (ch == '%' && col + 1 < len &&
         (line[col + 1] == '0' || line[col + 1] == '1'))) {
        int p = col + 1;
        while (p < len &&
               (is_ident_char((unsigned char)line[p]) || line[p] == '.'))
            p++;
        c->run_remaining = p - col;
        c->run_tok  = TOK_NUMBER;
        c->run_attr = lang->color[TOK_NUMBER];
        return;
    }

    /* Identifier: label / register / keyword / suffix number / plain text. */
    if (is_ident_start(ch)) {
        int p = col + 1;
        while (p < len && is_ident_char((unsigned char)line[p])) p++;
        wlen = p - col;

        /* Check custom hex/binary suffix */
        int is_suffix_num = 0;
        if (wlen > 1) {
            char last_c = line[p - 1];
            if (lang->hex_suffix && (last_c == lang->hex_suffix || (lang->case_insensitive && to_lower_ascii((unsigned char)last_c) == to_lower_ascii((unsigned char)lang->hex_suffix)))) {
                /* check if all preceding chars are valid hex digits */
                int k;
                int ok = 1;
                for (k = 0; k < wlen - 1; k++) {
                    if (!is_xdigit((unsigned char)line[col + k])) { ok = 0; break; }
                }
                if (ok) is_suffix_num = 1;
            }
            if (!is_suffix_num && lang->bin_suffix && (last_c == lang->bin_suffix || (lang->case_insensitive && to_lower_ascii((unsigned char)last_c) == to_lower_ascii((unsigned char)lang->bin_suffix)))) {
                /* check if all preceding chars are binary digits */
                int k;
                int ok = 1;
                for (k = 0; k < wlen - 1; k++) {
                    char dig = line[col + k];
                    if (dig != '0' && dig != '1') { ok = 0; break; }
                }
                if (ok) is_suffix_num = 1;
            }
        }

        if (is_suffix_num) {
            c->run_remaining = wlen;
            c->run_tok  = TOK_NUMBER;
            c->run_attr = lang->color[TOK_NUMBER];
            return;
        }

        if (lang->has_labels && col == 0) {
            if (reg_lookup(lang, &line[col], wlen)) {
                c->run_remaining = wlen;
                c->run_tok  = TOK_REGISTER;
                c->run_attr = lang->color[TOK_REGISTER];
                return;
            }
            if (kw_lookup(lang, &line[col], wlen)) {
                c->run_remaining = wlen;
                c->run_tok  = TOK_KEYWORD;
                c->run_attr = lang->color[TOK_KEYWORD];
                return;
            }
            /* Unrecognised column-0 identifier: a label.  Swallow an
             * immediately-following colon if present. */
            if (p < len && line[p] == ':') wlen++;
            c->run_remaining = wlen;
            c->run_tok  = TOK_LABEL;
            c->run_attr = lang->color[TOK_LABEL];
            return;
        }

        if (reg_lookup(lang, &line[col], wlen)) {
            c->run_remaining = wlen;
            c->run_tok  = TOK_REGISTER;
            c->run_attr = lang->color[TOK_REGISTER];
            return;
        }
        if (kw_lookup(lang, &line[col], wlen)) {
            c->run_remaining = wlen;
            c->run_tok  = TOK_KEYWORD;
            c->run_attr = lang->color[TOK_KEYWORD];
            return;
        }
        c->run_remaining = wlen;
        c->run_tok  = TOK_TEXT;
        c->run_attr = lang->color[TOK_TEXT];
        return;
    }

    /* Bracket / grouping character. */
    if (is_bracket(ch)) {
        c->run_remaining = 1;
        c->run_tok  = TOK_BRACKET;
        c->run_attr = lang->color[TOK_BRACKET];
        return;
    }

    /* Operator character -- single or recognised two-character pair.
     * '!' is included here even though it is not in is_operator() so that
     * '!=' is coloured as a unit rather than '!' falling through to plain
     * text and '=' being picked up separately on the next step. */
    if (is_operator(ch) || ch == '!') {
        if (col + 1 < len) {
            unsigned char nx = (unsigned char)line[col + 1];
            int two = 0;
            switch (ch) {
                case '!': two = (nx == '=');                           break;
                case '+': two = (nx == '+' || nx == '=');              break;
                case '-': two = (nx == '-' || nx == '>' || nx == '='); break;
                case '*': case '/': case '%':
                case '^': two = (nx == '=');                           break;
                case '=': two = (nx == '=');                           break;
                case '<': two = (nx == '<' || nx == '=');              break;
                case '>': two = (nx == '>' || nx == '=');              break;
                case '&': two = (nx == '&' || nx == '=');              break;
                case '|': two = (nx == '|' || nx == '=');              break;
                default:  break;
            }
            if (two) {
                c->run_remaining = 2;
                c->run_tok  = TOK_OPERATOR;
                c->run_attr = lang->color[TOK_OPERATOR];
                return;
            }
        }
        c->run_remaining = 1;
        c->run_tok  = TOK_OPERATOR;
        c->run_attr = lang->color[TOK_OPERATOR];
        return;
    }

    /* Anything else: one plain character. */
    c->run_remaining = 1;
    c->run_tok  = TOK_TEXT;
    c->run_attr = lang->color[TOK_TEXT];
}

void hl_begin(HLCursor *c, const HLang *lang, HLState start_state)
{
    c->lang = lang;
    c->state = start_state;
    c->run_remaining = 0;
    c->run_attr = 0;
    c->run_tok  = TOK_TEXT;
}

uint8_t hl_step(HLCursor *c, const char *line, int len, int col)
{
    if (!c->lang) return 0;
    if (c->run_remaining <= 0)
        classify_run(c, line, len, col);
    c->run_remaining--;
    return c->run_attr;
}

HLState hl_end_state(const HLCursor *c)
{
    return c->state;
}

HLTok hl_tok(const HLCursor *c)
{
    return c->run_tok;
}

void hl_update_states(const HLang *lang, TextBuffer *tb, int start_row)
{
    if (!lang || !tb) return;
    if (start_row < 0) start_row = 0;
    if (start_row >= tb->count) return;

    uint8_t state = (start_row == 0) ? HL_ST_NORMAL : tb->hl_states[start_row];
    int row;
    for (row = start_row; row < tb->count; row++) {
        tb->hl_states[row] = state;

        HLCursor c;
        hl_begin(&c, lang, state);

        const Line *l = &tb->lines[row];
        int col;
        for (col = 0; col < l->len; col++) {
            hl_step(&c, l->data, l->len, col);
        }

        uint8_t next_state = hl_end_state(&c);

        if (row + 1 < tb->count && tb->hl_states[row + 1] == next_state) {
            break;
        }
        state = next_state;
    }
}
