/*
 * wl_titlebar_tabs.c
 * Fork-specific: native titlebar tabs for Wayland CSD, replicating the macOS
 * implementation in kitty/cocoa_window.m (see todo.md for the exact spec).
 *
 * Distributed under terms of the GPL3 license.
 */

#include "wl_titlebar_tabs.h"

#include "wl_client_side_decorations.h"
#include "backend_utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sys/mman.h>
#include <unistd.h>
// Needed for the BTN_* defines
#ifdef __has_include
#if __has_include(<linux/input.h>)
#include <linux/input.h>
#elif __has_include(<dev/evdev/input.h>)
#include <dev/evdev/input.h>
#endif
#else
#include <linux/input.h>
#endif

#define decs window->wl.decorations

// temporary diagnostics for the tear-off (DETACH) investigation
static bool
tabs_debug_enabled(void) {
    static int cached = -1;
    if (cached < 0) cached = getenv("KITTY_WL_TABS_DEBUG") ? 1 : 0;
    return cached == 1;
}
#define TABS_DEBUG(...) do { if (tabs_debug_enabled()) { fprintf(stderr, "[titlebar-tabs] " __VA_ARGS__); fprintf(stderr, "\n"); fflush(stderr); } } while (0)

// Logical (unscaled) metrics: the macOS values scaled by PT_PARITY.
// macOS lays text out at 72 dpi while Linux effectively uses ~96, so the
// same-looking terminal cell takes fewer logical px on macOS; with the macOS
// tab metrics used verbatim the bar reads too small next to equal-size text.
// Measured side by side (equal cell size, macos.png vs kwin): mac tab 48
// physical px vs ours 41 -> scale the bar metrics by 7/6.
#define PT_PARITY (7. / 6.)
#define TAB_MAX_WIDTH (200. * PT_PARITY)
#define TAB_MIN_WIDTH (60. * PT_PARITY)
#define TAB_HEIGHT (24. * PT_PARITY)
#define TAB_SPACING (4. * PT_PARITY)
#define TAB_CORNER_RADIUS (6. * PT_PARITY)
#define PLUS_BUTTON_WIDTH (28. * PT_PARITY)
#define TAB_BAR_LEFT_MARGIN (8. * PT_PARITY)
#define BAR_RIGHT_PADDING (8. * PT_PARITY)
#define CLOSE_RECT_SIZE (14. * PT_PARITY)
#define CLOSE_RECT_RIGHT_OFFSET (20. * PT_PARITY)  // close rect x = tab_w - 20
#define CLOSE_HIT_EXPAND (2. * PT_PARITY)
#define CLOSE_CROSS_INSET (4.25 * PT_PARITY)
#define STROKE_WIDTH (1.2 * PT_PARITY)
#define PLUS_ARM (5.0 * PT_PARITY)
#define TEXT_LEFT_PADDING (8. * PT_PARITY)
#define TEXT_RIGHT_MARGIN (22. * PT_PARITY)
#define TEXT_SIZE (12. * PT_PARITY)
// visible titlebar height (logical) when tabs are shown. macOS (macos.png,
// @2x) is 28: 1px light border overlaying the bar top + 1px bar + 24px tab
// + 2px bar; 28 * PT_PARITY rounds to 33.
#define TABS_TITLEBAR_HEIGHT 33
#define WINDOW_TOP_CORNER_RADIUS 10.
// macOS-style light inner window border, 1 logical px. Measured from
// macos.png: top edge white@~0.30 over the bar, sides/bottom white@~0.20.
#define BORDER_TOP_ALPHA 0.30
#define BORDER_SIDE_ALPHA 0.20
#define ATTENTION_COLOR 0xff9500u  // approximation of NSColor.systemOrange
// compact window buttons (KDE-like size, drawn by us since the upstream ones
// are bar-height sized which is too big for the 28px tabs bar). Style matches
// the reference screenshot: bare full-opacity glyphs, no hover circle, hover
// feedback is a slightly thicker stroke.
#define BUTTON_CELL_WIDTH 28.
#define BUTTON_CHEVRON_ARM 5.0
#define BUTTON_CHEVRON_RISE 0.4  // vertical half-extent = arm * this
#define BUTTON_CROSS_ARM 4.5
#define BUTTON_STROKE_WIDTH 1.3
#define BUTTON_HOVER_STROKE_MULT 1.45
#define DRAG_THRESHOLD 4.
#define DETACH_MARGIN 40.
// Chrome/Breeze-style drop shadow for tabs windows. Parameters fitted to a
// measured KDE Breeze shadow profile (gaussian sigma ~21 logical px, shifted
// ~10 px down, peak alpha ~0.79), scaled down slightly so the tile margin
// stays a reasonable interactive resize-border size.
#define SHADOW_MARGIN 32       // logical px, replaces the upstream 12
#define SHADOW_SIGMA 18.
#define SHADOW_OFFSET_Y 9.
#define SHADOW_ALPHA 0.78
#define SHADOW_EDGE_FADE 6.    // ramp to zero over the outermost px of the tile
// All animations run for 0.18s with an ease-out curve, same as macOS
#define ANIM_DURATION ms_to_monotonic_t(180ll)
#define ANIM_FRAME_INTERVAL ms_to_monotonic_t(16ll)

// Animation engine {{{
typedef struct Anim {
    double from, to;
    monotonic_t start;
    bool active;
} Anim;

static void schedule_anim_frames(void);

static double
ease_out(double t) { const double u = 1 - t; return 1 - u * u * u; }

static bool
anim_is_running(Anim *a) {
    if (!a->active) return false;
    if (monotonic() - a->start >= ANIM_DURATION) { a->active = false; return false; }
    return true;
}

static double
anim_current(Anim *a) {
    if (!anim_is_running(a)) return a->to;
    const double t = (double)(monotonic() - a->start) / (double)ANIM_DURATION;
    return a->from + (a->to - a->from) * ease_out(t);
}

static void
set_anim(Anim *a, double v) { a->from = v; a->to = v; a->active = false; }

static void
start_anim(Anim *a, double from, double to) {
    a->from = from; a->to = to; a->start = monotonic();
    a->active = from != to;
    if (a->active) schedule_anim_frames();
}
// }}}

typedef struct WaylandTabEntry {
    unsigned long long tab_id;
    char *title;
    bool is_active, needs_attention;
    uint32_t fg, bg;  // 0xRRGGBB, target colors
    uint32_t from_fg, from_bg;  // colors the color anim starts from
    Anim color;   // 0 -> 1 progress of from_* towards fg/bg
    Anim move_x, move_w;  // animated geometry, scaled px
    Anim fade;    // alpha 0..1
    Anim hover;   // hover background progress 0..1
    bool dying;   // removed from the model, fading out at frozen geometry
    bool have_layout;  // move_x/move_w hold valid values
    bool taken;   // transient marker used while diffing
    // cached text alpha mask (rendered once, blitted every animation frame)
    uint8_t *mask;
    int mask_w, mask_h, mask_left, mask_text_w;
    unsigned mask_sz_px;
    // geometry from the last render, in scaled pixels
    int x, y, w, h;
    bool hovered, close_hovered;
} WaylandTabEntry;

typedef struct WaylandTabBarState {
    uintptr_t window_id;
    WaylandTabEntry *tabs;
    size_t count, capacity;
    struct { int x, y, w, h; bool hovered; } plus;
    Anim plus_hover, plus_move;
    bool plus_have_layout;
    // press tracking for click semantics
    enum { PRESS_NONE, PRESS_TAB, PRESS_CLOSE, PRESS_PLUS } pressed_on;
    unsigned long long pressed_tab_id;
    uint32_t pressed_button;
    // in-bar tab dragging (reorder)
    bool dragging;
    unsigned long long drag_tab_id;
    int pressed_x, pressed_y;   // scaled px
    int drag_grab_dx;           // scaled px: pointer x - tab x at press
    int drag_grab_dy;           // scaled px: pointer y - tab y at press
    int ghost_x, drag_cur_y;    // scaled px
    int drag_index;             // live slot the dragged tab currently occupies
    int last_drag_x;            // scaled px, for drag direction
    bool drag_out;              // cursor beyond bar +- DETACH_MARGIN: tear-off zone
    // translucent tab that follows the cursor outside the bar during a
    // tear-off drag; a desync child of the *titlebar* surface (which commits
    // on every drag redraw, applying our set_position)
    struct {
        struct wl_surface *surface;
        struct wl_subsurface *subsurface;
        struct wp_viewport *viewport;
        struct wl_buffer *buffer;
        uint8_t *map;
        size_t map_size;
        int w, h;  // scaled px
    } ghost;
    // layout of the last render, scaled px
    int tab_w, spacing, tab_y, tab_h, tabs_area_right;
    int layout_bar_width;       // bar width the move anims were computed for
    double layout_fscale;
    // off-screen canvas the bar is composed in before one memcpy into the shm
    // buffer: the CSD buffer pair is reused without waiting for wl_buffer
    // release, so during high-frequency drag redraws the compositor can
    // sample a buffer mid-render; rendering in place flashed the cleared bar
    // (no tabs, no border) visibly while dragging tabs
    uint32_t *render_scratch;
    size_t render_scratch_sz;
    // 0 = follow system scheme, 1 = forced light, 2 = forced dark
    // (mirrors macos_titlebar_color light/dark for cross-platform parity)
    int forced_appearance;
    // the titlebar subsurface we last switched to desync mode (see render_bar)
    struct wl_subsurface *desynced_subsurface;
    // shadow patches behind the four rounded-off window corners. The CSD
    // shadow subsurfaces all sit outside the window rectangle, so the corner
    // pixels cut to transparency (GL corner mask at the bottom, CSD
    // round_top_corners at the top) would otherwise show a bare right-angled
    // notch with no shadow in it. These sit *below* the parent surface and
    // carry the shadow tile's interior values, visible only through the cut.
    struct {
        struct wl_surface *surface;
        struct wl_subsurface *subsurface;
        struct wp_viewport *viewport;
        struct wl_buffer *buffer[2];  // [0]=focused, [1]=unfocused
    } corner_patch[4];  // top-left, top-right, bottom-left, bottom-right
    uint8_t *corner_map;
    size_t corner_map_size;
    int corner_px;  // scaled patch size the buffers were built for
    struct WaylandTabBarState *next;
} WaylandTabBarState;

static WaylandTabBarState *all_states = NULL;
static void destroy_drag_ghost(WaylandTabBarState *s);

