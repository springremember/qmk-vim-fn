/* engine.c — feed() parser loop, strict clear, mode dispatch. */
#include <string.h>
#include "../include/kv.h"
#include "queue.h"
#include "classify.h"
#include "ctx.h"
#include "emit.h"
#include "command.h"

/* ------------------------------------------------------------------ state */
static bool       s_enabled;
static kv_mode_t  s_mode;
static kv_state_t s_state;
static kv_ctx_t   s_ctx;
static kv_queue_t s_q;

/* repeat recording */
#define REC_MAX 8
static kv_keycode_t s_rec[REC_MAX];
static int          s_rec_len;
static kv_keycode_t s_last[REC_MAX];
static int          s_last_len;
static bool         s_replaying;

/* ------------------------------------------------------------------ helpers */
static void reset_pending(void) {
    kv_ctx_reset(&s_ctx);
    s_state = ST_IDLE;
}

static kv_motion_t motion_of(kv_keycode_t kc) {
    switch (kc) {
        case KV_H:       return M_H;
        case KV_J:       return M_J;
        case KV_K:       return M_K;
        case KV_L:       return M_L;
        case KV_W:       return M_W;
        case KV_C_W:     return M_WBIG;
        case KV_B:       return M_B;
        case KV_C_B:     return M_BBIG;
        case KV_E:       return M_E;
        case KV_C_E:     return M_EBIG;
        case KV_0:       return M_ZERO;
        case KV_C_CARET: return M_CARET;
        case KV_C_DLR:   return M_DOLLAR;
        case KV_C_G:     return M_G_BIG;
        default:         return M_NONE;
    }
}

static int digit_of(kv_keycode_t kc) {
    return (kc == KV_0) ? 0 : (int)(kc - KV_1 + 1);
}

static void rec_push(kv_keycode_t kc) {
    if (s_replaying) return;
    if (s_rec_len < REC_MAX) s_rec[s_rec_len++] = kc;
}

static void rec_commit(void) {
    if (s_replaying) return;
    if (s_rec_len > 0) {
        memcpy(s_last, s_rec, sizeof(kv_keycode_t) * (size_t)s_rec_len);
        s_last_len = s_rec_len;
    }
    s_rec_len = 0;
}

static void rec_clear(void) { s_rec_len = 0; }

static void rec_replay(void) {
    s_replaying = true;
    for (int i = 0; i < s_last_len; i++) kv_kbd(s_last[i]);
    s_replaying = false;
}

/* ------------------------------------------------------------------ commands */
static void do_single(kv_token_t t, kv_keycode_t kc) {
    switch (t) {
        case T_X:      kv_emit_delete_char();    break;
        case T_XUP:    kv_emit_backspace_char(); break;
        case T_s:      kv_emit_substitute();     break;
        case T_C_BIG:  kv_emit_change_to_eol();  break;
        case T_D_BIG:  kv_emit_delete_to_eol();  break;
        case T_Y_BIG:  kv_emit_yank_to_eol();    break;
        case T_P:      kv_emit_paste(false);     break;
        case T_PUP:    kv_emit_paste(true);      break;
        case T_JOIN:   kv_emit_join();           break;
        case T_UNDO:   kv_emit_undo();           break;
        case T_S_BIG:  kv_emit_line_op(KV_C, 1); break;
        case T_REPEAT:
            rec_replay();
            break;
        default: (void)kc; break;
    }
}

