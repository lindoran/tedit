#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""
mkfont.py  --  generate font_vga.h from Bm437_IBM_VGA_8x16.otb

Source font: Ultimate Oldschool PC Font Pack by VileR
  Copyright (C) 2016 VileR, CC BY-SA 4.0
  https://int10h.org/oldschool-pc-fonts/

Usage:
    python3 mkfont.py [path/to/Bm437_IBM_VGA_8x16.otb] > font_vga.h

Requires:
    sudo apt install python3-fonttools
"""

import struct
import sys
import os

# ---------------------------------------------------------------------------
# CP437 byte value -> Unicode code point
# ---------------------------------------------------------------------------
CP437_TO_UNICODE = [
    0x0000, 0x263A, 0x263B, 0x2665, 0x2666, 0x2663, 0x2660, 0x2022,
    0x25D8, 0x25CB, 0x25D9, 0x2642, 0x2640, 0x266A, 0x266B, 0x263C,
    0x25BA, 0x25C4, 0x2195, 0x203C, 0x00B6, 0x00A7, 0x25AC, 0x21A8,
    0x2191, 0x2193, 0x2192, 0x2190, 0x221F, 0x2194, 0x25B2, 0x25BC,
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
    0x0078, 0x0079, 0x007A, 0x007B, 0x007C, 0x007D, 0x007E, 0x2302,
    0x00C7, 0x00FC, 0x00E9, 0x00E2, 0x00E4, 0x00E0, 0x00E5, 0x00E7,
    0x00EA, 0x00EB, 0x00E8, 0x00EF, 0x00EE, 0x00EC, 0x00C4, 0x00C5,
    0x00C9, 0x00E6, 0x00C6, 0x00F4, 0x00F6, 0x00F2, 0x00FB, 0x00F9,
    0x00FF, 0x00D6, 0x00DC, 0x00A2, 0x00A3, 0x00A5, 0x20A7, 0x0192,
    0x00E1, 0x00ED, 0x00F3, 0x00FA, 0x00F1, 0x00D1, 0x00AA, 0x00BA,
    0x00BF, 0x2310, 0x00AC, 0x00BD, 0x00BC, 0x00A1, 0x00AB, 0x00BB,
    0x2591, 0x2592, 0x2593, 0x2502, 0x2524, 0x2561, 0x2562, 0x2556,
    0x2555, 0x2563, 0x2551, 0x2557, 0x255D, 0x255C, 0x255B, 0x2510,
    0x2514, 0x2534, 0x252C, 0x251C, 0x2500, 0x253C, 0x255E, 0x255F,
    0x255A, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550, 0x256C, 0x2567,
    0x2568, 0x2564, 0x2565, 0x2559, 0x2558, 0x2552, 0x2553, 0x256B,
    0x256A, 0x2518, 0x250C, 0x2588, 0x2584, 0x258C, 0x2590, 0x2580,
    0x03B1, 0x00DF, 0x0393, 0x03C0, 0x03A3, 0x03C3, 0x00B5, 0x03C4,
    0x03A6, 0x0398, 0x03A9, 0x03B4, 0x221E, 0x03C6, 0x03B5, 0x2229,
    0x2261, 0x00B1, 0x2265, 0x2264, 0x2320, 0x2321, 0x00F7, 0x2248,
    0x00B0, 0x2219, 0x00B7, 0x221A, 0x207F, 0x00B2, 0x25A0, 0x00A0,
]
assert len(CP437_TO_UNICODE) == 256

# Intentionally blank glyphs — not missing, just empty
INTENTIONAL_BLANKS = {0x0000, 0x0020, 0x00A0}

# ---------------------------------------------------------------------------
# OTB font reader
# ---------------------------------------------------------------------------
def load_otb(path):
    """
    Load an OTB bitmap font. Returns dict: unicode_codepoint -> 16-byte bitmap.
    Exits with a clear error if the file cannot be loaded.
    """
    try:
        from fontTools.ttLib import TTFont
    except ImportError:
        print("ERROR: fonttools is not installed.", file=sys.stderr)
        print("  sudo apt install python3-fonttools", file=sys.stderr)
        sys.exit(1)

    try:
        font = TTFont(path)
    except Exception as e:
        print(f"ERROR: could not open {path}: {e}", file=sys.stderr)
        sys.exit(1)

    if 'EBDT' not in font:
        print(f"ERROR: {path} has no EBDT bitmap table.", file=sys.stderr)
        sys.exit(1)

    cmap = font.getBestCmap()
    if not cmap:
        print(f"ERROR: {path} has no usable cmap.", file=sys.stderr)
        sys.exit(1)

    ebdt_strike = font['EBDT'].strikeData[0]
    result = {}

    for ucp, gname in cmap.items():
        bm = ebdt_strike.get(gname)
        if bm is None:
            continue
        fmt  = bm.getFormat()
        data = bm.imageData
        if fmt == 5:
            if len(data) == 16:
                result[ucp] = bytes(data)
        elif fmt == 2:
            sm        = bm.metrics
            row_bytes = (sm.width + 7) // 8
            rows      = bytearray(16)
            for i in range(min(sm.height, 16)):
                chunk = data[i * row_bytes:(i + 1) * row_bytes]
                if chunk:
                    rows[i] = chunk[0]
            result[ucp] = bytes(rows)

    return result


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
def main():
    if len(sys.argv) > 1:
        otb_path = sys.argv[1]
    else:
        otb_path = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                'Bm437_IBM_VGA_8x16.otb')

    if not os.path.exists(otb_path):
        print(f"ERROR: OTB font not found: {otb_path}", file=sys.stderr)
        print( "  The font file should be in the same directory as this script.", file=sys.stderr)
        print( "  Download from: https://int10h.org/oldschool-pc-fonts/", file=sys.stderr)
        sys.exit(1)

    unicode_bitmaps = load_otb(otb_path)
    print(f"# Loaded {len(unicode_bitmaps)} glyphs from {otb_path}", file=sys.stderr)

    # Build the 256-entry CP437 table
    cp437_bitmaps = []
    missing = []
    for byte_val in range(256):
        ucp = CP437_TO_UNICODE[byte_val]
        bmp = unicode_bitmaps.get(ucp)
        if bmp is None:
            if ucp not in INTENTIONAL_BLANKS:
                missing.append((byte_val, ucp))
            bmp = bytes(16)
        cp437_bitmaps.append(bmp)

    if missing:
        print(f"WARNING: {len(missing)} CP437 glyphs missing from OTB (blank fallback used):",
              file=sys.stderr)
        for bv, ucp in missing:
            print(f"  CP437 0x{bv:02X} = U+{ucp:04X}", file=sys.stderr)
        print("WARNING: box-drawing characters may render incorrectly.", file=sys.stderr)

    # Sanity check: box-drawing distinctness
    def idx(ucp):
        return CP437_TO_UNICODE.index(ucp)
    checks = [
        (0x2500, 0x2550, "─ vs ═"),
        (0x2502, 0x2551, "│ vs ║"),
        (0x2524, 0x2563, "┤ vs ╣"),
        (0x251C, 0x2560, "├ vs ╠"),
    ]
    for a, b, label in checks:
        if cp437_bitmaps[idx(a)] == cp437_bitmaps[idx(b)]:
            print(f"WARNING: {label} (U+{a:04X} / U+{b:04X}) have identical bitmaps — "
                  f"box-drawing will be wrong!", file=sys.stderr)

    # Emit the C header
    print("/* font_vga.h  --  VGA 8x16 CP437 font bitmap table")
    print(" *")
    print(" * Auto-generated by mkfont.py")
    print(" * Source: Bm437_IBM_VGA_8x16.otb")
    print(" *   Ultimate Oldschool PC Font Pack by VileR")
    print(" *   Copyright (C) 2016 VileR, CC BY-SA 4.0")
    print(" *   https://int10h.org/oldschool-pc-fonts/")
    print(" *")
    print(" * Regenerate:")
    print(" *   python3 mkfont.py [path/to/Bm437_IBM_VGA_8x16.otb] > font_vga.h")
    print(" *   Requires: sudo apt install python3-fonttools")
    print(" *")
    print(" * Each row is 8 pixels wide; MSB is the leftmost pixel.")
    print(" * Index with:  vga_font_8x16[cp437_byte][scanline_row]")
    print(" */")
    print()
    print("#ifndef FONT_VGA_H")
    print("#define FONT_VGA_H")
    print()
    print("/* 256 x 16 bytes = 4096 bytes total */")
    print("static const unsigned char vga_font_8x16[256][16] = {")
    for i, bmp in enumerate(cp437_bitmaps):
        ucp = CP437_TO_UNICODE[i]
        hex_bytes = ', '.join(f'0x{b:02X}' for b in bmp)
        print(f"    {{ {hex_bytes} }},  /* 0x{i:02X} U+{ucp:04X} */")
    print("};")
    print()
    print("#endif /* FONT_VGA_H */")


if __name__ == '__main__':
    main()