static WaylandTabBarState*
state_for_window(uintptr_t window_id, bool create) {
    for (WaylandTabBarState *s = all_states; s; s = s->next) if (s->window_id == window_id) return s;
    if (!create) return NULL;
    WaylandTabBarState *s = calloc(1, sizeof(WaylandTabBarState));
    if (!s) return NULL;
    s->window_id = window_id;
    s->next = all_states; all_states = s;
    return s;
}

static void
free_entry(WaylandTabEntry *t) {
    free(t->title); t->title = NULL;
    free(t->mask); t->mask = NULL;
}

static void
clear_tabs(WaylandTabBarState *s) {
    for (size_t i = 0; i < s->count; i++) free_entry(s->tabs + i);
    s->count = 0;
}

// Corner shadow patches {{{

static void
destroy_corner_patches(WaylandTabBarState *s) {
    for (int i = 0; i < 4; i++) {
        if (s->corner_patch[i].viewport) { wp_viewport_destroy(s->corner_patch[i].viewport); s->corner_patch[i].viewport = NULL; }
        if (s->corner_patch[i].subsurface) { wl_subsurface_destroy(s->corner_patch[i].subsurface); s->corner_patch[i].subsurface = NULL; }
        if (s->corner_patch[i].surface) { wl_surface_destroy(s->corner_patch[i].surface); s->corner_patch[i].surface = NULL; }
        for (int j = 0; j < 2; j++) {
            if (s->corner_patch[i].buffer[j]) { wl_buffer_destroy(s->corner_patch[i].buffer[j]); s->corner_patch[i].buffer[j] = NULL; }
        }
    }
    if (s->corner_map) { munmap(s->corner_map, s->corner_map_size); s->corner_map = NULL; s->corner_map_size = 0; }
    s->corner_px = 0;
}

// The buffers hold the shadow tile's values *inside* the rectangle the tile
// was blurred for: exactly the shadow a square window would cast at the spot
// the rounded corner no longer covers.
static bool
build_corner_patch_buffers(_GLFWwindow *window, WaylandTabBarState *s, int R) {
    for (int i = 0; i < 4; i++) for (int j = 0; j < 2; j++) {
        if (s->corner_patch[i].buffer[j]) { wl_buffer_destroy(s->corner_patch[i].buffer[j]); s->corner_patch[i].buffer[j] = NULL; }
    }
    if (s->corner_map) { munmap(s->corner_map, s->corner_map_size); s->corner_map = NULL; s->corner_map_size = 0; }
    s->corner_px = 0;
    const size_t one = (size_t)R * R * 4, total = one * 8;
    const int fd = createAnonymousFile(total);
    if (fd < 0) return false;
    s->corner_map = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (s->corner_map == MAP_FAILED) { close(fd); s->corner_map = NULL; return false; }
    s->corner_map_size = total;
    struct wl_shm_pool *pool = wl_shm_create_pool(_glfw.wl.shm, fd, total);
    close(fd);
    if (!pool) { munmap(s->corner_map, total); s->corner_map = NULL; s->corner_map_size = 0; return false; }
    const uint32_t *tile = decs.shadow_tile.data;
    const size_t S = decs.shadow_tile.stride, m = decs.shadow_tile.for_decoration_size;
    size_t off = 0;
    for (int corner = 0; corner < 4; corner++) {
        for (int state = 0; state < 2; state++) {
            uint32_t *dst = (uint32_t*)(s->corner_map + off);
            for (int y = 0; y < R; y++) {
                const size_t sy = (corner & 2) ? S - m - R + y : m + y;
                for (int x = 0; x < R; x++) {
                    const size_t sx = (corner & 1) ? S - m - R + x : m + x;
                    uint32_t a = (tile[sy * S + sx] >> 24) & 0xff;
                    if (state) a /= 2;  // unfocused shadows are lighter, same as render_shadows()
                    dst[y * R + x] = a << 24;  // premultiplied black
                }
            }
            s->corner_patch[corner].buffer[state] = wl_shm_pool_create_buffer(pool, off, R, R, R * 4, WL_SHM_FORMAT_ARGB8888);
            off += one;
        }
    }
    wl_shm_pool_destroy(pool);
    s->corner_px = R;
    return true;
}

static void restrict_shadow_input_regions(_GLFWwindow *window);

void
wl_titlebar_tabs_update_corner_patches(_GLFWwindow *window) {
    WaylandTabBarState *s = state_for_window(window->id, false);
    if (!s) return;
    const bool wanted = s->count > 0 && decs.titlebar.surface && decs.shadow_tile.data
        && !(window->wl.current.toplevel_states & TOPLEVEL_STATE_DOCKED);
    if (!wanted) { destroy_corner_patches(s); return; }
    restrict_shadow_input_regions(window);
    const double fscale = decs.for_window_state.fscale;
    const int R = (int)round(WINDOW_TOP_CORNER_RADIUS * fscale);
    const size_t S = decs.shadow_tile.stride, m = decs.shadow_tile.for_decoration_size;
    if (R <= 0 || (size_t)R + 2 * m > S) { destroy_corner_patches(s); return; }
    if (R != s->corner_px && !build_corner_patch_buffers(window, s, R)) return;
    const bool focused = window->id == _glfw.focusedWindowId;
    const int lr = (int)WINDOW_TOP_CORNER_RADIUS;
    const int px[4] = {0, window->wl.width - lr, 0, window->wl.width - lr};
    const int top_y = -(int)decs.metrics.visible_titlebar_height;
    const int py[4] = {top_y, top_y, window->wl.height - lr, window->wl.height - lr};
    for (int i = 0; i < 4; i++) {
        if (!s->corner_patch[i].surface) {
            struct wl_surface *surf = wl_compositor_create_surface(_glfw.wl.compositor);
            if (!surf) continue;
            wl_surface_set_user_data(surf, window);
            struct wl_subsurface *sub = wl_subcompositor_get_subsurface(_glfw.wl.subcompositor, surf, window->wl.surface);
            if (!sub) { wl_surface_destroy(surf); continue; }
            wl_subsurface_place_below(sub, window->wl.surface);
            wl_subsurface_set_desync(sub);
            // never steal pointer input from the surfaces above
            struct wl_region *empty = wl_compositor_create_region(_glfw.wl.compositor);
            if (empty) { wl_surface_set_input_region(surf, empty); wl_region_destroy(empty); }
            if (_glfw.wl.wp_viewporter) s->corner_patch[i].viewport = wp_viewporter_get_viewport(_glfw.wl.wp_viewporter, surf);
            s->corner_patch[i].surface = surf;
            s->corner_patch[i].subsurface = sub;
        }
        wl_surface_set_buffer_scale(s->corner_patch[i].surface, 1);
        wl_subsurface_set_position(s->corner_patch[i].subsurface, px[i], py[i]);
        wl_surface_attach(s->corner_patch[i].surface, s->corner_patch[i].buffer[focused ? 0 : 1], 0, 0);
        if (s->corner_patch[i].viewport) wp_viewport_set_destination(s->corner_patch[i].viewport, lr, lr);
        wl_surface_damage(s->corner_patch[i].surface, 0, 0, R, R);
        wl_surface_commit(s->corner_patch[i].surface);
    }
}

void
wl_titlebar_tabs_destroy_corner_patches(_GLFWwindow *window) {
    WaylandTabBarState *s = state_for_window(window->id, false);
    if (s) {
        destroy_corner_patches(s);
        destroy_drag_ghost(s);  // its parent (the titlebar surface) is going away
    }
}

// The shadow surfaces double as resize handles: with the SHADOW_MARGIN-wide
// soft shadow the grabbable border would be 32px, so clicks well outside the
// window would start resizes instead of reaching the window behind (native
// KDE shadows are not interactive). Restrict input to the innermost 12
// logical px, the upstream border width.
static void
restrict_shadow_input_regions(_GLFWwindow *window) {
    const int b = 12, w = SHADOW_MARGIN;
    const int side_h = window->wl.height + (int)decs.metrics.visible_titlebar_height;
#define SET(which, rx, ry, rw, rh) if (decs.which.surface) { \
        struct wl_region *reg = wl_compositor_create_region(_glfw.wl.compositor); \
        if (reg) { wl_region_add(reg, rx, ry, rw, rh); wl_surface_set_input_region(decs.which.surface, reg); wl_region_destroy(reg); wl_surface_commit(decs.which.surface); } }
    SET(shadow_left, w - b, 0, b, side_h);
    SET(shadow_right, 0, 0, b, side_h);
    SET(shadow_top, 0, w - b, window->wl.width, b);
    SET(shadow_bottom, 0, 0, window->wl.width, b);
    SET(shadow_upper_left, w - b, w - b, b, b);
    SET(shadow_upper_right, 0, w - b, b, b);
    SET(shadow_lower_left, w - b, 0, b, b);
    SET(shadow_lower_right, 0, 0, b, b);
#undef SET
}
// }}}

void
wl_titlebar_tabs_free(_GLFWwindow *window) {
    WaylandTabBarState **p = &all_states;
    while (*p) {
        if ((*p)->window_id == window->id) {
            WaylandTabBarState *s = *p;
            *p = s->next;
            clear_tabs(s);
            destroy_corner_patches(s);
            destroy_drag_ghost(s);
            free(s->tabs);
            free(s->render_scratch);
            free(s);
            return;
        }
        p = &(*p)->next;
    }
}

bool
wl_titlebar_tabs_active(_GLFWwindow *window) {
    WaylandTabBarState *s = state_for_window(window->id, false);
    return s && s->count > 0;
}

// True while a titlebar tab drag holds the pointer. Crossing between the
// client's own surfaces makes pointerHandleLeave zero
// _glfw.wl.pointer_button_count even though the button is still held and the
// press serial is still a valid implicit grab; _glfwPlatformStartDrag uses
// this to relax its early EPERM check in that situation.
bool
wl_titlebar_tabs_any_drag_active(void) {
    for (WaylandTabBarState *s = all_states; s; s = s->next)
        if (s->dragging && s->pressed_on == PRESS_TAB) return true;
    return false;
}

// Drawing helpers {{{

// All drawing is done directly on opaque ARGB8888 buffers with analytic
// coverage computed from 4x4 subsamples per pixel.
#define SUBSAMPLES 4

static uint32_t
blend_argb(uint32_t below, uint32_t above, double alpha) {
    if (alpha <= 0) return below;
    if (alpha > 1) alpha = 1;
    const double ca = 1 - alpha;
    uint32_t r = (uint32_t)(((below >> 16) & 0xff) * ca + ((above >> 16) & 0xff) * alpha + 0.5);
    uint32_t g = (uint32_t)(((below >> 8) & 0xff) * ca + ((above >> 8) & 0xff) * alpha + 0.5);
    uint32_t b = (uint32_t)((below & 0xff) * ca + (above & 0xff) * alpha + 0.5);
    return 0xff000000u | (r << 16) | (g << 8) | b;
}

