# tedit Makefile
#
# Requires libx11-dev (Xlib).
#
#   make             -- build ./tedit
#   make test        -- run logic tests (no X11 window needed)
#   make install     -- install binary + lang files (default PREFIX=/usr/local)
#   make uninstall   -- remove installed files
#   make clean       -- remove build artefacts
#
# Override PREFIX to change the install root, e.g.:
#   make install PREFIX=/usr

CC      = cc
PREFIX  = /usr/local
BINDIR  = $(PREFIX)/bin
DATADIR = $(PREFIX)/share/tedit

CFLAGS  = -O2 -Wall -Wextra -std=c99 -pedantic \
          -DTEDIT_DATADIR=\"$(DATADIR)/lang\"
LDFLAGS = -lX11 -lm

OBJS = main.o editor.o buffer.o console.o palette.o charmap.o render.o spell.o vio.o vgaterm.o undo.o hl.o enc.o

.PHONY: all clean test install uninstall

all: tedit

tedit: $(OBJS)
	$(CC) -o $@ $(OBJS) $(LDFLAGS)

main.o:    main.c editor.h console.h palette.h charmap.h render.h spell.h vio.h vgaterm.h hl.h
editor.o:  editor.c editor.h buffer.h vio.h vgaterm.h undo.h hl.h
buffer.o:  buffer.c buffer.h
console.o: console.c console.h palette.h charmap.h spell.h editor.h vio.h hl.h
palette.o: palette.c palette.h editor.h vio.h vgaterm.h
charmap.o: charmap.c charmap.h editor.h vio.h
render.o:  render.c render.h palette.h charmap.h spell.h editor.h vio.h hl.h
spell.o:   spell.c spell.h editor.h hl.h vio.h
vio.o:     vio.c vio.h vgaterm.h
vgaterm.o: vgaterm.c vgaterm.h font_vga.h
undo.o:    undo.c undo.h editor.h buffer.h
hl.o:      hl.c hl.h vgaterm.h buffer.h
enc.o:     enc.c enc.h cp437.h

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

# ---- logic-only test (no X11 window needed) ----------------------------
test: test.c editor.c buffer.c console.c palette.c undo.c
	$(CC) $(CFLAGS) -o $@ test.c editor.c buffer.c console.c palette.c undo.c -lm
	@echo "Running tests..."
	@./test

clean:
	rm -f *.o tedit test

# ---- install / uninstall -----------------------------------------------
install: tedit
	install -d $(DESTDIR)$(BINDIR)
	install -m 755 tedit $(DESTDIR)$(BINDIR)/tedit
	install -d $(DESTDIR)$(DATADIR)/lang
	install -m 644 lang/*.csv $(DESTDIR)$(DATADIR)/lang/

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/tedit
	rm -rf $(DESTDIR)$(DATADIR)
