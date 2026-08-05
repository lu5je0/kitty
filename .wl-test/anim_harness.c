// Offscreen harness verifying the pointer-driven animation logic of
// glfw/wl_titlebar_tabs.c with a fake clock: hover fade-in, drag reorder
// slide-in, and dying-tab fade-out.
#include "internal.h"
#include "wl_client_side_decorations.h"
#include "wl_titlebar_tabs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// stubs {{{
_GLFWlibrary _glfw;
static _GLFWwindow test_window;
static monotonic_t fake_now = 1000000000ll;
monotonic_t monotonic_start_time = 0;
monotonic_t monotonic_(void) { return fake_now; }

extern void glfwWaylandSetTitlebarTabs(GLFWwindow *handle, const GLFWTitlebarTab *tabs, size_t count, uint32_t bar_color, bool use_system_color, int forced_appearance);

_GLFWwindow* _glfwWindowForId(GLFWid id) { return id == test_window.id ? &test_window : NULL; }
char* _glfw_strdup(const char *s) { return strdup(s); }
void _glfwInputError(int code UNUSED, const char *fmt UNUSED, ...) {}

bool csd_should_window_be_decorated(_GLFWwindow *w UNUSED) { return true; }
void csd_set_visible(_GLFWwindow *w UNUSED, bool v UNUSED) {}
bool csd_set_titlebar_color(_GLFWwindow *w UNUSED, uint32_t c UNUSED, bool s UNUSED) { return false; }
// corner shadow patches never build here (no titlebar surface / shadow tile)
int createAnonymousFile(off_t size UNUSED) { return -1; }

#define BAR_W 800
#define BAR_H 28
static uint32_t bar_buf[BAR_W * 60];  // tall enough for 28 logical px at 2x
#define BAR_BG 0x393A39u  // forced dark + focused parity colour
static unsigned render_count = 0;
static void render(void) {
    wl_titlebar_tabs_render_bar(&test_window, (uint8_t*)bar_buf, BAR_BG, 0xffffff, 0x444444, true);
    render_count++;
}
bool csd_change_title(_GLFWwindow *w UNUSED) { render(); return true; }

static timer_callback_func timer_cb = NULL;
static int timer_enabled = 0;
id_type addTimer(EventLoopData *eld UNUSED, const char *name UNUSED, monotonic_t interval UNUSED, int enabled UNUSED, bool repeats UNUSED, timer_callback_func cb, void *cb_data UNUSED, GLFWuserdatafreefun free_fn UNUSED) {
    timer_cb = cb; return 42;
}
void toggleTimer(EventLoopData *eld UNUSED, id_type id UNUSED, int enabled) { timer_enabled = enabled; }

static int last_action = -1; static unsigned long long last_action_tab = 0; static int last_action_index = -1;
static void action_cb(GLFWwindow *w UNUSED, GLFWTitlebarTabAction a, unsigned long long tab_id, int index) {
    last_action = a; last_action_tab = tab_id; last_action_index = index;
}
static bool text_cb(GLFWwindow *w UNUSED, const char *t UNUSED, unsigned sz UNUSED, uint32_t fg UNUSED, uint32_t bg UNUSED, uint8_t *buf UNUSED, size_t width UNUSED, size_t height UNUSED, float xo UNUSED, float yo UNUSED, size_t rm UNUSED) {
    return false;  // no text: keeps sampled pixels clean
}
// }}}

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)

static uint32_t px(int x, int y) { return bar_buf[y * BAR_W + x] & 0xffffff; }
static int cdist(uint32_t a, uint32_t b) {
    int d = 0;
#define CH(sh) { int q = (int)((a >> sh) & 0xff) - (int)((b >> sh) & 0xff); if (q < 0) q = -q; if (q > d) d = q; }
    CH(16); CH(8); CH(0);
#undef CH
    return d;
}
static uint32_t mixc(uint32_t a, uint32_t b, double t) {
    uint32_t r = (uint32_t)(((a >> 16) & 0xff) * (1 - t) + ((b >> 16) & 0xff) * t + 0.5);
    uint32_t g = (uint32_t)(((a >> 8) & 0xff) * (1 - t) + ((b >> 8) & 0xff) * t + 0.5);
    uint32_t bl = (uint32_t)((a & 0xff) * (1 - t) + (b & 0xff) * t + 0.5);
    return (r << 16) | (g << 8) | bl;
}

#define C0 0x503030u
#define C1 0x305030u
#define C2 0x303050u
#define ACTIVE_BG 0x626366u

static void set_tabs3(unsigned long long a, unsigned long long b, unsigned long long c) {
    // tab colors keyed by id so reorders keep per-tab colors stable
    const uint32_t cols[4] = {0, C0, C1, C2};
    GLFWTitlebarTab tabs[3] = {
        {.tab_id = a, .title = "one", .is_active = false, .needs_attention = false, .fg = 0xcccccc, .bg = cols[a]},
        {.tab_id = b, .title = "two", .is_active = false, .needs_attention = false, .fg = 0xcccccc, .bg = cols[b]},
        {.tab_id = c, .title = "three", .is_active = false, .needs_attention = false, .fg = 0xcccccc, .bg = cols[c]},
    };
    glfwWaylandSetTitlebarTabs((GLFWwindow*)&test_window, tabs, 3, BAR_BG, false, 2);
}