static double
luminance(uint32_t rgb) {
    return (0.2126 * ((rgb >> 16) & 0xff) + 0.7152 * ((rgb >> 8) & 0xff) + 0.0722 * (rgb & 0xff)) / 255.;
}

static uint32_t
mix_rgb(uint32_t a, uint32_t b, double t) {
    uint32_t r = (uint32_t)(((a >> 16) & 0xff) * (1 - t) + ((b >> 16) & 0xff) * t + 0.5);
    uint32_t g = (uint32_t)(((a >> 8) & 0xff) * (1 - t) + ((b >> 8) & 0xff) * t + 0.5);
    uint32_t bl = (uint32_t)((a & 0xff) * (1 - t) + (b & 0xff) * t + 0.5);
    return (r << 16) | (g << 8) | bl;
}

// Signed distance style test: is point inside the rounded rect?
static bool
point_in_rounded_rect(double px, double py, double x, double y, double w, double h, double r) {
    if (px < x || px >= x + w || py < y || py >= y + h) return false;
    double cx = -1, cy = -1;
    if (px < x + r && py < y + r) { cx = x + r; cy = y + r; }
    else if (px >= x + w - r && py < y + r) { cx = x + w - r; cy = y + r; }
    else if (px < x + r && py >= y + h - r) { cx = x + r; cy = y + h - r; }
    else if (px >= x + w - r && py >= y + h - r) { cx = x + w - r; cy = y + h - r; }
    if (cx < 0) return true;
    const double dx = px - cx, dy = py - cy;
    return dx * dx + dy * dy <= r * r;
}

static double
rounded_rect_coverage(int px, int py, double x, double y, double w, double h, double r) {
    // fast path: the pixel square lies fully inside and away from the corners
    if (px >= x && px + 1 <= x + w && py >= y && py + 1 <= y + h) {
        if ((px >= x + r && px + 1 <= x + w - r) || (py >= y + r && py + 1 <= y + h - r)) return 1;
    }
    unsigned hit = 0;
    for (int sy = 0; sy < SUBSAMPLES; sy++) {
        const double fy = py + (sy + 0.5) / SUBSAMPLES;
        for (int sx = 0; sx < SUBSAMPLES; sx++) {
            if (point_in_rounded_rect(px + (sx + 0.5) / SUBSAMPLES, fy, x, y, w, h, r)) hit++;
        }
    }
    return hit / (double)(SUBSAMPLES * SUBSAMPLES);
}

static double
dist_to_segment(double px, double py, double x1, double y1, double x2, double y2) {
    const double dx = x2 - x1, dy = y2 - y1;
    const double len2 = dx * dx + dy * dy;
    double t = len2 > 0 ? ((px - x1) * dx + (py - y1) * dy) / len2 : 0;
    if (t < 0) t = 0; else if (t > 1) t = 1;
    const double ex = px - (x1 + t * dx), ey = py - (y1 + t * dy);
    return sqrt(ex * ex + ey * ey);
}

static double
segment_coverage(int px, int py, double x1, double y1, double x2, double y2, double half_width) {
    unsigned hit = 0;
    for (int sy = 0; sy < SUBSAMPLES; sy++) {
        const double fy = py + (sy + 0.5) / SUBSAMPLES;
        for (int sx = 0; sx < SUBSAMPLES; sx++) {
            if (dist_to_segment(px + (sx + 0.5) / SUBSAMPLES, fy, x1, y1, x2, y2) <= half_width) hit++;
        }
    }
    return hit / (double)(SUBSAMPLES * SUBSAMPLES);
}

static double
circle_coverage(int px, int py, double cx, double cy, double r) {
    unsigned hit = 0;
    for (int sy = 0; sy < SUBSAMPLES; sy++) {
        const double fy = py + (sy + 0.5) / SUBSAMPLES;
        for (int sx = 0; sx < SUBSAMPLES; sx++) {
            const double dx = px + (sx + 0.5) / SUBSAMPLES - cx, dy = fy - cy;
            if (dx * dx + dy * dy <= r * r) hit++;
        }
    }
    return hit / (double)(SUBSAMPLES * SUBSAMPLES);
}

typedef struct Canvas { uint32_t *px; int width, height; } Canvas;

static void
canvas_blend_segment(Canvas *c, double x1, double y1, double x2, double y2, double half_width, uint32_t color, double alpha) {
    const int left = (int)floor(fmin(x1, x2) - half_width - 1), right = (int)ceil(fmax(x1, x2) + half_width + 1);
    const int top = (int)floor(fmin(y1, y2) - half_width - 1), bottom = (int)ceil(fmax(y1, y2) + half_width + 1);
    for (int y = top < 0 ? 0 : top; y < bottom && y < c->height; y++) {
        uint32_t *row = c->px + (size_t)y * c->width;
        for (int x = left < 0 ? 0 : left; x < right && x < c->width; x++) {
            const double cov = segment_coverage(x, y, x1, y1, x2, y2, half_width);
            if (cov > 0) row[x] = blend_argb(row[x], 0xff000000u | color, cov * alpha);
        }
    }
}

static void
canvas_blend_circle(Canvas *c, double cx, double cy, double r, uint32_t color, double alpha) {
    const int left = (int)floor(cx - r - 1), right = (int)ceil(cx + r + 1);
    const int top = (int)floor(cy - r - 1), bottom = (int)ceil(cy + r + 1);
    for (int y = top < 0 ? 0 : top; y < bottom && y < c->height; y++) {
        uint32_t *row = c->px + (size_t)y * c->width;
        for (int x = left < 0 ? 0 : left; x < right && x < c->width; x++) {
            const double cov = circle_coverage(x, y, cx, cy, r);
            if (cov > 0) row[x] = blend_argb(row[x], 0xff000000u | color, cov * alpha);
        }
    }
}

static void
canvas_blend_rounded_rect(Canvas *c, double x, double y, double w, double h, double r, uint32_t color, double alpha) {
    const int left = (int)floor(x), right = (int)ceil(x + w);
    const int top = (int)floor(y), bottom = (int)ceil(y + h);
    for (int py = top < 0 ? 0 : top; py < bottom && py < c->height; py++) {
        uint32_t *row = c->px + (size_t)py * c->width;
        for (int px = left < 0 ? 0 : left; px < right && px < c->width; px++) {
            const double cov = rounded_rect_coverage(px, py, x, y, w, h, r);
            if (cov > 0) row[px] = blend_argb(row[px], 0xff000000u | color, cov * alpha);
        }
    }
}

// Composite src (same dimensions as the rounded rect) over dest through a
// rounded rect alpha mask positioned at (x, y) in dest, scaled by alpha.
static void
canvas_composite_rounded(Canvas *dest, const Canvas *src, int x, int y, double r, double alpha) {
    if (alpha <= 0) return;
    for (int sy = 0; sy < src->height; sy++) {
        const int dy = y + sy;
        if (dy < 0 || dy >= dest->height) continue;
        uint32_t *drow = dest->px + (size_t)dy * dest->width;
        const uint32_t *srow = src->px + (size_t)sy * src->width;
        for (int sx = 0; sx < src->width; sx++) {
            const int dx = x + sx;
            if (dx < 0 || dx >= dest->width) continue;
            const double cov = rounded_rect_coverage(sx, sy, 0, 0, src->width, src->height, r);
            if (cov > 0) drow[dx] = blend_argb(drow[dx], srow[sx], cov * alpha);
        }
    }
}
// }}}

// Text mask cache {{{
// Animation frames must not run FreeType; the title is rendered once as white
// on black at the maximum tab width, its green channel kept as an alpha mask,
// then every frame just blends fg through the mask.

// Renders text into a fresh alpha mask of mw x tab_h px. On success stores the
// mask and its tight horizontal bounds. Returns false when nothing was drawn.
static bool
build_text_mask(_GLFWwindow *window, const char *text, unsigned sz_px, int mw, int tab_h, uint8_t **mask_out, int *left_out, int *text_w_out) {
    *mask_out = NULL; *left_out = 0; *text_w_out = 0;
    if (!text || !text[0] || !_glfw.callbacks.titlebar_tab_text || mw <= 0 || tab_h <= 0) return false;
    uint32_t *scratch = malloc((size_t)mw * tab_h * sizeof(uint32_t));
    if (!scratch) return false;
    for (size_t i = 0; i < (size_t)mw * tab_h; i++) scratch[i] = 0xff000000u;
    if (!_glfw.callbacks.titlebar_tab_text(
            (GLFWwindow*)window, text, sz_px, 0xffffffffu, 0xff000000u,
            (uint8_t*)scratch, mw, tab_h, 0, 0, 0)) { free(scratch); return false; }
    uint8_t *mask = malloc((size_t)mw * tab_h);
    if (!mask) { free(scratch); return false; }
    int left = mw, right = -1;
    for (int y = 0; y < tab_h; y++) {
        for (int x = 0; x < mw; x++) {
            const uint8_t a = (scratch[(size_t)y * mw + x] >> 8) & 0xff;
            mask[(size_t)y * mw + x] = a;
            if (a) { if (x < left) left = x; if (x > right) right = x; }
        }
    }
    free(scratch);
    if (right < left) { free(mask); return false; }  // empty rendering
    *mask_out = mask; *left_out = left; *text_w_out = right - left + 1;
    return true;
}

static void
ensure_text_mask(_GLFWwindow *window, WaylandTabEntry *t, int tab_h, double fscale) {
    const unsigned sz_px = (unsigned)round(TEXT_SIZE * fscale);
    if (t->mask && t->mask_h == tab_h && t->mask_sz_px == sz_px) return;
    free(t->mask); t->mask = NULL; t->mask_text_w = 0; t->mask_left = 0;
    t->mask_h = tab_h; t->mask_sz_px = sz_px;
    const int mw = (int)round((TAB_MAX_WIDTH - TEXT_LEFT_PADDING - TEXT_RIGHT_MARGIN) * fscale);
    if (build_text_mask(window, t->title, sz_px, mw, tab_h, &t->mask, &t->mask_left, &t->mask_text_w)) t->mask_w = mw;
}

// Single cached ellipsis glyph mask, used for macOS-style tail truncation.
static struct {
    uint8_t *mask;
    int w, h, left, text_w;
    unsigned sz_px;
} ellipsis_cache;

