VERSION ?= 0.1.0
PREFIX  ?= /usr/local
BINDIR  ?= $(PREFIX)/bin
DATADIR ?= $(PREFIX)/share
LOCALEDIR ?= $(DATADIR)/locale
SYSTEMDUSERUNITDIR ?= $(DATADIR)/systemd/user
APP_ID  := io.github.SimonBlancoE.Ejecta

CC      ?= cc
CFLAGS  ?= -O2 -pipe
# Internal flags; CPPFLAGS/CFLAGS/LDFLAGS/LDLIBS from the user are appended.
M_CFLAGS := -std=gnu11 -Wall -Wextra -Wno-unused-parameter
M_CPPFLAGS := -DVERSION='"$(VERSION)"' -DLOCALEDIR='"$(LOCALEDIR)"'

# gtk4-layer-shell must be linked before libwayland-client. Use its headers
# when installed; some distributions only ship the library; without it the
# panel is an ordinary window.
LAYER_SHELL_SO := $(shell $(CC) -print-file-name=libgtk4-layer-shell.so.0)
ifeq ($(LAYER_SHELL),no)
  M_CPPFLAGS += -DNO_LAYER_SHELL $(shell pkg-config --cflags gtk4)
  M_LDLIBS   := $(shell pkg-config --libs gtk4)
else ifeq ($(shell pkg-config --exists gtk4-layer-shell-0 && echo yes),yes)
  M_CPPFLAGS += -DHAVE_LAYER_SHELL_HEADER $(shell pkg-config --cflags gtk4-layer-shell-0 gtk4)
  M_LDLIBS   := $(shell pkg-config --libs gtk4-layer-shell-0 gtk4)
else ifneq ($(wildcard $(LAYER_SHELL_SO)),)
  M_CPPFLAGS += $(shell pkg-config --cflags gtk4)
  M_LDLIBS   := -l:libgtk4-layer-shell.so.0 $(shell pkg-config --libs gtk4)
else
  $(warning gtk4-layer-shell not found: the panel will be an ordinary window)
  M_CPPFLAGS += -DNO_LAYER_SHELL $(shell pkg-config --cflags gtk4)
  M_LDLIBS   := $(shell pkg-config --libs gtk4)
endif

LINGUAS := $(basename $(notdir $(wildcard po/*.po)))
MOS     := $(foreach l,$(LINGUAS),locale/$(l)/LC_MESSAGES/ejecta.mo)

all: ejecta $(MOS)

# Rebuild when the flags change (e.g. a different PREFIX at install time).
FLAGS = $(M_CPPFLAGS) $(CPPFLAGS) $(M_CFLAGS) $(CFLAGS) $(LDFLAGS) $(M_LDLIBS) $(LDLIBS)
.build-flags: FORCE
	@echo '$(FLAGS)' | cmp -s - $@ || echo '$(FLAGS)' > $@

ejecta: ejecta.c .build-flags
	$(CC) $(M_CPPFLAGS) $(CPPFLAGS) $(M_CFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $< $(M_LDLIBS) $(LDLIBS)

locale/%/LC_MESSAGES/ejecta.mo: po/%.po
	@mkdir -p $(dir $@)
	msgfmt --check -o $@ $<

# Regenerate the template and merge it into the translations.
pot:
	xgettext --from-code=UTF-8 --add-comments=TRANSLATORS -k_ -kN_ --package-name=ejecta \
	  --package-version=$(VERSION) -o po/ejecta.pot ejecta.c
	for po in po/*.po; do msgmerge --update --backup=none $$po po/ejecta.pot; done

# Run from the source tree with translations.
run: all
	EJECTA_LOCALEDIR=$(CURDIR)/locale ./ejecta

install: all
	install -Dm755 ejecta $(DESTDIR)$(BINDIR)/ejecta
	install -Dm644 data/$(APP_ID).desktop $(DESTDIR)$(DATADIR)/applications/$(APP_ID).desktop
	sed 's|@BINDIR@|$(BINDIR)|' data/ejecta.service.in > ejecta.service
	install -Dm644 ejecta.service $(DESTDIR)$(SYSTEMDUSERUNITDIR)/ejecta.service
	for l in $(LINGUAS); do \
	  install -Dm644 locale/$$l/LC_MESSAGES/ejecta.mo $(DESTDIR)$(LOCALEDIR)/$$l/LC_MESSAGES/ejecta.mo; \
	done

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/ejecta $(DESTDIR)$(DATADIR)/applications/$(APP_ID).desktop \
	      $(DESTDIR)$(SYSTEMDUSERUNITDIR)/ejecta.service
	for l in $(LINGUAS); do rm -f $(DESTDIR)$(LOCALEDIR)/$$l/LC_MESSAGES/ejecta.mo; done

clean:
	rm -rf ejecta ejecta.service locale .build-flags

.PHONY: all pot run install uninstall clean FORCE
