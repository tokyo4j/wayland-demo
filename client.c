#include "ext-workspace-v1-protocol.h"
#include "xdg-shell-protocol.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <uv.h>
#include <wayland-client-protocol.h>
#include <wayland-client.h>

/* Wayland code */
struct client_state {
	/* Globals */
	struct wl_display *wl_display;
	struct wl_registry *wl_registry;
	struct wl_seat *wl_seat;
	struct ext_workspace_manager_v1 *workspace_manager;
	struct ext_workspace_group_handle_v1 *workspace_group_handle;

	uv_loop_t *loop;
	uv_poll_t poll_handle;
	uv_pipe_t stdin_pipe;
	struct wl_list workspaces;
};

struct workspace {
	struct wl_list link;
	struct ext_workspace_handle_v1 *handle;
	struct client_state *client_state;
	char *id;
	char *name;
	struct wl_array coordinates;
	uint32_t state;
	uint32_t caps;
};

static void
workspace_handle_id(void *data,
	struct ext_workspace_handle_v1 *ext_workspace_handle_v1, const char *id)
{
	struct workspace *ws = data;
	free(ws->id);
	ws->id = NULL;
	if (id) {
		ws->id = strdup(id);
	}
}

static void
workspace_handle_name(void *data,
	struct ext_workspace_handle_v1 *ext_workspace_handle_v1,
	const char *name)
{
	struct workspace *ws = data;
	free(ws->name);
	ws->name = NULL;
	if (name) {
		ws->name = strdup(name);
	}
}

static void
workspace_handle_coordinates(void *data,
	struct ext_workspace_handle_v1 *ext_workspace_handle_v1,
	struct wl_array *coordinates)
{
	struct workspace *ws = data;
	wl_array_release(&ws->coordinates);
	if (coordinates && coordinates->size > 0) {
		wl_array_init(&ws->coordinates);
		wl_array_copy(&ws->coordinates, coordinates);
	}
}

static void
workspace_handle_state(void *data,
	struct ext_workspace_handle_v1 *ext_workspace_handle_v1, uint32_t state)
{
	struct workspace *ws = data;
	ws->state = state;
}

static void
workspace_handle_capabilities(void *data,
	struct ext_workspace_handle_v1 *ext_workspace_handle_v1,
	uint32_t capabilities)
{
	struct workspace *ws = data;
	ws->caps = capabilities;
}

static void
workspace_handle_removed(
	void *data, struct ext_workspace_handle_v1 *ext_workspace_handle_v1)
{
	struct workspace *ws = data;
	fprintf(stderr, "Workspace \"%s\" removed\n", ws->name);
	wl_list_remove(&ws->link);
	free(ws);
}

static const struct ext_workspace_handle_v1_listener
	ext_workspace_handle_v1_listener = {
		.id = workspace_handle_id,
		.name = workspace_handle_name,
		.coordinates = workspace_handle_coordinates,
		.state = workspace_handle_state,
		.capabilities = workspace_handle_capabilities,
		.removed = workspace_handle_removed,
};

static void
workspace_manager_workspace_group(void *data,
	struct ext_workspace_manager_v1 *ext_workspace_manager_v1,
	struct ext_workspace_group_handle_v1 *workspace_group)
{
}

static void
workspace_manager_workspace(void *data,
	struct ext_workspace_manager_v1 *ext_workspace_manager_v1,
	struct ext_workspace_handle_v1 *workspace)
{
	struct workspace *ws = calloc(1, sizeof(struct workspace));
	ws->client_state = data;
	ws->handle = workspace;
	wl_list_insert(&ws->client_state->workspaces, &ws->link);
	ext_workspace_handle_v1_add_listener(workspace, &ext_workspace_handle_v1_listener, ws);
}

static void
print_workspace_info(struct workspace *ws)
{
	printf("workspace:\n");
	printf("- name=%s\n", ws->name);
	printf("- id=%s\n", ws->id);
	printf("- coordinates=(");
	int32_t *coords = ws->coordinates.data;
	for (size_t i = 0; i < ws->coordinates.size / sizeof(int32_t); i++) {
		printf("%d ", coords[i]);
	}
	printf(")\n");
	printf("- state=(");
	if (ws->state & EXT_WORKSPACE_HANDLE_V1_STATE_ACTIVE) {
		printf("active ");
	}
	if (ws->state & EXT_WORKSPACE_HANDLE_V1_STATE_URGENT) {
		printf("urgent ");
	}
	if (ws->state & EXT_WORKSPACE_HANDLE_V1_STATE_HIDDEN) {
		printf("hidden ");
	}
	printf(")\n");
	printf("- capabilities=(");
	if (ws->caps
		& EXT_WORKSPACE_HANDLE_V1_WORKSPACE_CAPABILITIES_ACTIVATE) {
		printf("activate ");
	}
	if (ws->caps
		& EXT_WORKSPACE_HANDLE_V1_WORKSPACE_CAPABILITIES_DEACTIVATE) {
		printf("deactivate ");
	}
	if (ws->caps & EXT_WORKSPACE_HANDLE_V1_WORKSPACE_CAPABILITIES_REMOVE) {
		printf("remove ");
	}
	if (ws->caps & EXT_WORKSPACE_HANDLE_V1_WORKSPACE_CAPABILITIES_ASSIGN) {
		printf("assign ");
	}
	printf(")\n");
}