static bool
ensure_ellipsis_mask(_GLFWwindow *window, int tab_h, unsigned sz_px) {
    if (ellipsis_cache.mask && ellipsis_cache.h == tab_h && ellipsis_cache.sz_px == sz_px) return true;
    free(ellipsis_cache.mask); memset(&ellipsis_cache, 0, sizeof(ellipsis_cache));
    ellipsis_cache.h = tab_h; ellipsis_cache.sz_px = sz_px;
    const int mw = 4 * (int)sz_px;
    if (build_text_mask(window, "…", sz_px, mw, tab_h, &ellipsis_cache.mask, &ellipsis_cache.left, &ellipsis_cache.text_w))
        ellipsis_cache.w = mw;
    return ellipsis_cache.mask != NULL;
}

static void
blend_mask_columns(Canvas *tab, const uint8_t *mask, int mask_stride, int mask_left, int cols, int dst_left, int clip_right, uint32_t fg_argb, int h) {
    for (int y = 0; y < h; y++) {
        uint32_t *row = tab->px + (size_t)y * tab->width;
        const uint8_t *mrow = mask + (size_t)y * mask_stride + mask_left;
        for (int i = 0; i < cols; i++) {
            const int x = dst_left + i;
            if (x >= clip_right || x >= tab->width) break;
            const uint8_t a = mrow[i];
            if (a) row[x] = blend_argb(row[x], fg_argb, a / 255.);
        }
    }
}

static void
draw_cached_text(_GLFWwindow *window, Canvas *tab, WaylandTabEntry *t, uint32_t fg, double fscale) {
    if (!t->mask || !t->mask_text_w) return;
    const int box_x = (int)round(TEXT_LEFT_PADDING * fscale);
    const int box_w = tab->width - box_x - (int)round(TEXT_RIGHT_MARGIN * fscale);
    if (box_w <= 0) return;
    const int h = tab->height < t->mask_h ? tab->height : t->mask_h;
    const uint32_t fg_argb = 0xff000000u | fg;
    if (t->mask_text_w <= box_w) {  // fits: centered
        const int dst_left = box_x + (box_w - t->mask_text_w) / 2;
        blend_mask_columns(tab, t->mask, t->mask_w, t->mask_left, t->mask_text_w, dst_left, box_x + box_w, fg_argb, h);
        return;
    }
    // overflow: macOS-style tail truncation with an ellipsis
    int avail = box_w;
    const bool have_ell = ensure_ellipsis_mask(window, t->mask_h, t->mask_sz_px);
    if (have_ell && ellipsis_cache.text_w < avail) avail -= ellipsis_cache.text_w;
    blend_mask_columns(tab, t->mask, t->mask_w, t->mask_left, avail, box_x, box_x + avail, fg_argb, h);
    if (have_ell && ellipsis_cache.text_w <= box_w - avail)
        blend_mask_columns(tab, ellipsis_cache.mask, ellipsis_cache.w, ellipsis_cache.left, ellipsis_cache.text_w,
                box_x + avail, box_x + box_w, fg_argb, h);
}
// }}}

static void
render_tab_canvas(_GLFWwindow *window, WaylandTabEntry *t, Canvas *tab, double fscale) {
    uint32_t bg = t->bg, fg = t->fg;
    if (anim_is_running(&t->color)) {
        const double ct = anim_current(&t->color);
        bg = mix_rgb(t->from_bg, t->bg, ct);
        fg = mix_rgb(t->from_fg, t->fg, ct);
    }
    const double hp = anim_current(&t->hover);
    if (hp > 0 && !t->is_active) {
        // mix bg towards white (dark bg) or black (light bg), up to 0.08
        bg = mix_rgb(bg, luminance(bg) < 0.5 ? 0xffffff : 0x000000, 0.08 * hp);
    }
    if (t->needs_attention && !t->is_active) fg = ATTENTION_COLOR;

    const uint32_t bg_argb = 0xff000000u | bg;
    for (size_t i = 0; i < (size_t)tab->width * tab->height; i++) tab->px[i] = bg_argb;

    // title text, centered, in the box (8, *, w - 8 - 22, full height)
    ensure_text_mask(window, t, tab->height, fscale);
    draw_cached_text(window, tab, t, fg, fscale);

    // close button: rect is (w-20, (h-14)/2, 14, 14) in logical units
    const double close_x = tab->width - CLOSE_RECT_RIGHT_OFFSET * fscale, close_size = CLOSE_RECT_SIZE * fscale;
    const double close_y = (tab->height - close_size) / 2;
    if (t->close_hovered) {
        canvas_blend_circle(tab, close_x + close_size / 2, close_y + close_size / 2, close_size / 2, fg, 0.25);
    }
    const double inset = CLOSE_CROSS_INSET * fscale, half_w = STROKE_WIDTH * fscale / 2;
    const double calpha = t->close_hovered ? 1.0 : 0.6;
    canvas_blend_segment(tab, close_x + inset, close_y + inset, close_x + close_size - inset, close_y + close_size - inset, half_w, fg, calpha);
    canvas_blend_segment(tab, close_x + inset, close_y + close_size - inset, close_x + close_size - inset, close_y + inset, half_w, fg, calpha);
}

static void
render_one_tab(_GLFWwindow *window, WaylandTabEntry *t, Canvas *bar, double fscale, int draw_x) {
    const double alpha = anim_current(&t->fade);
    if (alpha <= 0 || t->w <= 0 || t->h <= 0) return;
    Canvas tab = {.width = t->w, .height = t->h};
    tab.px = malloc((size_t)tab.width * tab.height * sizeof(uint32_t));
    if (!tab.px) return;
    render_tab_canvas(window, t, &tab, fscale);
    canvas_composite_rounded(bar, &tab, draw_x, t->y, TAB_CORNER_RADIUS * fscale, alpha);
    free(tab.px);
}

// Tear-off drag ghost {{{

#define GHOST_ALPHA 0.85

static void
destroy_drag_ghost(WaylandTabBarState *s) {
    if (s->ghost.viewport) { wp_viewport_destroy(s->ghost.viewport); s->ghost.viewport = NULL; }
    if (s->ghost.subsurface) { wl_subsurface_destroy(s->ghost.subsurface); s->ghost.subsurface = NULL; }
    if (s->ghost.surface) { wl_surface_destroy(s->ghost.surface); s->ghost.surface = NULL; }
    if (s->ghost.buffer) { wl_buffer_destroy(s->ghost.buffer); s->ghost.buffer = NULL; }
    if (s->ghost.map) { munmap(s->ghost.map, s->ghost.map_size); s->ghost.map = NULL; s->ghost.map_size = 0; }
    s->ghost.w = s->ghost.h = 0;
}

// Translucent snapshot of the dragged tab, following the cursor outside the
// bar so a tear-off shows where the tab is going (mirrors the macOS ghost).
static bool
show_drag_ghost(_GLFWwindow *window, WaylandTabBarState *s, WaylandTabEntry *t, double fscale) {
    const int w = s->tab_w, h = s->tab_h;
    if (w <= 0 || h <= 0 || !decs.titlebar.surface) return false;
    if (s->ghost.surface && (s->ghost.w != w || s->ghost.h != h)) destroy_drag_ghost(s);
    if (s->ghost.surface) return true;
    const size_t total = (size_t)w * h * 4;
    const int fd = createAnonymousFile(total);
    if (fd < 0) return false;
    s->ghost.map = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (s->ghost.map == MAP_FAILED) { close(fd); s->ghost.map = NULL; return false; }
    s->ghost.map_size = total;
    struct wl_shm_pool *pool = wl_shm_create_pool(_glfw.wl.shm, fd, total);
    close(fd);
    if (!pool) { destroy_drag_ghost(s); return false; }
    s->ghost.buffer = wl_shm_pool_create_buffer(pool, 0, w, h, w * 4, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    if (!s->ghost.buffer) { destroy_drag_ghost(s); return false; }

    Canvas tab = {.px = malloc(total), .width = w, .height = h};
    if (!tab.px) { destroy_drag_ghost(s); return false; }
    render_tab_canvas(window, t, &tab, fscale);
    const double r = TAB_CORNER_RADIUS * fscale;
    uint32_t *dst = (uint32_t*)s->ghost.map;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            const double a = rounded_rect_coverage(x, y, 0, 0, w, h, r) * GHOST_ALPHA;
            const uint32_t c = tab.px[(size_t)y * w + x];
            dst[(size_t)y * w + x] =  // premultiplied
                ((uint32_t)(a * 255 + 0.5) << 24)
                | ((uint32_t)(((c >> 16) & 0xff) * a + 0.5) << 16)
                | ((uint32_t)(((c >> 8) & 0xff) * a + 0.5) << 8)
                | (uint32_t)((c & 0xff) * a + 0.5);
        }
    }
    free(tab.px);

    struct wl_surface *surf = wl_compositor_create_surface(_glfw.wl.compositor);
    if (!surf) { destroy_drag_ghost(s); return false; }
    wl_surface_set_user_data(surf, window);
    struct wl_subsurface *sub = wl_subcompositor_get_subsurface(_glfw.wl.subcompositor, surf, decs.titlebar.surface);
    if (!sub) { wl_surface_destroy(surf); destroy_drag_ghost(s); return false; }
    s->ghost.surface = surf; s->ghost.subsurface = sub;
    wl_subsurface_set_desync(sub);
    // never steal pointer focus: the ghost rides under the cursor
    struct wl_region *empty = wl_compositor_create_region(_glfw.wl.compositor);
    if (empty) { wl_surface_set_input_region(surf, empty); wl_region_destroy(empty); }
    if (_glfw.wl.wp_viewporter) {
        s->ghost.viewport = wp_viewporter_get_viewport(_glfw.wl.wp_viewporter, surf);
        if (s->ghost.viewport) wp_viewport_set_destination(s->ghost.viewport, (int)round(w / fscale), (int)round(h / fscale));
    }
    wl_surface_set_buffer_scale(surf, 1);
    wl_surface_attach(surf, s->ghost.buffer, 0, 0);
    wl_surface_damage(surf, 0, 0, w, h);
    wl_surface_commit(surf);
    s->ghost.w = w; s->ghost.h = h;
    return true;
}
// }}}

static void
render_plus_button(WaylandTabBarState *s, Canvas *bar, uint32_t bar_bg, double fscale) {
    const bool is_dark = luminance(bar_bg & 0xffffff) < 0.5;
    const double hp = anim_current(&s->plus_hover);
    if (hp > 0) {
        canvas_blend_rounded_rect(bar, s->plus.x, s->plus.y, s->plus.w, s->plus.h,
                TAB_CORNER_RADIUS * fscale, is_dark ? 0xffffff : 0x000000, (is_dark ? 0.12 : 0.08) * hp);
    }
    // secondaryLabelColor approximation: fg at 55% over the bar background
    const uint32_t stroke = is_dark ? 0xffffff : 0x000000;
    const double cx = s->plus.x + s->plus.w / 2., cy = s->plus.y + s->plus.h / 2.;
    const double arm = PLUS_ARM * fscale, half_w = STROKE_WIDTH * fscale / 2;
    canvas_blend_segment(bar, cx - arm, cy, cx + arm, cy, half_w, stroke, 0.55);
    canvas_blend_segment(bar, cx, cy - arm, cx, cy + arm, half_w, stroke, 0.55);
}

