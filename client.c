#include "virtual-keyboard-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"
#include <errno.h>
#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <stdbool.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client-protocol.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

/* Shared memory support code */
static void randname(char *buf) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    long r = ts.tv_nsec;
    for (int i = 0; i < 6; ++i) {
        buf[i] = 'A' + (r & 15) + (r & 16) * 2;
        r >>= 5;
    }
}

static int create_shm_file(void) {
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

static int allocate_shm_file(size_t size) {
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
    struct zwp_virtual_keyboard_manager_v1 *vk_manager;
    /* Objects */
    struct wl_surface *wl_surface;
    struct xdg_surface *xdg_surface;
    struct xdg_toplevel *xdg_toplevel;
    struct wl_keyboard *wl_keyboard;
    struct zwp_virtual_keyboard_v1 *vk;
};

static void wl_buffer_release(void *data, struct wl_buffer *wl_buffer) {
    /* Sent by the compositor when it's no longer using this buffer */
    wl_buffer_destroy(wl_buffer);
}

static const struct wl_buffer_listener wl_buffer_listener = {
    .release = wl_buffer_release,
};

static struct wl_buffer *draw_frame(struct client_state *state) {
    const int width = 800, height = 800;
    int stride = width * 4;
    int size = stride * height;

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
    struct wl_buffer *buffer = wl_shm_pool_create_buffer(
        pool, 0, width, height, stride, WL_SHM_FORMAT_XRGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);

    /* Draw checkerboxed background */
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            if ((x + y / 8 * 8) % 16 < 8)
                data[y * width + x] = 0xFF666666;
            else
                data[y * width + x] = 0xFFEEEEEE;
        }
    }

    munmap(data, size);
    wl_buffer_add_listener(buffer, &wl_buffer_listener, NULL);
    return buffer;
}

static void xdg_surface_configure(void *data, struct xdg_surface *xdg_surface,
                                  uint32_t serial) {
    puts("xdg_surface.configure");
    struct client_state *state = data;
    xdg_surface_ack_configure(xdg_surface, serial);

    struct wl_buffer *buffer = draw_frame(state);
    wl_surface_attach(state->wl_surface, buffer, 0, 0);
    wl_surface_commit(state->wl_surface);
}

static const struct xdg_surface_listener xdg_surface_listener = {
    .configure = xdg_surface_configure,
};

static void xdg_wm_base_ping(void *data, struct xdg_wm_base *xdg_wm_base,
                             uint32_t serial) {
    xdg_wm_base_pong(xdg_wm_base, serial);
}

static const struct xdg_wm_base_listener xdg_wm_base_listener = {
    .ping = xdg_wm_base_ping,
};

static void noop() {}

static bool got_keymap = false;

void wl_keyboard_modifiers(void *data, struct wl_keyboard *wl_keyboard,
                           uint32_t serial, uint32_t mods_depressed,
                           uint32_t mods_latched, uint32_t mods_locked,
                           uint32_t group) {
    puts("wl_keyboard.modifiers");

#define COLOR_RESET "\033[0m"
#define COLOR_RED "\033[0;31m"
    if (!got_keymap)
        puts(COLOR_RED "Received modifier without keymap" COLOR_RESET);
}

void wl_keyboard_keymap(void *data, struct wl_keyboard *wl_keyboard,
                        uint32_t format, int32_t fd, uint32_t size) {
    puts("wl_keyboard.keymap");
    got_keymap = true;
}

static const struct wl_keyboard_listener wl_keyboard_listener = {
    .keymap = wl_keyboard_keymap,
    .enter = noop,
    .leave = noop,
    .key = noop,
    .modifiers = wl_keyboard_modifiers,
    .repeat_info = noop,
};

static void registry_global(void *data, struct wl_registry *wl_registry,
                            uint32_t name, const char *interface,
                            uint32_t version) {
    struct client_state *state = data;
    if (!strcmp(interface, wl_shm_interface.name)) {
        state->wl_shm =
            wl_registry_bind(wl_registry, name, &wl_shm_interface, 1);
    } else if (!strcmp(interface, wl_compositor_interface.name)) {
        state->wl_compositor =
            wl_registry_bind(wl_registry, name, &wl_compositor_interface, 4);
    } else if (!strcmp(interface, xdg_wm_base_interface.name)) {
        state->xdg_wm_base =
            wl_registry_bind(wl_registry, name, &xdg_wm_base_interface, 1);
        xdg_wm_base_add_listener(state->xdg_wm_base, &xdg_wm_base_listener,
                                 state);
    } else if (!strcmp(interface, wl_seat_interface.name)) {
        state->wl_seat =
            wl_registry_bind(wl_registry, name, &wl_seat_interface, 8);
    } else if (!strcmp(interface,
                       zwp_virtual_keyboard_manager_v1_interface.name)) {
        state->vk_manager = wl_registry_bind(
            wl_registry, name, &zwp_virtual_keyboard_manager_v1_interface, 1);
    }
}

static void registry_global_remove(void *data, struct wl_registry *wl_registry,
                                   uint32_t name) {
    /* This space deliberately left blank */
}

static const struct wl_registry_listener wl_registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

int main(int argc, char *argv[]) {
    struct client_state state = {0};
    state.wl_display = wl_display_connect(NULL);
    state.wl_registry = wl_display_get_registry(state.wl_display);
    wl_registry_add_listener(state.wl_registry, &wl_registry_listener, &state);
    wl_display_roundtrip(state.wl_display);

    state.wl_surface = wl_compositor_create_surface(state.wl_compositor);
    state.xdg_surface =
        xdg_wm_base_get_xdg_surface(state.xdg_wm_base, state.wl_surface);
    xdg_surface_add_listener(state.xdg_surface, &xdg_surface_listener, &state);
    state.xdg_toplevel = xdg_surface_get_toplevel(state.xdg_surface);
    xdg_toplevel_set_title(state.xdg_toplevel, "Example client");
    wl_surface_commit(state.wl_surface);

    puts("Created surface");

    state.vk = zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(
        state.vk_manager, state.wl_seat);
    struct xkb_context *xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    struct xkb_keymap *keymap =
        xkb_keymap_new_from_names(xkb_ctx, NULL, XKB_KEYMAP_COMPILE_NO_FLAGS);
    char *keymap_string =
        xkb_keymap_get_as_string(keymap, XKB_KEYMAP_FORMAT_TEXT_V1);
    int keymap_size = strlen(keymap_string) + 1;
    int keymap_fd = allocate_shm_file(keymap_size);
    char *keymap_dst = mmap(NULL, keymap_size, PROT_READ | PROT_WRITE,
                            MAP_SHARED, keymap_fd, 0);
    memcpy(keymap_dst, keymap_string, keymap_size);

    zwp_virtual_keyboard_v1_keymap(state.vk, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1,
                                   keymap_fd, keymap_size);
    zwp_virtual_keyboard_v1_key(state.vk, 0, KEY_X, 1);
    zwp_virtual_keyboard_v1_key(state.vk, 10, KEY_X, 0);
    zwp_virtual_keyboard_v1_destroy(state.vk);

    puts("Destroyed virtual keyboard");

    state.wl_keyboard = wl_seat_get_keyboard(state.wl_seat);
    wl_keyboard_add_listener(state.wl_keyboard, &wl_keyboard_listener, NULL);

    puts("Got keyboard");

    while (wl_display_dispatch(state.wl_display)) {
        /* This space deliberately left blank */
    }

    return 0;
}