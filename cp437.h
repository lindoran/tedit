/* SPDX-License-Identifier: MIT
 * cp437.h  --  CP437 <-> Unicode translation for thin-vga terminal emulator
 *
 * Two directions:
 *   unicode_to_cp437(uint32_t ucs4) -> uint8_t   (terminal output: lib gives UCS-4, vgaterm needs CP437)
 *   cp437_to_ucs4[256]                            (clipboard copy: vgaterm_mem has CP437, X11 wants UTF-8)
 *
 * ASCII passthrough (0x20-0x7E): codepoint == byte value, no table lookup needed.
 * Unmapped codepoints fall back to '?' (0x3F).
 *
 * Entries marked CHECK: mixed single/double box drawing and a few unusual glyphs.
 * Verify visually against font_vga.h bitmaps if they look wrong at runtime.
 *
 * Greek mu note: both U+00B5 (MICRO SIGN) and U+03BC (GREEK SMALL LETTER MU)
 * map to CP437 0xE6. The forward table uses U+00B5 (official mapping); the
 * reverse table includes both so either codepoint round-trips correctly.
 */

#ifndef CP437_H
#define CP437_H

#include <stdint.h>
#include <stddef.h>   /* size_t for bsearch */
#include <stdlib.h>   /* bsearch            */

/* -------------------------------------------------------------------------
 * Forward table: CP437 byte -> UCS-4 codepoint
 *
 * Use for clipboard copy: iterate vgaterm_mem, look up each character byte,
 * then encode the resulting codepoint as UTF-8.
 * ------------------------------------------------------------------------- */