// Cut the two top corners of the bar with premultiplied transparency so the
// window gets macOS-style rounded top corners (shm ARGB8888 is premultiplied).
static void
round_top_corners(Canvas *bar, double r) {
    const int ir = (int)ceil(r);
    for (int y = 0; y < ir && y < bar->height; y++) {
        uint32_t *row = bar->px + (size_t)y * bar->width;
        for (int i = 0; i < ir; i++) {
            const int xs[2] = {i, bar->width - 1 - i};
            for (int k = 0; k < 2; k++) {
                const int x = xs[k];
                if (x < 0 || x >= bar->width) continue;
                const double cx = k == 0 ? r : bar->width - r, cy = r;
                const double cov = circle_coverage(x, y, cx, cy, r);
                if (cov >= 1) continue;
                uint32_t p = row[x];
                const uint32_t a = (uint32_t)(((p >> 24) & 0xff) * cov + 0.5);
                const uint32_t rr = (uint32_t)(((p >> 16) & 0xff) * cov + 0.5);
                const uint32_t g = (uint32_t)(((p >> 8) & 0xff) * cov + 0.5);
                const uint32_t b = (uint32_t)((p & 0xff) * cov + 0.5);
                row[x] = (a << 24) | (rr << 16) | (g << 8) | b;
            }
        }
    }
}

// macOS-style 1 logical px light inner border on the titlebar part of the
// window: straight top edge, the two top corner arcs, and the side columns.
// Drawn before round_top_corners(); the arc stroke lies inside the boundary
// the cut trims along, so the two compose cleanly.
static void
draw_titlebar_border(Canvas *bar, double r, double fscale) {
    const double bw = fmax(1., round(fscale));
    const int ir = (int)ceil(r), ibw = (int)ceil(bw);
    // straight top edge between the arcs
    for (int y = 0; y < ibw && y < bar->height; y++) {
        uint32_t *row = bar->px + (size_t)y * bar->width;
        const double cov = fmin(1., bw - y);
        for (int x = ir; x < bar->width - ir; x++)
            row[x] = blend_argb(row[x], 0xffffffffu, cov * BORDER_TOP_ALPHA);
    }
    // side columns below the arcs
    for (int y = ir; y < bar->height; y++) {
        uint32_t *row = bar->px + (size_t)y * bar->width;
        for (int i = 0; i < ibw; i++) {
            const double cov = fmin(1., bw - i);
            const int xs[2] = {i, bar->width - 1 - i};
            for (int k = 0; k < 2; k++) {
                if (xs[k] < 0 || xs[k] >= bar->width) continue;
                row[xs[k]] = blend_argb(row[xs[k]], 0xffffffffu, cov * BORDER_SIDE_ALPHA);
            }
        }
    }
    // top corner arcs: stroke band [r - bw, r], alpha fading from the top
    // value at the horizontal end to the side value at the vertical end
    for (int y = 0; y < ir && y < bar->height; y++) {
        uint32_t *row = bar->px + (size_t)y * bar->width;
        const double topness = r > 0 ? fmax(0., (r - (y + 0.5)) / r) : 0;
        const double alpha = BORDER_SIDE_ALPHA + (BORDER_TOP_ALPHA - BORDER_SIDE_ALPHA) * topness;
        for (int i = 0; i < ir; i++) {
            const int xs[2] = {i, bar->width - 1 - i};
            for (int k = 0; k < 2; k++) {
                const int x = xs[k];
                if (x < 0 || x >= bar->width) continue;
                const double cx = k == 0 ? r : bar->width - r;
                const double cov = circle_coverage(x, y, cx, r, r) - circle_coverage(x, y, cx, r, r - bw);
                if (cov > 0) row[x] = blend_argb(row[x], 0xffffffffu, cov * alpha);
            }
        }
    }
}

static void
render_compact_window_buttons(_GLFWwindow *window, WaylandTabBarState *s, Canvas *bar, uint32_t fg, double fscale) {
    const int cell_w = (int)round(BUTTON_CELL_WIDTH * fscale);
    const double cy = bar->height / 2.;
    int right = bar->width - (int)round(BAR_RIGHT_PADDING * fscale / 2);
    const uint32_t fg_rgb = fg & 0xffffff;
#define stroke_for(which) (BUTTON_STROKE_WIDTH * fscale * (decs.which.hovered ? BUTTON_HOVER_STROKE_MULT : 1.) / 2)

    // order right-to-left: close, maximize, minimize (matches upstream order left-to-right)
    // close: plain cross, hover just thickens the stroke
    {
        const int left = right - cell_w;
        const double cx = left + cell_w / 2.;
        decs.close.left = left; decs.close.width = cell_w;
        const double a = BUTTON_CROSS_ARM * fscale, half_w = stroke_for(close);
        canvas_blend_segment(bar, cx - a, cy - a, cx + a, cy + a, half_w, fg_rgb, 1.0);
        canvas_blend_segment(bar, cx - a, cy + a, cx + a, cy - a, half_w, fg_rgb, 1.0);
        right = left;
    }
    // maximize: wide flat chevron up (down when maximized)
    if (window->wl.wm_capabilities.maximize) {
        const int left = right - cell_w;
        const double cx = left + cell_w / 2.;
        decs.maximize.left = left; decs.maximize.width = cell_w;
        const double a = BUTTON_CHEVRON_ARM * fscale, r = a * BUTTON_CHEVRON_RISE, half_w = stroke_for(maximize);
        if (window->wl.current.toplevel_states & TOPLEVEL_STATE_MAXIMIZED) {
            canvas_blend_segment(bar, cx - a, cy - r, cx, cy + r, half_w, fg_rgb, 1.0);
            canvas_blend_segment(bar, cx, cy + r, cx + a, cy - r, half_w, fg_rgb, 1.0);
        } else {
            canvas_blend_segment(bar, cx - a, cy + r, cx, cy - r, half_w, fg_rgb, 1.0);
            canvas_blend_segment(bar, cx, cy - r, cx + a, cy + r, half_w, fg_rgb, 1.0);
        }
        right = left;
    } else { decs.maximize.left = 0; decs.maximize.width = 0; }
    // minimize: wide flat chevron down
    if (window->wl.wm_capabilities.minimize) {
        const int left = right - cell_w;
        const double cx = left + cell_w / 2.;
        decs.minimize.left = left; decs.minimize.width = cell_w;
        const double a = BUTTON_CHEVRON_ARM * fscale, r = a * BUTTON_CHEVRON_RISE, half_w = stroke_for(minimize);
        canvas_blend_segment(bar, cx - a, cy - r, cx, cy + r, half_w, fg_rgb, 1.0);
        canvas_blend_segment(bar, cx, cy + r, cx + a, cy - r, half_w, fg_rgb, 1.0);
        right = left;
    } else { decs.minimize.left = 0; decs.minimize.width = 0; }
#undef stroke_for
    s->tabs_area_right = right - (int)round(TAB_SPACING * fscale);
}

// Replaces the freshly built shadow tile of a tabs window with a
// Chrome/Breeze-style drop shadow: a real gaussian blur of the window's
// rounded rectangle, shifted downwards, with parameters fitted to a measured
// KDE Breeze shadow. The upstream tile (a tight 12px 0.7-alpha box blur with
// square corners and no offset) reads as a dark outline next to native KDE
// windows. Hooked from create_shadow_tile(); the tile layout (stride,
// corner_size, repeating middle segments) is unchanged so the upstream
// slicing in render_shadows() and the corner patches keep working.
void
wl_titlebar_tabs_patch_shadow_tile(_GLFWwindow *window) {
    if (!wl_titlebar_tabs_active(window) || !decs.shadow_tile.data) return;
    double fscale = decs.for_window_state.fscale;
    if (fscale <= 0) fscale = 1.;
    const ssize_t S = (ssize_t)decs.shadow_tile.stride;
    const ssize_t m = (ssize_t)decs.shadow_tile.for_decoration_size;
    const double sigma = SHADOW_SIGMA * fscale;
    const ssize_t radius = (ssize_t)ceil(2.5 * sigma);
    float *bufs = malloc(sizeof(float) * (size_t)(2 * S * S + 2 * radius + 1));
    if (!bufs) return;
    float *mask = bufs, *scratch = bufs + S * S, *kernel = bufs + 2 * (size_t)S * S;
    double ksum = 0;
    for (ssize_t i = -radius; i <= radius; i++) {
        const double v = exp(-(double)(i * i) / (2 * sigma * sigma));
        kernel[i + radius] = (float)v; ksum += v;
    }
    for (ssize_t i = 0; i <= 2 * radius; i++) kernel[i] = (float)(kernel[i] / ksum);
    // base: the window's rounded rect, shifted down by the shadow offset
    const double r = WINDOW_TOP_CORNER_RADIUS * fscale;
    const double off = SHADOW_OFFSET_Y * fscale;
    const double rect_size = (double)(S - 2 * m);
    for (ssize_t y = 0; y < S; y++)
        for (ssize_t x = 0; x < S; x++)
            mask[y * S + x] = (float)rounded_rect_coverage((int)x, (int)y, (double)m, m + off, rect_size, rect_size, r);
    // separable gaussian: horizontal into scratch, vertical back into mask
    for (ssize_t y = 0; y < S; y++) {
        const float *src = mask + y * S;
        float *dst = scratch + y * S;
        for (ssize_t x = 0; x < S; x++) {
            double a = 0;
            const ssize_t k0 = x - radius < 0 ? radius - x : 0, k1 = x + radius >= S ? S - 1 - x + radius : 2 * radius;
            for (ssize_t k = k0; k <= k1; k++) a += src[x + k - radius] * kernel[k];
            dst[x] = (float)a;
        }
    }
    for (ssize_t x = 0; x < S; x++) {
        for (ssize_t y = 0; y < S; y++) {
            double a = 0;
            const ssize_t k0 = y - radius < 0 ? radius - y : 0, k1 = y + radius >= S ? S - 1 - y + radius : 2 * radius;
            for (ssize_t k = k0; k <= k1; k++) a += scratch[(y + k - radius) * S + x] * kernel[k];
            mask[y * S + x] = (float)a;
        }
    }
    // fade to zero at the tile boundary so the truncation is not a hard line
    const double fade = SHADOW_EDGE_FADE * fscale;
    for (ssize_t y = 0; y < S; y++) {
        for (ssize_t x = 0; x < S; x++) {
            ssize_t d = x; if (y < d) d = y; if (S - 1 - x < d) d = S - 1 - x; if (S - 1 - y < d) d = S - 1 - y;
            double f = fade > 0 ? d / fade : 1;
            if (f > 1) f = 1;
            const double a = mask[y * S + x] * SHADOW_ALPHA * f;
            decs.shadow_tile.data[y * S + x] = ((uint32_t)(a * 255 + 0.5)) << 24;
        }
    }
    free(bufs);
}

