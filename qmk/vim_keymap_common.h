// Copyright 2026 qmk-vim-fn
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

/* A §2.1 keyboard-layer shortcut: matched on the base keycode plus the
 * physically-held modifier set, then emitted with the physical modifiers
 * stripped.  Comparison is side-agnostic and subset-shaped: `mods_req` names a
 * mask (e.g. MOD_MASK_CTRL = LCTL|RCTL); no requirement means no masked
 * modifier may be down, otherwise the held set must be a non-empty subset. */
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

    /* "Back to typing" flash colour (design §4.12): while vim_insert_flash() is
     * true the keyboard replaces the INSERT mode colour with this 0xRRGGBB
     * value.  0 = no flash (plain INSERT colour). */
    uint32_t insert_flash_color;

    /* Pipeline hooks / myfn dispatch (may be NULL). */
    bool (*hook_pre)(uint16_t keycode, keyrecord_t *record);      /* true = consumed */
    bool (*hook_post_myfn)(uint16_t keycode, keyrecord_t *record);/* true = consumed */
    bool (*myfn_declared)(uint16_t keycode);                      /* myfn table membership */
    bool (*myfn)(uint16_t keycode, bool pressed);                  /* true = consume (do not pass to QMK); false = pass */
    void (*vim_set_enabled)(bool enabled);                        /* NULL -> kv_enable/disable */

    /* §2.1 shortcut table (base + mods), terminated by base == 0. */
    const vim_shortcut_t *shortcuts;
} vim_cfg_t;

/* Single-source interception chain (pipeline steps 0..9, design §4.12).
 * Returns true when QMK should keep processing the key (QMK polarity). */
bool vim_pipeline_process(uint16_t keycode, keyrecord_t *record, const vim_cfg_t *cfg);

/* Reset the shared-layer static state (mouse FSM, Caps tap/hold, escape grace)
 * and re-init the engine/glue.  Call from the keymap's keyboard_post_init_user
 * instead of vim_glue_init(). */
void vim_keymap_common_init(void);

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

/* True while the "back to typing" flash is due (design §4.12): vim is enabled,
 * the mode is INSERT, and the Esc grace window is still open — i.e. the current
 * INSERT was entered by an idle-Normal Esc, within VIM_ESC_GRACE_MS (3000 ms).
 * An in-window Esc restarts the window; leaving INSERT drops it immediately, so
 * boot / Caps-on / i,a,o,s,c ... report false, as do NORMAL / VISUAL / MOUSE.
 * The window is stamped with the 32-bit timer: the 16-bit reading wraps after
 * 65536 ms and would resurrect an already-expired window. */
bool vim_insert_flash(void);

/* Predicate AND colour in one call (design §4.12): returns true — and unpacks
 * cfg->insert_flash_color's 0xRRGGBB into the r/g/b out-params — only when
 * vim_insert_flash() holds and the configured colour is non-zero (0 = do not
 * override).  Returns false, leaving the out-params untouched, when the keyboard
 * must keep the mode colour.  The keyboard supplies the colour in cfg and
 * decides which LEDs to repaint. */
bool vim_insert_flash_color(uint8_t *r, uint8_t *g, uint8_t *b);

/* RGB indicator LED index carried by the active keyboard cfg (design §4.12:
 * the keyboard supplies only the LED position; consumed by its RGB helper). */
uint16_t vim_rgb_led_index(void);

/* Shared tap/hold timing helper (guards against a zero timer_read()). */
uint16_t vim_timer_start(void);
bool     vim_timer_elapsed(uint16_t start, uint16_t ms);

/* 32-bit variant for windows that may go unchecked across the 16-bit wrap
 * (design §4.12: the Esc grace window). */
uint32_t vim_timer_start32(void);
bool     vim_timer_elapsed32(uint32_t start, uint32_t ms);

/* Default §2.1 shortcut table (identical for every keyboard). */
extern const vim_shortcut_t vim_default_shortcuts[];

#ifdef __cplusplus
}
#endif
