#include "emit.h"

#define EMIT_CAP 256

static kv_keycode_t s_buf[EMIT_CAP];
static int          s_head;
static int          s_count;
static uint32_t     s_last_ms;
static bool         s_has_last;
static void       (*s_fn)(kv_keycode_t);

void kv_emit_set_fn(void (*fn)(kv_keycode_t kc)) {
    s_fn = fn;
}

void kv_emit_clear(void) {
    s_head = s_count = 0;
    s_has_last = false;
}

static void push(kv_keycode_t kc) {
    if (s_count >= EMIT_CAP) return; /* drop on overflow */
    s_buf[(s_head + s_count) % EMIT_CAP] = kc;
    s_count++;
}

void kv_emit_tap(kv_keycode_t kc) {
    push(kc);
}

void kv_emit_taps(kv_keycode_t kc, int n) {
    for (int i = 0; i < n; i++) push(kc);
}

void kv_emit_seq(const kv_keycode_t *seq, int n) {
    for (int i = 0; i < n; i++) push(seq[i]);
}

static void send_one(void) {
    if (s_count == 0) return;
    kv_keycode_t kc = s_buf[s_head];
    s_head = (s_head + 1) % EMIT_CAP;
    s_count--;
    if (s_fn) s_fn(kc);
}

void kv_emit_service(uint32_t now_ms) {
    if (s_count == 0) return;
    if (s_has_last && (uint32_t)(now_ms - s_last_ms) < KV_EMIT_GAP_MS) return;
    s_last_ms = now_ms;
    s_has_last = true;
    send_one();
}

void kv_emit_flush_now(void) {
    while (s_count > 0) send_one();
    s_has_last = false;
}

int kv_emit_pending(void) {
    return s_count;
}
