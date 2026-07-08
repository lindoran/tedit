#!/usr/bin/env python3
"""
mkitalic.py  --  Generate font_italic.h for thin-vga

Algorithmically slants every glyph in font_vga.h to produce an italic
variant that matches the VGA font's weight and style exactly.

Algorithm (N=2, circular rotation):
    For row r (0=top, 15=bottom):
        shift = (15 - r) >> 2      ->  0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3
    Each row is rotated RIGHT by 'shift' bits (MSB = leftmost pixel).
    Pixels that fall off the right edge wrap around to the left.
    The bottom stays fixed; the top leans right by 3 pixels — classic italic.

Usage:
    python3 mkitalic.py                        # reads deps/thin-vga/font_vga.h
    python3 mkitalic.py path/to/font_vga.h     # explicit source path
    python3 mkitalic.py --preview              # writes italic_preview.png (needs Pillow)

Output is printed to stdout; redirect to font_italic.h:
    python3 mkitalic.py > deps/thin-vga/font_italic.h
"""

import sys
import os

# ---------------------------------------------------------------------------
# CP437 -> Unicode map (for comments in the output header)
# ---------------------------------------------------------------------------
CP437_UNICODE = [
    0x0000,0x263A,0x263B,0x2665,0x2666,0x2663,0x2660,0x2022,
    0x25D8,0x25CB,0x25D9,0x2642,0x2640,0x266A,0x266B,0x263C,
    0x25BA,0x25C4,0x2195,0x203C,0x00B6,0x00A7,0x25AC,0x21A8,
    0x2191,0x2193,0x2192,0x2190,0x221F,0x2194,0x25B2,0x25BC,
    *range(0x0020, 0x007F),
    0x2302,
    0x00C7,0x00FC,0x00E9,0x00E2,0x00E4,0x00E0,0x00E5,0x00E7,
    0x00EA,0x00EB,0x00E8,0x00EF,0x00EE,0x00EC,0x00C4,0x00C5,
    0x00C9,0x00E6,0x00C6,0x00F4,0x00F6,0x00F2,0x00FB,0x00F9,
    0x00FF,0x00D6,0x00DC,0x00A2,0x00A3,0x00A5,0x20A7,0x0192,
    0x00E1,0x00ED,0x00F3,0x00FA,0x00F1,0x00D1,0x00AA,0x00BA,
    0x00BF,0x2310,0x00AC,0x00BD,0x00BC,0x00A1,0x00AB,0x00BB,
    0x2591,0x2592,0x2593,0x2502,0x2524,0x2561,0x2562,0x2556,
    0x2555,0x2563,0x2551,0x2557,0x255D,0x255C,0x255B,0x2510,
    0x2514,0x2534,0x252C,0x251C,0x2500,0x253C,0x255E,0x255F,
    0x255A,0x2554,0x2569,0x2566,0x2560,0x2550,0x256C,0x2567,
    0x2568,0x2564,0x2565,0x2559,0x2558,0x2552,0x2553,0x256B,
    0x256A,0x2518,0x250C,0x2588,0x2584,0x258C,0x2590,0x2580,
    0x03B1,0x00DF,0x0393,0x03C0,0x03A3,0x03C3,0x00B5,0x03C4,
    0x03A6,0x0398,0x03A9,0x03B4,0x221E,0x03C6,0x03B5,0x2229,
    0x2261,0x00B1,0x2265,0x2264,0x2320,0x2321,0x00F7,0x2248,
    0x00B0,0x2219,0x00B7,0x221A,0x207F,0x00B2,0x25A0,0x00A0,
]

# ---------------------------------------------------------------------------
# Load font_vga.h
# ---------------------------------------------------------------------------
def load_vga_font(path):
    glyphs = []
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line.startswith('{'):
                continue
            nums = line.split('{')[1].split('}')[0]
            vals = [int(x.strip(), 16) for x in nums.split(',')]
            if len(vals) == 16:
                glyphs.append(vals)
    assert len(glyphs) == 256, f"Expected 256 glyphs, got {len(glyphs)}"
    return glyphs

