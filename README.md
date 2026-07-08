# tedit

A minimal CP437/VGA text editor built on the **thin-vga** library.

Runs in an 80x25 X11 window with a genuine VGA bitmap font.  No
external dependencies beyond Xlib and the sources bundled here.

I made this to work on a specific project which is done now, if 
its useful to you please enjoy!
---

## Building

```
make
```

Requires a C99 compiler and `libx11-dev`.  All other sources (vgaterm,
vio, the VGA font) are bundled alongside the editor modules.

```
sudo make install
```

Install into path (typically)

---

## Usage

```
./tedit [--scale 1|2|4] [file]
```

If a filename is given the file is loaded immediately.  Without one,
tedit starts with an empty buffer.

`--scale N` (also `-s N` or `-scale N`) sets the initial display scale:
`1` for 640x400, `2` for 1280x800, `4` for 2560x1600.  The default is
`1`.  Scale can also be changed at any time from the keyboard or console
(see below).

---

## Keys

| Key | Action |
|-----|--------|
| **Arrows** | Move cursor |
| **Home** | Jump to start of line |
| **End** | Jump to end of line |
| **Ctrl+Home** | Jump to top of document |
| **Ctrl+End** | Jump to bottom of document |
| **Page Up/Down** | Move a full screen at a time |
| **Tab** | Insert spaces to the next tab stop |
| **Shift+Tab** | Remove up to one tab-stop of leading indentation |
| **Enter** | New line, inheriting current line's leading indentation |
| **Backspace** | Delete left; snaps to previous tab stop in leading whitespace |
| **Delete** | Delete right; joins the next line if at end of line |
| **Insert** | Toggle Insert / Overwrite (Insert is default) |
| **Shift+Arrow** | Extend selection |
| **Ctrl+S** | Quick Save document |
| **Ctrl+C** | Copy selection to clipboard |
| **Ctrl+X** | Cut selection |
| **Ctrl+V** | Paste from clipboard |
| **Ctrl+Z** | Undo |
| **Ctrl+Y** | Redo |
| **Ctrl+1** | Set display scale to 1x (640x400) |
| **Ctrl+2** | Set display scale to 2x (1280x800) |
| **Ctrl+4** | Set display scale to 4x (2560x1600) |
| **Esc** | Open the command console (see below) |
| **Ctrl+Q** | Quit (prompts if unsaved changes exist) |

Navigation is clamped to the buffer - you cannot move past the first or
last line, or past the end of a line.  Long lines scroll horizontally;
there is no word wrap.

---

## Mouse

| Action | Effect |
|--------|--------|
| **Click** | Move cursor to that cell, inside palets this behavior selects a list item, in the connsole this runs the console command|
| **Shift+Click** | Extend the current selection to that cell |
| **Click and drag** | Select a range of text |
| **Scroll wheel** | Scroll the view three lines at a time, inside palets this behavior interacts with lists |
| **Right Click** | open command pallet



---

## Command Console

Press **Esc** to open the console.  It appears in place of the status bar at the bottom of the screen.  
Type a command and press **Enter**; press **Esc** again (with an empty prompt) to close it.


Type `?` or `help` to open the **command palette** - a filtered list of all available commands.  Type to narrow the list; **Enter** selects. You can scroll and click this menu using the scroll wheel and the mouse.  You can bring this up by clicking the right mouse button.

### File commands

| Command | Action |
|---------|--------|
| `s` / `save` / `w [file]` | Save. Uses the current filename if omitted. |
| `save as <file>` / `w <file>` | Save to a new filename. |
| `o` / `open` / `e <file>` | Open a file (refuses if there are unsaved changes). |
| `o! <file>` | Open, discarding unsaved changes. |
| `new` | New empty buffer (refuses if unsaved). |
| `new!` | New empty buffer, discarding unsaved changes. |
| `wq [file]` / `x` | Save then quit. |
| `q` / `quit` | Quit (refuses if unsaved). |
| `q!` | Quit, discarding unsaved changes. |

### Find / Replace

| Command | Action |
|---------|--------|
| `find <text>` | Find next occurrence of text. |
| `next` / `find-next` | Repeat last find. |
| `replace <old> <new>` | Replace current match or find and replace next. |
| `replace-all <old> <new>` / `rall` | Replace all occurrences. |

