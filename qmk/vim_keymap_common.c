// Copyright 2026 qmk-vim-fn
// SPDX-License-Identifier: GPL-2.0-or-later
//
// vim_keymap_common.c — shared keymap layer for the qmk-vim-fn engine.
// Design authority: qmk-vim-fn/vim/design.md §2.1, §4.9, §4.10, §4.12.
//
// vim_pipeline_process() is the single-source interception chain; every
// keyboard feeds process_record_user() into it.  The steps are explicitly
// named and ordered (design §4.12):
//
//   0  modifier shadow update (before anything may swallow a modifier)
//   1  cfg->hook_pre
//   2  myfn skeleton (layer exemption / undeclared swallow / dispatch)
//   3  cfg->hook_post_myfn
//   4  mouse-mode state machine
//   5  Shift+Esc (Insert only)
//   6  Esc toggle (Insert <-> Normal, with the escape grace window)
//   7  Caps tap/hold (tap = vim on/off toggle, hold = momentary Normal)
//   8  §2.1 shortcut table
//   9  vim_glue_engine (Esc falls straight through to the engine)

#include "qmk-vim-fn/qmk/vim_keymap_common.h"
#include "qmk-vim-fn/qmk/vim_glue.h"

// ==========================================================================
// Shared helpers
// ==========================================================================
uint16_t vim_timer_start(void) {
    uint16_t t = timer_read();
    return t ? t : 1; // guard against a zero reading disabling the timer
}

bool vim_timer_elapsed(uint16_t start, uint16_t ms) {
    return start != 0 && timer_elapsed(start) >= ms;
}

// 32-bit stamp/compare for windows that may go unchecked across the 16-bit wrap:
// QMK's timer_read() is (uint16_t)timer_read32(), so an *expired* window re-reads as
// "elapsed" again after 65536 ms (design §4.12: Esc grace window).
uint32_t vim_timer_start32(void) {
    uint32_t t = timer_read32();
    return t ? t : 1; // same zero-reading guard as the 16-bit helper
}

bool vim_timer_elapsed32(uint32_t start, uint32_t ms) {
    return start != 0 && timer_elapsed32(start) >= ms;
}

// Real QMK (quantum/keycodes.h) always provides these; the glue-test host stub
// may not, so keep the shared layer compilable against both.
#ifndef IS_QK_TO
#define IS_QK_TO(kc) false
#endif
#ifndef IS_QK_TOGGLE_LAYER
#define IS_QK_TOGGLE_LAYER(kc) false
#endif
#ifndef IS_QK_DEF_LAYER
#define IS_QK_DEF_LAYER(kc) false
#endif

bool vim_is_layer_key(uint16_t keycode) {
    return IS_QK_MOMENTARY(keycode) || IS_QK_LAYER_TAP(keycode) || IS_QK_LAYER_MOD(keycode) ||
           IS_QK_LAYER_TAP_TOGGLE(keycode) || IS_QK_ONE_SHOT_LAYER(keycode) ||
           IS_QK_TO(keycode) || IS_QK_TOGGLE_LAYER(keycode) || IS_QK_DEF_LAYER(keycode);
}

void send_plain_tap(uint16_t keycode) {
    uint8_t saved = get_mods();
    clear_mods();
    tap_code16(keycode);
    set_mods(saved);
}

// ==========================================================================
// Step 7 — §2.1 keyboard-layer shortcut table
// ==========================================================================
static void sc_left(void) { send_plain_tap(KC_LEFT); }
static void sc_right(void) { send_plain_tap(KC_RGHT); }
static void sc_up_home(void) {
    send_plain_tap(KC_UP);
    send_plain_tap(KC_HOME);
}
static void sc_down_home(void) {
    send_plain_tap(KC_DOWN);
    send_plain_tap(KC_HOME);
}
static void sc_find(void) { send_plain_tap(LCTL(KC_F)); }
static void sc_pgdn(void) { send_plain_tap(KC_PGDN); }
static void sc_pgup(void) { send_plain_tap(KC_PGUP); }