static const uint32_t cp437_to_ucs4[256] = {
    /* 0x00 */ 0x0000,  /* NUL / blank cell                  */
    /* 0x01 */ 0x263A,  /* ☺ WHITE SMILING FACE              */
    /* 0x02 */ 0x263B,  /* ☻ BLACK SMILING FACE              */
    /* 0x03 */ 0x2665,  /* ♥ BLACK HEART SUIT                */
    /* 0x04 */ 0x2666,  /* ♦ BLACK DIAMOND SUIT              */
    /* 0x05 */ 0x2663,  /* ♣ BLACK CLUB SUIT                 */
    /* 0x06 */ 0x2660,  /* ♠ BLACK SPADE SUIT                */
    /* 0x07 */ 0x2022,  /* • BULLET                          */
    /* 0x08 */ 0x25D8,  /* ◘ INVERSE BULLET                  */
    /* 0x09 */ 0x25CB,  /* ○ WHITE CIRCLE                    */
    /* 0x0A */ 0x25D9,  /* ◙ INVERSE WHITE CIRCLE            */
    /* 0x0B */ 0x2642,  /* ♂ MALE SIGN                       */
    /* 0x0C */ 0x2640,  /* ♀ FEMALE SIGN                     */
    /* 0x0D */ 0x266A,  /* ♪ EIGHTH NOTE                     */
    /* 0x0E */ 0x266B,  /* ♫ BEAMED EIGHTH NOTES             */
    /* 0x0F */ 0x263C,  /* ☼ WHITE SUN WITH RAYS             */
    /* 0x10 */ 0x25BA,  /* ► BLACK RIGHT-POINTING POINTER    */
    /* 0x11 */ 0x25C4,  /* ◄ BLACK LEFT-POINTING POINTER     */
    /* 0x12 */ 0x2195,  /* ↕ UP DOWN ARROW                   */
    /* 0x13 */ 0x203C,  /* ‼ DOUBLE EXCLAMATION MARK         */
    /* 0x14 */ 0x00B6,  /* ¶ PILCROW SIGN                    */
    /* 0x15 */ 0x00A7,  /* § SECTION SIGN                    */
    /* 0x16 */ 0x25AC,  /* ▬ BLACK RECTANGLE                 */
    /* 0x17 */ 0x21A8,  /* ↨ UP DOWN ARROW WITH BASE         */
    /* 0x18 */ 0x2191,  /* ↑ UPWARDS ARROW                   */
    /* 0x19 */ 0x2193,  /* ↓ DOWNWARDS ARROW                 */
    /* 0x1A */ 0x2192,  /* → RIGHTWARDS ARROW                */
    /* 0x1B */ 0x2190,  /* ← LEFTWARDS ARROW                 */
    /* 0x1C */ 0x221F,  /* ∟ RIGHT ANGLE                     */
    /* 0x1D */ 0x2194,  /* ↔ LEFT RIGHT ARROW                */
    /* 0x1E */ 0x25B2,  /* ▲ BLACK UP-POINTING TRIANGLE      */
    /* 0x1F */ 0x25BC,  /* ▼ BLACK DOWN-POINTING TRIANGLE    */
    /* 0x20-0x7E: ASCII -- codepoint equals byte value       */
    0x0020, 0x0021, 0x0022, 0x0023, 0x0024, 0x0025, 0x0026, 0x0027,
    0x0028, 0x0029, 0x002A, 0x002B, 0x002C, 0x002D, 0x002E, 0x002F,
    0x0030, 0x0031, 0x0032, 0x0033, 0x0034, 0x0035, 0x0036, 0x0037,
    0x0038, 0x0039, 0x003A, 0x003B, 0x003C, 0x003D, 0x003E, 0x003F,
    0x0040, 0x0041, 0x0042, 0x0043, 0x0044, 0x0045, 0x0046, 0x0047,
    0x0048, 0x0049, 0x004A, 0x004B, 0x004C, 0x004D, 0x004E, 0x004F,
    0x0050, 0x0051, 0x0052, 0x0053, 0x0054, 0x0055, 0x0056, 0x0057,
    0x0058, 0x0059, 0x005A, 0x005B, 0x005C, 0x005D, 0x005E, 0x005F,
    0x0060, 0x0061, 0x0062, 0x0063, 0x0064, 0x0065, 0x0066, 0x0067,
    0x0068, 0x0069, 0x006A, 0x006B, 0x006C, 0x006D, 0x006E, 0x006F,
    0x0070, 0x0071, 0x0072, 0x0073, 0x0074, 0x0075, 0x0076, 0x0077,
    0x0078, 0x0079, 0x007A, 0x007B, 0x007C, 0x007D, 0x007E,
    /* 0x7F */ 0x2302,  /* ⌂ HOUSE                           */
    /* --- Latin extended (0x80-0xAF) ---                     */
    /* 0x80 */ 0x00C7,  /* Ç */
    /* 0x81 */ 0x00FC,  /* ü */
    /* 0x82 */ 0x00E9,  /* é */
    /* 0x83 */ 0x00E2,  /* â */
    /* 0x84 */ 0x00E4,  /* ä */
    /* 0x85 */ 0x00E0,  /* à */
    /* 0x86 */ 0x00E5,  /* å */
    /* 0x87 */ 0x00E7,  /* ç */
    /* 0x88 */ 0x00EA,  /* ê */
    /* 0x89 */ 0x00EB,  /* ë */
    /* 0x8A */ 0x00E8,  /* è */
    /* 0x8B */ 0x00EF,  /* ï */
    /* 0x8C */ 0x00EE,  /* î */
    /* 0x8D */ 0x00EC,  /* ì */
    /* 0x8E */ 0x00C4,  /* Ä */
    /* 0x8F */ 0x00C5,  /* Å */
    /* 0x90 */ 0x00C9,  /* É */
    /* 0x91 */ 0x00E6,  /* æ */
    /* 0x92 */ 0x00C6,  /* Æ */
    /* 0x93 */ 0x00F4,  /* ô */
    /* 0x94 */ 0x00F6,  /* ö */
    /* 0x95 */ 0x00F2,  /* ò */
    /* 0x96 */ 0x00FB,  /* û */
    /* 0x97 */ 0x00F9,  /* ù */
    /* 0x98 */ 0x00FF,  /* ÿ */
    /* 0x99 */ 0x00D6,  /* Ö */
    /* 0x9A */ 0x00DC,  /* Ü */
    /* 0x9B */ 0x00A2,  /* ¢ */
    /* 0x9C */ 0x00A3,  /* £ */
    /* 0x9D */ 0x00A5,  /* ¥ */
    /* 0x9E */ 0x20A7,  /* ₧ PESETA SIGN                     CHECK */
    /* 0x9F */ 0x0192,  /* ƒ */
    /* 0xA0 */ 0x00E1,  /* á */
    /* 0xA1 */ 0x00ED,  /* í */
    /* 0xA2 */ 0x00F3,  /* ó */
    /* 0xA3 */ 0x00FA,  /* ú */
    /* 0xA4 */ 0x00F1,  /* ñ */
    /* 0xA5 */ 0x00D1,  /* Ñ */
    /* 0xA6 */ 0x00AA,  /* ª */
    /* 0xA7 */ 0x00BA,  /* º */
    /* 0xA8 */ 0x00BF,  /* ¿ */
    /* 0xA9 */ 0x2310,  /* ⌐ REVERSED NOT SIGN               */
    /* 0xAA */ 0x00AC,  /* ¬ NOT SIGN                        */
    /* 0xAB */ 0x00BD,  /* ½ */
    /* 0xAC */ 0x00BC,  /* ¼ */
    /* 0xAD */ 0x00A1,  /* ¡ */
    /* 0xAE */ 0x00AB,  /* « */
    /* 0xAF */ 0x00BB,  /* » */
    /* --- Shading + box drawing (0xB0-0xDA) ---              */
    /* 0xB0 */ 0x2591,  /* ░ LIGHT SHADE                     */
    /* 0xB1 */ 0x2592,  /* ▒ MEDIUM SHADE                    */
    /* 0xB2 */ 0x2593,  /* ▓ DARK SHADE                      */
    /* 0xB3 */ 0x2502,  /* │ BOX LIGHT VERTICAL              */
    /* 0xB4 */ 0x2524,  /* ┤ BOX LIGHT VERT+LEFT             */
    /* 0xB5 */ 0x2561,  /* ╡ BOX VERT-SINGLE LEFT-DOUBLE     CHECK */
    /* 0xB6 */ 0x2562,  /* ╢ BOX VERT-DOUBLE LEFT-SINGLE     CHECK */
    /* 0xB7 */ 0x2556,  /* ╖ BOX DOWN-DOUBLE LEFT-SINGLE     CHECK */
    /* 0xB8 */ 0x2555,  /* ╕ BOX DOWN-SINGLE LEFT-DOUBLE     CHECK */
    /* 0xB9 */ 0x2563,  /* ╣ BOX DOUBLE VERT+LEFT            */
    /* 0xBA */ 0x2551,  /* ║ BOX DOUBLE VERTICAL             */
    /* 0xBB */ 0x2557,  /* ╗ BOX DOUBLE DOWN+LEFT            */
    /* 0xBC */ 0x255D,  /* ╝ BOX DOUBLE UP+LEFT              */
    /* 0xBD */ 0x255C,  /* ╜ BOX UP-DOUBLE LEFT-SINGLE       CHECK */
    /* 0xBE */ 0x255B,  /* ╛ BOX UP-SINGLE LEFT-DOUBLE       CHECK */
    /* 0xBF */ 0x2510,  /* ┐ BOX LIGHT DOWN+LEFT             */
    /* 0xC0 */ 0x2514,  /* └ BOX LIGHT UP+RIGHT              */
    /* 0xC1 */ 0x2534,  /* ┴ BOX LIGHT UP+HORIZ              */
    /* 0xC2 */ 0x252C,  /* ┬ BOX LIGHT DOWN+HORIZ            */
    /* 0xC3 */ 0x251C,  /* ├ BOX LIGHT VERT+RIGHT            */
    /* 0xC4 */ 0x2500,  /* ─ BOX LIGHT HORIZONTAL            */
    /* 0xC5 */ 0x253C,  /* ┼ BOX LIGHT VERT+HORIZ            */
    /* 0xC6 */ 0x255E,  /* ╞ BOX VERT-SINGLE RIGHT-DOUBLE    CHECK */
    /* 0xC7 */ 0x255F,  /* ╟ BOX VERT-DOUBLE RIGHT-SINGLE    CHECK */
    /* 0xC8 */ 0x255A,  /* ╚ BOX DOUBLE UP+RIGHT             */
    /* 0xC9 */ 0x2554,  /* ╔ BOX DOUBLE DOWN+RIGHT           */
    /* 0xCA */ 0x2569,  /* ╩ BOX DOUBLE UP+HORIZ             */
    /* 0xCB */ 0x2566,  /* ╦ BOX DOUBLE DOWN+HORIZ           */
    /* 0xCC */ 0x2560,  /* ╠ BOX DOUBLE VERT+RIGHT           */
    /* 0xCD */ 0x2550,  /* ═ BOX DOUBLE HORIZONTAL           */
    /* 0xCE */ 0x256C,  /* ╬ BOX DOUBLE VERT+HORIZ           */
    /* 0xCF */ 0x2567,  /* ╧ BOX UP-SINGLE HORIZ-DOUBLE      CHECK */
    /* 0xD0 */ 0x2568,  /* ╨ BOX UP-DOUBLE HORIZ-SINGLE      CHECK */
    /* 0xD1 */ 0x2564,  /* ╤ BOX DOWN-SINGLE HORIZ-DOUBLE    CHECK */
    /* 0xD2 */ 0x2565,  /* ╥ BOX DOWN-DOUBLE HORIZ-SINGLE    CHECK */
    /* 0xD3 */ 0x2559,  /* ╙ BOX UP-DOUBLE RIGHT-SINGLE      CHECK */
    /* 0xD4 */ 0x2558,  /* ╘ BOX UP-SINGLE RIGHT-DOUBLE      CHECK */
    /* 0xD5 */ 0x2552,  /* ╒ BOX DOWN-SINGLE RIGHT-DOUBLE    CHECK */
    /* 0xD6 */ 0x2553,  /* ╓ BOX DOWN-DOUBLE RIGHT-SINGLE    CHECK */
    /* 0xD7 */ 0x256B,  /* ╫ BOX VERT-DOUBLE HORIZ-SINGLE    CHECK */
    /* 0xD8 */ 0x256A,  /* ╪ BOX VERT-SINGLE HORIZ-DOUBLE    CHECK */
    /* 0xD9 */ 0x2518,  /* ┘ BOX LIGHT UP+LEFT               */
    /* 0xDA */ 0x250C,  /* ┌ BOX LIGHT DOWN+RIGHT            */
    /* --- Block elements (0xDB-0xDF) ---                     */
    /* 0xDB */ 0x2588,  /* █ FULL BLOCK                      */
    /* 0xDC */ 0x2584,  /* ▄ LOWER HALF BLOCK                */
    /* 0xDD */ 0x258C,  /* ▌ LEFT HALF BLOCK                 */
    /* 0xDE */ 0x2590,  /* ▐ RIGHT HALF BLOCK                */
    /* 0xDF */ 0x2580,  /* ▀ UPPER HALF BLOCK                */
    /* --- Greek / math (0xE0-0xFF) ---                       */
    /* 0xE0 */ 0x03B1,  /* α GREEK SMALL LETTER ALPHA        */
    /* 0xE1 */ 0x00DF,  /* ß LATIN SMALL LETTER SHARP S      */
    /* 0xE2 */ 0x0393,  /* Γ GREEK CAPITAL LETTER GAMMA      */
    /* 0xE3 */ 0x03C0,  /* π GREEK SMALL LETTER PI           */
    /* 0xE4 */ 0x03A3,  /* Σ GREEK CAPITAL LETTER SIGMA      */
    /* 0xE5 */ 0x03C3,  /* σ GREEK SMALL LETTER SIGMA        */
    /* 0xE6 */ 0x00B5,  /* µ MICRO SIGN (also: U+03BC μ)     CHECK */
    /* 0xE7 */ 0x03C4,  /* τ GREEK SMALL LETTER TAU          */
    /* 0xE8 */ 0x03A6,  /* Φ GREEK CAPITAL LETTER PHI        */
    /* 0xE9 */ 0x0398,  /* Θ GREEK CAPITAL LETTER THETA      */
    /* 0xEA */ 0x03A9,  /* Ω GREEK CAPITAL LETTER OMEGA      */
    /* 0xEB */ 0x03B4,  /* δ GREEK SMALL LETTER DELTA        */
    /* 0xEC */ 0x221E,  /* ∞ INFINITY                        */
    /* 0xED */ 0x03C6,  /* φ GREEK SMALL LETTER PHI          CHECK */
    /* 0xEE */ 0x03B5,  /* ε GREEK SMALL LETTER EPSILON      */
    /* 0xEF */ 0x2229,  /* ∩ INTERSECTION                    */
    /* 0xF0 */ 0x2261,  /* ≡ IDENTICAL TO                    */
    /* 0xF1 */ 0x00B1,  /* ± PLUS-MINUS SIGN                 */
    /* 0xF2 */ 0x2265,  /* ≥ GREATER-THAN OR EQUAL TO        */
    /* 0xF3 */ 0x2264,  /* ≤ LESS-THAN OR EQUAL TO           */
    /* 0xF4 */ 0x2320,  /* ⌠ TOP HALF INTEGRAL               */
    /* 0xF5 */ 0x2321,  /* ⌡ BOTTOM HALF INTEGRAL            */
    /* 0xF6 */ 0x00F7,  /* ÷ DIVISION SIGN                   */
    /* 0xF7 */ 0x2248,  /* ≈ ALMOST EQUAL TO                 */
    /* 0xF8 */ 0x00B0,  /* ° DEGREE SIGN                     */
    /* 0xF9 */ 0x2219,  /* ∙ BULLET OPERATOR                 */
    /* 0xFA */ 0x00B7,  /* · MIDDLE DOT                      */
    /* 0xFB */ 0x221A,  /* √ SQUARE ROOT                     */
    /* 0xFC */ 0x207F,  /* ⁿ SUPERSCRIPT LATIN SMALL LETTER N */
    /* 0xFD */ 0x00B2,  /* ² SUPERSCRIPT TWO                 */
    /* 0xFE */ 0x25A0,  /* ■ BLACK SQUARE                    */
    /* 0xFF */ 0x00A0,  /* (NO-BREAK SPACE)                  */
};

