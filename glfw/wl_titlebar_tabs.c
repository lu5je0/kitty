/*
 * wl_titlebar_tabs.c
 * Fork-specific: native titlebar tabs for Wayland CSD, replicating the macOS
 * implementation in kitty/cocoa_window.m (see todo.md for the exact spec).
 *
 * Distributed under terms of the GPL3 license.
 */

#include "wl_titlebar_tabs.h"

#include "wl_client_side_decorations.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
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

// Logical (unscaled) metrics, must match the macOS implementation
#define TAB_MAX_WIDTH 200.
#define TAB_MIN_WIDTH 60.
#define TAB_HEIGHT 24.
#define TAB_SPACING 4.
#define TAB_CORNER_RADIUS 6.
#define PLUS_BUTTON_WIDTH 28.
#define TAB_BAR_LEFT_MARGIN 8.
#define BAR_RIGHT_PADDING 8.
#define CLOSE_RECT_SIZE 14.
#define CLOSE_RECT_RIGHT_OFFSET 20.  // close rect x = tab_w - 20
#define CLOSE_HIT_EXPAND 2.
#define CLOSE_CROSS_INSET 4.25
#define STROKE_WIDTH 1.2
#define PLUS_ARM 5.0
#define TEXT_LEFT_PADDING 8.
#define TEXT_RIGHT_MARGIN 22.
#define TEXT_SIZE 12.
// visible titlebar height (logical) when tabs are shown; 24px tabs centered in it.
// Measured from macos.png: the macOS titlebar with tabs is 28 logical px tall.
#define TABS_TITLEBAR_HEIGHT 28
#define WINDOW_TOP_CORNER_RADIUS 10.
#define ATTENTION_COLOR 0xff9500u  // approximation of NSColor.systemOrange
// compact window buttons (KDE-like size, drawn by us since the upstream ones
// are bar-height sized which is too big for the 28px tabs bar)
#define BUTTON_CELL_WIDTH 28.
#define BUTTON_ICON_ARM 5.
#define BUTTON_HOVER_DIAMETER 20.
#define DRAG_THRESHOLD 4.
#define DETACH_MARGIN 40.

typedef struct WaylandTabEntry {
    unsigned long long tab_id;
    char *title;
    bool is_active, needs_attention;
    uint32_t fg, bg;  // 0xRRGGBB
    // geometry from the last render, in scaled pixels
    int x, y, w, h;
    bool hovered, close_hovered;
} WaylandTabEntry;

typedef struct WaylandTabBarState {
    uintptr_t window_id;
    WaylandTabEntry *tabs;
    size_t count, capacity;
    struct { int x, y, w, h; bool hovered; } plus;
    // press tracking for click semantics
    enum { PRESS_NONE, PRESS_TAB, PRESS_CLOSE, PRESS_PLUS } pressed_on;
    unsigned long long pressed_tab_id;
    uint32_t pressed_button;
    // in-bar tab dragging (reorder)
    bool dragging;
    unsigned long long drag_tab_id;
    int pressed_x, pressed_y;   // scaled px
    int drag_grab_dx;           // scaled px: pointer x - tab x at press
    int ghost_x, drag_cur_y;    // scaled px
    // layout of the last render, scaled px
    int tab_w, spacing, tab_y, tab_h, tabs_area_right;
    struct WaylandTabBarState *next;
} WaylandTabBarState;

static WaylandTabBarState *all_states = NULL;

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
clear_tabs(WaylandTabBarState *s) {
    for (size_t i = 0; i < s->count; i++) free(s->tabs[i].title);
    s->count = 0;
}

