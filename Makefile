PROTO_DIR=/usr/share/wayland-protocols

client: client.c \
				xdg-shell-protocol.c \
				xdg-shell-protocol.h \
				ext-foreign-toplevel-state-v1-protocol.c \
				ext-foreign-toplevel-state-v1-protocol.h \
				ext-foreign-toplevel-list-v1-protocol.c \
				ext-foreign-toplevel-list-v1-protocol.h
	gcc -g -Wall -lwayland-client -luv -o $@ \
		client.c \
		xdg-shell-protocol.c \
		ext-foreign-toplevel-state-v1-protocol.c \
		ext-foreign-toplevel-list-v1-protocol.c

xdg-shell-protocol.c:
	wayland-scanner private-code $(PROTO_DIR)/stable/xdg-shell/xdg-shell.xml $@

xdg-shell-protocol.h:
	wayland-scanner client-header $(PROTO_DIR)/stable/xdg-shell/xdg-shell.xml $@

ext-foreign-toplevel-list-v1-protocol.c:
	wayland-scanner private-code $(PROTO_DIR)/staging/ext-foreign-toplevel-list/ext-foreign-toplevel-list-v1.xml $@

ext-foreign-toplevel-list-v1-protocol.h:
	wayland-scanner client-header $(PROTO_DIR)/staging/ext-foreign-toplevel-list/ext-foreign-toplevel-list-v1.xml $@

ext-foreign-toplevel-state-v1-protocol.c:
	wayland-scanner private-code ext-foreign-toplevel-state-v1.xml $@

ext-foreign-toplevel-state-v1-protocol.h:
	wayland-scanner client-header ext-foreign-toplevel-state-v1.xml $@

clean:
	rm -rf *-protocol.* client