/* -------------------------------------------------------------------------
 * Reverse table: UCS-4 -> CP437 byte
 *
 * Sorted by ucs4 for bsearch.
 * ASCII 0x20-0x7E: handled inline by unicode_to_cp437() -- no entry here.
 * NUL (U+0000): handled inline as space (0x20).
 * Unmapped codepoints: fall back to '?' (0x3F).
 *
 * Greek mu: both U+00B5 (MICRO SIGN) and U+03BC (GREEK SMALL LETTER MU)
 * are present; either maps to 0xE6.
 * ------------------------------------------------------------------------- */
typedef struct { uint32_t ucs4; uint8_t cp437; } Cp437Pair;

static const Cp437Pair ucs4_to_cp437_map[] = {
    { 0x00A0, 0xFF },  /* NO-BREAK SPACE */
    { 0x00A1, 0xAD },  /* ¡ */
    { 0x00A2, 0x9B },  /* ¢ */
    { 0x00A3, 0x9C },  /* £ */
    { 0x00A5, 0x9D },  /* ¥ */
    { 0x00A7, 0x15 },  /* § */
    { 0x00AA, 0xA6 },  /* ª */
    { 0x00AB, 0xAE },  /* « */
    { 0x00AC, 0xAA },  /* ¬ */
    { 0x00B0, 0xF8 },  /* ° */
    { 0x00B1, 0xF1 },  /* ± */
    { 0x00B2, 0xFD },  /* ² */
    { 0x00B5, 0xE6 },  /* µ MICRO SIGN */
    { 0x00B6, 0x14 },  /* ¶ */
    { 0x00B7, 0xFA },  /* · */
    { 0x00BA, 0xA7 },  /* º */
    { 0x00BB, 0xAF },  /* » */
    { 0x00BC, 0xAC },  /* ¼ */
    { 0x00BD, 0xAB },  /* ½ */
    { 0x00BF, 0xA8 },  /* ¿ */
    { 0x00C4, 0x8E },  /* Ä */
    { 0x00C5, 0x8F },  /* Å */
    { 0x00C6, 0x92 },  /* Æ */
    { 0x00C7, 0x80 },  /* Ç */
    { 0x00C9, 0x90 },  /* É */
    { 0x00D1, 0xA5 },  /* Ñ */
    { 0x00D6, 0x99 },  /* Ö */
    { 0x00DC, 0x9A },  /* Ü */
    { 0x00DF, 0xE1 },  /* ß */
    { 0x00E0, 0x85 },  /* à */
    { 0x00E1, 0xA0 },  /* á */
    { 0x00E2, 0x83 },  /* â */
    { 0x00E4, 0x84 },  /* ä */
    { 0x00E5, 0x86 },  /* å */
    { 0x00E6, 0x91 },  /* æ */
    { 0x00E7, 0x87 },  /* ç */
    { 0x00E8, 0x8A },  /* è */
    { 0x00E9, 0x82 },  /* é */
    { 0x00EA, 0x88 },  /* ê */
    { 0x00EB, 0x89 },  /* ë */
    { 0x00EC, 0x8D },  /* ì */
    { 0x00ED, 0xA1 },  /* í */
    { 0x00EE, 0x8C },  /* î */
    { 0x00EF, 0x8B },  /* ï */
    { 0x00F1, 0xA4 },  /* ñ */
    { 0x00F2, 0x95 },  /* ò */
    { 0x00F3, 0xA2 },  /* ó */
    { 0x00F4, 0x93 },  /* ô */
    { 0x00F6, 0x94 },  /* ö */
    { 0x00F7, 0xF6 },  /* ÷ */
    { 0x00F9, 0x97 },  /* ù */
    { 0x00FA, 0xA3 },  /* ú */
    { 0x00FB, 0x96 },  /* û */
    { 0x00FC, 0x81 },  /* ü */
    { 0x00FF, 0x98 },  /* ÿ */
    { 0x0192, 0x9F },  /* ƒ */
    { 0x0393, 0xE2 },  /* Γ */
    { 0x0398, 0xE9 },  /* Θ */
    { 0x03A3, 0xE4 },  /* Σ */
    { 0x03A6, 0xE8 },  /* Φ */
    { 0x03A9, 0xEA },  /* Ω */
    { 0x03B1, 0xE0 },  /* α */
    { 0x03B4, 0xEB },  /* δ */
    { 0x03B5, 0xEE },  /* ε */
    { 0x03BC, 0xE6 },  /* μ GREEK SMALL LETTER MU (alias for 0x00B5) */
    { 0x03C0, 0xE3 },  /* π */
    { 0x03C3, 0xE5 },  /* σ */
    { 0x03C4, 0xE7 },  /* τ */
    { 0x03C6, 0xED },  /* φ */
    { 0x2022, 0x07 },  /* • BULLET */
    { 0x203C, 0x13 },  /* ‼ */
    { 0x207F, 0xFC },  /* ⁿ */
    { 0x20A7, 0x9E },  /* ₧ PESETA SIGN */
    { 0x2190, 0x1B },  /* ← */
    { 0x2191, 0x18 },  /* ↑ */
    { 0x2192, 0x1A },  /* → */
    { 0x2193, 0x19 },  /* ↓ */
    { 0x2194, 0x1D },  /* ↔ */
    { 0x2195, 0x12 },  /* ↕ */
    { 0x21A8, 0x17 },  /* ↨ */
    { 0x2219, 0xF9 },  /* ∙ BULLET OPERATOR */
    { 0x221A, 0xFB },  /* √ */
    { 0x221E, 0xEC },  /* ∞ */
    { 0x221F, 0x1C },  /* ∟ */
    { 0x2229, 0xEF },  /* ∩ */
    { 0x2248, 0xF7 },  /* ≈ */
    { 0x2261, 0xF0 },  /* ≡ */
    { 0x2264, 0xF3 },  /* ≤ */
    { 0x2265, 0xF2 },  /* ≥ */
    { 0x2302, 0x7F },  /* ⌂ */
    { 0x2310, 0xA9 },  /* ⌐ */
    { 0x2320, 0xF4 },  /* ⌠ */
    { 0x2321, 0xF5 },  /* ⌡ */
    { 0x2500, 0xC4 },  /* ─ */
    { 0x2502, 0xB3 },  /* │ */
    { 0x250C, 0xDA },  /* ┌ */
    { 0x2510, 0xBF },  /* ┐ */
    { 0x2514, 0xC0 },  /* └ */
    { 0x2518, 0xD9 },  /* ┘ */
    { 0x251C, 0xC3 },  /* ├ */
    { 0x2524, 0xB4 },  /* ┤ */
    { 0x252C, 0xC2 },  /* ┬ */
    { 0x2534, 0xC1 },  /* ┴ */
    { 0x253C, 0xC5 },  /* ┼ */
    { 0x2550, 0xCD },  /* ═ */
    { 0x2551, 0xBA },  /* ║ */
    { 0x2552, 0xD5 },  /* ╒ */
    { 0x2553, 0xD6 },  /* ╓ */
    { 0x2554, 0xC9 },  /* ╔ */
    { 0x2555, 0xB8 },  /* ╕ */
    { 0x2556, 0xB7 },  /* ╖ */
    { 0x2557, 0xBB },  /* ╗ */
    { 0x2558, 0xD4 },  /* ╘ */
    { 0x2559, 0xD3 },  /* ╙ */
    { 0x255A, 0xC8 },  /* ╚ */
    { 0x255B, 0xBE },  /* ╛ */
    { 0x255C, 0xBD },  /* ╜ */
    { 0x255D, 0xBC },  /* ╝ */
    { 0x255E, 0xC6 },  /* ╞ */
    { 0x255F, 0xC7 },  /* ╟ */
    { 0x2560, 0xCC },  /* ╠ */
    { 0x2561, 0xB5 },  /* ╡ */
    { 0x2562, 0xB6 },  /* ╢ */
    { 0x2563, 0xB9 },  /* ╣ */
    { 0x2564, 0xD1 },  /* ╤ */
    { 0x2565, 0xD2 },  /* ╥ */
    { 0x2566, 0xCB },  /* ╦ */
    { 0x2567, 0xCF },  /* ╧ */
    { 0x2568, 0xD0 },  /* ╨ */
    { 0x2569, 0xCA },  /* ╩ */
    { 0x256A, 0xD8 },  /* ╪ */
    { 0x256B, 0xD7 },  /* ╫ */
    { 0x256C, 0xCE },  /* ╬ */
    { 0x2580, 0xDF },  /* ▀ */
    { 0x2584, 0xDC },  /* ▄ */
    { 0x2588, 0xDB },  /* █ */
    { 0x258C, 0xDD },  /* ▌ */
    { 0x2590, 0xDE },  /* ▐ */
    { 0x2591, 0xB0 },  /* ░ */
    { 0x2592, 0xB1 },  /* ▒ */
    { 0x2593, 0xB2 },  /* ▓ */
    { 0x25A0, 0xFE },  /* ■ */
    { 0x25AC, 0x16 },  /* ▬ */
    { 0x25B2, 0x1E },  /* ▲ */
    { 0x25BA, 0x10 },  /* ► */
    { 0x25BC, 0x1F },  /* ▼ */
    { 0x25C4, 0x11 },  /* ◄ */
    { 0x25CB, 0x09 },  /* ○ */
    { 0x25D8, 0x08 },  /* ◘ */
    { 0x25D9, 0x0A },  /* ◙ */
    { 0x263A, 0x01 },  /* ☺ */
    { 0x263B, 0x02 },  /* ☻ */
    { 0x263C, 0x0F },  /* ☼ */
    { 0x2640, 0x0C },  /* ♀ */
    { 0x2642, 0x0B },  /* ♂ */
    { 0x2660, 0x06 },  /* ♠ */
    { 0x2663, 0x05 },  /* ♣ */
    { 0x2665, 0x03 },  /* ♥ */
    { 0x2666, 0x04 },  /* ♦ */
    { 0x266A, 0x0D },  /* ♪ */
    { 0x266B, 0x0E },  /* ♫ */
};

