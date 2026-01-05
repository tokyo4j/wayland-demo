PROTO_DIR=/usr/share/wayland-protocols

client: client.c \
				xdg-shell-protocol.c \
				xdg-shell-protocol.h \
				ext-workspace-v1-protocol.c \
				ext-workspace-v1-protocol.h
	gcc -g -Wall -lwayland-client -luv -o $@ \
		client.c \
		xdg-shell-protocol.c \
		ext-workspace-v1-protocol.c

xdg-shell-protocol.c:
	wayland-scanner private-code $(PROTO_DIR)/stable/xdg-shell/xdg-shell.xml $@

xdg-shell-protocol.h:
	wayland-scanner client-header $(PROTO_DIR)/stable/xdg-shell/xdg-shell.xml $@

ext-workspace-v1-protocol.c:
	wayland-scanner private-code $(PROTO_DIR)/staging/ext-workspace/ext-workspace-v1.xml $@

ext-workspace-v1-protocol.h:
	wayland-scanner client-header $(PROTO_DIR)/staging/ext-workspace/ext-workspace-v1.xml $@

clean:
	rm -rf *-protocol.* client