static void
workspace_manager_done(
	void *data, struct ext_workspace_manager_v1 *ext_workspace_manager_v1)
{
	struct client_state *state = data;
	printf("============= received .done event ===========\n");
	struct workspace *handle;
	wl_list_for_each(handle, &state->workspaces, link) {
		print_workspace_info(handle);
	}
	printf("===========================================\n");
}

static void
workspace_manager_finished(
	void *data, struct ext_workspace_manager_v1 *ext_workspace_manager_v1)
{
}

static const struct ext_workspace_manager_v1_listener
	ext_workspace_manager_v1_listener = {
		.workspace_group = workspace_manager_workspace_group,
		.workspace = workspace_manager_workspace,
		.done = workspace_manager_done,
		.finished = workspace_manager_finished,
};

static void
handle_registry_global(void *data, struct wl_registry *wl_registry,
	uint32_t name, const char *interface, uint32_t version)
{
	struct client_state *state = data;
	if (!strcmp(interface, ext_workspace_manager_v1_interface.name)) {
		state->workspace_manager = wl_registry_bind(wl_registry, name,
			&ext_workspace_manager_v1_interface, 1);
		ext_workspace_manager_v1_add_listener(state->workspace_manager,
			&ext_workspace_manager_v1_listener, state);
	}
}

static void
handle_registry_global_remove(
	void *data, struct wl_registry *wl_registry, uint32_t name)
{
	/* This space deliberately left blank */
}

static const struct wl_registry_listener wl_registry_listener = {
	.global = handle_registry_global,
	.global_remove = handle_registry_global_remove,
};

static void
on_wayland_event(uv_poll_t *handle, int status, int events)
{
	struct client_state *state = handle->data;
	if (events & UV_READABLE) {
		if (wl_display_dispatch(state->wl_display) == -1) {
			fprintf(stderr, "Failed to dispatch Wayland events.\n");
			exit(1);
		}
	}
	wl_display_flush(state->wl_display);
}

static struct workspace *
find_workspace_by_name(struct client_state *state, const char *name)
{
	struct workspace *ws;
	wl_list_for_each(ws, &state->workspaces, link) {
		if (ws->name && strcmp(ws->name, name) == 0) {
			return ws;
		}
	}
	return NULL;
}

static void
handle_command(struct client_state *state, const char *line)
{
	char action[64];
	char name[256];
	// let name consume all the rest of the line including spaces
	if (sscanf(line, "%63s  %255[^\n]", action, name) != 2) {
		fprintf(stderr, "Invalid command format. Use: <action> "
				"<workspace-name>\n");
		return;
	}

	struct workspace *ws = find_workspace_by_name(state, name);
	if (!ws) {
		fprintf(stderr, "Workspace not found: %s\n", name);
		return;
	}
	if (strcmp(action, "activate") == 0) {
		ext_workspace_handle_v1_activate(ws->handle);
	} else if (strcmp(action, "deactivate") == 0) {
		ext_workspace_handle_v1_deactivate(ws->handle);
	} else if (strcmp(action, "assign") == 0) {
		// TODO
	} else if (strcmp(action, "remove") == 0) {
		ext_workspace_handle_v1_remove(ws->handle);
	} else {
		fprintf(stderr, "Unknown action: %s\n", action);
	}
	ext_workspace_manager_v1_commit(state->workspace_manager);
	wl_display_flush(state->wl_display);
}

static void
on_stdin_read(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf)
{
	struct client_state *state = stream->data;
	if (nread < 0) {
		if (nread == UV_EOF) {
			uv_close((uv_handle_t *)stream, NULL);
		}
	} else if (nread > 0) {
		char *newline = strrchr(buf->base, '\n');
		if (newline) {
			*newline = '\0';
			handle_command(state, buf->base);
		} else {
			fprintf(stderr, "Missing new line\n");
		}
	}
	free(buf->base);
}

static void
alloc_buffer(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf)
{
	buf->base = malloc(suggested_size);
	buf->len = suggested_size;
}

int
main(int argc, char *argv[])
{
	printf("Type commands in the format: <action> <workspace-name>\n"
	       "Available actions:\n"
	       "  activate\n"
	       "  deactivate\n"
	       "  assign\n"
	       "  remove\n");

	struct client_state state = {};
	wl_list_init(&state.workspaces);
	state.wl_display = wl_display_connect(NULL);
	state.wl_registry = wl_display_get_registry(state.wl_display);
	wl_registry_add_listener(
		state.wl_registry, &wl_registry_listener, &state);
	wl_display_roundtrip(state.wl_display);
	assert(state.workspace_manager);

	state.loop = uv_default_loop();
	state.poll_handle.data = &state;
	uv_poll_init(state.loop, &state.poll_handle,
		wl_display_get_fd(state.wl_display));
	wl_display_flush(state.wl_display);
	uv_poll_start(&state.poll_handle, UV_READABLE, on_wayland_event);

	state.stdin_pipe.data = &state;
	uv_pipe_init(state.loop, &state.stdin_pipe, 0);
	uv_pipe_open(&state.stdin_pipe, 0);
	uv_read_start(
		(uv_stream_t *)&state.stdin_pipe, alloc_buffer, on_stdin_read);

	uv_run(state.loop, UV_RUN_DEFAULT);

	return 0;
}
