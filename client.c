#include "ext-foreign-toplevel-list-v1-protocol.h"
#include "ext-foreign-toplevel-state-v1-protocol.h"
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
	struct ext_foreign_toplevel_list_v1 *toplevel_list;
	struct ext_foreign_toplevel_state_manager_v1 *state_manager;

	uv_loop_t *loop;
	uv_poll_t poll_handle;
	uv_pipe_t stdin_pipe;
	struct wl_list state_handles;
	int next_output_idx;
	struct wl_list outputs;
	uint32_t notification_caps;
	uint32_t action_caps;
};

struct toplevel_handle {
	struct wl_list link;
	struct ext_foreign_toplevel_handle_v1 *list_handle;
	struct ext_foreign_toplevel_state_handle_v1 *state_handle;
	struct client_state *client_state;
	char *app_id;
	char *identifier;
	char *title;
	struct toplevel_handle *parent;
	uint32_t state;
	uint32_t outputs;
};

struct output {
	int idx;
	struct wl_list link;
	struct wl_output *wl_output;
	char *name;
};

static void
output_geometry(void *data, struct wl_output *wl_output, int32_t x, int32_t y,
	int32_t physical_width, int32_t physical_height, int32_t subpixel,
	const char *make, const char *model, int32_t transform)
{
}

static void
output_mode(void *data, struct wl_output *wl_output, uint32_t flags,
	int32_t width, int32_t height, int32_t refresh)
{
}

static void
output_done(void *data, struct wl_output *wl_output)
{
}

static void
output_scale(void *data, struct wl_output *wl_output, int32_t factor)
{
}

static void
output_name(void *data, struct wl_output *wl_output, const char *name)
{
	struct output *output = data;
	free(output->name);
	output->name = strdup(name);
}

static void
output_description(
	void *data, struct wl_output *wl_output, const char *description)
{
}

static const struct wl_output_listener wl_output_listener = {
	.geometry = output_geometry,
	.mode = output_mode,
	.done = output_done,
	.scale = output_scale,
	.name = output_name,
	.description = output_description,
};

