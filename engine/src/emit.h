/* emit.h — command -> fixed host key sequence + non-blocking send queue. */
#ifndef KV_EMIT_H
#define KV_EMIT_H

#include "../include/kv_kc.h"

/* gap between two emitted keys (ms) */
#define KV_EMIT_GAP_MS 1

void kv_emit_set_fn(void (*fn)(kv_keycode_t kc));

/* clear the pending send queue (does not call the callback) */
void kv_emit_clear(void);

/* enqueue a single tap */
void kv_emit_tap(kv_keycode_t kc);

/* enqueue n taps of the same keycode */
void kv_emit_taps(kv_keycode_t kc, int n);

/* enqueue a sequence of taps */
void kv_emit_seq(const kv_keycode_t *seq, int n);

/* send queued keys according to their timer (called from kv_task) */
void kv_emit_service(uint32_t now_ms);

/* test helper: drain the queue immediately, ignoring the timer */
void kv_emit_flush_now(void);

/* test helper: number of keys still queued */
int  kv_emit_pending(void);

#endif /* KV_EMIT_H */
