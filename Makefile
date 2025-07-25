PROTO_DIR=/usr/share/wayland-protocols

client: client.c \
				xdg-shell-protocol.c \
				xdg-shell-protocol.h \
				xdg-decoration-protocol.c \
				xdg-decoration-protocol.h
	gcc -g -lwayland-client -luv -o $@ \
		client.c \
		xdg-shell-protocol.c \
		xdg-decoration-protocol.c

xdg-shell-protocol.c:
	wayland-scanner private-code $(PROTO_DIR)/stable/xdg-shell/xdg-shell.xml $@

xdg-shell-protocol.h:
	wayland-scanner client-header $(PROTO_DIR)/stable/xdg-shell/xdg-shell.xml $@

xdg-decoration-protocol.c:
	wayland-scanner private-code $(PROTO_DIR)/unstable/xdg-decoration/xdg-decoration-unstable-v1.xml $@

xdg-decoration-protocol.h:
	wayland-scanner client-header $(PROTO_DIR)/unstable/xdg-decoration/xdg-decoration-unstable-v1.xml $@

clean:
	rm -rf *-protocol.* client