#define VIM_NO_CAG_MASK (MOD_MASK_CTRL | MOD_MASK_ALT | MOD_MASK_GUI)

const vim_shortcut_t vim_default_shortcuts[] = {
    {KC_BSPC, 0, VIM_NO_CAG_MASK, sc_left},
    {KC_SPC, 0, VIM_NO_CAG_MASK, sc_right},
    {KC_MINS, 0, VIM_NO_CAG_MASK | MOD_MASK_SHIFT, sc_up_home},
    {KC_EQL, MOD_MASK_SHIFT, VIM_NO_CAG_MASK | MOD_MASK_SHIFT, sc_down_home},
    {KC_SLSH, 0, VIM_NO_CAG_MASK | MOD_MASK_SHIFT, sc_find},
    {KC_F, MOD_MASK_CTRL, VIM_NO_CAG_MASK, sc_pgdn},
    {KC_B, MOD_MASK_CTRL, VIM_NO_CAG_MASK, sc_pgup},
    {0, 0, 0, NULL}, // terminator
};

// ==========================================================================
// Step 4 — mouse-mode state machine (design §4.9)
// ==========================================================================
static const vim_cfg_t *s_cfg;

static kv_mode_t s_mouse_entry_mode;
static uint16_t s_mouse_timer;  // trigger-key hold timer
static bool     s_mouse_held;   // trigger long-press -> modifier registered
static uint16_t s_mouse_mod_reg; // exact modifier keycode actually registered
static uint16_t s_lbtn_timer;   // Space hold timer
static bool     s_lbtn_held;    // Space long-press -> left button held

// Remember the keycode actually registered per movement key, so the release
// unregisters exactly that one even if Shift changes mid-hold (P0-2).
enum { MV_H = 0, MV_J, MV_K, MV_L, MV_COUNT };
static uint16_t s_move_reg[MV_COUNT];

static bool fn_layer_active(void) {
    return layer_state_cmp(layer_state | default_layer_state, s_cfg->fn_layer);
}

static uint16_t mouse_active_mod(void) {
    if (s_cfg->is_mac && s_cfg->is_mac()) return s_cfg->mod_mac;
    return s_cfg->mod_win;
}

static bool mouse_link_ok(void) { return s_cfg->link_ok ? s_cfg->link_ok() : true; }

// Single source of truth: MOUSE liveness is exactly the engine mode (A-P1-6).
static bool mouse_active(void) { return kv_get_mode() == KV_MODE_MOUSE; }

static void mouse_release_all(void) {
    for (int i = 0; i < MV_COUNT; i++) {
        if (s_move_reg[i]) {
            unregister_code(s_move_reg[i]);
            s_move_reg[i] = KC_NO;
        }
    }
    unregister_code(MS_BTN1); // harmless if it was only tapped
    unregister_code(MS_BTN2);
    s_lbtn_held  = false;
    s_lbtn_timer = 0;
}

static void mouse_enter(void) {
    if (!kv_vim_enabled()) return; // vim off: never enter MOUSE (design §4.9)
    s_mouse_entry_mode = kv_get_mode();
    vim_glue_release_all(); // held motion arrows must not stick (design §4.10)
    kv_set_mode(KV_MODE_MOUSE);
}

static void mouse_exit(void) {
    mouse_release_all();
    kv_set_mode(s_mouse_entry_mode); // back to the entry mode
    vim_glue_release_all();
}

