PROTO_DIR=/usr/share/wayland-protocols

client: client.c \
				xdg-shell-protocol.c \
				xdg-shell-protocol.h \
				relative-pointer-unstable-v1-protocol.c \
				relative-pointer-unstable-v1-protocol.h \
				pointer-constraints-unstable-v1-protocol.c \
				pointer-constraints-unstable-v1-protocol.h
	gcc -g -Wall -lwayland-client -luv -o $@ \
		client.c \
		xdg-shell-protocol.c \
		relative-pointer-unstable-v1-protocol.c \
		pointer-constraints-unstable-v1-protocol.c

xdg-shell-protocol.c:
	wayland-scanner private-code $(PROTO_DIR)/stable/xdg-shell/xdg-shell.xml $@

xdg-shell-protocol.h:
	wayland-scanner client-header $(PROTO_DIR)/stable/xdg-shell/xdg-shell.xml $@

relative-pointer-unstable-v1-protocol.c:
	wayland-scanner private-code $(PROTO_DIR)/unstable/relative-pointer/relative-pointer-unstable-v1.xml $@

relative-pointer-unstable-v1-protocol.h:
	wayland-scanner client-header $(PROTO_DIR)/unstable/relative-pointer/relative-pointer-unstable-v1.xml $@

pointer-constraints-unstable-v1-protocol.c:
	wayland-scanner private-code $(PROTO_DIR)/unstable/pointer-constraints/pointer-constraints-unstable-v1.xml $@

pointer-constraints-unstable-v1-protocol.h:
	wayland-scanner client-header $(PROTO_DIR)/unstable/pointer-constraints/pointer-constraints-unstable-v1.xml $@

clean:
	rm -rf *-protocol.* client