#define CP437_MAP_LEN  \
    ((int)(sizeof(ucs4_to_cp437_map) / sizeof(ucs4_to_cp437_map[0])))

/* -------------------------------------------------------------------------
 * unicode_to_cp437  --  convert one UCS-4 codepoint to a CP437 byte.
 *
 * Fast path: NUL -> space, ASCII 0x20-0x7E -> direct cast.
 * Slow path: bsearch over 161-entry reverse table (max 8 comparisons).
 * Fallback:  '?' for anything not in the table.
 * ------------------------------------------------------------------------- */
static int cp437_pair_cmp(const void *key, const void *elem)
{
    uint32_t k = *(const uint32_t *)key;
    uint32_t e = ((const Cp437Pair *)elem)->ucs4;
    return (k > e) - (k < e);
}

static uint8_t unicode_to_cp437(uint32_t ucs4)
{
    const Cp437Pair *p;

    if (ucs4 == 0)
        return 0x20;                  /* empty cell -> space              */
    if (ucs4 >= 0x20 && ucs4 <= 0x7E)
        return (uint8_t)ucs4;         /* ASCII passthrough                */

    p = (const Cp437Pair *)bsearch(
            &ucs4,
            ucs4_to_cp437_map,
            (size_t)CP437_MAP_LEN,
            sizeof(ucs4_to_cp437_map[0]),
            cp437_pair_cmp);

    return p ? p->cp437 : 0x3F;      /* fallback: '?'                    */
}

