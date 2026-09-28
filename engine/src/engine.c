/* engine.c — feed() parser loop, strict clear, mode dispatch. */
#include <string.h>
#include "../include/kv.h"
#include "classify.h"
#include "ctx.h"
#include "emit.h"
#include "command.h"

/* ------------------------------------------------------------------ state */
static bool       s_enabled;
static kv_mode_t  s_mode;
static kv_state_t s_state;
static kv_ctx_t   s_ctx;

/* repeat recording */
#define REC_MAX 8
static kv_keycode_t s_rec[REC_MAX];
static int          s_rec_len;
static kv_keycode_t s_last[REC_MAX];
static int          s_last_len;
static bool         s_replaying;

/* internal feed result: consumed / pass-through / needs re-identification */
typedef enum { R_CONSUMED = 0, R_PASSTHROUGH, R_REIDENTIFY } kv_feed_t;

/* ------------------------------------------------------------------ helpers */
/* 可视模式内的输入状态（reset_pending 会一并清掉）：
 *   s_visual_digits = 已累积的计数位数（上限 2，design §4.9/§4.8）
 *   s_visual_gp     = 可视模式内的 g 前缀（gg，design §4.9） */
static uint8_t s_visual_digits;
static bool    s_visual_gp;

/* VISUAL_LINE 行选状态（design §4.9 v2）：off = 光标行 − 锚行 A（A = 按 V 时所在行）
 *   s_vl_up  : true = UP 态（锚在 A+1 行首）／false = DOWN 态（锚在 A 行首）
 *   s_vl_abs : G/gg 之后行号未知，动作不再重建、只靠当前选区 */
static int  s_vl_off;
static bool s_vl_up;
static bool s_vl_abs;

/* 行选跨度上限（= 光标行与锚行 A 的最大距离）。
 * 为什么必须有：gg/G 与方向翻转的重锚都是 O(|off|) 键码，而发送队列只有
 * EMIT_CAP=256 格、溢出时**静默丢键**（选区错乱 → 后续 d/y 作用在错误范围 =
 * 数据损坏）。取 100 使单条命令键码 ≤102，与 Normal 的 99dd(103) 同量级；
 * 与 Normal/Visual 的"2 位计数 ≤99"语义也一致。 */
#define KV_VLINE_MAX_OFF 100

static void vline_reset(void) { s_vl_off = 0; s_vl_up = false; s_vl_abs = false; }

/* 行选移动：把 off 从 s_vl_off 变成 s_vl_off±n，并让宿主字符选区**始终覆盖整行**。
 * 重锚只在方向翻转时发生（键数受当前 |off| 限制），不做全量重建。 */
static void vline_move(bool up, int n) {
    if (n < 1) n = 1;
    if (n > 99) n = 99;
    if (s_vl_abs) {                       /* 行号未知：只做纵向扩展 */
        kv_emit_vline_move(up, n);
        kv_emit_vline_move_tail();
        return;
    }
    int off = s_vl_off + (up ? -n : n);
    if (off >  KV_VLINE_MAX_OFF) off =  KV_VLINE_MAX_OFF;
    if (off < -KV_VLINE_MAX_OFF) off = -KV_VLINE_MAX_OFF;
    n = off - s_vl_off;                   /* 实际移动行数（受跨度上限约束） */
    if (n == 0) return;                   /* 已到跨度上限：不再扩展（不发键） */
    up = (n < 0); if (up) n = -n;
    if (!up && !s_vl_up) {                /* DOWN 态向下：直接扩展 */
        kv_emit_vline_move(false, n);
        kv_emit_vline_move_tail();
    } else if (!up) {                     /* UP 态向下：越过 A 后重锚回 DOWN */
        if (off > 0) { kv_emit_vline_reanchor(false, s_vl_off, off); s_vl_up = false; }
        else         { kv_emit_vline_move(false, n); }
    } else if (s_vl_up) {                 /* UP 态向上：直接扩展 */
        kv_emit_vline_move(true, n);
    } else {                              /* DOWN 态向上：越过 A 后重锚到 UP */
        if (off >= 0) { kv_emit_vline_move(true, n); kv_emit_vline_move_tail(); }
        else          { kv_emit_vline_reanchor(true, s_vl_off, off); s_vl_up = true; }
    }
    s_vl_off = off;
}

