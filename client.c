#include "xdg-decoration-protocol.h"
#include "xdg-shell-protocol.h"
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
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
	struct zxdg_decoration_manager_v1 *zxdg_decoration_manager_v1;
	/* Objects */
	struct wl_surface *wl_surface;
	struct xdg_surface *xdg_surface;
	struct xdg_toplevel *xdg_toplevel;
	struct zxdg_toplevel_decoration_v1 *zxdg_toplevel_decoration_v1;

	int width, height;

	uv_loop_t *loop;
	uv_poll_t poll_handle;
};

static void
wl_buffer_release(void *data, struct wl_buffer *wl_buffer)
{
	/* Sent by the compositor when it's no longer using this buffer */
	wl_buffer_destroy(wl_buffer);
}

static const struct wl_buffer_listener wl_buffer_listener = {
	.release = wl_buffer_release,
};

static struct wl_buffer *
draw_frame(struct client_state *state)
{
	int stride = state->width * 4;
	int size = stride * state->height;

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

	struct wl_shm_pool *pool = wl_shm_create_pool(state->wl_shm, fd, size);
	struct wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0,
		state->width, state->height, stride, WL_SHM_FORMAT_XRGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);

	/* Draw checkerboxed background */
	for (int y = 0; y < state->height; ++y) {
		for (int x = 0; x < state->width; ++x) {
			if ((x + y / 8 * 8) % 16 < 8)
				data[y * state->width + x] = 0xFF666666;
			else
				data[y * state->width + x] = 0xFFEEEEEE;
		}
	}

	munmap(data, size);
	wl_buffer_add_listener(buffer, &wl_buffer_listener, NULL);
	return buffer;
}

static void
xdg_surface_configure(
	void *data, struct xdg_surface *xdg_surface, uint32_t serial)
{
	struct client_state *state = data;
	xdg_surface_ack_configure(xdg_surface, serial);
	wl_surface_commit(state->wl_surface);

	struct wl_buffer *buffer = draw_frame(state);
	wl_surface_attach(state->wl_surface, buffer, 0, 0);
	wl_surface_commit(state->wl_surface);
}

static const struct xdg_surface_listener xdg_surface_listener = {
	.configure = xdg_surface_configure,
};

static void
xdg_wm_base_ping(void *data, struct xdg_wm_base *xdg_wm_base, uint32_t serial)
{
	xdg_wm_base_pong(xdg_wm_base, serial);
}

static const struct xdg_wm_base_listener xdg_wm_base_listener = {
	.ping = xdg_wm_base_ping,
};

static void
draw(struct client_state *state)
{
	struct timespec ts = {0};
	clock_gettime(CLOCK_MONOTONIC, &ts);
	uint32_t time = ts.tv_nsec / 1000000 + ts.tv_sec * 1000;

	struct wl_buffer *buffer = draw_frame(state);
	wl_surface_attach(state->wl_surface, buffer, 0, 0);
	wl_surface_damage_buffer(state->wl_surface, 0, 0, INT32_MAX, INT32_MAX);
	wl_surface_commit(state->wl_surface);
}

static void
registry_global(void *data, struct wl_registry *wl_registry, uint32_t name,
	const char *interface, uint32_t version)
{
	struct client_state *state = data;
	if (!strcmp(interface, wl_shm_interface.name)) {
		state->wl_shm = wl_registry_bind(
			wl_registry, name, &wl_shm_interface, 1);
	} else if (!strcmp(interface, wl_compositor_interface.name)) {
		state->wl_compositor = wl_registry_bind(
			wl_registry, name, &wl_compositor_interface, 4);
	} else if (!strcmp(interface, xdg_wm_base_interface.name)) {
		state->xdg_wm_base = wl_registry_bind(
			wl_registry, name, &xdg_wm_base_interface, 1);
		xdg_wm_base_add_listener(
			state->xdg_wm_base, &xdg_wm_base_listener, state);
	} else if (!strcmp(interface, wl_seat_interface.name)) {
		state->wl_seat = wl_registry_bind(
			wl_registry, name, &wl_seat_interface, 8);
	} else if (!strcmp(interface,
			   zxdg_decoration_manager_v1_interface.name)) {
		state->zxdg_decoration_manager_v1 =
			wl_registry_bind(wl_registry, name,
				&zxdg_decoration_manager_v1_interface, 1);
	}
}

