/* hl_test.c -- standalone sanity test for hl.c, not part of the build.
 * Run with:
 *   cc -O2 -Wall -Wextra -std=c99 -pedantic -o hl_test hl_test.c hl.c
 *   ./hl_test
 */
#include "hl.h"

#include <stdio.h>
#include <string.h>

static const char *tok_name(uint8_t attr, const HLang *lang)
{
    int i;
    static const char *names[TOK_COUNT] = {
        "text", "keyword", "register", "string",
        "comment", "number", "preproc", "label"
    };
    for (i = 0; i < TOK_COUNT; i++)
        if (lang->color[i] == attr) return names[i];
    return "?";
}

/* Lexes `lines[0..n)` exactly as render.c will: hl_begin() per row with
 * the carried-over state, hl_step() once per character left to right,
 * hl_end_state() feeding the next row.  Prints each char tagged with
 * its classified token type. */
static void run_lang(const HLang *lang, const char *const *lines, int n)
{
    HLState state = HL_ST_NORMAL;
    int row;

    printf("=== %s ===\n", lang->name);
    for (row = 0; row < n; row++) {
        HLCursor c;
        const char *line = lines[row];
        int len = (int)strlen(line);
        int col;

        hl_begin(&c, lang, state);
        printf("%2d: ", row);
        for (col = 0; col < len; col++) {
            uint8_t attr = hl_step(&c, line, len, col);
            printf("%c[%s] ", line[col], tok_name(attr, lang));
        }
        printf("\n");
        state = hl_end_state(&c);
    }
}

int main(void)
{
    HLang c_lang, z80_lang;
    int fails = 0;

    const char *c_src[] = {
        "#include <stdio.h>",
        "/* block",
        "   comment */ int x = 0x1F;",
        "int main(void) { /* hi */",
        "    return 0; // not a comment in c89, just text",
        "}"
    };

    const char *z80_src[] = {
        "start: LD A, $1F   ; load hex",
        "       ADD A, B",
        "       JR NZ, start",
        "count  DB %1010"
    };

    if (hl_load(&c_lang, "lang/ansi-c.csv") != 0) {
        printf("FAIL: could not load lang/ansi-c.csv\n");
        return 1;
    }
    printf("loaded '%s': %d keywords, ext[0]='%s'\n",
           c_lang.name, c_lang.n_keywords, c_lang.ext[0]);

    if (hl_load(&z80_lang, "lang/z80.csv") != 0) {
        printf("FAIL: could not load lang/z80.csv\n");
        return 1;
    }
    printf("loaded '%s': %d keywords, %d registers, ext[0]='%s'\n\n",
           z80_lang.name, z80_lang.n_keywords, z80_lang.n_registers,
           z80_lang.ext[0]);

    run_lang(&c_lang, c_src, 6);
    printf("\n");
    run_lang(&z80_lang, z80_src, 4);

    /* ---- a few hard assertions on specific cells ---- */
#define CHECK(cond, msg) do { \
        if (!(cond)) { printf("FAIL: %s\n", msg); fails++; } \
    } while (0)

    {
        /* "#include..." whole line should be preproc */
        HLCursor c;
        const char *l = c_src[0];
        hl_begin(&c, &c_lang, HL_ST_NORMAL);
        CHECK(hl_step(&c, l, (int)strlen(l), 0) == c_lang.color[TOK_PREPROC],
              "C: '#' starts a preproc run");
    }
    {
        /* block comment opened on line 1, should carry into line 2 */
        HLCursor c1, c2;
        HLState st;
        const char *l1 = c_src[1];
        const char *l2 = c_src[2];
        hl_begin(&c1, &c_lang, HL_ST_NORMAL);
        hl_step(&c1, l1, (int)strlen(l1), 0); /* classify the opening slash */
        /* drain rest of line 1 */
        { int i; for (i = 1; i < (int)strlen(l1); i++) hl_step(&c1, l1, (int)strlen(l1), i); }
        st = hl_end_state(&c1);
        CHECK(st == HL_ST_BLOCK_COMMENT, "C: block comment state carries to next line");

        hl_begin(&c2, &c_lang, st);
        CHECK(hl_step(&c2, l2, (int)strlen(l2), 0) == c_lang.color[TOK_COMMENT],
              "C: carried-over comment colors start of next line");
    }
    {
        /* keyword lookup: "int" at col 13 of c_src[3] = "int main..." line index 3 col 0 */
        const char *l = c_src[3];
        HLCursor c;
        hl_begin(&c, &c_lang, HL_ST_NORMAL);
        CHECK(hl_step(&c, l, (int)strlen(l), 0) == c_lang.color[TOK_KEYWORD],
              "C: 'int' recognised as keyword");
    }
    {
        /* z80: "start:" at col0 should be a label, not a keyword */
        const char *l = z80_src[0];
        HLCursor c;
        hl_begin(&c, &z80_lang, HL_ST_NORMAL);
        CHECK(hl_step(&c, l, (int)strlen(l), 0) == z80_lang.color[TOK_LABEL],
              "Z80: 'start:' at column 0 recognised as label");
    }
    {
        /* z80: "A" register after "LD " keyword */
        const char *l = z80_src[0];
        int pos = (int)(strstr(l, "A,") - l); /* index of 'A' */
        HLCursor c;
        int i;
        hl_begin(&c, &z80_lang, HL_ST_NORMAL);
        for (i = 0; i <= pos; i++) {
            uint8_t a = hl_step(&c, l, (int)strlen(l), i);
            if (i == pos) CHECK(a == z80_lang.color[TOK_REGISTER],
                                 "Z80: 'A' recognised as register");
        }
    }
    {
        /* z80: $1F should be a number */
        const char *l = z80_src[0];
        int pos = (int)(strstr(l, "$1F") - l);
        HLCursor c;
        int i;
        hl_begin(&c, &z80_lang, HL_ST_NORMAL);
        for (i = 0; i <= pos; i++) {
            uint8_t a = hl_step(&c, l, (int)strlen(l), i);
            if (i == pos) CHECK(a == z80_lang.color[TOK_NUMBER],
                                 "Z80: '$1F' recognised as number");
        }
    }
    {
        /* z80: %1010 should be a number */
        const char *l = z80_src[3];
        int pos = (int)(strstr(l, "%1010") - l);
        HLCursor c;
        int i;
        hl_begin(&c, &z80_lang, HL_ST_NORMAL);
        for (i = 0; i <= pos; i++) {
            uint8_t a = hl_step(&c, l, (int)strlen(l), i);
            if (i == pos) CHECK(a == z80_lang.color[TOK_NUMBER],
                                 "Z80: '%1010' recognised as binary number");
        }
    }

    printf("\n%s\n", fails == 0 ? "ALL CHECKS PASSED" : "SOME CHECKS FAILED");
    return fails == 0 ? 0 : 1;
}
