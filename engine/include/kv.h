/* kv.h — public API of the vim engine (QMK-agnostic core).
 *
 * Design authority: ../vim/design.md (v4.7 关键接口).
 * The engine turns vim commands into host key sequences.  It never blocks:
 * emitted keys are queued and drained by kv_task() under a timer.
 */
#ifndef KV_ENGINE_H
#define KV_ENGINE_H

#include <stdint.h>
#include <stdbool.h>
#include "kv_kc.h"

typedef enum {
    KV_MODE_INSERT = 0,
    KV_MODE_NORMAL,
    KV_MODE_VISUAL,
    KV_MODE_VISUAL_LINE,
    KV_MODE_MOUSE, /* keyboard-layer mode (and any future mode >= this):
                    * the engine never parses it; kv_kbd() returns
                    * KV_PASSTHROUGH for the whole keyboard-layer range. */
} kv_mode_t;

/* Result of feeding a key-down. */
typedef enum {
    KV_CONSUMED = 0, /* the engine handled it (a vim command or a swallow) */
    KV_PASSTHROUGH,  /* not a vim keycode: the caller must emit it itself */
} kv_result_t;

/* Output callback: the engine hands each host keycode to it. */
typedef void (*kv_emit_fn)(kv_keycode_t kc);

/* Reset all state, start in INSERT with vim disabled. */
void kv_init(void);

/* Parser entry: feed a key-down.  Returns CONSUMED or PASSTHROUGH.
 * MOUSE and any keyboard-layer mode (kv_mode_t >= KV_MODE_MOUSE) always
 * returns KV_PASSTHROUGH: those modes belong to the keyboard layer. */
kv_result_t kv_kbd(kv_keycode_t kc);

/* Install the emit callback (a recorder in host tests). */
void kv_set_emit(kv_emit_fn fn);

/* Housekeeping: send queued emit events according to their timers. */
void kv_task(uint32_t now_ms);

/* ---- query / set (added for the keyboard layer) ---- */
kv_mode_t kv_get_mode(void);        /* current mode */
bool      kv_vim_enabled(void);     /* vim master switch */
bool      kv_pending(void);         /* any pending (count/op/prefix/indent) */
void      kv_set_mode(kv_mode_t m); /* set mode directly */
void      kv_enable(void);          /* enable vim */
void      kv_disable(void);         /* disable vim */
void      kv_cancel(void);          /* clear pending without emitting */

#endif /* KV_ENGINE_H */
