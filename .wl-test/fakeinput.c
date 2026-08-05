// Minimal KWin fake-input driver for verifying pointer-driven titlebar tab
// behaviour. Usage: fakeinput <cmd>... where cmd is one of:
//   abs X Y      absolute pointer motion (logical coords)
//   down | up    left button press / release
//   sleep MS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wayland-client.h>
#include "fake-input-client.h"
#include <linux/input.h>

static struct org_kde_kwin_fake_input *fi = NULL;

static void
global_add(void *data, struct wl_registry *reg, uint32_t name, const char *iface, uint32_t version) {
    (void)data;
    if (strcmp(iface, org_kde_kwin_fake_input_interface.name) == 0)
        fi = wl_registry_bind(reg, name, &org_kde_kwin_fake_input_interface, version < 4 ? version : 4);
}
static void global_remove(void *d, struct wl_registry *r, uint32_t n) { (void)d; (void)r; (void)n; }
static const struct wl_registry_listener reg_listener = { global_add, global_remove };

int
main(int argc, char **argv) {
    struct wl_display *dpy = wl_display_connect(NULL);
    if (!dpy) { fprintf(stderr, "no wayland display\n"); return 1; }
    struct wl_registry *reg = wl_display_get_registry(dpy);
    wl_registry_add_listener(reg, &reg_listener, NULL);
    wl_display_roundtrip(dpy);
    if (!fi) { fprintf(stderr, "no org_kde_kwin_fake_input global\n"); return 1; }
    org_kde_kwin_fake_input_authenticate(fi, "kitty-titlebar-tabs-test", "verify hover/drag animations");
    wl_display_roundtrip(dpy);
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "abs") == 0 && i + 2 < argc) {
            double x = atof(argv[i+1]), y = atof(argv[i+2]); i += 2;
            org_kde_kwin_fake_input_pointer_motion_absolute(fi, wl_fixed_from_double(x), wl_fixed_from_double(y));
        } else if (strcmp(argv[i], "down") == 0) {
            org_kde_kwin_fake_input_button(fi, BTN_LEFT, 1);
        } else if (strcmp(argv[i], "up") == 0) {
            org_kde_kwin_fake_input_button(fi, BTN_LEFT, 0);
        } else if (strcmp(argv[i], "sleep") == 0 && i + 1 < argc) {
            wl_display_flush(dpy);
            usleep(atoi(argv[++i]) * 1000);
            continue;
        } else { fprintf(stderr, "bad arg: %s\n", argv[i]); return 1; }
        wl_display_roundtrip(dpy);
    }
    wl_display_flush(dpy);
    wl_display_disconnect(dpy);
    return 0;
}