/* returns true if the key was consumed (not passed through) */
static bool feed_normal(kv_keycode_t kc) {
    kv_token_t t = (s_state == ST_CNT || s_state == ST_OPCNT || s_state == ST_ANGCnt)
                       ? kv_classify_digit(kc)
                       : kv_classify(kc);

    if (kc == KV_ESC) {
        if (s_state != ST_IDLE) { reset_pending(); return true; }
        kv_emit_tap(KV_ESC);
        return true;
    }

    switch (s_state) {
        case ST_IDLE:
            switch (t) {
                case T_COUNT:  s_state = ST_CNT; s_ctx.count = digit_of(kc); return true;
                case T_OP:     s_state = ST_OP;  s_ctx.op = kc; s_ctx.has_op = true; return true;
                case T_INDENT: s_state = ST_ANG; s_ctx.ang = kc; s_ctx.has_ang = true; return true;
                case T_g_LOWER: s_state = ST_GP; return true;
                case T_Z_BIG:  s_state = ST_ZP; return true;
                case T_MOTION: case T_ZERO: case T_CARET: case T_DOLLAR:
                    kv_emit_motion(motion_of(kc), 1); return true;
                case T_G_BIG:  kv_emit_motion(M_G_BIG, 1); return true;
                case T_S_BIG:  kv_emit_line_op(KV_C, 1); return true;
                case T_INSERT: kv_emit_enter_insert(kc); s_mode = KV_MODE_INSERT; return true;
                case T_VISUAL: s_mode = (kc == KV_C_V) ? KV_MODE_VISUAL_LINE : KV_MODE_VISUAL; return true;
                case T_X: case T_XUP: case T_s: case T_C_BIG: case T_D_BIG:
                case T_Y_BIG: case T_P: case T_PUP: case T_JOIN: case T_UNDO:
                    do_single(t, kc); return true;
                case T_REPEAT:
                    rec_replay(); return true;
                default:
                    kv_emit_tap(kc); return true; /* pass-through */
            }

        case ST_CNT: {
            int n = kv_ctx_n(&s_ctx);
            switch (t) {
                case T_DIGIT:
                    if (s_ctx.count < 10) s_ctx.count = s_ctx.count * 10 + digit_of(kc);
                    return true;
                case T_OP:     s_state = ST_OP; s_ctx.op = kc; s_ctx.has_op = true; return true;
                case T_INDENT: s_state = ST_ANG; s_ctx.ang = kc; s_ctx.has_ang = true; return true;
                case T_MOTION: case T_ZERO: case T_CARET: case T_DOLLAR:
                    kv_emit_motion(motion_of(kc), n); reset_pending(); return true;
                case T_G_BIG:  kv_emit_motion(M_G_BIG, 1); reset_pending(); return true;
                case T_g_LOWER: s_state = ST_GP; return true;
                case T_Z_BIG:  s_state = ST_ZP; return true;
                case T_S_BIG:  kv_emit_line_op(KV_C, n); reset_pending(); return true;
                case T_INSERT: kv_emit_enter_insert(kc); s_mode = KV_MODE_INSERT; reset_pending(); return true;
                case T_VISUAL: s_mode = (kc == KV_C_V) ? KV_MODE_VISUAL_LINE : KV_MODE_VISUAL; reset_pending(); return true;
                default: /* drop count, re-identify */
                    reset_pending();
                    return feed_normal(kc);
            }
        }

        case ST_OP: {
            int n = kv_ctx_n(&s_ctx);
            switch (t) {
                case T_OP:     kv_emit_line_op(s_ctx.op, n); reset_pending(); return true;
                case T_MOTION: case T_ZERO: case T_CARET: case T_DOLLAR:
                    kv_emit_op_motion(s_ctx.op, motion_of(kc), n); reset_pending(); return true;
                case T_G_BIG:  kv_emit_op_motion(s_ctx.op, M_G_BIG, 1); reset_pending(); return true;
                case T_g_LOWER: s_state = ST_GP; return true;
                case T_COUNT:  s_state = ST_OPCNT; s_ctx.count2 = digit_of(kc); return true;
                default:
                    reset_pending();
                    return feed_normal(kc);
            }
        }

        case ST_OPCNT: {
            switch (t) {
                case T_DIGIT:
                    if (s_ctx.count2 < 10) s_ctx.count2 = s_ctx.count2 * 10 + digit_of(kc);
                    return true;
                case T_MOTION: case T_ZERO: case T_CARET: case T_DOLLAR:
                    kv_emit_op_motion(s_ctx.op, motion_of(kc), kv_ctx_n(&s_ctx) * s_ctx.count2);
                    reset_pending(); return true;
                case T_G_BIG:  kv_emit_op_motion(s_ctx.op, M_G_BIG, 1); reset_pending(); return true;
                case T_g_LOWER: s_state = ST_GP; return true;
                default:
                    reset_pending();
                    return feed_normal(kc);
            }
        }

        case ST_ANG: {
            int n = kv_ctx_n(&s_ctx);
            switch (t) {
                case T_INDENT: kv_emit_indent_line(s_ctx.ang, n); reset_pending(); return true;
                case T_MOTION: case T_ZERO: case T_CARET: case T_DOLLAR:
                    kv_emit_indent_motion(s_ctx.ang, motion_of(kc), n); reset_pending(); return true;
                case T_G_BIG:  kv_emit_indent_motion(s_ctx.ang, M_G_BIG, 1); reset_pending(); return true;
                case T_g_LOWER: s_state = ST_GP; return true;
                case T_COUNT:  s_state = ST_ANGCnt; s_ctx.count2 = digit_of(kc); return true;
                default:
                    reset_pending();
                    return feed_normal(kc);
            }
        }

        case ST_ANGCnt: {
            switch (t) {
                case T_DIGIT:
                    if (s_ctx.count2 < 10) s_ctx.count2 = s_ctx.count2 * 10 + digit_of(kc);
                    return true;
                case T_MOTION: case T_ZERO: case T_CARET: case T_DOLLAR:
                    kv_emit_indent_motion(s_ctx.ang, motion_of(kc), kv_ctx_n(&s_ctx) * s_ctx.count2);
                    reset_pending(); return true;
                case T_G_BIG:  kv_emit_indent_motion(s_ctx.ang, M_G_BIG, 1); reset_pending(); return true;
                case T_g_LOWER: s_state = ST_GP; return true;
                default:
                    reset_pending();
                    return feed_normal(kc);
            }
        }

        case ST_GP:
            if (t == T_g_LOWER) {
                if (s_ctx.has_op)      kv_emit_op_motion(s_ctx.op, M_GG, 1);
                else if (s_ctx.has_ang) kv_emit_indent_motion(s_ctx.ang, M_GG, 1);
                else                    kv_emit_motion(M_GG, 1);
                reset_pending();
                return true;
            }
            reset_pending();
            return feed_normal(kc);

        case ST_ZP:
            if (t == T_Z_BIG) { kv_emit_save(); reset_pending(); return true; }
            reset_pending();
            return feed_normal(kc);

        default:
            reset_pending();
            return feed_normal(kc);
    }
}

