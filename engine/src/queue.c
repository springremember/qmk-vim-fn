#include "queue.h"

void kv_queue_init(kv_queue_t *q) {
    q->head = q->tail = q->count = 0;
}

bool kv_queue_push(kv_queue_t *q, kv_keycode_t kc) {
    if (q->count >= KV_QUEUE_CAP) return false;
    q->buf[q->tail] = kc;
    q->tail = (uint8_t)((q->tail + 1) % KV_QUEUE_CAP);
    q->count++;
    return true;
}

bool kv_queue_pop(kv_queue_t *q, kv_keycode_t *out) {
    if (q->count == 0) return false;
    *out = q->buf[q->head];
    q->head = (uint8_t)((q->head + 1) % KV_QUEUE_CAP);
    q->count--;
    return true;
}

bool kv_queue_peek(const kv_queue_t *q, kv_keycode_t *out) {
    if (q->count == 0) return false;
    *out = q->buf[q->head];
    return true;
}

bool kv_queue_has(const kv_queue_t *q) {
    return q->count > 0;
}

void kv_queue_flush(kv_queue_t *q) {
    q->head = q->tail = q->count = 0;
}
