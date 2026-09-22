// Copyright 2026 qk61-vim
// SPDX-License-Identifier: GPL-2.0-or-later
//
// vim_keymap_common.h — shared keymap layer for the qmk-vim-fn engine.
// Design authority: qmk-vim-fn/vim/design.md §4.9 (mouse mode), §4.10
// (key-up / modifiers), §4.12 (single-source vim_pipeline_process chain).
//
// All keyboards share this file.  Keyboard-specific bits (RGB LED index,
// vendor combos, physical key definitions, VIA) stay in the keymap.

#pragma once

#include <stdint.h>
#include <stdbool.h>

#include QMK_KEYBOARD_H
#include "qmk-vim-fn/engine/include/kv.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A §2.1 keyboard-layer shortcut: matched on the base keycode plus a subset of
 * the held modifiers, then emitted with the physical modifiers stripped.
 * (get_mods() & mods_mask) == mods_req. */
typedef void (*vim_shortcut_fn)(void);

typedef struct {
    uint16_t        base;      /* base QMK keycode; 0 terminates the table */
    uint8_t         mods_req;  /* modifier bits that must be held */
    uint8_t         mods_mask; /* modifier bits compared against mods_req */
    vim_shortcut_fn action;
} vim_shortcut_t;

/* Per-keyboard configuration for the shared layer.  Either of the boolean
 * callbacks may be NULL (link_ok -> always true, is_mac -> false). */
typedef struct {
    uint8_t  fn_layer;     /* myfn layer index */

    /* Mouse mode (keyboard-layer mode; engine only reports it). */
    uint16_t trigger_kc;   /* key that toggles mouse mode on tap */
    uint16_t mod_win;      /* modifier emitted on trigger long-press (Win) */
    uint16_t mod_mac;      /* modifier emitted on trigger long-press (Mac) */
    bool   (*is_mac)(void);  /* platform select, NULL -> Win */
    bool   (*link_ok)(void); /* mouse link gate, NULL -> always ok */
    uint16_t hold_ms;        /* tap/hold threshold (200) */

    /* Shift+Esc combo (Insert only). */
    bool     shift_esc_enable;

    /* RGB indicator LED index used by vim_rgb_state_color(). */
    uint16_t led_index;

    /* Pipeline hooks / myfn dispatch (may be NULL). */
    bool (*hook_pre)(uint16_t keycode, keyrecord_t *record);      /* true = consumed */
    bool (*hook_post_myfn)(uint16_t keycode, keyrecord_t *record);/* true = consumed */
    bool (*myfn_declared)(uint16_t keycode);                      /* myfn table membership */
    void (*myfn)(uint16_t keycode, bool pressed);
    void (*vim_set_enabled)(bool enabled);                        /* NULL -> kv_enable/disable */

    /* §2.1 shortcut table (base + mods), terminated by base == 0. */
    const vim_shortcut_t *shortcuts;
} vim_cfg_t;

/* Single-source interception chain (pipeline steps 0..8, design §4.12).
 * Returns true when QMK should keep processing the key (QMK polarity). */
bool vim_pipeline_process(uint16_t keycode, keyrecord_t *record, const vim_cfg_t *cfg);

/* Housekeeping: drain the engine emit queue and service mouse long-presses. */
void vim_keymap_common_task(uint32_t now_ms);

/* Send a bare keycode with the physical modifiers temporarily stripped and
 * then restored (design §2.1 "剥修饰发裸键"). */
void send_plain_tap(uint16_t keycode);

/* QK layer-key exemption (fn readme §3): press/release must always pass QMK. */
bool vim_is_layer_key(uint16_t keycode);

/* Six-state RGB colour: red (off) / cyan (mouse) / purple (visual) /
 * yellow (normal pending) / blue (normal) / green (insert).  `pending` never
 * overrides Visual. */
void vim_rgb_state_color(bool enabled, kv_mode_t m, bool pending, bool mouse, uint8_t *r, uint8_t *g, uint8_t *b);

/* RGB indicator LED index carried by the active keyboard cfg (design §4.12:
 * the keyboard supplies only the LED position; consumed by its RGB helper). */
uint16_t vim_rgb_led_index(void);

/* Shared tap/hold timing helper (guards against a zero timer_read()). */
uint16_t vim_timer_start(void);
bool     vim_timer_elapsed(uint16_t start, uint16_t ms);

/* Default §2.1 shortcut table (identical for every keyboard). */
extern const vim_shortcut_t vim_default_shortcuts[];

#ifdef __cplusplus
}
#endif