### Settings

| Command | Action |
|---------|--------|
| `tab <n>` / `ts <n>` | Set tab size to n (1-16). Default is **4**. |
| `ln` / `linenum` / `numbers` | Toggle the line-number gutter on/off. |
| `tabs` / `hardtabs` | Switch to hard-tab mode (stores `\t` characters, shown as `»`). |
| `spaces` / `softtabs` | Switch to soft-tab mode (stores spaces). Default. |
| `lf` / `unix` | Use Unix (LF) line endings on save. |
| `crlf` / `dos` | Use DOS (CRLF) line endings on save. |
| `enc utf8` / `encoding utf8` | Save as Unicode UTF-8. |
| `enc cp437` / `encoding cp437` | Save as raw CP437 bytes. |
| `enc ascii` / `encoding ascii` | Save as 7-bit ASCII (non-ASCII replaced with `?`). |
| `enc` | Report the current encoding. |
| `syntax <file>` | Load a syntax highlighting definition* (e.g. `ansi-c`). |
| `syntax off` | Disable syntax highlighting. |
| `scale <n>` / `zoom <n>` / `sz <n>` | Set display scale to 1, 2, or 4. Omit `n` to report current. |
| `coalesce <ms>` / `undo_coalesce <ms>` | Set undo-coalesce timeout in milliseconds (0-60000). Omit to report current. |
| `?` / `help` | Open the command palette. |

*=this will search for the definition stored at `/usr/local/share/tedit` you do not have to enter the extension of the file.

### Spell Check

```
spell           check all text in the buffer
spell code      check only strings and comments (requires syntax highlighting)
```

Requires `ispell` or `aspell` to be installed.  If neither is found,
tedit shows a helpful error message and does nothing.

While the spell-check dialog is visible:

| Key | Action |
|-----|--------|
| **1**-**7** | Replace with the Nth suggestion |
| **r** | Replace with custom text (type then Enter) |
| **i** | Ignore this occurrence (for the current session) |
| **a** | Add word to your personal dictionary |
| **n** / **]** | Skip to the next misspelling |
| **Esc** | Stop the spell check |

### Character Map

```
charmap         (alias: chars)
```

Opens a 32x8 grid showing all 256 CP437 glyphs.  Useful for inserting
box-drawing characters, block elements, and other CP437 symbols.

the pallet shows but limits your access to characters that fall into the
control code segment because these really aren't ment to be put into a 
text file (though they can be) they may cause unpredictable behavior and 
rendering.

While the character-map dialog is visible:

| Key | Action |
|-----|--------|
| **Arrows** | Move the selection cursor |
| **Home** / **End** | Jump to start / end of the current row |
| **Ctrl+Home** / **Ctrl+End** | Jump to glyph 0x00 / 0xFF |
| **Page Up** / **Page Down** | Move 4 rows up / down |
| **Type a character** | Jump cursor to that character's code point |
| **Enter** / **Space** | Insert the selected glyph and close |
| **Esc** | Close without inserting |
| **Mouse click** | Insert the clicked glyph (clicking outside the box closes it) |

---

## Encoding

tedit stores text internally as CP437 bytes.  On file operations it
translates at the boundary:

**Loading** - the file is scanned before any line is split.  If valid
UTF-8 multi-byte sequences are found the file is treated as UTF-8 and
decoded to CP437.  Raw CP437 bytes (values 0x80-0xFF that are not valid
UTF-8) keep the file in CP437 mode.  Pure ASCII files default to UTF-8
since the two encodings are identical for that range.

If any Unicode codepoint has no CP437 equivalent it becomes `?` and the
open message reports how many replacements were made.

**Saving** - uses whatever encoding is currently set (`enc utf8` /
`enc cp437` / `enc ascii`), which defaults to whatever was detected on
load.  Saving as ASCII replaces any byte outside printable ASCII
(plus tab, LF, CR) with `?` and reports the count in the status bar.

**Clipboard** - X11 always exchanges text as UTF-8, so copy encodes
CP437UTF-8 before sending to X11 and paste decodes UTF-8CP437 on
arrival.  The internal clipboard (used for fast self-paste) stays in
CP437.  Unmapped codepoints pasted from external applications become
`?` with a warning.