static int
tabs_left_margin(double fscale) {
    return (int)round(TAB_BAR_LEFT_MARGIN * fscale);
}

void
wl_titlebar_tabs_render_bar(_GLFWwindow *window, uint8_t *output, uint32_t bar_bg, uint32_t fg, uint32_t hover_bg UNUSED, bool is_dark) {
    WaylandTabBarState *s = state_for_window(window->id, false);
    if (!s || !s->count) return;
    const double fscale = decs.for_window_state.fscale;
    Canvas bar = {.px = (uint32_t*)output, .width = (int)decs.titlebar.buffer.width, .height = (int)decs.titlebar.buffer.height};
    const size_t bar_sz = (size_t)bar.width * bar.height * sizeof(uint32_t);
    if (s->render_scratch_sz != bar_sz) {
        free(s->render_scratch);
        s->render_scratch = malloc(bar_sz);
        s->render_scratch_sz = s->render_scratch ? bar_sz : 0;
    }
    if (s->render_scratch) bar.px = s->render_scratch;

    // Keep the titlebar subsurface desynced while tabs are shown. The default
    // sync mode latches commits until the parent (GL) surface commits; mixing
    // those latched frames with the animation timer's immediate commits shows
    // buffers out of order, which flickered visibly while dragging tabs.
    if (decs.titlebar.subsurface && decs.titlebar.subsurface != s->desynced_subsurface) {
        wl_subsurface_set_desync(decs.titlebar.subsurface);
        s->desynced_subsurface = decs.titlebar.subsurface;
    }

    // Pixel parity with the macOS build. macos_titlebar_color light/dark is
    // honored here too (an explicit wayland_titlebar_color still wins), and
    // the system-color path uses the measured macOS titlebar colors: the
    // upstream CSD picks #303030 for dark, but the macOS dark titlebar
    // measures #393A39 (macos.png). Only the dark, focused value is verified;
    // the others are approximations of the corresponding macOS appearances.
    const bool forced = s->forced_appearance && !decs.use_custom_titlebar_color;
    if (forced || !decs.use_custom_titlebar_color) {
        const bool dark = forced ? s->forced_appearance == 2 : is_dark;
        const bool is_focused = window->id == _glfw.focusedWindowId;
        if (dark) { bar_bg = is_focused ? 0x393A39 : 0x2C2C2C; fg = is_focused ? 0xffffff : 0xcccccc; }
        else { bar_bg = is_focused ? 0xECECEC : 0xF6F6F6; fg = is_focused ? 0x444444 : 0x888888; }
    }

    const uint32_t bar_argb = 0xff000000u | (bar_bg & 0xffffff);
    for (size_t i = 0; i < (size_t)bar.width * bar.height; i++) bar.px[i] = bar_argb;

    // window buttons first: they define how much room tabs have
    render_compact_window_buttons(window, s, &bar, fg, fscale);

    // layout, all in scaled pixels; dying tabs keep their frozen geometry and
    // do not take part in it
    const int left_margin = tabs_left_margin(fscale);
    const int tab_h = (int)round(TAB_HEIGHT * fscale), spacing = (int)round(TAB_SPACING * fscale);
    const int plus_w = (int)round(PLUS_BUTTON_WIDTH * fscale);
    int n = 0;
    for (size_t i = 0; i < s->count; i++) if (!s->tabs[i].dying) n++;
    const int available = s->tabs_area_right - left_margin - plus_w - spacing;
    int tab_w = n > 0 ? (available - spacing * (n - 1)) / n : (int)round(TAB_MAX_WIDTH * fscale);
    const int min_w = (int)round(TAB_MIN_WIDTH * fscale), max_w = (int)round(TAB_MAX_WIDTH * fscale);
    if (tab_w < min_w) tab_w = min_w;
    if (tab_w > max_w) tab_w = max_w;
    const int tab_y = (bar.height - tab_h) / 2;
    s->tab_w = tab_w; s->spacing = spacing; s->tab_y = tab_y; s->tab_h = tab_h;
    // window resizes and scale changes must not trail behind an animation
    const bool snap = s->layout_bar_width != bar.width || s->layout_fscale != fscale;

    WaylandTabEntry *dragged = NULL;
    int x = left_margin;
    for (size_t i = 0; i < s->count; i++) {
        WaylandTabEntry *t = s->tabs + i;
        if (t->dying) {
            if (snap) set_anim(&t->fade, 0);  // stale geometry, drop it
            continue;
        }
        if (!t->have_layout || snap) {
            set_anim(&t->move_x, x); set_anim(&t->move_w, tab_w);
            t->have_layout = true;
        } else {
            if (t->move_x.to != x) start_anim(&t->move_x, anim_current(&t->move_x), x);
            if (t->move_w.to != tab_w) start_anim(&t->move_w, anim_current(&t->move_w), tab_w);
        }
        t->x = (int)round(anim_current(&t->move_x));
        t->w = (int)round(anim_current(&t->move_w));
        t->y = tab_y; t->h = tab_h;
        x += tab_w + spacing;
    }
    for (size_t i = 0; i < s->count; i++) {
        WaylandTabEntry *t = s->tabs + i;
        if (s->dragging && t->tab_id == s->drag_tab_id) { dragged = t; continue; }
        render_one_tab(window, t, &bar, fscale, t->x);
    }
    if (!s->plus_have_layout || snap) { set_anim(&s->plus_move, x); s->plus_have_layout = true; }
    else if (s->plus_move.to != x) start_anim(&s->plus_move, anim_current(&s->plus_move), x);
    s->plus.x = (int)round(anim_current(&s->plus_move));
    s->plus.y = tab_y; s->plus.w = plus_w; s->plus.h = tab_h;
    render_plus_button(s, &bar, bar_bg & 0xffffff, fscale);
    // in-bar ghost on top; hidden while torn off (the subsurface ghost shows)
    if (dragged && !s->drag_out) render_one_tab(window, dragged, &bar, fscale, s->ghost_x);
    draw_titlebar_border(&bar, WINDOW_TOP_CORNER_RADIUS * fscale, fscale);
    round_top_corners(&bar, WINDOW_TOP_CORNER_RADIUS * fscale);
    if (bar.px != (uint32_t*)output) memcpy(output, bar.px, bar_sz);
    s->layout_bar_width = bar.width;
    s->layout_fscale = fscale;
}

// Animation timer {{{
static id_type anim_timer = 0;
static bool anim_timer_enabled = false;

static bool
entry_animating(WaylandTabEntry *t) {
    return t->dying || anim_is_running(&t->move_x) || anim_is_running(&t->move_w)
        || anim_is_running(&t->fade) || anim_is_running(&t->hover) || anim_is_running(&t->color);
}

static bool
state_animating(WaylandTabBarState *s) {
    if (anim_is_running(&s->plus_hover) || anim_is_running(&s->plus_move)) return true;
    for (size_t i = 0; i < s->count; i++) if (entry_animating(s->tabs + i)) return true;
    return false;
}

static void
anim_tick(id_type timer_id UNUSED, void *data UNUSED) {
    bool any_active = false;
    for (WaylandTabBarState *s = all_states; s; s = s->next) {
        _GLFWwindow *window = _glfwWindowForId(s->window_id);
        if (!window) continue;
        // reap tabs that have finished fading out
        size_t i = 0;
        while (i < s->count) {
            WaylandTabEntry *t = s->tabs + i;
            if (t->dying && !anim_is_running(&t->fade)) {
                free_entry(t);
                memmove(t, t + 1, (s->count - i - 1) * sizeof(*t));
                s->count--;
            } else i++;
        }
        if (!state_animating(s)) continue;
        any_active = true;
        csd_change_title(window);
    }
    if (!any_active && anim_timer_enabled) {
        toggleTimer(&_glfw.wl.eventLoopData, anim_timer, 0);
        anim_timer_enabled = false;
    }
}

static void
schedule_anim_frames(void) {
    if (!anim_timer) anim_timer = addTimer(&_glfw.wl.eventLoopData, "titlebar-tabs-animation",
            ANIM_FRAME_INTERVAL, 0, true, anim_tick, NULL, NULL);
    if (anim_timer && !anim_timer_enabled) {
        toggleTimer(&_glfw.wl.eventLoopData, anim_timer, 1);
        anim_timer_enabled = true;
    }
}
// }}}

// Event handling {{{

static WaylandTabEntry*
entry_for_id(WaylandTabBarState *s, unsigned long long tab_id) {
    for (size_t i = 0; i < s->count; i++)
        if (!s->tabs[i].dying && s->tabs[i].tab_id == tab_id) return s->tabs + i;
    return NULL;
}

static WaylandTabEntry*
tab_at(WaylandTabBarState *s, int sx, int sy) {
    for (size_t i = 0; i < s->count; i++) {
        WaylandTabEntry *t = s->tabs + i;
        if (t->dying) continue;
        if (sx >= t->x && sx < t->x + t->w && sy >= t->y && sy < t->y + t->h) return t;
    }
    return NULL;
}

static bool
in_close_hit_area(WaylandTabEntry *t, int sx, int sy, double fscale) {
    // close rect expanded by 2 logical px on all sides
    const double close_size = CLOSE_RECT_SIZE * fscale, expand = CLOSE_HIT_EXPAND * fscale;
    const double cx = t->x + t->w - CLOSE_RECT_RIGHT_OFFSET * fscale - expand;
    const double cy = t->y + (t->h - close_size) / 2 - expand;
    return sx >= cx && sx < cx + close_size + 2 * expand && sy >= cy && sy < cy + close_size + 2 * expand;
}

static bool
in_plus(WaylandTabBarState *s, int sx, int sy) {
    return sx >= s->plus.x && sx < s->plus.x + s->plus.w && sy >= s->plus.y && sy < s->plus.y + s->plus.h;
}

static int
live_index_of(WaylandTabBarState *s, unsigned long long tab_id) {
    int idx = 0;
    for (size_t i = 0; i < s->count; i++) {
        WaylandTabEntry *t = s->tabs + i;
        if (t->dying) continue;
        if (t->tab_id == tab_id) return idx;
        idx++;
    }
    return 0;
}

