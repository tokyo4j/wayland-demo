#include "wlr-layer-shell-unstable-v1-protocol.h"
#include "xdg-shell-protocol.h"
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <uv.h>
#include <wayland-client-protocol.h>
#include <wayland-client.h>

/* Shared memory support code */
static void
randname(char *buf)
{
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	long r = ts.tv_nsec;
	for (int i = 0; i < 6; ++i) {
		buf[i] = 'A' + (r & 15) + (r & 16) * 2;
		r >>= 5;
	}
}

static int
create_shm_file(void)
{
	int retries = 100;
	do {
		char name[] = "/wl_shm-XXXXXX";
		randname(name + sizeof(name) - 7);
		--retries;
		int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
		if (fd >= 0) {
			shm_unlink(name);
			return fd;
		}
	} while (retries > 0 && errno == EEXIST);
	return -1;
}

static int
allocate_shm_file(size_t size)
{
	int fd = create_shm_file();
	if (fd < 0)
		return -1;
	int ret;
	do {
		ret = ftruncate(fd, size);
	} while (ret < 0 && errno == EINTR);
	if (ret < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

/* Wayland code */
struct client_state {
	/* Globals */
	struct wl_display *wl_display;
	struct wl_registry *wl_registry;
	struct wl_shm *wl_shm;
	struct wl_compositor *wl_compositor;
	struct xdg_wm_base *xdg_wm_base;
	struct wl_seat *wl_seat;
	struct wl_subcompositor *wl_subcompositor;
	struct zwlr_layer_shell_v1 *zwlr_layer_shell_v1;
	/* Objects */
	struct wl_surface *wl_surface;
	struct wl_pointer *wl_pointer;
	struct wl_keyboard *wl_keyboard;
	struct zwlr_layer_surface_v1 *zwlr_layer_surface_v1;
	struct {
		struct wl_surface *wl_surface;
		struct xdg_surface *xdg_surface;
		struct xdg_popup *xdg_popup;
		struct xdg_positioner *xdg_positioner;
		int width, height;
		struct {
			struct wl_surface *wl_surface;
			struct wl_subsurface *wl_subsurface;
		} subsurface;
	} popup;

	int width, height;

	uv_loop_t *loop;
	uv_poll_t poll_handle;

	struct {
		int height;
		uint32_t colors[2];
		uint32_t popup_colors[2];
		int popup_x;
		int popup_anchor_width;
		int popup_width;
		int popup_height;
		int popup_subsurface_x;
		int popup_subsurface_y;
		int popup_subsurface_width;
		int popup_subsurface_height;
		uint32_t popup_subsurface_colors[2];
	} defaults;
};

static void
handle_wl_buffer_release(void *data, struct wl_buffer *wl_buffer)
{
	/* Sent by the compositor when it's no longer using this buffer */
	wl_buffer_destroy(wl_buffer);
}

static const struct wl_buffer_listener wl_buffer_listener = {
	.release = handle_wl_buffer_release,
};

static struct wl_buffer *
draw_frame(struct wl_shm *wl_shm, int w, int h, uint32_t colors[static 2])
{
	int stride = w * 4;
	int size = stride * h;

	int fd = allocate_shm_file(size);
	if (fd == -1) {
		return NULL;
	}

	uint32_t *data =
		mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (data == MAP_FAILED) {
		close(fd);
		return NULL;
	}

	struct wl_shm_pool *pool = wl_shm_create_pool(wl_shm, fd, size);
	struct wl_buffer *buffer = wl_shm_pool_create_buffer(
		pool, 0, w, h, stride, WL_SHM_FORMAT_XRGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);

	/* Draw checkerboxed background */
	for (int y = 0; y < h; ++y) {
		for (int x = 0; x < w; ++x) {
			if ((x + y / 8 * 8) % 16 < 8)
				data[y * w + x] = colors[0];
			else
				data[y * w + x] = colors[1];
		}
	}

	munmap(data, size);
	wl_buffer_add_listener(buffer, &wl_buffer_listener, NULL);
	return buffer;
}

static void
handle_xdg_wm_base_ping(
	void *data, struct xdg_wm_base *xdg_wm_base, uint32_t serial)
{
	xdg_wm_base_pong(xdg_wm_base, serial);
}

static const struct xdg_wm_base_listener xdg_wm_base_listener = {
	.ping = handle_xdg_wm_base_ping,
};

static void
handle_registry_global(void *data, struct wl_registry *wl_registry,
	uint32_t name, const char *interface, uint32_t version)
{
	struct client_state *state = data;
	if (!strcmp(interface, wl_shm_interface.name)) {
		state->wl_shm = wl_registry_bind(
			wl_registry, name, &wl_shm_interface, 1);
	} else if (!strcmp(interface, wl_compositor_interface.name)) {
		state->wl_compositor = wl_registry_bind(
			wl_registry, name, &wl_compositor_interface, 4);
	} else if (!strcmp(interface, wl_subcompositor_interface.name)) {
		state->wl_subcompositor = wl_registry_bind(
			wl_registry, name, &wl_subcompositor_interface, 1);
	} else if (!strcmp(interface, xdg_wm_base_interface.name)) {
		state->xdg_wm_base = wl_registry_bind(
			wl_registry, name, &xdg_wm_base_interface, 1);
		xdg_wm_base_add_listener(
			state->xdg_wm_base, &xdg_wm_base_listener, state);
	} else if (!strcmp(interface, wl_seat_interface.name)) {
		state->wl_seat = wl_registry_bind(
			wl_registry, name, &wl_seat_interface, 8);
	} else if (!strcmp(interface, zwlr_layer_shell_v1_interface.name)) {
		state->zwlr_layer_shell_v1 = wl_registry_bind(
			wl_registry, name, &zwlr_layer_shell_v1_interface, 4);
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
handle_zwlr_layer_surface_v1_configure(void *data,
	struct zwlr_layer_surface_v1 *zwlr_layer_surface_v1, uint32_t serial,
	uint32_t width, uint32_t height)
{
	struct client_state *state = data;
	state->width = width;
	state->height = height;

	zwlr_layer_surface_v1_ack_configure(
		state->zwlr_layer_surface_v1, serial);

	struct wl_buffer *buffer = draw_frame(state->wl_shm, state->width,
		state->height, state->defaults.colors);
	wl_surface_attach(state->wl_surface, buffer, 0, 0);
	wl_surface_commit(state->wl_surface);
}

static void
handle_zwlr_layer_surface_v1_closed(
	void *data, struct zwlr_layer_surface_v1 *zwlr_layer_surface_v1)
{
}

static const struct zwlr_layer_surface_v1_listener
	zwlr_layer_surface_v1_listener = {
		.configure = handle_zwlr_layer_surface_v1_configure,
		.closed = handle_zwlr_layer_surface_v1_closed,
};

static void
handle_popup_xdg_surface_configure(
	void *data, struct xdg_surface *xdg_surface, uint32_t serial)
{
	struct client_state *state = data;
	xdg_surface_ack_configure(xdg_surface, serial);

	struct wl_buffer *buffer = draw_frame(state->wl_shm, state->popup.width,
		state->popup.height, state->defaults.popup_colors);
	wl_surface_attach(state->popup.wl_surface, buffer, 0, 0);

	struct wl_buffer *sub_buffer = draw_frame(state->wl_shm,
		state->defaults.popup_subsurface_width,
		state->defaults.popup_subsurface_height,
		state->defaults.popup_subsurface_colors);
	wl_surface_attach(state->popup.subsurface.wl_surface, sub_buffer, 0, 0);
	wl_subsurface_set_position(state->popup.subsurface.wl_subsurface,
		state->defaults.popup_subsurface_x,
		state->defaults.popup_subsurface_y);
	wl_surface_commit(state->popup.subsurface.wl_surface);

	wl_surface_commit(state->popup.wl_surface);
}

static const struct xdg_surface_listener popup_xdg_surface_listener = {
	.configure = handle_popup_xdg_surface_configure,
};

static void
handle_popup_xdg_popup_configure(void *data, struct xdg_popup *xdg_popup,
	int32_t x, int32_t y, int32_t width, int32_t height)
{
	struct client_state *state = data;
	state->popup.width = width;
	state->popup.height = height;
}

static void
handle_popup_xdg_popup_popup_done(void *data, struct xdg_popup *xdg_popup)
{
}

static void
handle_popup_xdg_popup_repositioned(
	void *data, struct xdg_popup *xdg_popup, uint32_t token)
{
}

static const struct xdg_popup_listener popup_xdg_popup_listener = {
	.configure = handle_popup_xdg_popup_configure,
	.popup_done = handle_popup_xdg_popup_popup_done,
	.repositioned = handle_popup_xdg_popup_repositioned,
};

int
main(int argc, char *argv[])
{
	struct client_state state = {
		.defaults =
			{
				.height = 100,
				.colors = {0xff666666, 0xffeeeeee},
				.popup_colors = {0xffff6666, 0xffeeeeee},
				.popup_x = 200,
				.popup_width = 100,
				.popup_height = 50,
				.popup_anchor_width = 50,
				.popup_subsurface_x = 10,
				.popup_subsurface_y = 10,
				.popup_subsurface_width = 20,
				.popup_subsurface_height = 20,
				.popup_subsurface_colors = {0xff66ff66,
					0xffeeeeee},
			},
	};
	state.wl_display = wl_display_connect(NULL);
	state.wl_registry = wl_display_get_registry(state.wl_display);
	wl_registry_add_listener(
		state.wl_registry, &wl_registry_listener, &state);
	wl_display_roundtrip(state.wl_display);

	state.wl_pointer = wl_seat_get_pointer(state.wl_seat);
	state.wl_keyboard = wl_seat_get_keyboard(state.wl_seat);

	state.wl_surface = wl_compositor_create_surface(state.wl_compositor);
	state.zwlr_layer_surface_v1 = zwlr_layer_shell_v1_get_layer_surface(
		state.zwlr_layer_shell_v1, state.wl_surface, NULL,
		ZWLR_LAYER_SHELL_V1_LAYER_TOP, "wayland-demo");
	zwlr_layer_surface_v1_add_listener(state.zwlr_layer_surface_v1,
		&zwlr_layer_surface_v1_listener, &state);
	zwlr_layer_surface_v1_set_anchor(state.zwlr_layer_surface_v1,
		ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM
			| ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT
			| ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
	zwlr_layer_surface_v1_set_exclusive_zone(
		state.zwlr_layer_surface_v1, state.defaults.height);
	zwlr_layer_surface_v1_set_size(
		state.zwlr_layer_surface_v1, 0, state.defaults.height);
	zwlr_layer_surface_v1_set_keyboard_interactivity(
		state.zwlr_layer_surface_v1,
		ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_ON_DEMAND);

	state.popup.wl_surface =
		wl_compositor_create_surface(state.wl_compositor);
	state.popup.xdg_surface = xdg_wm_base_get_xdg_surface(
		state.xdg_wm_base, state.popup.wl_surface);
	xdg_surface_add_listener(
		state.popup.xdg_surface, &popup_xdg_surface_listener, &state);
	state.popup.xdg_positioner =
		xdg_wm_base_create_positioner(state.xdg_wm_base);
	xdg_positioner_set_anchor_rect(state.popup.xdg_positioner,
		state.defaults.popup_x, 0, state.defaults.popup_anchor_width,
		state.defaults.height);
	xdg_positioner_set_anchor(
		state.popup.xdg_positioner, XDG_POSITIONER_ANCHOR_TOP_LEFT);
	xdg_positioner_set_gravity(
		state.popup.xdg_positioner, XDG_POSITIONER_GRAVITY_TOP_RIGHT);
	xdg_positioner_set_size(state.popup.xdg_positioner,
		state.defaults.popup_width, state.defaults.popup_height);
	state.popup.xdg_popup = xdg_surface_get_popup(
		state.popup.xdg_surface, NULL, state.popup.xdg_positioner);
	xdg_popup_add_listener(
		state.popup.xdg_popup, &popup_xdg_popup_listener, &state);
	zwlr_layer_surface_v1_get_popup(
		state.zwlr_layer_surface_v1, state.popup.xdg_popup);

	state.popup.subsurface.wl_surface =
		wl_compositor_create_surface(state.wl_compositor);
	state.popup.subsurface.wl_subsurface = wl_subcompositor_get_subsurface(
		state.wl_subcompositor, state.popup.subsurface.wl_surface,
		state.popup.wl_surface);

	wl_surface_commit(state.wl_surface);
	wl_surface_commit(state.popup.wl_surface);
	wl_display_flush(state.wl_display);

	state.loop = uv_default_loop();
	state.poll_handle.data = &state;
	uv_poll_init(state.loop, &state.poll_handle,
		wl_display_get_fd(state.wl_display));
	uv_poll_start(&state.poll_handle, UV_READABLE, on_wayland_event);

	uv_run(state.loop, UV_RUN_DEFAULT);

	return 0;
}