// Returns true when the event is consumed by the mouse state machine.
static bool mouse_process(uint16_t keycode, keyrecord_t *record) {
    const bool pressed = record->event.pressed;

    // Trigger key: tap toggles mouse mode, hold = Win/Mac modifier.
    if (keycode == s_cfg->trigger_kc) {
        if (pressed) {
            s_mouse_timer = vim_timer_start();
            s_mouse_held  = false;
        } else {
            if (s_mouse_held) {
                // Unregister the exact keycode registered on long-press; is_mac()
                // may have flipped mid-hold (P2), so never recompute it here.
                if (s_mouse_mod_reg) unregister_code(s_mouse_mod_reg);
                s_mouse_mod_reg = KC_NO;
            } else if (s_mouse_timer) {
                if (mouse_active()) {
                    mouse_exit();
                } else if (mouse_link_ok()) {
                    mouse_enter();
                }
            }
            s_mouse_timer = 0;
            s_mouse_held  = false;
        }
        return true;
    }

    if (!mouse_active()) return false;

    // Modifier handling (design §4.9).  Shift stays in MOUSE so that Shift+J /
    // Shift+K can produce wheel-down / wheel-up; its press is paired (consumed)
    // and its release falls through so a Shift QMK registered before entering
    // MOUSE can still be unregistered.  Ctrl/Alt/GUI exit MOUSE on press and
    // are re-identified in the entry mode (the pipeline continues, so QMK
    // registers the modifier normally); their release then passes through.
    if (IS_MODIFIER_KEYCODE(keycode)) {
        if (pressed) {
            if (keycode == KC_LSFT || keycode == KC_RSFT) {
                vim_glue_swallow(keycode);
                return true;
            }
            mouse_exit(); // force-release mouse keys/pointer/wheel first
            return false; // re-identify the modifier in the entry mode
        }
        return false;
    }

    const bool shift = (vim_glue_mods() & MOD_MASK_SHIFT) != 0;

    if (pressed) {
        switch (keycode) {
            case KC_H:
                s_move_reg[MV_H] = MS_LEFT;
                register_code(MS_LEFT);
                vim_glue_swallow(keycode);
                return true;
            case KC_J:
                s_move_reg[MV_J] = shift ? MS_WHLD : MS_DOWN;
                register_code(s_move_reg[MV_J]);
                vim_glue_swallow(keycode);
                return true;
            case KC_K:
                s_move_reg[MV_K] = shift ? MS_WHLU : MS_UP;
                register_code(s_move_reg[MV_K]);
                vim_glue_swallow(keycode);
                return true;
            case KC_L:
                s_move_reg[MV_L] = MS_RGHT;
                register_code(MS_RGHT);
                vim_glue_swallow(keycode);
                return true;
            case KC_SPC:
                s_lbtn_timer = vim_timer_start();
                vim_glue_swallow(keycode);
                return true;
            case KC_ENT:
                register_code(MS_BTN2);
                vim_glue_swallow(keycode);
                return true;
            default:
                // Any other key leaves mouse mode; force-release every mouse
                // key and re-identify the key by continuing the pipeline.
                mouse_exit();
                return false;
        }
    }

    // Release: unregister the host mouse code, then do NOT consume; step 8's
    // shared pairing table consumes the paired release (design §4.10/§4.12).
    switch (keycode) {
        case KC_H:
            if (s_move_reg[MV_H]) {
                unregister_code(s_move_reg[MV_H]);
                s_move_reg[MV_H] = KC_NO;
            }
            return false;
        case KC_J:
            if (s_move_reg[MV_J]) {
                unregister_code(s_move_reg[MV_J]);
                s_move_reg[MV_J] = KC_NO;
            }
            return false;
        case KC_K:
            if (s_move_reg[MV_K]) {
                unregister_code(s_move_reg[MV_K]);
                s_move_reg[MV_K] = KC_NO;
            }
            return false;
        case KC_L:
            if (s_move_reg[MV_L]) {
                unregister_code(s_move_reg[MV_L]);
                s_move_reg[MV_L] = KC_NO;
            }
            return false;
        case KC_SPC:
            if (s_lbtn_held) {
                unregister_code(MS_BTN1);
                s_lbtn_held = false;
            } else if (s_lbtn_timer) {
                tap_code(MS_BTN1);
            }
            s_lbtn_timer = 0;
            return false;
        case KC_ENT:
            unregister_code(MS_BTN2);
            return false;
        default:
            return false;
    }
}

