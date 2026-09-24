// Copyright 2026 qmk-vim-fn
// SPDX-License-Identifier: GPL-2.0-or-later
//
// vim_glue.h — QMK adapter for the QMK-agnostic qmk-vim-fn engine.
// Design authority: qmk-vim-fn/vim/design.md §4.10 (key-up / modifiers) and
// §4.12 (glue layer responsibilities).
//
// All keyboards share this file; implementations must not be copied into a
// keymap.  The header depends on the QMK API on purpose (adapter layer).

#pragma once

#include <stdint.h>
#include <stdbool.h>

#include QMK_KEYBOARD_H

#ifdef __cplusplus
extern "C" {
#endif

/* Reset the engine, install the emit callback and clear the physical modifier
 * shadow / press-release pairing table (design §4.12). */
void vim_glue_init(void);

/* Physical modifier shadow update — pipeline step 0.  Must run before any key
 * may be swallowed (myfn swallows modifiers, so get_mods() is unreliable). */
void vim_glue_mod_update(uint16_t keycode, bool pressed);

/* Read-only shadow query in QMK's 8-bit modifier mask (design §4.10). */
uint8_t vim_glue_mods(void);

/* Engine dispatch tail — pipeline step 8.  Performs the Shift-fold / CAG
 * passthrough decision, feeds kv_kbd(), records consumed presses and services
 * their releases plus the held-motion h/j/k/l exception.
 * Returns true when QMK should keep processing the key (QMK polarity:
 * true = let through, false = consumed). */
bool vim_glue_engine(uint16_t keycode, keyrecord_t *record);

/* Register a keyboard-layer consumed press so its release is consumed by the
 * shared pairing table (design §4.10). */
void vim_glue_swallow(uint16_t keycode);

/* Drain the engine's non-blocking emit queue (design #7). */
void vim_glue_task(uint32_t now_ms);

/* Unregister any held-motion host arrows (mode switch / disable / mouse enter,
 * design §4.10). */
void vim_glue_release_all(void);

#ifdef __cplusplus
}
#endif