// macOS dropIndexForPoint: like Chrome, swap once the ghost covers more than
// half of the neighboring tab. Slot midpoints are computed from the layout
// targets (the gap for the dragged tab included), matching NSView frames.
static int
live_drop_index(WaylandTabBarState *s, int ghost_left, bool moving_right, double fscale) {
    const int step = s->tab_w + s->spacing;
    const int ghost_right = ghost_left + s->tab_w;
    const int left_margin = tabs_left_margin(fscale);
    int idx = 0, slot = 0;
    for (size_t i = 0; i < s->count; i++) {
        WaylandTabEntry *t = s->tabs + i;
        if (t->dying) continue;
        const int mid = left_margin + slot * step + s->tab_w / 2;
        slot++;
        if (t->tab_id == s->drag_tab_id) continue;
        if (moving_right ? ghost_right > mid : mid < ghost_left) idx++;
    }
    return idx;
}

// move the dragged tab to live slot idx. Live entries always form the array
// prefix (the diff in glfwWaylandSetTitlebarTabs appends dying ones), so this
// is a plain rotate within that prefix.
static void
reorder_dragged(WaylandTabBarState *s, int idx) {
    size_t from = 0, live = 0;
    bool found = false;
    for (size_t i = 0; i < s->count && !s->tabs[i].dying; i++, live++)
        if (s->tabs[i].tab_id == s->drag_tab_id) { from = i; found = true; }
    if (!found || !live) return;
    size_t to = (size_t)(idx < 0 ? 0 : idx);
    if (to >= live) to = live - 1;
    if (to == from) return;
    WaylandTabEntry tmp = s->tabs[from];
    if (from < to) memmove(s->tabs + from, s->tabs + from + 1, (to - from) * sizeof(tmp));
    else memmove(s->tabs + to + 1, s->tabs + to, (from - to) * sizeof(tmp));
    s->tabs[to] = tmp;
}

bool
wl_titlebar_tabs_handle_motion(_GLFWwindow *window, double x, double y) {
    WaylandTabBarState *s = state_for_window(window->id, false);
    if (!s || !s->count) return false;
    const double fscale = decs.for_window_state.fscale;
    const int sx = (int)round(fscale * x), sy = (int)round(fscale * y);
    if (s->pressed_on == PRESS_TAB) {
        if (!s->dragging && (abs(sx - s->pressed_x) >= DRAG_THRESHOLD * fscale || abs(sy - s->pressed_y) >= DRAG_THRESHOLD * fscale)) {
            s->dragging = true;
            s->drag_tab_id = s->pressed_tab_id;
            s->drag_index = live_index_of(s, s->drag_tab_id);
            s->last_drag_x = s->pressed_x;
            TABS_DEBUG("drag start tab=%llu", s->drag_tab_id);
            // macOS behaviour: activate the tab as soon as the drag starts
            if (_glfw.callbacks.titlebar_tab_action)
                _glfw.callbacks.titlebar_tab_action((GLFWwindow*)window, GLFW_TITLEBAR_TAB_ACTIVATE, s->drag_tab_id, 0);
        }
        if (s->dragging) {
            int gx = sx - s->drag_grab_dx;
            const int max_x = (int)decs.titlebar.buffer.width - s->tab_w;
            if (gx < 0) gx = 0;
            if (gx > max_x) gx = max_x;
            // live reorder, same as macOS: once the ghost covers more than
            // half of a neighboring tab the tabs swap (animated)
            const bool moving_right = sx >= s->last_drag_x;
            s->last_drag_x = sx;
            const int idx = live_drop_index(s, gx, moving_right, fscale);
            if (idx != s->drag_index) {
                reorder_dragged(s, idx);
                s->drag_index = idx;
            }
            s->ghost_x = gx; s->drag_cur_y = sy;
            const bool out = sy < -(int)(DETACH_MARGIN * fscale)
                || sy > (int)decs.titlebar.buffer.height + (int)(DETACH_MARGIN * fscale);
            if (out != s->drag_out) {
                s->drag_out = out;
                if (!out) destroy_drag_ghost(s);
                else if (_glfw.callbacks.titlebar_tab_action) {
                    // ask kitty to start a real DND session so the tab can be
                    // dropped onto other kitty windows (upstream mime drag).
                    // Until the compositor takes the pointer (see
                    // handle_leave) the in-client ghost below keeps tracking.
                    TABS_DEBUG("drag out: requesting DND handoff for tab=%llu", s->drag_tab_id);
                    _glfw.callbacks.titlebar_tab_action((GLFWwindow*)window, GLFW_TITLEBAR_TAB_DRAG_OUT, s->drag_tab_id, 0);
                }
            }
            if (out) {
                WaylandTabEntry *d = entry_for_id(s, s->drag_tab_id);
                if (d && show_drag_ghost(window, s, d, fscale)) {
                    // logical coords relative to the titlebar surface; applied
                    // by the titlebar commit of the redraw below
                    wl_subsurface_set_position(s->ghost.subsurface,
                        (int)round((sx - s->drag_grab_dx) / fscale), (int)round((sy - s->drag_grab_dy) / fscale));
                }
            }
            TABS_DEBUG("drag motion sx=%d sy=%d bar_h=%d out=%d", sx, sy, (int)decs.titlebar.buffer.height, (int)out);
            decs.titlebar_needs_update = true;
            return true;
        }
    }
    bool over_something = false, changed = false;
    for (size_t i = 0; i < s->count; i++) {
        WaylandTabEntry *t = s->tabs + i;
        if (t->dying) continue;
        const bool hovered = sx >= t->x && sx < t->x + t->w && sy >= t->y && sy < t->y + t->h;
        const bool close_hovered = hovered && in_close_hit_area(t, sx, sy, fscale);
        if (hovered != t->hovered) {
            start_anim(&t->hover, anim_current(&t->hover), hovered ? 1 : 0);
            changed = true;
        }
        if (close_hovered != t->close_hovered) changed = true;
        t->hovered = hovered; t->close_hovered = close_hovered;
        if (hovered) over_something = true;
    }
    const bool plus_hovered = in_plus(s, sx, sy);
    if (plus_hovered != s->plus.hovered) {
        start_anim(&s->plus_hover, anim_current(&s->plus_hover), plus_hovered ? 1 : 0);
        changed = true;
    }
    s->plus.hovered = plus_hovered;
    if (plus_hovered) over_something = true;
    if (changed) decs.titlebar_needs_update = true;
    return over_something;
}

bool
wl_titlebar_tabs_handle_leave(_GLFWwindow *window) {
    WaylandTabBarState *s = state_for_window(window->id, false);
    if (!s) return false;
    bool changed = false;
    for (size_t i = 0; i < s->count; i++) {
        WaylandTabEntry *t = s->tabs + i;
        if (t->hovered) start_anim(&t->hover, anim_current(&t->hover), 0);
        if (t->hovered || t->close_hovered) changed = true;
        t->hovered = false; t->close_hovered = false;
    }
    if (s->plus.hovered) {
        start_anim(&s->plus_hover, anim_current(&s->plus_hover), 0);
        changed = true;
    }
    s->plus.hovered = false;
    if (s->dragging && s->pressed_on == PRESS_TAB && !_glfw.wl.drag.source) {
        // kwin re-picks the focused surface among the client's own surfaces
        // even while a button is held: dragging a tab off the titlebar sends
        // leave + enter(main surface). Keep the drag alive; the events are
        // forwarded by wl_titlebar_tabs_forward_grabbed_pointer().
        TABS_DEBUG("leave during drag: keeping the drag alive");
    } else {
        // either a normal leave or the DND session (_glfw.wl.drag.source)
        // took the pointer over: the tab now travels as a drag payload
        s->pressed_on = PRESS_NONE;
        if (s->dragging) { TABS_DEBUG("leave cancels drag (dnd=%d)", _glfw.wl.drag.source != NULL); s->dragging = false; changed = true; }
        s->drag_out = false;
        destroy_drag_ghost(s);
    }
    if (changed) decs.titlebar_needs_update = true;
    return changed;
}

bool
wl_titlebar_tabs_handle_button(_GLFWwindow *window, uint32_t button, uint32_t state, double x, double y) {
    WaylandTabBarState *s = state_for_window(window->id, false);
    if (!s || !s->count) return false;
    if (button != BTN_LEFT && button != BTN_MIDDLE) return false;
    const double fscale = decs.for_window_state.fscale;
    const int sx = (int)round(fscale * x), sy = (int)round(fscale * y);
    WaylandTabEntry *t = tab_at(s, sx, sy);
    const bool on_plus = !t && in_plus(s, sx, sy);
    if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
        if (!t && !on_plus) return false;
        s->pressed_button = button;
        if (on_plus) { s->pressed_on = PRESS_PLUS; }
        else {
            s->pressed_on = (button == BTN_LEFT && in_close_hit_area(t, sx, sy, fscale)) ? PRESS_CLOSE : PRESS_TAB;
            s->pressed_tab_id = t->tab_id;
            s->pressed_x = sx; s->pressed_y = sy;
            s->drag_grab_dx = sx - t->x;
            s->drag_grab_dy = sy - t->y;
        }
        s->dragging = false;
        s->drag_out = false;
        destroy_drag_ghost(s);
        return true;
    }
    // release
    if (s->dragging && button == s->pressed_button) {
        const bool detach = s->drag_cur_y < -(int)(DETACH_MARGIN * fscale)
            || s->drag_cur_y > (int)decs.titlebar.buffer.height + (int)(DETACH_MARGIN * fscale);
        TABS_DEBUG("release: drag_cur_y=%d bar_h=%d detach=%d", s->drag_cur_y, (int)decs.titlebar.buffer.height, (int)detach);
        // the drop index was maintained during the drag by the live reorder
        int idx = s->drag_index;
        size_t live = 0;
        for (size_t i = 0; i < s->count; i++) if (!s->tabs[i].dying) live++;
        if (idx < 0) idx = 0;
        if (live && idx > (int)live - 1) idx = (int)live - 1;
        const unsigned long long tab_id = s->drag_tab_id;
        // let the dragged tab animate from where it was dropped to its slot
        WaylandTabEntry *d = entry_for_id(s, tab_id);
        if (d) { set_anim(&d->move_x, s->ghost_x); d->x = s->ghost_x; }
        s->dragging = false; s->pressed_on = PRESS_NONE;
        s->drag_out = false;
        destroy_drag_ghost(s);
        decs.titlebar_needs_update = true;
        if (_glfw.callbacks.titlebar_tab_action) {
            if (detach) _glfw.callbacks.titlebar_tab_action((GLFWwindow*)window, GLFW_TITLEBAR_TAB_DETACH, tab_id, 0);
            else _glfw.callbacks.titlebar_tab_action((GLFWwindow*)window, GLFW_TITLEBAR_TAB_DROP, tab_id, idx);
        }
        return true;
    }
    if (s->pressed_on == PRESS_NONE || s->pressed_button != button) return t != NULL || on_plus;
    const int was_pressed_on = s->pressed_on;
    s->pressed_on = PRESS_NONE;
    if (!_glfw.callbacks.titlebar_tab_action) return true;
    switch (was_pressed_on) {
        case PRESS_PLUS:
            if (on_plus) _glfw.callbacks.titlebar_tab_action((GLFWwindow*)window, GLFW_TITLEBAR_TAB_NEW, 0, 0);
            break;
        case PRESS_CLOSE:
            if (t && t->tab_id == s->pressed_tab_id && in_close_hit_area(t, sx, sy, fscale))
                _glfw.callbacks.titlebar_tab_action((GLFWwindow*)window, GLFW_TITLEBAR_TAB_CLOSE, t->tab_id, 0);
            break;
        case PRESS_TAB:
            if (t && t->tab_id == s->pressed_tab_id) {
                _glfw.callbacks.titlebar_tab_action((GLFWwindow*)window,
                    button == BTN_MIDDLE ? GLFW_TITLEBAR_TAB_CLOSE : GLFW_TITLEBAR_TAB_ACTIVATE, t->tab_id, 0);
            }
            break;
    }
    return true;
}

