/* queue.h — fixed-capacity ring buffer of keycodes. */
#ifndef KV_QUEUE_H
#define KV_QUEUE_H

#include <stdint.h>
#include <stdbool.h>
#include "kv_kc.h"

#define KV_QUEUE_CAP 16

typedef struct {
    kv_keycode_t buf[KV_QUEUE_CAP];
    uint8_t head;
    uint8_t tail;
    uint8_t count;
} kv_queue_t;

void         kv_queue_init(kv_queue_t *q);
bool         kv_queue_push(kv_queue_t *q, kv_keycode_t kc);
bool         kv_queue_pop(kv_queue_t *q, kv_keycode_t *out);
bool         kv_queue_peek(const kv_queue_t *q, kv_keycode_t *out);
bool         kv_queue_has(const kv_queue_t *q);
void         kv_queue_flush(kv_queue_t *q);

#endif /* KV_QUEUE_H */
