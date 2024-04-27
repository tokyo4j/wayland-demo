PROTO_DIR=/usr/share/wayland-protocols

client: client.c \
				xdg-shell-client-protocol.c \
				xdg-shell-client-protocol.h
	gcc -g -lwayland-client -o $@ \
		client.c \
		xdg-shell-client-protocol.c

xdg-shell-client-protocol.c:
	wayland-scanner private-code $(PROTO_DIR)/stable/xdg-shell/xdg-shell.xml $@

xdg-shell-client-protocol.h:
	wayland-scanner client-header $(PROTO_DIR)/stable/xdg-shell/xdg-shell.xml $@

clean:
	rm -rf *-protocol.* client
