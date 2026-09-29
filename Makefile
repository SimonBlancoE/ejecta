PREFIX  ?= $(HOME)/.local
CC      ?= cc
CFLAGS  ?= -O2 -pipe
CFLAGS  += -std=gnu11 -Wall -Wextra -Wno-unused-parameter $(shell pkg-config --cflags gtk4)
# gtk4-layer-shell debe enlazarse antes que libwayland-client.
LDLIBS  := -l:libgtk4-layer-shell.so.0 $(shell pkg-config --libs gtk4)

muelle: muelle.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

install: muelle
	install -Dm755 muelle $(DESTDIR)$(PREFIX)/bin/muelle
	install -Dm644 muelle.desktop $(DESTDIR)$(PREFIX)/share/applications/muelle.desktop

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/muelle $(DESTDIR)$(PREFIX)/share/applications/muelle.desktop

clean:
	rm -f muelle

.PHONY: install uninstall clean