/* 行选内的移动键 → 纵向行数；h/l/0/^/$ 不改行范围（真实 Vim 亦然），无输出。 */
static void vline_motion(kv_keycode_t kc, int n) {
    switch (kc) {
        case KV_J: case KV_W: case KV_E: case KV_C_W: case KV_C_E: vline_move(false, n); break;
        case KV_K: case KV_B: case KV_C_B:                          vline_move(true,  n); break;
        default: break;
    }
}

static void reset_pending(void) {
    kv_ctx_reset(&s_ctx);
    s_state = ST_IDLE;
    s_visual_digits = 0; /* 可视模式计数随输入一起作废（design §4.9/§4.10） */
    s_visual_gp     = false;
}

static int digit_of(kv_keycode_t kc) {
    return (kc == KV_0) ? 0 : (int)(kc - KV_1 + 1);
}

/* Fold two counts by multiplication, clamped so the emitted key sequence can
 * never overflow the non-blocking queue (design #2; 2d3w = d6w). */
static int fold_counts(int n, int n2) {
    int r = n * n2;
    if (r > 99) r = 99;
    return r;
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

static void rec_push(kv_keycode_t kc) {
    if (s_replaying) return;
    if (s_rec_len < REC_MAX) s_rec[s_rec_len++] = kc;
}

/* Only "change-like" commands are worth replaying with '.'.  Insert/visual
 * entries, prefixes that never complete, and '.' itself are excluded. */
static bool rec_should_record(kv_token_t t) {
    switch (t) {
        case T_COUNT: case T_OP: case T_INDENT: case T_g_LOWER: case T_Z_BIG:
        case T_MOTION: case T_ZERO: case T_CARET: case T_DOLLAR: case T_G_BIG:
        case T_S_BIG: case T_X: case T_XUP: case T_s: case T_C_BIG:
        case T_D_BIG: case T_Y_BIG: case T_P: case T_PUP: case T_JOIN:
            return true;
        default:
            return false;
    }
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

/* Shared abort path for every mode/enable transition (design #4.7):
 * drop the in-progress state machine AND the in-progress repeat recording.
 * s_last is deliberately preserved so '.' can replay a completed command
 * across a mode round-trip. */
static void abort_input(void) {
    reset_pending();
    rec_clear();
}

static void rec_replay(void) {
    if (s_replaying || s_last_len == 0) return; /* never re-enter '.' */
    s_replaying = true;
    for (int i = 0; i < s_last_len; i++) kv_kbd(s_last[i]);
    s_replaying = false;
}

/* ------------------------------------------------------------------ commands */
static void do_single(kv_token_t t, kv_keycode_t kc) {
    (void)kc;
    switch (t) {
        case T_X:      kv_emit_delete_char();    break;
        case T_XUP:    kv_emit_backspace_char(); break;
        case T_s:      kv_emit_substitute();     s_mode = KV_MODE_INSERT; break;
        case T_C_BIG:  kv_emit_change_to_eol();  s_mode = KV_MODE_INSERT; break;
        case T_D_BIG:  kv_emit_delete_to_eol();  break;
        case T_Y_BIG:  kv_emit_yank_to_eol();    break;
        case T_P:      kv_emit_paste(false);     break;
        case T_PUP:    kv_emit_paste(true);      break;
        case T_JOIN:   kv_emit_join();           break;
        case T_UNDO:   kv_emit_undo();           break;
        default: break;
    }
}

/* Normal-mode state machine.  Returns R_CONSUMED / R_PASSTHROUGH /
 * R_REIDENTIFY (strict clear: caller re-feeds the key in IDLE). */
static kv_feed_t feed_normal(kv_keycode_t kc) {
    kv_token_t t = (s_state == ST_CNT || s_state == ST_OPCNT || s_state == ST_ANGCnt)
                       ? kv_classify_digit(kc)
                       : kv_classify(kc);

    if (KV_BASIC(kc) == KV_ESC) {
        if (s_state != ST_IDLE) { reset_pending(); return R_CONSUMED; }
        return R_PASSTHROUGH; /* real Esc handled by the caller */
    }

    switch (s_state) {
        case ST_IDLE:
            switch (t) {
                case T_COUNT:  s_state = ST_CNT; s_ctx.count = digit_of(kc); return R_CONSUMED;
                case T_OP:     s_state = ST_OP;  s_ctx.op = kc; s_ctx.has_op = true; return R_CONSUMED;
                case T_INDENT: s_state = ST_ANG; s_ctx.ang = kc; s_ctx.has_ang = true; return R_CONSUMED;
                case T_g_LOWER: s_state = ST_GP; return R_CONSUMED;
                case T_Z_BIG:  s_state = ST_ZP; return R_CONSUMED;
                case T_MOTION: case T_ZERO: case T_CARET: case T_DOLLAR:
                    kv_emit_motion(motion_of(kc), 1); return R_CONSUMED;
                case T_G_BIG:  kv_emit_motion(M_G_BIG, 1); return R_CONSUMED;
                case T_S_BIG:  kv_emit_line_op(KV_C, 1); s_mode = KV_MODE_INSERT; return R_CONSUMED;
                case T_INSERT: kv_emit_enter_insert(kc); s_mode = KV_MODE_INSERT; return R_CONSUMED;
                case T_VISUAL:
                    s_mode = (kc == KV_C_V) ? KV_MODE_VISUAL_LINE : KV_MODE_VISUAL;
                    vline_reset();
                    if (s_mode == KV_MODE_VISUAL_LINE) kv_emit_visual_line_enter(); /* 选中整行 */
                    return R_CONSUMED;
                case T_X: case T_XUP: case T_s: case T_C_BIG: case T_D_BIG:
                case T_Y_BIG: case T_P: case T_PUP: case T_JOIN: case T_UNDO:
                    do_single(t, kc); return R_CONSUMED;
                case T_REPEAT:
                    rec_replay(); return R_CONSUMED;
                default:
                    return R_PASSTHROUGH; /* not a vim keycode */
            }

        case ST_CNT: {
            int n = kv_ctx_n(&s_ctx);
            switch (t) {
                case T_DIGIT:
                    if (s_ctx.count < 10) s_ctx.count = s_ctx.count * 10 + digit_of(kc);
                    return R_CONSUMED;
                case T_OP:     s_state = ST_OP; s_ctx.op = kc; s_ctx.has_op = true; return R_CONSUMED;
                case T_INDENT: s_state = ST_ANG; s_ctx.ang = kc; s_ctx.has_ang = true; return R_CONSUMED;
                case T_MOTION: case T_ZERO: case T_CARET: case T_DOLLAR:
                    kv_emit_motion(motion_of(kc), n); reset_pending(); return R_CONSUMED;
                case T_G_BIG:  kv_emit_motion(M_G_BIG, 1); reset_pending(); return R_CONSUMED;
                case T_g_LOWER: s_state = ST_GP; return R_CONSUMED;
                case T_Z_BIG:  s_state = ST_ZP; return R_CONSUMED;
                case T_S_BIG:  kv_emit_line_op(KV_C, n); s_mode = KV_MODE_INSERT; reset_pending(); return R_CONSUMED;
                case T_INSERT: kv_emit_enter_insert(kc); s_mode = KV_MODE_INSERT; reset_pending(); return R_CONSUMED;
                case T_VISUAL:
                    s_mode = (kc == KV_C_V) ? KV_MODE_VISUAL_LINE : KV_MODE_VISUAL;
                    vline_reset();
                    if (s_mode == KV_MODE_VISUAL_LINE) kv_emit_visual_line_enter(); /* 选中整行 */
                    reset_pending();
                    return R_CONSUMED;
                default: /* drop count, re-identify */
                    reset_pending();
                    return R_REIDENTIFY;
            }
        }

        case ST_OP: {
            int n = kv_ctx_n(&s_ctx);
            switch (t) {
                case T_OP:
                    if (kc == s_ctx.op) {
                        kv_emit_line_op(s_ctx.op, n);
                        if (s_ctx.op == KV_C) s_mode = KV_MODE_INSERT;
                        reset_pending();
                        return R_CONSUMED;
                    }
                    reset_pending();
                    return R_REIDENTIFY; /* operator mismatch: d y is not dy */
                case T_MOTION: case T_CARET: case T_DOLLAR:
                    kv_emit_op_motion(s_ctx.op, motion_of(kc), n);
                    if (s_ctx.op == KV_C) s_mode = KV_MODE_INSERT;
                    reset_pending(); return R_CONSUMED;
                case T_ZERO: /* d0: drop count */
                    kv_emit_op_motion(s_ctx.op, M_ZERO, 1);
                    if (s_ctx.op == KV_C) s_mode = KV_MODE_INSERT;
                    reset_pending(); return R_CONSUMED;
                case T_G_BIG:
                    kv_emit_op_motion(s_ctx.op, M_G_BIG, 1);
                    if (s_ctx.op == KV_C) s_mode = KV_MODE_INSERT;
                    reset_pending(); return R_CONSUMED;
                case T_g_LOWER: s_state = ST_GP; return R_CONSUMED;
                case T_COUNT:  s_state = ST_OPCNT; s_ctx.count2 = digit_of(kc); return R_CONSUMED;
                default:
                    reset_pending();
                    return R_REIDENTIFY;
            }
        }

        case ST_OPCNT: {
            switch (t) {
                case T_DIGIT:
                    if (s_ctx.count2 < 10) s_ctx.count2 = s_ctx.count2 * 10 + digit_of(kc);
                    return R_CONSUMED;
                case T_MOTION: case T_CARET: case T_DOLLAR:
                    kv_emit_op_motion(s_ctx.op, motion_of(kc), fold_counts(kv_ctx_n(&s_ctx), s_ctx.count2));
                    if (s_ctx.op == KV_C) s_mode = KV_MODE_INSERT;
                    reset_pending(); return R_CONSUMED;
                case T_ZERO: /* d20: 0 continues the count; d20 alone is not a command */
                    if (s_ctx.count2 < 10) s_ctx.count2 = s_ctx.count2 * 10;
                    return R_CONSUMED;
                case T_G_BIG:
                    kv_emit_op_motion(s_ctx.op, M_G_BIG, 1);
                    if (s_ctx.op == KV_C) s_mode = KV_MODE_INSERT;
                    reset_pending(); return R_CONSUMED;
                case T_g_LOWER: s_state = ST_GP; return R_CONSUMED;
                default:
                    reset_pending();
                    return R_REIDENTIFY;
            }
        }

        case ST_ANG: {
            int n = kv_ctx_n(&s_ctx);
            switch (t) {
                case T_INDENT:
                    if (kc == s_ctx.ang) { kv_emit_indent_line(s_ctx.ang, n); reset_pending(); return R_CONSUMED; }
                    reset_pending();
                    return R_REIDENTIFY; /* > < is not >> */
                case T_MOTION: case T_CARET: case T_DOLLAR:
                    kv_emit_indent_motion(s_ctx.ang, motion_of(kc), n); reset_pending(); return R_CONSUMED;
                case T_ZERO: /* >0: drop count */
                    kv_emit_indent_motion(s_ctx.ang, M_ZERO, 1); reset_pending(); return R_CONSUMED;
                case T_G_BIG:  kv_emit_indent_motion(s_ctx.ang, M_G_BIG, 1); reset_pending(); return R_CONSUMED;
                case T_g_LOWER: s_state = ST_GP; return R_CONSUMED;
                case T_COUNT:  s_state = ST_ANGCnt; s_ctx.count2 = digit_of(kc); return R_CONSUMED;
                default:
                    reset_pending();
                    return R_REIDENTIFY;
            }
        }

        case ST_ANGCnt: {
            switch (t) {
                case T_DIGIT:
                    if (s_ctx.count2 < 10) s_ctx.count2 = s_ctx.count2 * 10 + digit_of(kc);
                    return R_CONSUMED;
                case T_MOTION: case T_CARET: case T_DOLLAR:
                    kv_emit_indent_motion(s_ctx.ang, motion_of(kc), fold_counts(kv_ctx_n(&s_ctx), s_ctx.count2));
                    reset_pending(); return R_CONSUMED;
                case T_ZERO:
                    if (s_ctx.count2 < 10) s_ctx.count2 = s_ctx.count2 * 10;
                    return R_CONSUMED;
                case T_G_BIG:  kv_emit_indent_motion(s_ctx.ang, M_G_BIG, 1); reset_pending(); return R_CONSUMED;
                case T_g_LOWER: s_state = ST_GP; return R_CONSUMED;
                default:
                    reset_pending();
                    return R_REIDENTIFY;
            }
        }

        case ST_GP:
            if (t == T_g_LOWER) {
                if (s_ctx.has_op) {
                    kv_emit_op_motion(s_ctx.op, M_GG, 1);
                    if (s_ctx.op == KV_C) s_mode = KV_MODE_INSERT;
                } else if (s_ctx.has_ang) {
                    kv_emit_indent_motion(s_ctx.ang, M_GG, 1);
                } else {
                    kv_emit_motion(M_GG, 1);
                }
                reset_pending();
                return R_CONSUMED;
            }
            reset_pending();
            return R_REIDENTIFY;

        case ST_ZP:
            if (t == T_Z_BIG) { kv_emit_save(); reset_pending(); return R_CONSUMED; }
            reset_pending();
            return R_REIDENTIFY;

        default:
            reset_pending();
            return R_REIDENTIFY;
    }
}

static kv_feed_t feed_visual(kv_keycode_t kc) {
    kv_token_t t = kv_classify(kc);
    const bool vline = (s_mode == KV_MODE_VISUAL_LINE);
    // Esc 永远退出可视（design §4.9），**必须先于 g 前缀分支**：否则 `v g Esc` 会被
    // 当作"g 后接非法键"吞掉、退不出可视（第 3 轮对抗审核 D）。
    if (KV_BASIC(kc) == KV_ESC) {
        /* 真实 Vim：Esc 取消选区。宿主在 Shift 扩展后保留高亮选区，不取消则
         * 下一个按键会替换它（第 3 轮审核 D8）。 */
        if (vline) kv_emit_tap(KV_ESC);
        s_mode = KV_MODE_NORMAL;
        s_visual_digits = 0;
        s_visual_gp     = false;
        vline_reset();
        reset_pending();
        return R_CONSUMED;
    }
    // g 前缀已按下：第二击 g = gg（发 Ctrl+Shift+Home）；其它键按非法键吞掉。
    // 必须先于 T_g_LOWER 分支判定，否则第二个 g 只会再次设置前缀。
    if (s_visual_gp) {
        s_visual_gp     = false;
        s_visual_digits = 0;
        s_ctx.count     = 0;
        if (t == T_g_LOWER) { // gg：只认小写 g（Shift+G 不是 gg 的第二击）
            if (vline) {
                /* 行选：范围 = [文首, A]。abs 之后 off 未知，按 UP 分支尽力处理。 */
                kv_emit_vline_gg(s_vl_abs ? true : s_vl_up, s_vl_off);
                s_vl_up = true; s_vl_abs = true;
            } else {
                kv_emit_tap(KV_CS(KV_HOME));
            }
            reset_pending();
            return R_CONSUMED;
        }
        // design §4.9：`gg` 之外的第二击按非法键处理 —— 吞掉、不产生任何输出，
        // 并（按"非法键立即消费计数"）清掉计数与 g 前缀。
        reset_pending();
        return R_CONSUMED;
    }
    // 0 在计数中作数字（design §4.3/§4.9）：已有位数时把 0 当数字处理
    if (t == T_ZERO && s_visual_digits > 0) t = T_COUNT;
    /* design §4.8/§4.9: 可视模式同样支持"独立移动 ×n" —— 计数以 s_ctx.count 累积
     * （ST_CNT 在 feed_normal 里收集），这里按 n 重复对应基础序列。 */
    // design §4.9: 数字先在可视模式内累积（与 §4.8 一致，最多 2 位）；
    // 累积不算多键 pending（kv_pending() 在 Visual 下恒为 false）。
    if (t == T_COUNT) {
        if (s_visual_digits < 2) { // 上限 2 位（≤99）；第 3 位起忽略
            s_ctx.count = (s_ctx.count < 100 ? s_ctx.count : 99) * 10 + digit_of(kc);
            if (s_ctx.count > 99) s_ctx.count = 99;
            s_visual_digits++;
        }
        return R_CONSUMED;
    }
    if (t == T_g_LOWER) { // gg 前缀（design §4.9）：可视模式内自行处理
        s_visual_gp = true;
        return R_CONSUMED;
    }
    // 非数字键立即消费计数（design §4.9）：含非法键与透传键；
    // 计数位数清 0，使其不泄漏到更后面的 motion。
    s_visual_digits = 0;
    const int n = kv_ctx_n(&s_ctx);
    /* design §4.9: 动作后退出可视（Vim 语义）—— y/d/x/p 回 NORMAL，c/s 回 NORMAL 再进 INSERT。
     * 行选下动作前先把"整行"选中（Home+Shift+End），使 d/y/c/s 作用于整行。 */
    /* design §4.9：动作后退出可视（Vim 语义）。行选下动作与选区方向无关：
     * kv_emit_vline_action 统一按"整行 + 行尾换行"处理。 */
    if (kc == KV_D || kc == KV_X) {
        if (vline) kv_emit_vline_action(KV_D, s_vl_up); else kv_emit_delete_to_eol();
        s_mode = KV_MODE_NORMAL; vline_reset(); reset_pending(); return R_CONSUMED;
    }
    if (kc == KV_Y) {
        if (vline) kv_emit_vline_action(KV_Y, s_vl_up); else kv_emit_yank_to_eol();
        s_mode = KV_MODE_NORMAL; vline_reset(); reset_pending(); return R_CONSUMED;
    }
    if (kc == KV_C) {
        if (vline) kv_emit_vline_action(KV_C, s_vl_up); else kv_emit_change_to_eol();
        s_mode = KV_MODE_INSERT; vline_reset(); reset_pending(); return R_CONSUMED;
    }
    if (kc == KV_S) {
        /* 真实 Vim：V s ≡ V c（删整行 + 留一个空行 + Insert），不再只删 1 字符。 */
        if (vline) kv_emit_vline_action(KV_C, s_vl_up); else kv_emit_substitute();
        s_mode = KV_MODE_INSERT; vline_reset(); reset_pending(); return R_CONSUMED;
    }
    if (kc == KV_P) {
        if (vline) kv_emit_vline_action(KV_P, s_vl_up); else kv_emit_paste(false);
        s_mode = KV_MODE_NORMAL; vline_reset(); reset_pending(); return R_CONSUMED;
    }
    if (t == T_VISUAL) {
        /* 真实 Vim：VISUAL 内按 V 切到行选；VISUAL_LINE 内按 v 切回字符选（不发键）。 */
        if (vline) {
            if (kc == KV_V) { s_mode = KV_MODE_VISUAL; vline_reset(); }
        } else if (kc == KV_C_V) {
            s_mode = KV_MODE_VISUAL_LINE;
            vline_reset();
            kv_emit_visual_line_enter();
        }
        s_visual_digits = 0;
        reset_pending();
        return R_CONSUMED;
    }
    switch (t) {
        case T_MOTION: case T_ZERO: case T_CARET: case T_DOLLAR:
            if (vline) {
                vline_motion(kc, n);  /* 行选：整行推进 n 行（h/l/0/^/$ 不改行范围） */
            } else {
                for (int i = 0; i < n; i++) kv_emit_visual_motion(kc); /* 字符/词级 */
            }
            s_visual_digits = 0; /* 计数已消费 */
            reset_pending();
            return R_CONSUMED;
        case T_G_BIG: /* G 丢计数（readme §5）：无论 n 都只发一次 */
            if (vline) {
                /* 行选：范围 = [A, 文末]。abs 之后 off 未知，按 DOWN 分支尽力处理。 */
                kv_emit_vline_G(s_vl_abs ? false : s_vl_up, s_vl_off);
                s_vl_up = false; s_vl_abs = true;
            } else {
                kv_emit_visual_motion(kc);
            }
            s_visual_digits = 0;
            reset_pending();
            return R_CONSUMED;
        default:
            s_visual_digits = 0; /* 非法键立即消费计数（design §4.9） */
            reset_pending();
            return R_CONSUMED; /* illegal key: stay in Visual (swallow) */
    }
}

/* ------------------------------------------------------------------ public API */
void kv_init(void) {
    s_enabled = false;
    s_mode = KV_MODE_INSERT;
    vline_reset();
    reset_pending();
    rec_clear();
    s_last_len = 0;
    s_replaying = false;
    kv_emit_clear();
}

void kv_set_emit(kv_emit_fn fn) {
    kv_emit_set_fn(fn);
}

kv_result_t kv_kbd(kv_keycode_t kc) {
    if (!s_enabled) return KV_PASSTHROUGH;

    /* loop instead of recursion for strict-clear re-identification */
    for (;;) {
        /* INSERT: every key passes through to the host, including Esc.  The
         * Esc <-> NORMAL toggle is a keyboard-layer concern (the shared keymap
         * layer owns it), so the engine leaves INSERT only via kv_set_mode().
         * MOUSE and any keyboard-layer mode (kv_mode_t >= KV_MODE_MOUSE) is
         * wholly delegated to the keyboard layer. */
        if (s_mode == KV_MODE_INSERT) {
            return KV_PASSTHROUGH;
        }
        if (s_mode >= KV_MODE_MOUSE) return KV_PASSTHROUGH;

        if (s_mode == KV_MODE_VISUAL || s_mode == KV_MODE_VISUAL_LINE) {
            feed_visual(kc);
            return KV_CONSUMED;
        }

        kv_feed_t r = feed_normal(kc);
        if (r == R_REIDENTIFY) {
            rec_clear(); /* the discarded prefix must not pollute repeat */
            continue;    /* re-feed in IDLE */
        }

        if (r == R_CONSUMED) {
            if (KV_BASIC(kc) == KV_ESC) {
                rec_clear();
            } else if (kv_is_vim_key(kc) && rec_should_record(kv_classify(kc))) {
                rec_push(kc);
            }
            if (s_state == ST_IDLE && s_rec_len > 0) rec_commit();
            if (s_mode == KV_MODE_INSERT) rec_clear(); /* mode left NORMAL */
            return KV_CONSUMED;
        }
        if (s_rec_len > 0) rec_clear(); /* pass-through abandons a partial prefix */
        return KV_PASSTHROUGH;
    }
}

void kv_task(uint32_t now_ms) {
    kv_emit_service(now_ms);
}

kv_mode_t kv_get_mode(void) { return s_mode; }
bool      kv_vim_enabled(void) { return s_enabled; }
bool      kv_pending(void) { return s_state != ST_IDLE; }
bool      kv_visual_count_pending(void) { return s_visual_digits > 0 || s_visual_gp; }
void      kv_visual_cancel(void) {
    // 与 kv_cancel() 等价：作废未完成输入（计数/操作符/g 前缀）+ 丢弃 repeat 记录。
    // s_visual_digits / s_visual_gp 由 reset_pending() 一并清零。
    kv_cancel();
}

void kv_set_mode(kv_mode_t m) { s_mode = m; vline_reset(); abort_input(); }
void kv_enable(void) { s_enabled = true; vline_reset(); abort_input(); s_mode = KV_MODE_INSERT; }
void kv_disable(void) { s_enabled = false; abort_input(); kv_emit_clear(); }

void kv_cancel(void) { abort_input(); }