static bool feed_visual(kv_keycode_t kc) {
    kv_token_t t = kv_classify(kc);
    if (kc == KV_ESC) { s_mode = KV_MODE_NORMAL; return true; }
    if (kc == KV_D || kc == KV_X) { kv_emit_delete_to_eol(); return true; } /* cut selection */
    if (kc == KV_Y) { kv_emit_yank_to_eol(); return true; }
    if (kc == KV_C) { kv_emit_change_to_eol(); s_mode = KV_MODE_INSERT; return true; }
    if (kc == KV_S) { kv_emit_substitute(); s_mode = KV_MODE_INSERT; return true; }
    if (kc == KV_P) { kv_emit_paste(false); return true; }
    switch (t) {
        case T_MOTION: case T_ZERO: case T_CARET: case T_DOLLAR:
            kv_emit_visual_motion(kc);
            return true;
        default:
            return true; /* illegal key: stay in Visual (swallow) */
    }
}

/* ------------------------------------------------------------------ public API */
void kv_init(void) {
    s_enabled = false;
    s_mode = KV_MODE_INSERT;
    reset_pending();
    kv_queue_init(&s_q);
    rec_clear();
    s_last_len = 0;
    kv_emit_clear();
}

void kv_set_emit(kv_emit_fn fn) {
    kv_emit_set_fn(fn);
}

void kv_kbd(kv_keycode_t kc) {
    if (!s_enabled) return;
    kv_queue_push(&s_q, kc);
    while (kv_queue_has(&s_q)) {
        kv_keycode_t cur;
        kv_queue_pop(&s_q, &cur);
        if (s_mode == KV_MODE_INSERT) {
            kv_emit_tap(cur); /* Insert: everything passes through */
        } else if (s_mode == KV_MODE_VISUAL || s_mode == KV_MODE_VISUAL_LINE) {
            feed_visual(cur);
        } else {
            if (cur == KV_ESC) {
                rec_clear();
            } else if (kv_is_vim_key(cur)) {
                rec_push(cur);
            }
            feed_normal(cur);
            if (s_state == ST_IDLE && s_rec_len > 0) rec_commit();
        }
    }
}

void kv_task(uint32_t now_ms) {
    kv_emit_service(now_ms);
}

kv_mode_t kv_get_mode(void) { return s_mode; }
bool      kv_vim_enabled(void) { return s_enabled; }
bool      kv_pending(void) { return s_state != ST_IDLE; }

void kv_set_mode(kv_mode_t m) { s_mode = m; reset_pending(); }
void kv_enable(void) { s_enabled = true; }
void kv_disable(void) { s_enabled = false; reset_pending(); }

void kv_cancel(void) { reset_pending(); }