// ==========================================================================
// Step 5 — Shift+Esc (Insert only, design §4.10 / readme §4)
// ==========================================================================
static bool shift_esc_process(uint16_t keycode, keyrecord_t *record) {
    if (!kv_vim_enabled()) return false; // vim off: never hijack Shift+Esc
    if (!s_cfg->shift_esc_enable) return false;
    if (keycode != KC_ESC || !record->event.pressed) return false;
    if (kv_get_mode() != KV_MODE_INSERT) return false;

    uint8_t mods = vim_glue_mods();
    if (mods & (MOD_MASK_CTRL | MOD_MASK_ALT | MOD_MASK_GUI)) return false;

    if (mods & MOD_BIT_LSHIFT) {
        send_plain_tap(LSFT(KC_GRV)); // ~
        vim_glue_swallow(keycode);
        return true;
    }
    if (mods & MOD_BIT_RSHIFT) {
        send_plain_tap(KC_GRV); // `
        vim_glue_swallow(keycode);
        return true;
    }
    return false;
}

// ==========================================================================
// Step 6 — Esc toggle (Insert <-> Normal) + escape grace window
// ==========================================================================
//
// With vim on, Esc toggles typing <-> command:
//   Insert  -> swallow the Esc and drop into NORMAL (no host Esc);
//   Normal  -> emit the real Esc, return to INSERT, and open a short grace
//              window so a burst of Escapes (e.g. leaving a shell prompt)
//              stays a real Esc instead of re-entering NORMAL.
// The window is 3 s, is opened ONLY by this Normal -> Insert transition, and
// is reset by every in-window Esc.  Visual / pending-Normal / CAG Escapes are
// left to the engine and shortcut layers, unchanged.
#define VIM_ESC_GRACE_MS 3000
static uint32_t s_esc_grace; // 0 = no window; else vim_timer_start32() stamp (32-bit: no wrap)

static bool esc_process(uint16_t keycode, keyrecord_t *record) {
    if (keycode != KC_ESC || !record->event.pressed) return false;
    if (!kv_vim_enabled()) return false; // vim off: plain Esc

    uint8_t mods = vim_glue_mods();
    if (mods & (MOD_MASK_CTRL | MOD_MASK_ALT | MOD_MASK_GUI)) return false; // CAG
    kv_mode_t m = kv_get_mode();

    // Visual: the engine exits to NORMAL and emits nothing.
    if (m == KV_MODE_VISUAL || m == KV_MODE_VISUAL_LINE) return false;

    // NORMAL with a pending prefix/operator: the engine cancels it (no key).
    if (m == KV_MODE_NORMAL && kv_pending()) return false;

    if (m == KV_MODE_NORMAL) {
        // Normal idle: real Esc, back to typing, open the grace window.
        kv_set_mode(KV_MODE_INSERT);
        s_esc_grace = vim_timer_start32();
        return false; // pass -> host receives the real Esc
    }

    // INSERT.
    if (s_esc_grace && !vim_timer_elapsed32(s_esc_grace, VIM_ESC_GRACE_MS)) {
        s_esc_grace = vim_timer_start32(); // in-window Esc: real Esc, reset window
        return false;
    }
    // No window (entered Insert another way) or it expired: swallow, go NORMAL.
    s_esc_grace = 0;
    kv_cancel();
    kv_set_mode(KV_MODE_NORMAL);
    vim_glue_swallow(KC_ESC);
    return true;
}

// ==========================================================================
// Step 7 — Caps tap/hold
// ==========================================================================
static uint16_t  s_caps_timer;
static kv_mode_t s_caps_entry_mode; // full entry mode, restored on long press