// kwin re-picks the pointer-focused surface among a client's own surfaces
// even while a button is held down, so dragging a tab off the titlebar gets a
// leave + enter(main surface) pair and the remaining motion/button events are
// delivered relative to the newly focused surface. This forwards them to the
// drag with coordinates translated into titlebar space, so the ghost keeps
// following and the release still produces DROP/DETACH. Returns true when the
// event was consumed. Hooked at the top of pointerHandleMotion/Button.
bool
wl_titlebar_tabs_forward_grabbed_pointer(_GLFWwindow *window, int button, uint32_t state) {
    WaylandTabBarState *s = state_for_window(window->id, false);
    if (!s || !s->dragging || s->pressed_on != PRESS_TAB) return false;
    if (decs.focus == CSD_titlebar) return false;  // the regular CSD path handles these
    const double W = decs.metrics.width, vth = decs.metrics.visible_titlebar_height;
    double dx, dy;
    switch (decs.focus) {
        case CENTRAL_WINDOW: dx = 0; dy = vth; break;
        case CSD_shadow_top: dx = 0; dy = -W; break;
        case CSD_shadow_bottom: dx = 0; dy = window->wl.height + vth; break;
        case CSD_shadow_left: dx = -W; dy = 0; break;
        case CSD_shadow_right: dx = window->wl.width; dy = 0; break;
        case CSD_shadow_upper_left: dx = -W; dy = -W; break;
        case CSD_shadow_upper_right: dx = window->wl.width; dy = -W; break;
        case CSD_shadow_lower_left: dx = -W; dy = window->wl.height + vth; break;
        case CSD_shadow_lower_right: dx = window->wl.width; dy = window->wl.height + vth; break;
        default: return false;
    }
    const double tx = window->wl.allCursorPosX + dx, ty = window->wl.allCursorPosY + dy;
    decs.titlebar_needs_update = false;
    bool consumed;
    if (button < 0) consumed = wl_titlebar_tabs_handle_motion(window, tx, ty);
    else consumed = wl_titlebar_tabs_handle_button(window, (uint32_t)button, state, tx, ty);
    TABS_DEBUG("forwarded grabbed %s focus=%d -> (%.0f,%.0f) consumed=%d",
               button < 0 ? "motion" : "button", (int)decs.focus, tx, ty, (int)consumed);
    if (decs.titlebar_needs_update) {
        decs.titlebar_needs_update = false;
        csd_change_title(window);
        if (!window->wl.waiting_for_swap_to_commit) wl_surface_commit(window->wl.surface);
    }
    return consumed;
}
// }}}

GLFWAPI void
glfwWaylandSetTitlebarTabs(GLFWwindow *handle, const GLFWTitlebarTab *tabs, size_t count, uint32_t bar_color, bool use_system_color, int forced_appearance) {
    _GLFW_REQUIRE_INIT();
    _GLFWwindow *window = (_GLFWwindow*)handle;
    if (!window) return;
    WaylandTabBarState *s = state_for_window(window->id, true);
    if (!s) return;
    s->forced_appearance = forced_appearance;

    // Diff the incoming list against the current entries by tab_id so
    // animation state survives updates.
    const size_t old_count = s->count;
    bool had_live = false;
    for (size_t i = 0; i < old_count; i++) {
        s->tabs[i].taken = false;
        if (!s->tabs[i].dying) had_live = true;
    }
    WaylandTabEntry *fresh = NULL;
    const size_t max_count = count + old_count;
    if (max_count) {
        fresh = calloc(max_count, sizeof(WaylandTabEntry));
        if (!fresh) return;
    }
    size_t n = 0;
    for (size_t i = 0; i < count; i++) {
        WaylandTabEntry *match = NULL;
        for (size_t j = 0; j < old_count; j++) {
            WaylandTabEntry *o = s->tabs + j;
            if (!o->taken && !o->dying && o->tab_id == tabs[i].tab_id) { match = o; break; }
        }
        WaylandTabEntry *t = fresh + n++;
        const uint32_t fg = tabs[i].fg & 0xffffff, bg = tabs[i].bg & 0xffffff;
        if (match) {
            *t = *match; match->taken = true;
            if (!t->title || !tabs[i].title || strcmp(t->title, tabs[i].title) != 0) {
                free(t->title);
                t->title = tabs[i].title ? _glfw_strdup(tabs[i].title) : NULL;
                free(t->mask); t->mask = NULL;  // title changed, mask is stale
            }
            t->needs_attention = tabs[i].needs_attention;
            if (fg != t->fg || bg != t->bg) {
                if (!t->is_active && tabs[i].is_active) {
                    // macOS animates the colour change only on the tab
                    // becoming active; capture the currently displayed colour
                    const double ct = anim_current(&t->color);
                    t->from_fg = mix_rgb(t->from_fg, t->fg, ct);
                    t->from_bg = mix_rgb(t->from_bg, t->bg, ct);
                    start_anim(&t->color, 0, 1);
                } else {
                    t->from_fg = fg; t->from_bg = bg;
                    set_anim(&t->color, 1);
                }
                t->fg = fg; t->bg = bg;
            }
            t->is_active = tabs[i].is_active;
        } else {
            t->tab_id = tabs[i].tab_id;
            t->title = tabs[i].title ? _glfw_strdup(tabs[i].title) : NULL;
            t->is_active = tabs[i].is_active; t->needs_attention = tabs[i].needs_attention;
            t->fg = fg; t->bg = bg; t->from_fg = fg; t->from_bg = bg;
            set_anim(&t->color, 1);
            set_anim(&t->hover, 0);
            // new tabs appear in place with a fade-in; the very first batch
            // (window creation) appears without one
            if (had_live) start_anim(&t->fade, 0, 1); else set_anim(&t->fade, 1);
            t->have_layout = false;
        }
    }
    bool any_dying = false;
    for (size_t j = 0; j < old_count; j++) {
        WaylandTabEntry *o = s->tabs + j;
        if (o->taken) continue;
        WaylandTabEntry *t = fresh + n++;
        *t = *o;
        if (!t->dying) {
            t->dying = true;
            t->hovered = false; t->close_hovered = false;
            start_anim(&t->fade, anim_current(&t->fade), 0);
        }
        any_dying = true;
    }
    free(s->tabs);
    s->tabs = fresh; s->count = n; s->capacity = max_count;
    if (any_dying) schedule_anim_frames();  // make sure fully faded tabs get reaped

    // Compositors like mutter default to server side decorations, which
    // would leave no titlebar to draw tabs into: force CSD.
    if (s->count > 0 && decs.serverSide) {
        decs.serverSide = false;
        if (window->wl.xdg.decoration) zxdg_toplevel_decoration_v1_set_mode(window->wl.xdg.decoration, ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE);
        csd_set_visible(window, csd_should_window_be_decorated(window));
    }
    // Refresh the opaque region right away: update_regions() in wl_window.c
    // only runs on create/resize, so without this the bottom corner cutouts
    // (rendered by kitty's GL corner mask) would stay opaque until a resize.
    if (!window->wl.transparent && window->wl.surface) {
        struct wl_region *region = wl_compositor_create_region(_glfw.wl.compositor);
        if (region) {
            wl_region_add(region, 0, 0, window->wl.width, window->wl.height);
            if (s->count > 0) {
                wl_region_subtract(region, 0, window->wl.height - 10, 10, 10);
                wl_region_subtract(region, window->wl.width - 10, window->wl.height - 10, 10, 10);
            }
            wl_surface_set_opaque_region(window->wl.surface, region);
            wl_region_destroy(region);
        }
    }
    // Use a taller titlebar so the 24px tabs are vertically centered with
    // breathing room, mirroring the macOS look. Metrics feed all the CSD
    // geometry calculations, so just updating them and forcing a rebuild is
    // enough.
    if (s->count > 0 && decs.metrics.visible_titlebar_height != TABS_TITLEBAR_HEIGHT) {
        // wider shadow margin for the Chrome/Breeze-style drop shadow (the
        // interactive resize border stays 12px, see restrict_shadow_input_regions)
        decs.metrics.width = SHADOW_MARGIN;
        decs.metrics.horizontal = 2 * decs.metrics.width;
        decs.metrics.top = decs.metrics.width + TABS_TITLEBAR_HEIGHT;
        decs.metrics.visible_titlebar_height = TABS_TITLEBAR_HEIGHT;
        decs.metrics.vertical = decs.metrics.width + decs.metrics.top;
        decs.for_window_state.width = 0;  // force ensure_csd_resources() to rebuild buffers
    }
    // csd_set_titlebar_color() also redraws the titlebar. Passing the color
    // here (instead of relying on glfwWaylandSetTitlebarColor at window
    // creation) matters because that call is dropped while decorations are
    // still server side, which they are until the forced CSD switch above.
    if (csd_set_titlebar_color(window, bar_color, use_system_color) && !window->wl.waiting_for_swap_to_commit)
        wl_surface_commit(window->wl.surface);
    // cover the rounded-off corners with shadow continuations (also runs from
    // ensure_csd_resources on resize/scale/focus changes)
    wl_titlebar_tabs_update_corner_patches(window);
}
