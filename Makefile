# freak — Wayland gamma control (C version)

CC            ?= cc
PKG_CONFIG    ?= pkg-config
WAYLAND_SCANNER ?= wayland-scanner
PREFIX        ?= /usr/local
BINDIR        ?= $(PREFIX)/bin
DATADIR       ?= $(PREFIX)/share

CFLAGS  ?= -O2 -Wall -Wextra
CFLAGS  += -I. $(shell $(PKG_CONFIG) --cflags wayland-client)
WL_LIBS  = $(shell $(PKG_CONFIG) --libs wayland-client) -lm

PROTO_XML = protocol/wlr-gamma-control-unstable-v1.xml
PROTO_H   = wlr-gamma-control-unstable-v1-client-protocol.h
PROTO_C   = wlr-gamma-control-unstable-v1-protocol.c

COMMON_OBJS = params.o preset.o ipc.o
FREAK_OBJS  = main.o tui.o daemon.o gamma.o sway.o $(COMMON_OBJS) $(PROTO_C:.c=.o)
CTL_OBJS    = ctl.o $(COMMON_OBJS)

all: freak freakctl

freak: $(FREAK_OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(FREAK_OBJS) $(WL_LIBS)

freakctl: $(CTL_OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(CTL_OBJS)

$(PROTO_H): $(PROTO_XML)
	$(WAYLAND_SCANNER) client-header < $< > $@

$(PROTO_C): $(PROTO_XML)
	$(WAYLAND_SCANNER) private-code < $< > $@

%.o: %.c freak.h $(PROTO_H)
	$(CC) $(CFLAGS) -c -o $@ $<

install: all
	install -Dm755 freak    $(DESTDIR)$(BINDIR)/freak
	install -Dm755 freakctl $(DESTDIR)$(BINDIR)/freakctl
	install -d $(DESTDIR)$(DATADIR)/freak/presets
	install -m644 -t $(DESTDIR)$(DATADIR)/freak/presets presets/*.freak

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/freak $(DESTDIR)$(BINDIR)/freakctl
	rm -rf $(DESTDIR)$(DATADIR)/freak

clean:
	rm -f freak freakctl *.o $(PROTO_H) $(PROTO_C)

.PHONY: all install uninstall clean