static void set_vim_enabled(bool enabled) {
    s_esc_grace = 0; // an enable/disable transition invalidates the window
    if (s_cfg->vim_set_enabled) {
        s_cfg->vim_set_enabled(enabled);
    } else if (enabled) {
        kv_enable(); // always restarts in INSERT (design §4.7)
    } else {
        kv_disable();
    }
    // Mode/enable transition: no held arrow may stick (both paths).
    vim_glue_release_all();
}

static bool caps_process(uint16_t keycode, keyrecord_t *record) {
    if (keycode != KC_CAPS) return false;

    if (record->event.pressed) {
        // Caps is always owned (it toggles vim), so it never works as Caps Lock.
        // While vim is on we momentarily enter NORMAL so a long press previews
        // command mode and restores the entry mode on release.
        s_caps_entry_mode = kv_get_mode();
        s_caps_timer      = vim_timer_start();
        if (kv_vim_enabled()) {
            kv_set_mode(KV_MODE_NORMAL);
            vim_glue_release_all();
        }
        vim_glue_swallow(KC_CAPS);
        return true;
    }

    if (s_caps_timer) {
        bool held    = vim_timer_elapsed(s_caps_timer, s_cfg->hold_ms);
        s_caps_timer = 0;
        if (held) {
            // Long press: momentary NORMAL, restore the exact entry mode.
            if (kv_vim_enabled()) {
                kv_set_mode(s_caps_entry_mode);
                vim_glue_release_all();
            }
        } else {
            // Tap: toggle vim (enabling always restarts in INSERT).
            set_vim_enabled(!kv_vim_enabled());
        }
    }
    return false; // paired release consumed by the shared table
}

// ==========================================================================
// Step 2 — myfn skeleton (fn readme §3)
// ==========================================================================
// Returns true when the key is consumed here.
//
// Declared keys are passed back to QMK unchanged (design §4.12 "已声明放行/
// 分发"): cfg->myfn() still runs on both edges for any per-key keyboard action,
// but ownership stays with QMK, so later pipeline steps and any keyboard's
// process_record_kb tail (e.g. other vendor keys) still see the
// key.  Undeclared keys (incl. modifiers) are swallowed on press
// (fn readme rule 3).
static bool myfn_process(uint16_t keycode, keyrecord_t *record) {
    if (vim_is_layer_key(keycode)) return false; // exempt: always pass to QMK

    if (!fn_layer_active()) return false;

    bool declared = s_cfg->myfn_declared && s_cfg->myfn_declared(keycode);

    if (!declared) {
        // Undeclared keys (incl. modifiers) are swallowed on press to keep
        // Fn+<mod>+<key> from leaking.  The press is registered in the shared
        // pairing table; the release is left to the pipeline so QMK can
        // unregister a modifier it registered before Fn went down (a swallowed
        // press is never registered, so its paired release is harmless).  This
        // fixes "Shift down -> Fn -> Shift up" leaving Shift stuck.
        if (record->event.pressed) {
            vim_glue_swallow(keycode);
            return true;
        }
        return false;
    }

    // Declared key: the keyboard decides per edge.  Returning true consumes
    // it (paired release swallowed via the shared table); false passes it to
    // QMK (e.g. F-keys, or vendor keys handled in the process_record_kb tail).
    bool consume = s_cfg->myfn ? s_cfg->myfn(keycode, record->event.pressed) : false;
    if (consume) {
        if (record->event.pressed) {
            vim_glue_swallow(keycode);
            return true;
        }
        return false; // release consumed by step 8's pairing table
    }
    return false; // pass to QMK
}