// Fractional scale smoke checks: geometry is discovered by scanning pixel
// runs instead of predicting the scaled layout, then hover/drag are driven
// with logical coordinates like the real pointer path.
static int find_run(uint32_t want, int y, int start_x, int *run_w) {
    int x = start_x;
    while (x < BAR_W && cdist(px(x, y), want) > 2) x++;
    if (x >= BAR_W) return -1;
    int e = x;
    while (e < BAR_W && cdist(px(e, y), want) <= 2) e++;
    *run_w = e - x;
    return x;
}

static void run_scale_checks(double f, unsigned long long win_id) {
    test_window.id = win_id;  /* fresh per-scale state */
    _glfw.focusedWindowId = win_id;
    test_window.wl.decorations.for_window_state.fscale = f;
    test_window.wl.decorations.titlebar.buffer.height = (int)(BAR_H * f);  // bar height scales too
    const int y = (int)(14 * f);
    set_tabs3(1, 2, 3);
    render();
    int w0 = 0, w1 = 0, w2 = 0;
    const int x0 = find_run(C0, y, 0, &w0);
    const int x1 = x0 < 0 ? -1 : find_run(C1, y, x0 + w0, &w1);
    const int x2 = x1 < 0 ? -1 : find_run(C2, y, x1 + w1, &w2);
    CHECK(x0 >= 0 && x1 > x0 && x2 > x1, "scale %.2f: tabs in order (%d, %d, %d)", f, x0, x1, x2);
    if (x2 < 0) return;
    CHECK(abs(w0 - w1) <= 2 && abs(w1 - w2) <= 2, "scale %.2f: uniform widths (%d, %d, %d)", f, w0, w1, w2);
    const double lx = (x1 + w1 / 2) / f, ly = y / f;
    CHECK(wl_titlebar_tabs_handle_motion(&test_window, lx, ly), "scale %.2f: motion hits tab1", f);
    fake_now += ms_to_monotonic_t(300ll); render();
    const uint32_t hover_full = mixc(C1, 0xffffff, 0.08);
    CHECK(cdist(px(x1 + w1 / 2, y), hover_full) <= 2, "scale %.2f: hover colour, got %06x", f, px(x1 + w1 / 2, y));
    wl_titlebar_tabs_handle_leave(&test_window);
    fake_now += ms_to_monotonic_t(300ll); render();
    wl_titlebar_tabs_handle_button(&test_window, 0x110, 1, (x0 + w0 / 2) / f, ly);
    wl_titlebar_tabs_handle_motion(&test_window, (x2 + w2 / 2) / f, ly);
    wl_titlebar_tabs_handle_button(&test_window, 0x110, 0, (x2 + w2 / 2) / f, ly);
    CHECK(last_action == GLFW_TITLEBAR_TAB_DROP && last_action_tab == 1 && last_action_index == 2,
          "scale %.2f: drop idx, got action %d tab %llu idx %d", f, last_action, last_action_tab, last_action_index);
    fake_now += ms_to_monotonic_t(300ll); render();
}