# ---------------------------------------------------------------------------
# Slant algorithm
# ---------------------------------------------------------------------------
def slant_glyph(glyph):
    """
    Slant algorithm using 16-bit workspace:
    1. Expand 8-bit glyph to 16-bit with character in upper byte
    2. Shift right by (15-r)>>2 bits for slant (bits move into lower byte, no wraparound)
    3. Shift left by 1 to align back to left edge
    4. Extract upper byte and return as 8-bit result
    
    This avoids losing bits to wraparound since we have room in the lower byte.
    """
    result = []
    for r, byte in enumerate(glyph):
        shift = (15 - r) >> 2
        
        # Expand to 16 bits: character in upper byte, zeros in lower
        word = byte << 8
        
        # Shift right for slant (bits move right, no wraparound, they go to lower byte)
        word = word >> shift
        
        # Shift left by 1 to align back to left edge
        word = word << 1
        
        # Extract upper byte (the slanted character)
        result.append((word >> 8) & 0xFF)
    
    return result

# ---------------------------------------------------------------------------
# Preview (optional, requires Pillow)
# ---------------------------------------------------------------------------
def save_preview(glyphs, path='italic_preview.png'):
    try:
        from PIL import Image
    except ImportError:
        print("Pillow not available — skipping preview.", file=sys.stderr)
        return

    cols, rows_count = 32, 8
    scale = 3
    img = Image.new('RGB', (cols * 8 * scale, rows_count * 16 * scale),
                    (0x18, 0x18, 0x18))
    pixels = img.load()

    for idx in range(256):
        glyph = glyphs[idx]
        col   = idx % cols
        row   = idx // cols
        bx    = col * 8 * scale
        by    = row * 16 * scale
        for r, byte in enumerate(glyph):
            for b in range(8):
                lit   = bool(byte & (0x80 >> b))
                color = (0xFF, 0xFF, 0xFF) if lit else (0x18, 0x18, 0x18)
                for sy in range(scale):
                    for sx in range(scale):
                        pixels[bx + b*scale + sx, by + r*scale + sy] = color

    img.save(path)
    print(f"Preview saved to {path}", file=sys.stderr)

# ---------------------------------------------------------------------------
# Emit C header
# ---------------------------------------------------------------------------
def emit_header(glyphs):
    print("/* font_italic.h  --  VGA 8x16 italic font bitmap table")
    print(" *")
    print(" * Auto-generated by mkitalic.py")
    print(" * Source: font_vga.h with algorithmic italic slant")
    print(" *")
    print(" * Algorithm: row r rotated right by (15-r)>>2 bits (with wrap-around)")
    print(" *   rows  0- 3  ->  rotate 3 px")
    print(" *   rows  4- 7  ->  rotate 2 px")
    print(" *   rows  8-11  ->  rotate 1 px")
    print(" *   rows 12-15  ->  rotate 0 px  (anchor)")
    print(" *")
    print(" * Regenerate:")
    print(" *   python3 mkitalic.py > deps/thin-vga/font_italic.h")
    print(" */")
    print()
    print("#ifndef FONT_ITALIC_H")
    print("#define FONT_ITALIC_H")
    print()
    print("/* 256 x 16 bytes = 4096 bytes total */")
    print("static const unsigned char vga_font_italic_8x16[256][16] = {")

    for i, glyph in enumerate(glyphs):
        hex_vals = ", ".join(f"0x{v:02X}" for v in glyph)
        u = CP437_UNICODE[i] if i < len(CP437_UNICODE) else 0
        print(f"    {{ {hex_vals} }},  /* 0x{i:02X} U+{u:04X} */")

    print("};")
    print()
    print("#endif /* FONT_ITALIC_H */")

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
def main():
    preview_mode = '--preview' in sys.argv
    args = [a for a in sys.argv[1:] if not a.startswith('--')]

    candidates = [
        args[0] if args else None,
        'deps/thin-vga/font_vga.h',
        'font_vga.h',
        '../thin-vga/font_vga.h',
    ]
    vga_path = next((c for c in candidates if c and os.path.exists(c)), None)
    if vga_path is None:
        print("ERROR: font_vga.h not found; run from the macguffin root directory",
              file=sys.stderr)
        sys.exit(1)

    print(f"Source: {vga_path}", file=sys.stderr)
    base   = load_vga_font(vga_path)
    glyphs = [slant_glyph(g) for g in base]

    if preview_mode:
        save_preview(glyphs)
        return

    emit_header(glyphs)

if __name__ == '__main__':
    main()