/* -------------------------------------------------------------------------
 * cp437_to_utf8  --  encode one CP437 byte as UTF-8 into buf[].
 *
 * buf must be at least 4 bytes.  Returns the number of bytes written.
 * Used for clipboard copy: iterate vgaterm_mem, call this per character byte.
 * ------------------------------------------------------------------------- */
static int cp437_to_utf8(uint8_t cp437, char buf[4])
{
    uint32_t ucs4 = cp437_to_ucs4[cp437];

    if (ucs4 == 0) {
        buf[0] = ' ';                 /* blank cell -> space              */
        return 1;
    }
    if (ucs4 < 0x80) {
        buf[0] = (char)ucs4;
        return 1;
    }
    if (ucs4 < 0x800) {
        buf[0] = (char)(0xC0 | (ucs4 >> 6));
        buf[1] = (char)(0x80 | (ucs4 & 0x3F));
        return 2;
    }
    /* All CP437 non-ASCII codepoints fit in 3-byte UTF-8 (max U+266B) */
    buf[0] = (char)(0xE0 | (ucs4 >> 12));
    buf[1] = (char)(0x80 | ((ucs4 >> 6) & 0x3F));
    buf[2] = (char)(0x80 | (ucs4 & 0x3F));
    return 3;
}

#endif /* CP437_H */