// ==========================================================================
// Step 7 — shortcut dispatch
// ==========================================================================
static bool shortcuts_process(uint16_t keycode, keyrecord_t *record) {
    if (!record->event.pressed) return false;
    if (!kv_vim_enabled() || kv_get_mode() != KV_MODE_NORMAL) return false;
    if (!s_cfg->shortcuts) return false;

    uint8_t mods = vim_glue_mods(); // physical shadow, not get_mods() (design §4.10)
    for (const vim_shortcut_t *s = s_cfg->shortcuts; s->base; s++) {
        if (keycode != s->base) continue;
        // `mods_req` names a *side-agnostic* mask (e.g. MOD_MASK_CTRL = LCTL|RCTL),
        // so compare on the masked, physically-held set: no requirement means no
        // masked modifier may be down, otherwise the held set must be a non-empty
        // subset of the requirement (either Ctrl / either Shift qualifies).
        uint8_t held = (uint8_t)(mods & s->mods_mask);
        bool    hit  = (s->mods_req == 0)
                           ? (held == 0)
                           : ((held & s->mods_req) != 0 && (held & (uint8_t)~s->mods_req) == 0);
        if (hit) {
            kv_cancel(); // drop any half-typed command first
            s->action();
            vim_glue_swallow(keycode); // consume the matching release too
            return true;
        }
    }
    return false;
}

// ==========================================================================
// Public pipeline / task / RGB
// ==========================================================================
// Keyboard hooks may consume a press; the shared layer then owns its release
// (design §4.10) and pairs it automatically, so a hook never needs to track
// key-up itself.  A hook's claim to consume a *release* is deliberately
// ignored: releases are always governed by the shared pairing table, so a
// stale release predicate (e.g. a keyboard hook checking get_mods()) can never
// strand a host key.
static bool hook_process(uint16_t keycode, keyrecord_t *record, bool (*hook)(uint16_t, keyrecord_t *)) {
    if (!hook) return false;
    if (!hook(keycode, record)) return false;
    if (record->event.pressed) {
        vim_glue_swallow(keycode);
        return true;
    }
    return false; // release: fall through to the shared pairing table
}

static bool vim_dispatch(uint16_t keycode, keyrecord_t *record, const vim_cfg_t *cfg) {
    // 0 — physical modifier shadow (must precede every swallow).
    vim_glue_mod_update(keycode, record->event.pressed);

    // 1 — keyboard pre-hook (high-priority keyboard combos).
    if (hook_process(keycode, record, cfg->hook_pre)) return false;

    // 2 — myfn skeleton.
    if (myfn_process(keycode, record)) return false;

    // 3 — keyboard post-myfn hook (per-key keyboard actions).
    if (hook_process(keycode, record, cfg->hook_post_myfn)) return false;

    // 4 — mouse mode.
    if (mouse_process(keycode, record)) return false;

    // 5 — Shift+Esc.
    if (shift_esc_process(keycode, record)) return false;

    // 6 — Esc toggle (opens/resets the escape grace window on Normal->Insert).
    if (esc_process(keycode, record)) return false;

    // 7 — Caps tap/hold.
    if (caps_process(keycode, record)) return false;

    // 8 — §2.1 shortcuts.
    if (shortcuts_process(keycode, record)) return false;

    // 9 — engine (Esc falls straight through here; no keyboard Esc branch).
    return vim_glue_engine(keycode, record);
}

bool vim_pipeline_process(uint16_t keycode, keyrecord_t *record, const vim_cfg_t *cfg) {
    s_cfg = cfg;

    // The escape grace window only exists while typing; any key observed outside
    // INSERT invalidates it (esc_process re-opens it on Normal->Insert).
    if (kv_get_mode() != KV_MODE_INSERT) s_esc_grace = 0;

    return vim_dispatch(keycode, record, cfg);
}

void vim_keymap_common_init(void) {
    // Reset the shared-layer static state.  vim_glue_init() resets the engine +
    // glue; the rest is owned here (mouse FSM, Caps tap/hold, escape grace).
    s_cfg             = NULL;
    s_mouse_entry_mode = KV_MODE_INSERT;
    s_mouse_timer      = 0;
    s_mouse_held       = false;
    s_mouse_mod_reg    = KC_NO;
    s_lbtn_timer       = 0;
    s_lbtn_held        = false;
    for (int i = 0; i < MV_COUNT; i++) s_move_reg[i] = KC_NO;
    s_caps_timer       = 0;
    s_caps_entry_mode  = KV_MODE_INSERT;
    s_esc_grace        = 0;
    vim_glue_init();
}

