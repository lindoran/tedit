/* SPDX-License-Identifier: MIT
 * hl.h  --  syntax highlighting: language definitions and line lexer
 *
 * Languages are loaded from a small CSV definition file at runtime (see
 * hl_load()) so adding or tweaking a dialect never requires a recompile.
 * The lexer is a single-pass per-character state machine driven in
 * lockstep with the render loop -- no parse tree, no side buffer of
 * attributes.  render.c's draw_text_row() calls hl_step() once per
 * character and gets back a VGA_ATTR byte to hand straight to
 * vio_setattr(); the VGA cell itself (char + attr) is the only place
 * the result is stored, and vio's existing dirty-check covers it.
 *
 * All language data (keywords, registers, comment markers) is treated
 * as plain bytes restricted to the lower 128 (ASCII) range.  CP437 and
 * the VGA terminal layer have no concept of Unicode -- nothing here may
 * compare against or emit a multibyte sequence.
 *
 * Sizes are fixed (no malloc) and chosen to comfortably cover ANSI C
 * plus the four target asm dialects (Z80, 6502, 6809, 6800); see the
 * comment above each #define.
 */

#ifndef HL_H
#define HL_H

#include <stdint.h>
#include "buffer.h"

/* 6809+6309 combined is the fattest target (~200 mnemonics + directives). */
#define HL_MAX_KEYWORDS    256
/* Longest realistic entry is an ANSI C keyword ("continue", 8) or a
 * future Pascal-ism ("implementation", 14); 16 incl. NUL is safe. */
#define HL_MAX_KW_LEN       16
/* Z80 is the most register-rich target (~22 incl. IXH/IYL etc). */
#define HL_MAX_REGISTERS    32
#define HL_MAX_REG_LEN        8   /* e.g. "IXH" + NUL */
#define HL_MAX_EXTS           8   /* e.g. ".c", ".h"  */
#define HL_MAX_EXT_LEN        6   /* ".asm" + NUL, etc. */
#define HL_MAX_LANGS         16   /* simultaneous loaded language definitions */
#define HL_MAX_COMMENT_LEN    4   /* longest marker (line, or block open/close) + NUL */
#define HL_NAME_LEN          24   /* display name, e.g. "ansi-c" */

/* Token classes -- one VGA_ATTR is configured per class via the CSV
 * "color" directive. */
typedef enum {
    TOK_TEXT = 0,   /* default / unclassified character  */
    TOK_KEYWORD,    /* language keyword or mnemonic       */
    TOK_REGISTER,   /* CPU register name (asm dialects)   */
    TOK_STRING,     /* "..." or '...' literal             */
    TOK_COMMENT,    /* line or block comment              */
    TOK_NUMBER,     /* numeric literal                    */
    TOK_PREPROC,    /* '#' directive line (C)             */
    TOK_LABEL,      /* column-0 identifier / colon label  */
    TOK_OPERATOR,   /* operators like +, -, *, /, =       */
    TOK_BRACKET,    /* grouping chars: ( ) [ ] { }        */
    TOK_COUNT
} HLTok;

/* Lexer states that must persist across a line boundary.  Everything
 * else (mid-string, mid-number, ...) resolves within one line and is
 * never carried over -- vintage source doesn't have multi-line string
 * literals worth worrying about, but block comments do span lines. */
typedef enum {
    HL_ST_NORMAL = 0,
    HL_ST_BLOCK_COMMENT,
    HL_ST_COUNT
} HLState;

typedef struct {
    char name[HL_NAME_LEN];                  /* "ansi-c", "z80", ...      */
    char ext[HL_MAX_EXTS][HL_MAX_EXT_LEN];    /* ".c", ".h", ...           */
    int  n_ext;

    char keywords[HL_MAX_KEYWORDS][HL_MAX_KW_LEN];   /* kept sorted        */
    int  n_keywords;

    char registers[HL_MAX_REGISTERS][HL_MAX_REG_LEN]; /* kept sorted       */
    int  n_registers;

    char line_comment[HL_MAX_COMMENT_LEN];   /* e.g. "//" or ";"; empty = none      */
    char block_open[HL_MAX_COMMENT_LEN];     /* opening block-comment marker; empty = none */
    char block_close[HL_MAX_COMMENT_LEN];    /* closing block-comment marker; empty = none */

    char preproc_char;      /* if set, this char at column 0 starts a TOK_PREPROC line */
    int  has_labels;        /* column-0 identifiers are TOK_LABEL         */
    int  case_insensitive;  /* keyword/register matching                  */

    char hex_suffix;        /* hex suffix character, e.g. 'h'            */
    char bin_suffix;        /* binary suffix character, e.g. 'b'         */
    char string_quotes[4];  /* configured quote characters, e.g. "\'"   */

    uint8_t color[TOK_COUNT];   /* VGA_ATTR per token class                */
} HLang;

/* Cursor used while walking one line.  Treat as opaque; manipulated only
 * via hl_begin() / hl_step() / hl_end_state() / hl_tok(). */
typedef struct {
    const HLang *lang;
    HLState      state;
    int          run_remaining;  /* chars left in the cached run         */
    uint8_t      run_attr;       /* VGA attr for the cached run          */
    HLTok        run_tok;        /* token class for the cached run       */
} HLCursor;

/* ---- language table --------------------------------------------------- */

/* Load a language definition from a CSV file.  Returns 0 on success, -1
 * on failure (file missing / malformed) -- on failure *lang is zeroed
 * and unusable.  See hl.c's top-of-file comment for the CSV format. */
int hl_load(HLang *lang, const char *path);

/* Pick a loaded language by matching `filename`'s extension against one
 * of `n` candidates in `table`.  Returns the matching HLang*, or NULL if
 * none match (caller should fall back to plain A_TEXT, no highlighting). */
const HLang *hl_match_ext(const HLang *table, int n, const char *filename);

/* ---- per-line lexing ----------------------------------------------------
 *
 * hl_begin() resets a cursor to the start of a line, given the state the
 * line was entered in (HL_ST_NORMAL unless a block comment carried over
 * from a previous line).
 *
 * hl_step() classifies the character at `line[col]` and returns the
 * VGA_ATTR it should be drawn with.  Call once per character, with `col`
 * advancing by exactly 1 each call, matching the column walk already
 * done in draw_text_row().  Internally it looks ahead within `line` to
 * classify a whole run (word, string, comment, number) at once and
 * caches the result, so the per-character cost amortizes to O(1).
 *
 * After the last character of a line has been stepped, hl_end_state()
 * gives the state to store for the *next* line (Editor.hl_line_state[]).
 * ------------------------------------------------------------------- */

void    hl_begin(HLCursor *c, const HLang *lang, HLState start_state);
uint8_t hl_step(HLCursor *c, const char *line, int len, int col);
HLState hl_end_state(const HLCursor *c);
/* Return the token class of the character most recently classified by
 * hl_step().  Call immediately after hl_step() for meaningful results. */
HLTok   hl_tok(const HLCursor *c);
void    hl_update_states(const HLang *lang, TextBuffer *tb, int start_row);

#endif /* HL_H */