static void
handle_registry_global(void *data, struct wl_registry *wl_registry,
	uint32_t name, const char *interface, uint32_t version)
{
	struct client_state *state = data;
	if (!strcmp(interface, wl_output_interface.name)) {
		struct output *output = calloc(1, sizeof(*output));
		output->wl_output = wl_registry_bind(
			wl_registry, name, &wl_output_interface, 4);
		output->idx = state->next_output_idx++;
		assert(output->idx < 32);
		wl_output_add_listener(
			output->wl_output, &wl_output_listener, output);
		wl_list_insert(&state->outputs, &output->link);
	} else if (!strcmp(interface, wl_seat_interface.name)) {
		state->wl_seat = wl_registry_bind(
			wl_registry, name, &wl_seat_interface, 1);
	} else if (!strcmp(interface,
			   ext_foreign_toplevel_list_v1_interface.name)) {
		state->toplevel_list = wl_registry_bind(wl_registry, name,
			&ext_foreign_toplevel_list_v1_interface, 1);
	} else if (!strcmp(interface,
			   ext_foreign_toplevel_state_manager_v1_interface
				   .name)) {
		state->state_manager = wl_registry_bind(wl_registry, name,
			&ext_foreign_toplevel_state_manager_v1_interface, 1);
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

static void
list_handle_closed(void *data,
	struct ext_foreign_toplevel_handle_v1 *ext_foreign_toplevel_handle_v1)
{
	struct toplevel_handle *toplevel_handle = data;
	printf("toplevel[%s]: closed\n", toplevel_handle->identifier);
	wl_list_remove(&toplevel_handle->link);
	free(toplevel_handle->app_id);
	free(toplevel_handle->identifier);
	free(toplevel_handle->title);
	free(toplevel_handle);
}

static void
print_toplevel_info(struct toplevel_handle *toplevel_handle)
{
	printf("toplevel: %s\n", toplevel_handle->identifier);
	printf("- title='%s'\n", toplevel_handle->title);
	printf("- app_id='%s'\n", toplevel_handle->app_id);
	printf("- parent=%s\n", toplevel_handle->parent
					? toplevel_handle->parent->identifier
					: "null");
	printf("- outputs=(");
	struct output *out;
	wl_list_for_each(out, &toplevel_handle->client_state->outputs, link) {
		if (toplevel_handle->outputs & (1 << out->idx)) {
			printf("%s ", out->name);
		}
	}
	printf(")\n");
	printf("- state=(");
	if (toplevel_handle->state
		& EXT_FOREIGN_TOPLEVEL_STATE_HANDLE_V1_STATE_MAXIMIZED) {
		printf("maximized ");
	}
	if (toplevel_handle->state
		& EXT_FOREIGN_TOPLEVEL_STATE_HANDLE_V1_STATE_MINIMIZED) {
		printf("minimized ");
	}
	if (toplevel_handle->state
		& EXT_FOREIGN_TOPLEVEL_STATE_HANDLE_V1_STATE_ACTIVATED) {
		printf("activated ");
	}
	if (toplevel_handle->state
		& EXT_FOREIGN_TOPLEVEL_STATE_HANDLE_V1_STATE_FULLSCREEN) {
		printf("fullscreen ");
	}
	if (toplevel_handle->state
		& EXT_FOREIGN_TOPLEVEL_STATE_HANDLE_V1_STATE_ALWAYS_ON_TOP) {
		printf("always-on-top ");
	}
	if (toplevel_handle->state
		& EXT_FOREIGN_TOPLEVEL_STATE_HANDLE_V1_STATE_STICKY) {
		printf("sticky ");
	}
	if (toplevel_handle->state
		& EXT_FOREIGN_TOPLEVEL_STATE_HANDLE_V1_STATE_SHADED) {
		printf("shaded ");
	}
	printf(")\n");
}

static void
list_handle_done(void *data,
	struct ext_foreign_toplevel_handle_v1 *ext_foreign_toplevel_handle_v1)
{
	struct toplevel_handle *toplevel_handle = data;
	printf("============= received .done event ===========\n");
	struct toplevel_handle *handle;
	wl_list_for_each(
		handle, &toplevel_handle->client_state->state_handles, link) {
		print_toplevel_info(handle);
	}
	printf("===========================================\n");
}

static void
list_handle_title(void *data,
	struct ext_foreign_toplevel_handle_v1 *ext_foreign_toplevel_handle_v1,
	const char *title)
{
	struct toplevel_handle *toplevel_handle = data;
	free(toplevel_handle->title);
	toplevel_handle->title = NULL;
	if (title) {
		toplevel_handle->title = strdup(title);
	}
}

static void
list_handle_app_id(void *data,
	struct ext_foreign_toplevel_handle_v1 *ext_foreign_toplevel_handle_v1,
	const char *app_id)
{
	struct toplevel_handle *toplevel_handle = data;
	free(toplevel_handle->app_id);
	toplevel_handle->app_id = NULL;
	if (app_id) {
		toplevel_handle->app_id = strdup(app_id);
	}
}

static void
list_handle_identifier(void *data,
	struct ext_foreign_toplevel_handle_v1 *ext_foreign_toplevel_handle_v1,
	const char *identifier)
{
	struct toplevel_handle *toplevel_handle = data;
	free(toplevel_handle->identifier);
	toplevel_handle->identifier = strdup(identifier);
}

static struct ext_foreign_toplevel_handle_v1_listener list_handle_listener = {
	.closed = list_handle_closed,
	.done = list_handle_done,
	.title = list_handle_title,
	.app_id = list_handle_app_id,
	.identifier = list_handle_identifier,
};

static void
state_handle_state(void *data,
	struct ext_foreign_toplevel_state_handle_v1
		*ext_foreign_toplevel_state_handle_v1,
	uint32_t state)
{
	struct toplevel_handle *toplevel_handle = data;
	toplevel_handle->state = state;
}

static void
state_handle_output_enter(void *data,
	struct ext_foreign_toplevel_state_handle_v1
		*ext_foreign_toplevel_state_handle_v1,
	struct wl_output *output)
{
	struct toplevel_handle *toplevel_handle = data;
	struct output *out;
	wl_list_for_each(out, &toplevel_handle->client_state->outputs, link) {
		if (out->wl_output == output) {
			toplevel_handle->outputs |= (1 << out->idx);
		}
	}
}

static void
state_handle_output_leave(void *data,
	struct ext_foreign_toplevel_state_handle_v1
		*ext_foreign_toplevel_state_handle_v1,
	struct wl_output *output)
{
	struct toplevel_handle *toplevel_handle = data;
	struct output *out;
	wl_list_for_each(out, &toplevel_handle->client_state->outputs, link) {
		if (out->wl_output == output) {
			toplevel_handle->outputs &= ~(1 << out->idx);
		}
	}
}

static void
state_handle_parent(void *data,
	struct ext_foreign_toplevel_state_handle_v1
		*ext_foreign_toplevel_state_handle_v1,
	struct ext_foreign_toplevel_handle_v1 *parent)
{
	struct toplevel_handle *toplevel_handle = data;
	if (parent) {
		struct toplevel_handle *parent_handle;
		wl_list_for_each(parent_handle,
			&toplevel_handle->client_state->state_handles, link) {
			if (parent_handle->list_handle == parent) {
				toplevel_handle->parent = parent_handle;
				break;
			}
		}
	} else {
		toplevel_handle->parent = NULL;
	}
}

static struct ext_foreign_toplevel_state_handle_v1_listener
	state_handle_listener = {
		.state = state_handle_state,
		.output_enter = state_handle_output_enter,
		.output_leave = state_handle_output_leave,
		.parent = state_handle_parent,
};

static void
toplevel_list_toplevel(void *data,
	struct ext_foreign_toplevel_list_v1 *ext_foreign_toplevel_list_v1,
	struct ext_foreign_toplevel_handle_v1 *toplevel)
{
	struct client_state *state = data;
	struct toplevel_handle *toplevel_handle =
		calloc(1, sizeof(*toplevel_handle));
	wl_list_insert(&state->state_handles, &toplevel_handle->link);
	toplevel_handle->client_state = state;
	toplevel_handle->list_handle = toplevel;
	ext_foreign_toplevel_handle_v1_add_listener(
		toplevel, &list_handle_listener, toplevel_handle);
	toplevel_handle->state_handle =
		ext_foreign_toplevel_state_manager_v1_get_state_handle(
			state->state_manager, toplevel);
	ext_foreign_toplevel_state_handle_v1_add_listener(
		toplevel_handle->state_handle, &state_handle_listener,
		toplevel_handle);
}

static void
toplevel_list_finished(void *data,
	struct ext_foreign_toplevel_list_v1 *ext_foreign_toplevel_list_v1)
{
}

struct ext_foreign_toplevel_list_v1_listener toplevel_list_listener = {
	.toplevel = toplevel_list_toplevel,
	.finished = toplevel_list_finished,
};

static void
state_manager_capabilities(void *data,
	struct ext_foreign_toplevel_state_manager_v1
		*ext_foreign_toplevel_state_manager_v1,
	uint32_t notifications, uint32_t actions)
{
	struct client_state *state = data;
	state->notification_caps = notifications;
	state->action_caps = actions;

	printf("============= received .capabilities event ============\n");
	printf("- notification_caps=(");
	if (state->notification_caps
		& EXT_FOREIGN_TOPLEVEL_STATE_MANAGER_V1_NOTIFICATIONS_MAXIMIZE) {
		printf("maximize ");
	}
	if (state->notification_caps
		& EXT_FOREIGN_TOPLEVEL_STATE_MANAGER_V1_NOTIFICATIONS_MINIMIZE) {
		printf("minimize ");
	}
	if (state->notification_caps
		& EXT_FOREIGN_TOPLEVEL_STATE_MANAGER_V1_NOTIFICATIONS_ACTIVATED) {
		printf("activated ");
	}
	if (state->notification_caps
		& EXT_FOREIGN_TOPLEVEL_STATE_MANAGER_V1_NOTIFICATIONS_FULLSCREEN) {
		printf("fullscreen ");
	}
	if (state->notification_caps
		& EXT_FOREIGN_TOPLEVEL_STATE_MANAGER_V1_NOTIFICATIONS_ALWAYS_ON_TOP) {
		printf("always-on-top ");
	}
	if (state->notification_caps
		& EXT_FOREIGN_TOPLEVEL_STATE_MANAGER_V1_NOTIFICATIONS_STICKY) {
		printf("sticky ");
	}
	if (state->notification_caps
		& EXT_FOREIGN_TOPLEVEL_STATE_MANAGER_V1_NOTIFICATIONS_SHADED) {
		printf("shaded ");
	}
	printf(")\n");
	printf("- action_caps=(");
	if (state->action_caps
		& EXT_FOREIGN_TOPLEVEL_STATE_MANAGER_V1_ACTIONS_CLOSE) {
		printf("close ");
	}
	if (state->action_caps
		& EXT_FOREIGN_TOPLEVEL_STATE_MANAGER_V1_ACTIONS_MAXIMIZE) {
		printf("maximize ");
	}
	if (state->action_caps
		& EXT_FOREIGN_TOPLEVEL_STATE_MANAGER_V1_ACTIONS_MINIMIZE) {
		printf("minimize ");
	}
	if (state->action_caps
		& EXT_FOREIGN_TOPLEVEL_STATE_MANAGER_V1_ACTIONS_ACTIVATED) {
		printf("activated ");
	}
	if (state->action_caps
		& EXT_FOREIGN_TOPLEVEL_STATE_MANAGER_V1_ACTIONS_FULLSCREEN) {
		printf("fullscreen ");
	}
	if (state->action_caps
		& EXT_FOREIGN_TOPLEVEL_STATE_MANAGER_V1_ACTIONS_ALWAYS_ON_TOP) {
		printf("always-on-top ");
	}
	if (state->action_caps
		& EXT_FOREIGN_TOPLEVEL_STATE_MANAGER_V1_ACTIONS_STICKY) {
		printf("sticky ");
	}
	if (state->action_caps
		& EXT_FOREIGN_TOPLEVEL_STATE_MANAGER_V1_ACTIONS_SHADED) {
		printf("shaded ");
	}
	printf(")\n");
	printf("=====================================================\n");
}

struct ext_foreign_toplevel_state_manager_v1_listener state_manager_listener = {
	.capabilities = state_manager_capabilities,
};

static struct toplevel_handle *
find_toplevel_handle_by_identifier(
	struct client_state *state, const char *identifier)
{
	struct toplevel_handle *toplevel_handle;
	wl_list_for_each(toplevel_handle, &state->state_handles, link) {
		if (toplevel_handle->identifier
			&& strcmp(toplevel_handle->identifier, identifier)
				   == 0) {
			return toplevel_handle;
		}
	}
	return NULL;
}

static void
handle_command(struct client_state *state, const char *line)
{
	char action[64];
	char identifier[256];
	if (sscanf(line, "%63s %255s", action, identifier) != 2) {
		fprintf(stderr, "Invalid command format. Use: <action> "
				"<toplevel-identifier>\n");
		return;
	}

	struct toplevel_handle *toplevel_handle =
		find_toplevel_handle_by_identifier(state, identifier);
	if (!toplevel_handle) {
		fprintf(stderr, "Toplevel not found: %s\n", identifier);
		return;
	}
	if (strcmp(action, "activate") == 0) {
		ext_foreign_toplevel_state_handle_v1_activate(
			toplevel_handle->state_handle, state->wl_seat);
	} else if (strcmp(action, "set-maximized") == 0) {
		ext_foreign_toplevel_state_handle_v1_set_maximized(
			toplevel_handle->state_handle);
	} else if (strcmp(action, "unset-maximized") == 0) {
		ext_foreign_toplevel_state_handle_v1_unset_maximized(
			toplevel_handle->state_handle);
	} else if (strcmp(action, "set-minimized") == 0) {
		ext_foreign_toplevel_state_handle_v1_set_minimized(
			toplevel_handle->state_handle);
	} else if (strcmp(action, "unset-minimized") == 0) {
		ext_foreign_toplevel_state_handle_v1_unset_minimized(
			toplevel_handle->state_handle);
	} else if (strcmp(action, "set-fullscreen") == 0) {
		ext_foreign_toplevel_state_handle_v1_set_fullscreen(
			toplevel_handle->state_handle, NULL);
	} else if (strcmp(action, "unset-fullscreen") == 0) {
		ext_foreign_toplevel_state_handle_v1_unset_fullscreen(
			toplevel_handle->state_handle);
	} else if (strcmp(action, "set-always-on-top") == 0) {
		ext_foreign_toplevel_state_handle_v1_set_always_on_top(
			toplevel_handle->state_handle);
	} else if (strcmp(action, "unset-always-on-top") == 0) {
		ext_foreign_toplevel_state_handle_v1_unset_always_on_top(
			toplevel_handle->state_handle);
	} else if (strcmp(action, "set-sticky") == 0) {
		ext_foreign_toplevel_state_handle_v1_set_sticky(
			toplevel_handle->state_handle);
	} else if (strcmp(action, "unset-sticky") == 0) {
		ext_foreign_toplevel_state_handle_v1_unset_sticky(
			toplevel_handle->state_handle);
	} else if (strcmp(action, "set-shaded") == 0) {
		ext_foreign_toplevel_state_handle_v1_set_shaded(
			toplevel_handle->state_handle);
	} else if (strcmp(action, "unset-shaded") == 0) {
		ext_foreign_toplevel_state_handle_v1_unset_shaded(
			toplevel_handle->state_handle);
	} else if (strcmp(action, "close") == 0) {
		ext_foreign_toplevel_state_handle_v1_close(
			toplevel_handle->state_handle);
	} else {
		fprintf(stderr, "Unknown action: %s\n", action);
	}
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
	printf("Type commands in the format: <action> <toplevel-identifier>\n"
	       "Available actions:\n"
	       "  activate\n"
	       "  set-maximized\n"
	       "  unset-maximized\n"
	       "  set-minimized\n"
	       "  unset-minimized\n"
	       "  set-fullscreen\n"
	       "  unset-fullscreen\n"
	       "  set-always-on-top\n"
	       "  unset-always-on-top\n"
	       "  set-sticky\n"
	       "  unset-sticky\n"
	       "  set-shaded\n"
	       "  unset-shaded\n"
	       "  close\n");

	struct client_state state = {};
	wl_list_init(&state.state_handles);
	wl_list_init(&state.outputs);
	state.wl_display = wl_display_connect(NULL);
	state.wl_registry = wl_display_get_registry(state.wl_display);
	wl_registry_add_listener(
		state.wl_registry, &wl_registry_listener, &state);
	wl_display_roundtrip(state.wl_display);
	assert(state.toplevel_list && state.state_manager);

	ext_foreign_toplevel_list_v1_add_listener(
		state.toplevel_list, &toplevel_list_listener, &state);
	ext_foreign_toplevel_state_manager_v1_add_listener(
		state.state_manager, &state_manager_listener, &state);
	wl_display_roundtrip(state.wl_display);

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
