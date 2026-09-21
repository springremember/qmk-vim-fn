/* ctx.h — parser state and accumulated multi-key context. */
#ifndef KV_CTX_H
#define KV_CTX_H

#include "kv_kc.h"

typedef enum {
    ST_IDLE = 0,
    ST_CNT,    /* count collected */
    ST_OP,     /* operator collected */
    ST_OPCNT,  /* operator + postfix count */
    ST_ANG,    /* indent collected */
    ST_ANGCnt, /* indent + postfix count */
    ST_GP,     /* 'g' collected */
    ST_ZP,     /* 'Z' collected */
} kv_state_t;

typedef struct {
    int          count;  /* n  (prefix / postfix count) */
    int          count2; /* n2 (postfix count after an operator/indent) */
    kv_keycode_t op;     /* d / y / c */
    kv_keycode_t ang;    /* < / > */
    bool         has_op;
    bool         has_ang;
} kv_ctx_t;

void kv_ctx_reset(kv_ctx_t *ctx);

/* fold a prefix count into the "n" slot; returns false if the ctx is empty */
int  kv_ctx_n(const kv_ctx_t *ctx);

#endif /* KV_CTX_H */
