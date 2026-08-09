/*
 * wl_titlebar_tabs.h
 * Fork-specific: native titlebar tabs for Wayland CSD, mirroring the macOS
 * implementation in kitty/cocoa_window.m.
 *
 * Distributed under terms of the GPL3 license.
 */

#pragma once

#include "internal.h"

// True when this window has titlebar tabs to draw (tab list is non-empty)
bool wl_titlebar_tabs_active(_GLFWwindow *window);
// True when the decorative frame (rounded corners + light inner border) should
// be drawn: tabs are active and the window is free-floating. Docked
// (maximized/fullscreen/tiled) windows are square and borderless.
bool wl_titlebar_tabs_rounded(_GLFWwindow *window);
// True while a titlebar tab drag holds the pointer (the implicit grab is
// alive even if pointer_button_count was zeroed by a same-client leave)
bool wl_titlebar_tabs_any_drag_active(void);
// Renders the whole bar (tabs, + button, compact window buttons, rounded top
// corners) into the titlebar buffer being rendered by render_title_bar().
void wl_titlebar_tabs_render_bar(_GLFWwindow *window, uint8_t *output, uint32_t bar_bg, uint32_t fg, uint32_t hover_bg, bool is_dark);
// Pointer handling. Returns true when the event was consumed by the tab bar.
bool wl_titlebar_tabs_handle_motion(_GLFWwindow *window, double x, double y);
bool wl_titlebar_tabs_handle_button(_GLFWwindow *window, uint32_t button, uint32_t state, double x, double y);
bool wl_titlebar_tabs_handle_leave(_GLFWwindow *window);
// Keeps an in-progress tab drag fed with events after the compositor moves
// pointer focus to another surface of the same window (kwin does this even
// with a button held). button < 0 means motion. Hooked in wl_init.c.
bool wl_titlebar_tabs_forward_grabbed_pointer(_GLFWwindow *window, int button, uint32_t state);
void wl_titlebar_tabs_free(_GLFWwindow *window);
// Small shadow-filled subsurfaces below the parent surface that show through
// the transparent notches left by the rounded corners. Update is called from
// ensure_csd_resources; destroy from free_csd_surfaces.
void wl_titlebar_tabs_update_corner_patches(_GLFWwindow *window);
void wl_titlebar_tabs_destroy_corner_patches(_GLFWwindow *window);
// Overwrites the shadow tile of a tabs window with the Chrome-style drop
// shadow (MD elevation-16 key+ambient gaussians, rounded-rect base). Called
// at the end
// of create_shadow_tile(); a no-op for windows without titlebar tabs.
void wl_titlebar_tabs_patch_shadow_tile(_GLFWwindow *window);
// While tabs are shown, keeps CSD buffers alive when the compositor releases
// them instead of letting buffer_release_event destroy them (which would force
// a full CSD buffer rebuild on the next redraw). Hooked in buffer_release_event.
bool wl_titlebar_tabs_retain_released_buffer(_GLFWwindow *window, struct wl_buffer *buffer);
// Temporary lag diagnostics (see AGENTS.md): three probes hooked into
// wl_window.c measuring the click -> frame-callback -> content-swap chain.
void wl_titlebar_tabs_note_frame_request(_GLFWwindow *window);
void wl_titlebar_tabs_note_frame_done(_GLFWwindow *window);
void wl_titlebar_tabs_note_swap(_GLFWwindow *window);