TODO: If anybody has feedback on this importer, specific to elections I 
made I would be happy to expand the importer or consider any substitution.
this worked for my usecase but may not for everyone.
---

## Syntax Highlighting

Load a language definition with `syntax <file>`.  Definitions live in
the `lang/` directory:

| File | Language |
|------|----------|
| `lang/ansi-c.csv` | ANSI C |
| `lang/z80.csv` | Z80 assembly |

Highlighting covers: block and line comments, string literals, keywords,
preprocessor directives, numbers (decimal, hex, binary), operators
(including two-character pairs like `->`, `==`, `!=`, `<=`, `>=`,
`&&`, `||`, `++`, `--`, `<<`, `>>`), and brackets.

---

## Status Bar

The bottom row shows (left to right):

```
 filename [+]  message          row/total:col  INS/OVR  LF/CRLF  ENC  T<n>  SPC/TAB  [#]
```

| Field | Meaning |
|-------|---------|
| `[+]` | Buffer has unsaved changes |
| `message` | Result of the last command |
| `row/total:col` | Current line, total lines, column (1-based) |
| `INS`/`OVR` | Insert or Overwrite mode |
| `LF`/`CRLF` | Line-ending style for next save |
| `UTF8`/`CP437`/`ASCII` | Character encoding for next save |
| `T<n>` | Current tab size |
| `SPC`/`TAB` | Spaces or hard-tab indent mode |
| `#` | Present when line numbers are on |

---

## Behaviour Notes

* **Tabs** - by default tabs are stored as spaces, expanded
  column-relative to the current tab size.  Hard tabs can be toggled on
  with `tabs` / `hardtabs` from the console.
* **Line endings** - detected from the first newline in the file; Unix
  (LF) is the default for new files.  Written consistently to every line
  on save.
* **Encoding** - detected automatically on load; the detected encoding
  becomes the default for save.  Override any time with `enc <mode>`.
  The only lossy operation is an explicit save to a narrower encoding
  (CP437 from UTF-8 with unmapped codepoints, or ASCII from CP437);
  tedit always warns before writing `?` substitutions.
* **Auto-indent** - pressing Enter carries forward the current line's
  leading spaces, but never more than the column the cursor sits at.
* **Overwrite mode** - replaces characters in place for printable input;
  Tab always inserts in both modes so indentation stays predictable.

---

## Source Layout

```
main.c        entry point, event loop
editor.h/c    editor state + key dispatch (editing, navigation, clipboard)
buffer.h/c    dynamic line-oriented text buffer, file I/O
enc.h/c       encoding detection and conversion (UTF-8 / CP437 / ASCII)
cp437.h       CP437  Unicode bidirectional lookup tables
console.h/c   Esc-activated command console
palette.h/c   command palette (filtered list, opened with '?')
render.h/c    screen drawing (uses the full vio API)
hl.h/c        syntax highlighting: CSV language loader and line lexer
undo.h/c      undo / redo stack
spell.h/c     spell check via ispell/aspell
charmap.h/c   CP437 character-map palette
vio.h/c       thin-vga I/O layer: keyboard input, mouse (click, drag, scroll wheel), clipboard, drawing helpers, display scaling  (bundled from thin-vga)
vgaterm.h/c   thin-vga X11 window + VGA buffer  (bundled)
font_vga.h    CP437 8x16 bitmap font  (bundled)
lang/         syntax definition files (CSV format)
```

---

## License

tedit is just a silly little editor I wrote up for a project to restore
documents that were lost from the 90's.  It gives you a way to work with
a specific code page and I made it nice if you need a quick editor.  If
it's useful you should use it! otherwise, well pick one of the many better
editors out there.

MIT License

Copyright (c) 2026 David Collins

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

### Font

`font_vga.h` and `Bm437_IBM_VGA_8x16.otb` are derived from the
[Ultimate Oldschool PC Font Pack](https://int10h.org/oldschool-pc-fonts/)
by VileR, and are licensed separately under
[CC BY-SA 4.0](https://creativecommons.org/licenses/by-sa/4.0/).

`font_italic.h` is algorithmically derived from `font_vga.h` and is therefore
also derivative of the original font pack. It is licensed under the same
[CC BY-SA 4.0](https://creativecommons.org/licenses/by-sa/4.0/) terms.