void vim_keymap_common_task(uint32_t now_ms) {
    vim_glue_task(now_ms);

    if (!s_cfg) return;

    // Trigger-key long press: register the Win/Mac modifier and remember the
    // exact code so the release unregisters the same one even if is_mac() flips.
    if (s_mouse_timer && !s_mouse_held && vim_timer_elapsed(s_mouse_timer, s_cfg->hold_ms)) {
        s_mouse_held    = true;
        s_mouse_mod_reg = mouse_active_mod();
        register_code(s_mouse_mod_reg);
    }

    // Space long press inside mouse mode: hold the left button (drag).
    if (mouse_active() && s_lbtn_timer && !s_lbtn_held && vim_timer_elapsed(s_lbtn_timer, s_cfg->hold_ms)) {
        s_lbtn_held = true;
        register_code(MS_BTN1);
    }
}

uint16_t vim_rgb_led_index(void) { return s_cfg ? s_cfg->led_index : 0; }

void vim_rgb_state_color(bool enabled, kv_mode_t m, bool pending, bool mouse, uint8_t *r, uint8_t *g, uint8_t *b) {    // MOUSE cyan wins over the "vim off" red (design §4.9).
    if (mouse) {
        *r = 0x00; *g = 0xFF; *b = 0xFF; // cyan: mouse mode
        return;
    }
    if (!enabled) {
        *r = 0xFF; *g = 0x00; *b = 0x00; // red: vim off
        return;
    }
    switch (m) {
        case KV_MODE_VISUAL:
        case KV_MODE_VISUAL_LINE:
            // pending never overrides Visual
            *r = 0x80; *g = 0x00; *b = 0x80; // purple
            break;
        case KV_MODE_NORMAL:
            if (pending) {
                *r = 0xFF; *g = 0xFF; *b = 0x00; // yellow: pending
            } else {
                *r = 0x00; *g = 0x00; *b = 0xFF; // blue
            }
            break;
        case KV_MODE_INSERT:
        default:
            *r = 0x00; *g = 0xFF; *b = 0x00; // green
            break;
    }
}

bool vim_insert_flash(void) {
    // Design §4.12: true iff vim on + mode INSERT + the Esc grace window is still
    // open.  That window is opened by exactly one event — esc_process()'s
    // "idle-Normal Esc -> INSERT" — is restarted by an in-window Esc, and is
    // dropped by vim_pipeline_process() as soon as the mode leaves INSERT.  So it
    // is precisely "this INSERT came from an idle-Normal Esc, less than 3 s ago".
    if (!kv_vim_enabled()) return false;             // vim off: mode colour is red
    if (kv_get_mode() != KV_MODE_INSERT) return false;
    return s_esc_grace != 0 && !vim_timer_elapsed32(s_esc_grace, VIM_ESC_GRACE_MS);
}

bool vim_insert_flash_color(uint8_t *r, uint8_t *g, uint8_t *b) {
    // design §4.12: the shared layer owns the whole contract — predicate AND the
    // cfg->insert_flash_color decision (0 = do not override).  The keyboard only
    // supplies the colour value and decides which LEDs to repaint.
    if (!vim_insert_flash()) return false;
    if (!s_cfg || s_cfg->insert_flash_color == 0) return false;
    if (r) *r = (uint8_t)((s_cfg->insert_flash_color >> 16) & 0xFF);
    if (g) *g = (uint8_t)((s_cfg->insert_flash_color >> 8) & 0xFF);
    if (b) *b = (uint8_t)(s_cfg->insert_flash_color & 0xFF);
    return true;
}
