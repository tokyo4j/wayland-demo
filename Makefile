PROTO_DIR=/usr/share/wayland-protocols

client: client.c \
				xdg-shell-protocol.c \
				xdg-shell-protocol.h
	gcc -g -Wall -lwayland-client -luv -o $@ \
		client.c \
		xdg-shell-protocol.c

xdg-shell-protocol.c:
	wayland-scanner private-code $(PROTO_DIR)/stable/xdg-shell/xdg-shell.xml $@

xdg-shell-protocol.h:
	wayland-scanner client-header $(PROTO_DIR)/stable/xdg-shell/xdg-shell.xml $@

clean:
	rm -rf *-protocol.* client