int main(void) {
    _glfw.initialized = true;
    _glfw.callbacks.titlebar_tab_action = action_cb;
    _glfw.callbacks.titlebar_tab_text = text_cb;
    test_window.id = 7;
    _glfw.focusedWindowId = test_window.id;  // forced dark + focused bar = 0x393A39
    test_window.wl.width = BAR_W; test_window.wl.height = 600;
    test_window.wl.transparent = true;  // skip the opaque region wayland calls
    test_window.wl.wm_capabilities.maximize = true;
    test_window.wl.wm_capabilities.minimize = true;
    test_window.wl.decorations.for_window_state.fscale = 1.0;
    test_window.wl.decorations.titlebar.buffer.width = BAR_W;
    test_window.wl.decorations.titlebar.buffer.height = BAR_H;

    // layout with fscale 1: tabs at x = 8, 212, 416 (w=200, spacing 4), centres 108/312/516, y centre 14
    set_tabs3(1, 2, 3);
    render();
    CHECK(cdist(px(108, 14), C0) <= 2, "tab0 colour, got %06x", px(108, 14));
    CHECK(cdist(px(312, 14), C1) <= 2, "tab1 colour, got %06x", px(312, 14));
    CHECK(cdist(px(516, 14), C2) <= 2, "tab2 colour, got %06x", px(516, 14));

    // --- hover fade-in on tab1 ---
    wl_titlebar_tabs_handle_motion(&test_window, 312, 14);
    CHECK(timer_enabled == 1, "hover should start the animation timer");
    const uint32_t hover_full = mixc(C1, 0xffffff, 0.08);
    fake_now += ms_to_monotonic_t(90ll); render();
    const uint32_t mid = px(312, 14);
    CHECK(cdist(mid, C1) > 2 && cdist(mid, hover_full) > 2, "hover at 90ms should be mid-fade, got %06x (base %06x full %06x)", mid, C1, hover_full);
    fake_now += ms_to_monotonic_t(200ll); render();
    CHECK(cdist(px(312, 14), hover_full) <= 2, "hover settled, got %06x want %06x", px(312, 14), hover_full);
    // leave: fades back
    wl_titlebar_tabs_handle_leave(&test_window);
    fake_now += ms_to_monotonic_t(300ll); render();
    CHECK(cdist(px(312, 14), C1) <= 2, "hover faded back, got %06x", px(312, 14));

    // --- drag tab0 to the end: tabs reorder live once the ghost passes their
    // midpoints (same as macOS), then DROP commits the already-final order ---
    wl_titlebar_tabs_handle_button(&test_window, 0x110 /*BTN_LEFT*/, 1 /*pressed*/, 108, 14);
    wl_titlebar_tabs_handle_motion(&test_window, 500, 14);  // exceeds threshold, ghost_x = 400
    CHECK(last_action == GLFW_TITLEBAR_TAB_ACTIVATE && last_action_tab == 1, "drag start activates tab");
    render();  // starts the live reorder slide
    fake_now += ms_to_monotonic_t(10ll); render();
    CHECK(cdist(px(108, 14), C1) > 10, "slot0 should not yet be tab1's colour mid-slide, got %06x", px(108, 14));
    fake_now += ms_to_monotonic_t(300ll); render();
    CHECK(cdist(px(108, 14), C1) <= 2, "live reorder: slot0 shows tab1 while still dragging, got %06x", px(108, 14));
    CHECK(cdist(px(312, 14), C2) <= 2, "live reorder: slot1 shows tab2 while still dragging, got %06x", px(312, 14));
    CHECK(cdist(px(500, 14), C0) <= 2, "ghost follows the cursor, got %06x", px(500, 14));
    wl_titlebar_tabs_handle_button(&test_window, 0x110, 0 /*released*/, 500, 14);
    CHECK(last_action == GLFW_TITLEBAR_TAB_DROP && last_action_tab == 1 && last_action_index == 2,
          "drop at end, got action %d tab %llu idx %d", last_action, last_action_tab, last_action_index);
    set_tabs3(2, 3, 1);  // what kitty would send back
    render();  // the dragged tab slides from the ghost position into its slot
    fake_now += ms_to_monotonic_t(300ll); render();
    CHECK(cdist(px(108, 14), C1) <= 2, "slot0 settled to old tab1, got %06x", px(108, 14));
    CHECK(cdist(px(312, 14), C2) <= 2, "slot1 settled to old tab2, got %06x", px(312, 14));
    CHECK(cdist(px(516, 14), C0) <= 2, "slot2 settled to dragged tab0, got %06x", px(516, 14));

    // --- drag far below the bar and release: DETACH ---
    wl_titlebar_tabs_handle_button(&test_window, 0x110, 1, 108, 14);
    wl_titlebar_tabs_handle_motion(&test_window, 108, 120);  // 120 > bar 28 + margin 40
    wl_titlebar_tabs_handle_button(&test_window, 0x110, 0, 108, 120);
    CHECK(last_action == GLFW_TITLEBAR_TAB_DETACH && last_action_tab == 2,
          "detach below bar, got action %d tab %llu", last_action, last_action_tab);
    fake_now += ms_to_monotonic_t(300ll); render();

    // --- closing a tab fades it out and reaps it ---
    {
        const GLFWTitlebarTab tabs[2] = {
            {.tab_id = 2, .title = "one", .is_active = false, .fg = 0xcccccc, .bg = C1},
            {.tab_id = 3, .title = "two", .is_active = false, .fg = 0xcccccc, .bg = C2},
        };
        glfwWaylandSetTitlebarTabs((GLFWwindow*)&test_window, tabs, 2, BAR_BG, false, 2);
    }
    render();  // remaining tabs keep targets; tab id=1 dying at x=416..616
    fake_now += ms_to_monotonic_t(30ll); render();  // ease-out finishes fast: sample early
    const uint32_t fading = px(516, 14);
    CHECK(cdist(fading, C0) > 4 && cdist(fading, BAR_BG) > 4, "dying tab mid-fade, got %06x", fading);
    fake_now += ms_to_monotonic_t(300ll); render();
    CHECK(cdist(px(516, 14), BAR_BG) <= 2, "dying tab fully faded, got %06x", px(516, 14));
    CHECK(timer_cb != NULL, "timer registered");
    timer_cb(42, NULL);  // reaps the dead entry, then disables the timer
    timer_cb(42, NULL);
    CHECK(timer_enabled == 0, "timer stops when nothing animates");

    // fractional scale smoke suites
    run_scale_checks(1.25, 8);
    run_scale_checks(2.0, 9);

    if (failures) { printf("%d FAILURES\n", failures); return 1; }
    printf("all animation harness checks passed (%u renders)\n", render_count);
    return 0;
}
