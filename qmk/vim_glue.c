// Copyright 2026 qk61-vim
// SPDX-License-Identifier: GPL-2.0-or-later
//
// vim_glue.c — QMK adapter for the QMK-agnostic qmk-vim-fn engine.
// Design authority: qmk-vim-fn/vim/design.md §4.7 (API), §4.10 (key-up /
// modifiers / held motion), §4.12 (glue layer responsibilities).

#include "qmk-vim-fn/qmk/vim_glue.h"
#include "qmk-vim-fn/engine/include/kv.h"

// --------------------------------------------------------------------------
// Physical modifier shadow (design §4.10 / §4.12 #4).
//
// Records every physical modifier down/up independently of get_mods(), so it
// survives myfn swallowing a modifier and is immune to oneshot / locked mods.
//
// Known exception: the QK61 vendor layer rewrites `record->event.pressed` to
// false before forwarding to process_record_user (Win-lock for LGUI/RGUI/APP,
// and EE_CLR).  The shadow faithfully records what it is given, so while Win
// lock is on those modifier edges are intentionally not seen.
// --------------------------------------------------------------------------
static uint8_t s_shadow;

static uint8_t shadow_bit_of(uint16_t keycode) {
    switch (keycode) {
        case KC_LCTL: return MOD_BIT_LCTRL;
        case KC_LSFT: return MOD_BIT_LSHIFT;
        case KC_LALT: return MOD_BIT_LALT;
        case KC_LGUI: return MOD_BIT_LGUI;
        case KC_RCTL: return MOD_BIT_RCTRL;
        case KC_RSFT: return MOD_BIT_RSHIFT;
        case KC_RALT: return MOD_BIT_RALT;
        case KC_RGUI: return MOD_BIT_RGUI;
        default:      return 0;
    }
}

void vim_glue_mod_update(uint16_t keycode, bool pressed) {
    uint8_t bit = shadow_bit_of(keycode);
    if (!bit) return;
    if (pressed) {
        s_shadow |= bit;
    } else {
        s_shadow &= (uint8_t)~bit;
    }
}

uint8_t vim_glue_mods(void) { return s_shadow; }

// --------------------------------------------------------------------------
// Held-motion exception (design §4.8 / §4.10).
//
// A *bare* h/j/k/l — the first key of a fresh command, no pending count /
// operator / prefix and no Ctrl/Alt/GUI — may register-hold the host arrow so
// the host auto-repeats, and unregister on key-up.  A single "expected" slot
// records which arrow the next emit is allowed to hold: `3l` / `dl` (pending)
// tap normally, and an arrow emitted by an unrelated key (e.g. `a` -> KV_RGHT
// while `l` is held) is not mistaken for held motion.
//
// Only the NORMAL-mode bare motion is covered here.  Visual's held motion
// would have to hold the Shift+arrow variant (design §4.9), which needs a
// different fold/emit path and is intentionally out of scope for now.
// --------------------------------------------------------------------------
static const uint16_t s_motion_kc[4] = {KC_H, KC_J, KC_K, KC_L};
static const uint16_t s_arrow_kc[4]  = {KC_LEFT, KC_DOWN, KC_UP, KC_RIGHT};
static bool           s_held_expect[4]; // one-shot: next emit of this arrow may hold
static bool           s_arrow_reg[4];   // arrow currently register-held

static int motion_index(uint16_t keycode) {
    for (int i = 0; i < 4; i++) {
        if (keycode == s_motion_kc[i]) return i;
    }
    return -1;
}

// The engine emits HOST ARROWS (KV_LEFT/DOWN/UP/RGHT), not the physical
// h/j/k/l letters, so vim_emit() must index the held-motion table by arrow.
static int arrow_index(uint16_t keycode) {
    for (int i = 0; i < 4; i++) {
        if (keycode == s_arrow_kc[i]) return i;
    }
    return -1;
}

void vim_glue_release_all(void) {
    for (int i = 0; i < 4; i++) {
        if (s_arrow_reg[i]) {
            unregister_code(s_arrow_kc[i]);
            s_arrow_reg[i] = false;
        }
        s_held_expect[i] = false; // a mode switch cancels the pending hold too
    }
}

// --------------------------------------------------------------------------
// Unified press/release pairing table (design §4.10 / §4.12 #2).
//
// A press consumed by the engine (KV_CONSUMED) or by the keyboard layer
// (vim_glue_swallow) records the keycode here; the matching release is then
// consumed instead of leaking an orphan key-up.  When the table is full the
// oldest entry is overwritten (new key wins).
// --------------------------------------------------------------------------
#define PAIR_CAP 16
static uint16_t s_pair[PAIR_CAP];
static uint8_t  s_pair_n;

