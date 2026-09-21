#include "ctx.h"

void kv_ctx_reset(kv_ctx_t *ctx) {
    ctx->count = 0;
    ctx->count2 = 0;
    ctx->op = 0;
    ctx->ang = 0;
    ctx->has_op = false;
    ctx->has_ang = false;
}

int kv_ctx_n(const kv_ctx_t *ctx) {
    return ctx->count > 0 ? ctx->count : 1;
}
