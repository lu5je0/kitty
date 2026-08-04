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
// Renders the whole bar (tabs, + button, compact window buttons, rounded top
// corners) into the titlebar buffer being rendered by render_title_bar().
void wl_titlebar_tabs_render_bar(_GLFWwindow *window, uint8_t *output, uint32_t bar_bg, uint32_t fg, uint32_t hover_bg, bool is_dark);
// Pointer handling. Returns true when the event was consumed by the tab bar.
bool wl_titlebar_tabs_handle_motion(_GLFWwindow *window, double x, double y);
bool wl_titlebar_tabs_handle_button(_GLFWwindow *window, uint32_t button, uint32_t state, double x, double y);
bool wl_titlebar_tabs_handle_leave(_GLFWwindow *window);
void wl_titlebar_tabs_free(_GLFWwindow *window);