static void pair_add(uint16_t keycode) {
    for (uint8_t i = 0; i < s_pair_n; i++) {
        if (s_pair[i] == keycode) return; // idempotent: a key is down at most once
    }
    if (s_pair_n < PAIR_CAP) {
        s_pair[s_pair_n++] = keycode;
        return;
    }
    for (uint8_t i = 1; i < PAIR_CAP; i++) s_pair[i - 1] = s_pair[i];
    s_pair[PAIR_CAP - 1] = keycode;
}

static bool pair_take(uint16_t keycode) {
    for (uint8_t i = 0; i < s_pair_n; i++) {
        if (s_pair[i] == keycode) {
            s_pair[i] = s_pair[--s_pair_n];
            return true;
        }
    }
    return false;
}

void vim_glue_swallow(uint16_t keycode) { pair_add(keycode); }

// --------------------------------------------------------------------------
// Emit callback (design §4.12 #5): the engine hands us a host keycode with
// packed 5-bit modifier bits (bits 8..12).  Register the modifiers as real
// keys around the tap; never write back the modifier report (E2).
// --------------------------------------------------------------------------
static uint8_t packed_mods_to_hid(uint8_t m) {
    uint8_t out = 0;
    if (m & 0x10) { // right-side flag: 0x11 = RCTL, never also LCTL
        switch (m & 0x0F) {
            case 0x01: out |= MOD_BIT_RCTRL; break;
            case 0x02: out |= MOD_BIT_RSHIFT; break;
            case 0x04: out |= MOD_BIT_RALT; break;
            case 0x08: out |= MOD_BIT_RGUI; break;
            default: break;
        }
    } else {
        if (m & 0x01) out |= MOD_BIT_LCTRL;
        if (m & 0x02) out |= MOD_BIT_LSHIFT;
        if (m & 0x04) out |= MOD_BIT_LALT;
        if (m & 0x08) out |= MOD_BIT_LGUI;
    }
    return out;
}

static void vim_emit(kv_keycode_t kc) {
    uint16_t basic = (uint16_t)(kc & 0x00FF);
    uint8_t  mods  = packed_mods_to_hid((uint8_t)((kc >> 8) & 0x1F));

    // Held motion: only an emit that a bare h/j/k/l explicitly announced via
    // the expected slot may register-hold the arrow instead of tapping it.
    int ai = arrow_index(basic);
    if (ai >= 0 && mods == 0 && s_held_expect[ai]) {
        s_held_expect[ai] = false;
        if (!s_arrow_reg[ai]) {
            register_code(s_arrow_kc[ai]);
            s_arrow_reg[ai] = true;
        }
        return;
    }

    // Register only the modifier bits that are not already held, then remove
    // exactly those again.  This never writes back the modifier report and
    // never drops a physically held modifier (design §4.10 / E2).
    uint8_t add = (uint8_t)(mods & ~get_mods());
    if (add) register_mods(add);
    register_code(basic);
    unregister_code(basic);
    if (add) unregister_mods(add);
}

// --------------------------------------------------------------------------
// Lifecycle
// --------------------------------------------------------------------------
void vim_glue_init(void) {
    kv_init();
    kv_set_emit(vim_emit);
    kv_enable();
    kv_set_mode(KV_MODE_INSERT); // start typing

    s_shadow  = 0;
    s_pair_n  = 0;
    for (int i = 0; i < 4; i++) {
        s_held_expect[i] = false;
        s_arrow_reg[i]   = false;
    }
}

void vim_glue_task(uint32_t now_ms) { kv_task(now_ms); }

// --------------------------------------------------------------------------
// Engine dispatch tail (pipeline step 8).
// --------------------------------------------------------------------------
bool vim_glue_engine(uint16_t keycode, keyrecord_t *record) {
    int mi = motion_index(keycode);

    if (!record->event.pressed) {
        // key-up: pass through unless the matching press was consumed.  The
        // held-motion arrow is unregistered here (design §4.10).
        if (mi >= 0) {
            s_held_expect[mi] = false;
            if (s_arrow_reg[mi]) {
                unregister_code(s_arrow_kc[mi]);
                s_arrow_reg[mi] = false;
            }
        }
        return !pair_take(keycode);
    }

    if (!kv_vim_enabled()) return true;

    uint8_t m = vim_glue_mods();
    if (m & (MOD_MASK_CTRL | MOD_MASK_ALT | MOD_MASK_GUI)) return true; // CAG: QMK handles it

    // Snapshot pending *before* the engine consumes this key.  Only a h/j/k/l
    // that starts a fresh command (nothing pending) is a bare held motion; a
    // h/j/k/l completing `3l` / `dl` must tap.  CAG is already excluded above.
    bool was_pending = kv_pending();

    kv_keycode_t kc = (kv_keycode_t)keycode;
    if (m & MOD_MASK_SHIFT) kc |= KV_MOD_LSFT;

    if (kv_kbd(kc) == KV_CONSUMED) {
        pair_add(keycode);
        if (mi >= 0 && !was_pending) {
            s_held_expect[mi] = true; // vim_emit() may now register-hold
        }
        return false;
    }
    return true;
}