static void
registry_global_remove(
	void *data, struct wl_registry *wl_registry, uint32_t name)
{
	/* This space deliberately left blank */
}

static const struct wl_registry_listener wl_registry_listener = {
	.global = registry_global,
	.global_remove = registry_global_remove,
};

static void
on_wayland_event(uv_poll_t *handle, int status, int events)
{
	struct client_state *state = handle->data;
	if (events & UV_READABLE) {
		if (wl_display_dispatch(state->wl_display) == -1)
			fprintf(stderr, "Failed to dispatch Wayland events.\n");
	}
	wl_display_flush(state->wl_display);
}

static void
xdg_toplevel_configure(void *data, struct xdg_toplevel *xdg_toplevel,
	int32_t width, int32_t height, struct wl_array *states)
{
	struct client_state *state = data;
	if (width > 0 || height > 0) {
		state->width = width;
		state->height = height;
	} else if (state->width == 0 || state->height == 0) {
		state->width = 600;
		state->height = 600;
	}
}

static void
xdg_toplevel_close(void *data, struct xdg_toplevel *xdg_toplevel)
{
	_exit(0);
}

static void
xdg_toplevel_configure_bounds(void *data, struct xdg_toplevel *xdg_toplevel,
	int32_t width, int32_t height)
{
}

static void
xdg_toplevel_wm_capabilities(void *data, struct xdg_toplevel *xdg_toplevel,
	struct wl_array *capabilities)
{
}

static const struct xdg_toplevel_listener xdg_toplevel_listener = {
	.configure = xdg_toplevel_configure,
	.close = xdg_toplevel_close,
	.configure_bounds = xdg_toplevel_configure_bounds,
	.wm_capabilities = xdg_toplevel_wm_capabilities,
};

int
main(int argc, char *argv[])
{
	struct client_state state = {0};
	state.wl_display = wl_display_connect(NULL);
	state.wl_registry = wl_display_get_registry(state.wl_display);
	wl_registry_add_listener(
		state.wl_registry, &wl_registry_listener, &state);
	wl_display_roundtrip(state.wl_display);

	state.wl_surface = wl_compositor_create_surface(state.wl_compositor);
	state.xdg_surface = xdg_wm_base_get_xdg_surface(
		state.xdg_wm_base, state.wl_surface);
	xdg_surface_add_listener(
		state.xdg_surface, &xdg_surface_listener, &state);
	state.xdg_toplevel = xdg_surface_get_toplevel(state.xdg_surface);
	xdg_toplevel_add_listener(
		state.xdg_toplevel, &xdg_toplevel_listener, &state);
	xdg_toplevel_set_title(state.xdg_toplevel, "Example client");
	state.zxdg_toplevel_decoration_v1 =
		zxdg_decoration_manager_v1_get_toplevel_decoration(
			state.zxdg_decoration_manager_v1, state.xdg_toplevel);
	zxdg_toplevel_decoration_v1_set_mode(state.zxdg_toplevel_decoration_v1,
		ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
	wl_surface_commit(state.wl_surface);
	wl_display_flush(state.wl_display);

	state.loop = uv_default_loop();
	state.poll_handle.data = &state;
	uv_poll_init(state.loop, &state.poll_handle,
		wl_display_get_fd(state.wl_display));
	uv_poll_start(&state.poll_handle, UV_READABLE, on_wayland_event);

	uv_run(state.loop, UV_RUN_DEFAULT);

	return 0;
}
