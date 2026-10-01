#include "emit.h"

#define EMIT_CAP 256
/* 环形下标必须用 `&` 而不是 `%`：Cortex-M0 上 `int % 256` 对**带符号**被除数要生成符号修正
 * （反汇编实测 `ands`+`bpl`+`subs`/`orrs`/`adds`），而 `& 255` 只需一条 `uxtb`。
 * `push`/`send_one` 都在每个键的热路径上，且这是纯字节收益（实测更小，见 engineering-spec §5）。
 * 前提：EMIT_CAP 是 2 的幂 —— 下面这行在编译期强制，改动 EMIT_CAP 会直接编译失败。 */
typedef char kv_emit_cap_must_be_power_of_two[(EMIT_CAP & (EMIT_CAP - 1)) == 0 ? 1 : -1];
#define EMIT_MASK (EMIT_CAP - 1)

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
    s_buf[(s_head + s_count) & EMIT_MASK] = kc;
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
    s_head = (s_head + 1) & EMIT_MASK;
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