void
wl_titlebar_tabs_free(_GLFWwindow *window) {
    WaylandTabBarState **p = &all_states;
    while (*p) {
        if ((*p)->window_id == window->id) {
            WaylandTabBarState *s = *p;
            *p = s->next;
            clear_tabs(s);
            free(s->tabs);
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
// rounded rect alpha mask positioned at (x, y) in dest.
static void
canvas_composite_rounded(Canvas *dest, const Canvas *src, int x, int y, double r) {
    for (int sy = 0; sy < src->height; sy++) {
        const int dy = y + sy;
        if (dy < 0 || dy >= dest->height) continue;
        uint32_t *drow = dest->px + (size_t)dy * dest->width;
        const uint32_t *srow = src->px + (size_t)sy * src->width;
        for (int sx = 0; sx < src->width; sx++) {
            const int dx = x + sx;
            if (dx < 0 || dx >= dest->width) continue;
            const double cov = rounded_rect_coverage(sx, sy, 0, 0, src->width, src->height, r);
            if (cov > 0) drow[dx] = blend_argb(drow[dx], srow[sx], cov);
        }
    }
}
// }}}

static void
render_one_tab(_GLFWwindow *window, WaylandTabBarState *s UNUSED, WaylandTabEntry *t, Canvas *bar, double fscale) {
    uint32_t bg = t->bg, fg = t->fg;
    if (t->hovered && !t->is_active) {
        // mix bg 0.08 towards white (dark bg) or black (light bg)
        bg = mix_rgb(bg, luminance(bg) < 0.5 ? 0xffffff : 0x000000, 0.08);
    }
    if (t->needs_attention && !t->is_active) fg = ATTENTION_COLOR;

    Canvas tab = {.width = t->w, .height = t->h};
    tab.px = malloc((size_t)tab.width * tab.height * sizeof(uint32_t));
    if (!tab.px) return;
    const uint32_t bg_argb = 0xff000000u | bg;
    for (size_t i = 0; i < (size_t)tab.width * tab.height; i++) tab.px[i] = bg_argb;

    // title text, centered, in the box (8, *, w - 8 - 22, full height)
    if (t->title && t->title[0] && _glfw.callbacks.titlebar_tab_text) {
        _glfw.callbacks.titlebar_tab_text(
            (GLFWwindow*)window, t->title, (unsigned)round(TEXT_SIZE * fscale),
            0xff000000u | fg, bg_argb, (uint8_t*)tab.px, tab.width, tab.height,
            (float)(TEXT_LEFT_PADDING * fscale), 0, (size_t)round(TEXT_RIGHT_MARGIN * fscale));
    }

    // close button: rect is (w-20, (h-14)/2, 14, 14) in logical units
    const double close_x = t->w - CLOSE_RECT_RIGHT_OFFSET * fscale, close_size = CLOSE_RECT_SIZE * fscale;
    const double close_y = (t->h - close_size) / 2;
    if (t->close_hovered) {
        canvas_blend_circle(&tab, close_x + close_size / 2, close_y + close_size / 2, close_size / 2, fg, 0.25);
    }
    const double inset = CLOSE_CROSS_INSET * fscale, half_w = STROKE_WIDTH * fscale / 2;
    const double alpha = t->close_hovered ? 1.0 : 0.6;
    canvas_blend_segment(&tab, close_x + inset, close_y + inset, close_x + close_size - inset, close_y + close_size - inset, half_w, fg, alpha);
    canvas_blend_segment(&tab, close_x + inset, close_y + close_size - inset, close_x + close_size - inset, close_y + inset, half_w, fg, alpha);

    canvas_composite_rounded(bar, &tab, t->x, t->y, TAB_CORNER_RADIUS * fscale);
    free(tab.px);
}

static void
render_plus_button(WaylandTabBarState *s, Canvas *bar, uint32_t bar_bg, double fscale) {
    const bool is_dark = luminance(bar_bg & 0xffffff) < 0.5;
    if (s->plus.hovered) {
        canvas_blend_rounded_rect(bar, s->plus.x, s->plus.y, s->plus.w, s->plus.h,
                TAB_CORNER_RADIUS * fscale, is_dark ? 0xffffff : 0x000000, is_dark ? 0.12 : 0.08);
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

static void
render_compact_window_buttons(_GLFWwindow *window, WaylandTabBarState *s, Canvas *bar, uint32_t bar_bg, uint32_t fg, uint32_t hover_bg, bool is_dark, double fscale) {
    (void)bar_bg;
    const int cell_w = (int)round(BUTTON_CELL_WIDTH * fscale);
    const double arm = BUTTON_ICON_ARM * fscale, half_w = STROKE_WIDTH * fscale / 2;
    const double hover_r = BUTTON_HOVER_DIAMETER * fscale / 2;
    const double cy = bar->height / 2.;
    int right = bar->width - (int)round(BAR_RIGHT_PADDING * fscale / 2);
    const uint32_t fg_rgb = fg & 0xffffff, hover_rgb = hover_bg & 0xffffff;

    // order right-to-left: close, maximize, minimize (matches upstream order left-to-right)
    // close
    {
        const int left = right - cell_w;
        const double cx = left + cell_w / 2.;
        decs.close.left = left; decs.close.width = cell_w;
        if (decs.close.hovered) {
            canvas_blend_circle(bar, cx, cy, hover_r, is_dark ? 0x880000 : 0xc80000, 1.0);
        }
        canvas_blend_segment(bar, cx - arm, cy - arm, cx + arm, cy + arm, half_w, fg_rgb, 1.0);
        canvas_blend_segment(bar, cx - arm, cy + arm, cx + arm, cy - arm, half_w, fg_rgb, 1.0);
        right = left;
    }
    // maximize (chevron up, KDE Breeze style)
    if (window->wl.wm_capabilities.maximize) {
        const int left = right - cell_w;
        const double cx = left + cell_w / 2.;
        decs.maximize.left = left; decs.maximize.width = cell_w;
        if (decs.maximize.hovered) canvas_blend_circle(bar, cx, cy, hover_r, hover_rgb, 1.0);
        const double a = arm * 0.9;
        if (window->wl.current.toplevel_states & TOPLEVEL_STATE_MAXIMIZED) {
            canvas_blend_segment(bar, cx - a, cy - a / 2, cx, cy + a / 2, half_w, fg_rgb, 1.0);
            canvas_blend_segment(bar, cx, cy + a / 2, cx + a, cy - a / 2, half_w, fg_rgb, 1.0);
        } else {
            canvas_blend_segment(bar, cx - a, cy + a / 2, cx, cy - a / 2, half_w, fg_rgb, 1.0);
            canvas_blend_segment(bar, cx, cy - a / 2, cx + a, cy + a / 2, half_w, fg_rgb, 1.0);
        }
        right = left;
    } else { decs.maximize.left = 0; decs.maximize.width = 0; }
    // minimize (chevron down)
    if (window->wl.wm_capabilities.minimize) {
        const int left = right - cell_w;
        const double cx = left + cell_w / 2.;
        decs.minimize.left = left; decs.minimize.width = cell_w;
        if (decs.minimize.hovered) canvas_blend_circle(bar, cx, cy, hover_r, hover_rgb, 1.0);
        const double a = arm * 0.9;
        canvas_blend_segment(bar, cx - a, cy - a / 2, cx, cy + a / 2, half_w, fg_rgb, 1.0);
        canvas_blend_segment(bar, cx, cy + a / 2, cx + a, cy - a / 2, half_w, fg_rgb, 1.0);
        right = left;
    } else { decs.minimize.left = 0; decs.minimize.width = 0; }
    s->tabs_area_right = right - (int)round(TAB_SPACING * fscale);
}

void
wl_titlebar_tabs_render_bar(_GLFWwindow *window, uint8_t *output, uint32_t bar_bg, uint32_t fg, uint32_t hover_bg, bool is_dark) {
    WaylandTabBarState *s = state_for_window(window->id, false);
    if (!s || !s->count) return;
    const double fscale = decs.for_window_state.fscale;
    Canvas bar = {.px = (uint32_t*)output, .width = (int)decs.titlebar.buffer.width, .height = (int)decs.titlebar.buffer.height};

    const uint32_t bar_argb = 0xff000000u | (bar_bg & 0xffffff);
    for (size_t i = 0; i < (size_t)bar.width * bar.height; i++) bar.px[i] = bar_argb;

    // window buttons first: they define how much room tabs have
    render_compact_window_buttons(window, s, &bar, bar_argb, fg, hover_bg, is_dark, fscale);

    // layout, all in scaled pixels
    const int left_margin = (int)round(TAB_BAR_LEFT_MARGIN * fscale);
    const int tab_h = (int)round(TAB_HEIGHT * fscale), spacing = (int)round(TAB_SPACING * fscale);
    const int plus_w = (int)round(PLUS_BUTTON_WIDTH * fscale);
    const int n = (int)s->count;
    const int available = s->tabs_area_right - left_margin - plus_w - spacing;
    int tab_w = n > 0 ? (available - spacing * (n - 1)) / n : (int)round(TAB_MAX_WIDTH * fscale);
    const int min_w = (int)round(TAB_MIN_WIDTH * fscale), max_w = (int)round(TAB_MAX_WIDTH * fscale);
    if (tab_w < min_w) tab_w = min_w;
    if (tab_w > max_w) tab_w = max_w;
    const int tab_y = (bar.height - tab_h) / 2;
    s->tab_w = tab_w; s->spacing = spacing; s->tab_y = tab_y; s->tab_h = tab_h;

    WaylandTabEntry *dragged = NULL;
    int x = left_margin;
    for (int i = 0; i < n; i++) {
        WaylandTabEntry *t = s->tabs + i;
        t->x = x; t->y = tab_y; t->w = tab_w; t->h = tab_h;
        if (s->dragging && t->tab_id == s->drag_tab_id) dragged = t;
        else render_one_tab(window, s, t, &bar, fscale);
        x += tab_w + spacing;
    }
    s->plus.x = x; s->plus.y = tab_y; s->plus.w = plus_w; s->plus.h = tab_h;
    render_plus_button(s, &bar, bar_bg & 0xffffff, fscale);
    if (dragged) {  // ghost follows the pointer, drawn on top
        const int home_x = dragged->x;
        dragged->x = s->ghost_x;
        render_one_tab(window, s, dragged, &bar, fscale);
        dragged->x = home_x;
    }
    round_top_corners(&bar, WINDOW_TOP_CORNER_RADIUS * fscale);
}

// Event handling {{{

static WaylandTabEntry*
tab_at(WaylandTabBarState *s, int sx, int sy) {
    for (size_t i = 0; i < s->count; i++) {
        WaylandTabEntry *t = s->tabs + i;
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
            // macOS behaviour: activate the tab as soon as the drag starts
            if (_glfw.callbacks.titlebar_tab_action)
                _glfw.callbacks.titlebar_tab_action((GLFWwindow*)window, GLFW_TITLEBAR_TAB_ACTIVATE, s->drag_tab_id, 0);
        }
        if (s->dragging) {
            int gx = sx - s->drag_grab_dx;
            const int max_x = (int)decs.titlebar.buffer.width - s->tab_w;
            if (gx < 0) gx = 0;
            if (gx > max_x) gx = max_x;
            s->ghost_x = gx; s->drag_cur_y = sy;
            decs.titlebar_needs_update = true;
            return true;
        }
    }
    bool over_something = false, changed = false;
    for (size_t i = 0; i < s->count; i++) {
        WaylandTabEntry *t = s->tabs + i;
        const bool hovered = sx >= t->x && sx < t->x + t->w && sy >= t->y && sy < t->y + t->h;
        const bool close_hovered = hovered && in_close_hit_area(t, sx, sy, fscale);
        if (hovered != t->hovered || close_hovered != t->close_hovered) changed = true;
        t->hovered = hovered; t->close_hovered = close_hovered;
        if (hovered) over_something = true;
    }
    const bool plus_hovered = in_plus(s, sx, sy);
    if (plus_hovered != s->plus.hovered) changed = true;
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
        if (s->tabs[i].hovered || s->tabs[i].close_hovered) changed = true;
        s->tabs[i].hovered = false; s->tabs[i].close_hovered = false;
    }
    if (s->plus.hovered) changed = true;
    s->plus.hovered = false;
    s->pressed_on = PRESS_NONE;
    if (s->dragging) { s->dragging = false; changed = true; }
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
        }
        s->dragging = false;
        return true;
    }
    // release
    if (s->dragging && button == s->pressed_button) {
        const bool detach = s->drag_cur_y < -(int)(DETACH_MARGIN * fscale)
            || s->drag_cur_y > (int)decs.titlebar.buffer.height + (int)(DETACH_MARGIN * fscale);
        int idx = 0;
        if (s->count > 1 && s->tab_w + s->spacing > 0) {
            const int ghost_centre = s->ghost_x + s->tab_w / 2;
            idx = (ghost_centre - s->tabs[0].x) / (s->tab_w + s->spacing);
            if (idx < 0) idx = 0;
            if (idx > (int)s->count - 1) idx = (int)s->count - 1;
        }
        const unsigned long long tab_id = s->drag_tab_id;
        s->dragging = false; s->pressed_on = PRESS_NONE;
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
// }}}

GLFWAPI void
glfwWaylandSetTitlebarTabs(GLFWwindow *handle, const GLFWTitlebarTab *tabs, size_t count, uint32_t bar_color, bool use_system_color) {
    _GLFW_REQUIRE_INIT();
    _GLFWwindow *window = (_GLFWwindow*)handle;
    if (!window) return;
    WaylandTabBarState *s = state_for_window(window->id, true);
    if (!s) return;
    clear_tabs(s);
    if (count > s->capacity) {
        WaylandTabEntry *q = realloc(s->tabs, count * sizeof(WaylandTabEntry));
        if (!q) return;
        s->tabs = q; s->capacity = count;
    }
    for (size_t i = 0; i < count; i++) {
        WaylandTabEntry *t = s->tabs + s->count;
        memset(t, 0, sizeof(*t));
        t->tab_id = tabs[i].tab_id;
        t->title = tabs[i].title ? _glfw_strdup(tabs[i].title) : NULL;
        t->is_active = tabs[i].is_active; t->needs_attention = tabs[i].needs_attention;
        t->fg = tabs[i].fg & 0xffffff; t->bg = tabs[i].bg & 0xffffff;
        s->count++;
    }
    // Compositors like mutter default to server side decorations, which
    // would leave no titlebar to draw tabs into: force CSD.
    if (s->count > 0 && decs.serverSide) {
        decs.serverSide = false;
        if (window->wl.xdg.decoration) zxdg_toplevel_decoration_v1_set_mode(window->wl.xdg.decoration, ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE);
        csd_set_visible(window, csd_should_window_be_decorated(window));
    }
    // Use a taller titlebar so the 24px tabs are vertically centered with
    // breathing room, mirroring the macOS look. Metrics feed all the CSD
    // geometry calculations, so just updating them and forcing a rebuild is
    // enough.
    if (s->count > 0 && decs.metrics.visible_titlebar_height != TABS_TITLEBAR_HEIGHT) {
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
}
