PROTO_DIR=/usr/share/wayland-protocols

client: client.c \
				xdg-shell-client-protocol.c \
				xdg-shell-client-protocol.h \
				virtual-keyboard-v1-client-protocol.c \
				virtual-keyboard-v1-client-protocol.h
	gcc -g -lxkbcommon -lwayland-client -o $@ \
		client.c \
		xdg-shell-client-protocol.c \
		virtual-keyboard-v1-client-protocol.c

xdg-shell-client-protocol.c:
	wayland-scanner private-code $(PROTO_DIR)/stable/xdg-shell/xdg-shell.xml $@

xdg-shell-client-protocol.h:
	wayland-scanner client-header $(PROTO_DIR)/stable/xdg-shell/xdg-shell.xml $@

virtual-keyboard-v1-client-protocol.c:
	wayland-scanner private-code virtual-keyboard-unstable-v1.xml $@

virtual-keyboard-v1-client-protocol.h:
	wayland-scanner client-header virtual-keyboard-unstable-v1.xml $@

clean:
	rm -rf *-protocol.* client
