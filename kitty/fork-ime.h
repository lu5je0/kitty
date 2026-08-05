/*
 * fork-ime.h
 *
 * Fork-local. See agents.md.
 *
 * Understands the "tui-bridge" OSC that this author's nvim and zsh configs
 * already emit, so those configs need no kitty-specific branch:
 *
 *   OSC 1337 ; SetUserVar=tui-bridge=<base64 of
 *       {"id":1,"module":"ime","method":"normal"|"insert","params":{}}> BEL
 *
 * method=normal disables the IME for the window, method=insert re-enables it.
 * This is the only channel; no DEC private mode is allocated, which is why
 * modes.h and set_mode_from_const stay identical to upstream.
 *
 * The flag lives on ScreenModes because that struct is already per kitty window
 * and is zeroed by do_screen_reset, so RIS clears the state for free.
 *
 * Include this after screen.h so Screen is visible, and after state.h for
 * update_ime_position_for_window.
 */

#pragma once

#include "base64.h"

/* Matches "<key>" : "<value>" in a flat JSON object. Deliberately not a real
 * parser: the only producers are the configs described above, which emit a
 * fixed shape. */
static inline bool
fork_json_str_field_is(const char *json, size_t sz, const char *key, const char *value) {
    const size_t klen = strlen(key), vlen = strlen(value);
    for (size_t p = 0; p + klen + 2 <= sz; p++) {
        if (json[p] != '"' || memcmp(json + p + 1, key, klen) != 0 || json[p + 1 + klen] != '"') continue;
        size_t q = p + klen + 2;
        while (q < sz && (json[q] == ' ' || json[q] == ':')) q++;
        if (q >= sz || json[q] != '"') return false;
        q++;
        return q + vlen < sz && memcmp(json + q, value, vlen) == 0 && json[q + vlen] == '"';
    }
    return false;
}

static inline void
fork_ime_set_disabled(Screen *screen, bool val) {
    if (screen->modes.mDISABLE_IME == val) return;
    screen->modes.mDISABLE_IME = val;
    // Discard any half-composed candidate when disabling, restore IME focus when re-enabling.
    // Purely cosmetic: the macOS key handler reads the flag live on every keypress.
    update_ime_position_for_window(screen->window_id, false, val ? -1 : 1);
}

/* Returns true when the payload was an IME command and has been applied, so the
 * caller can skip the regular OSC 1337 handling. */
static inline bool
fork_ime_handle_osc1337(Screen *screen, const char *payload, size_t sz) {
    static const char prefix[] = "SetUserVar=tui-bridge=";
    const size_t plen = sizeof(prefix) - 1;
    if (sz <= plen || memcmp(payload, prefix, plen) != 0) return false;

    uint8_t json[256];
    size_t json_sz = sizeof(json);
    if (required_buffer_size_for_base64_decode(sz - plen) > json_sz) return false;
    if (!base64_decode8((const uint8_t*)payload + plen, sz - plen, json, &json_sz)) return false;

    if (!fork_json_str_field_is((const char*)json, json_sz, "module", "ime")) return false;
    if (fork_json_str_field_is((const char*)json, json_sz, "method", "normal")) {
        fork_ime_set_disabled(screen, true); return true;
    }
    if (fork_json_str_field_is((const char*)json, json_sz, "method", "insert")) {
        fork_ime_set_disabled(screen, false); return true;
    }
    return false;
}

/* Screen.ime_disabled, so the state is observable from tests and from the debug
 * shell, and settable by the enable_ime action. Wired into screen.c's getsetters. */
static inline PyObject*
ime_disabled_get(Screen *self, void *closure UNUSED) {
    PyObject *ans = self->modes.mDISABLE_IME ? Py_True : Py_False;
    Py_INCREF(ans);
    return ans;
}

static inline int
ime_disabled_set(Screen *self, PyObject *val, void *closure UNUSED) {
    if (!val) { PyErr_SetString(PyExc_TypeError, "cannot delete ime_disabled"); return -1; }
    const int is_true = PyObject_IsTrue(val);
    if (is_true < 0) return -1;
    fork_ime_set_disabled(self, is_true == 1);
    return 0;
}
