PREFIX ?= $(HOME)/.local
CFLAGS ?= -O2 -Wall -Wextra
PROTO = protocol/wlr-layer-shell-unstable-v1.xml
XDG = $(shell pkg-config --variable=pkgdatadir wayland-protocols)/stable/xdg-shell/xdg-shell.xml
GEN = wlr-layer-shell-unstable-v1-client-protocol.h wlr-layer-shell-unstable-v1-protocol.c xdg-shell-protocol.c

facelight: facelight.c $(GEN)
	$(CC) $(CFLAGS) -o $@ facelight.c wlr-layer-shell-unstable-v1-protocol.c xdg-shell-protocol.c \
		$$(pkg-config --cflags --libs wayland-client) -lm

wlr-layer-shell-unstable-v1-client-protocol.h: $(PROTO)
	wayland-scanner client-header $< $@

wlr-layer-shell-unstable-v1-protocol.c: $(PROTO)
	wayland-scanner private-code $< $@

# layer-shell references xdg_popup_interface
xdg-shell-protocol.c: $(XDG)
	wayland-scanner private-code $< $@

install: facelight
	install -Dm755 facelight $(DESTDIR)$(PREFIX)/bin/facelight
	install -Dm644 facelight.service $(DESTDIR)$(HOME)/.config/systemd/user/facelight.service
	sed -i 's|@BINDIR@|$(PREFIX)/bin|' $(DESTDIR)$(HOME)/.config/systemd/user/facelight.service

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/facelight $(DESTDIR)$(HOME)/.config/systemd/user/facelight.service

clean:
	rm -f facelight $(GEN)

.PHONY: install uninstall clean
