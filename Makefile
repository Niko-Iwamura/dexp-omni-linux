# DEXP OMNI keyboard configurator (Linux, C + GTK3)
#
# Targets:
#   make            build omni-cli and (if GTK3 dev files exist) omni-gtk
#   make omni-cli   command line tool only (no GTK needed)
#   make omni-gtk   GUI configurator
#   make install    install to $(PREFIX) (default /usr/local)
#
# On Kubuntu:  sudo apt install build-essential libgtk-3-dev
# then:        make && sudo make install

CC       ?= cc
CFLAGS   ?= -O2 -g -Wall -Wextra -std=c11
LDFLAGS  ?=
PREFIX   ?= /usr/local

CORE_SRC  = src/omni.c src/omni_keys.c src/omni_layout.c src/omni_profile.c
GUI_SRC   = src/gui-main.c src/gui-keys.c src/gui-light.c src/gui-macro.c
CORE_HDR  = src/omni.h src/gui.h

GTK_CFLAGS ?= $(shell pkg-config --cflags gtk+-3.0 2>/dev/null)
GTK_LIBS   ?= $(shell pkg-config --libs gtk+-3.0 2>/dev/null)
HAVE_GTK   := $(if $(GTK_LIBS),yes,no)

OBJS_CLI = $(CORE_SRC:.c=.o) src/omni-cli.o
OBJS_GUI = $(CORE_SRC:.c=.o) $(GUI_SRC:.c=.o)

.PHONY: all clean install

all: omni-cli
ifeq ($(HAVE_GTK),yes)
all: omni-gtk
else
$(warning GTK3 development files not found (libgtk-3-dev) — building omni-cli only)
endif

%.o: %.c $(CORE_HDR)
	$(CC) $(CFLAGS) -Isrc -c -o $@ $<

src/gui-%.o: src/gui-%.c $(CORE_HDR)
	$(CC) $(CFLAGS) $(GTK_CFLAGS) -Isrc -c -o $@ $<

omni-cli: $(OBJS_CLI)
	$(CC) $(LDFLAGS) -o $@ $(OBJS_CLI)

omni-gtk: $(OBJS_GUI)
	$(CC) $(LDFLAGS) -o $@ $(OBJS_GUI) $(GTK_LIBS)

clean:
	rm -f omni-cli omni-gtk $(OBJS_CLI) $(OBJS_GUI)

install: all
	install -Dm755 omni-cli $(DESTDIR)$(PREFIX)/bin/omni-cli
	install -Dm644 README.md $(DESTDIR)$(PREFIX)/share/doc/dexp-omni/README.md
	install -Dm644 PROTOCOL.md $(DESTDIR)$(PREFIX)/share/doc/dexp-omni/PROTOCOL.md
ifeq ($(HAVE_GTK),yes)
	install -Dm755 omni-gtk $(DESTDIR)$(PREFIX)/bin/omni-gtk
endif
	@echo ""
	@echo "To allow access without root, install the udev rule:"
	@echo "  sudo cp udev/60-dexp-omni.rules /etc/udev/rules.d/"
	@echo "  sudo udevadm control --reload && sudo udevadm trigger"
