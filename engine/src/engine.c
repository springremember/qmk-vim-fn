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
static bool         s_rec_change; /* 本次录制里是否含「修改缓冲区」的命令 */
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

/* 字符级 VISUAL 选区状态（design §4.9，缺陷 D1/D12）。宿主选区是半开区间
 * [A+lo, A+hi)，A = 按 `v` 时所在列（引擎无需知道 A 的绝对值，全部用相对偏移）。
 *   s_v_end_r   : true = 宿主光标在 hi 端（Vim 光标 = hi−1）；false = 在 lo 端（= lo）。
 *   s_v_abs     : 偏移已失效（0/^/$/G/词/纵向等"目标列依赖文本"的动作之后）→
 *                 回退到旧的"每个动作一个 Shift+方向"，绝不发出错误的重锚。
 *   s_v_word_ok : 宿主光标恰在 Vim 光标右侧一格、且锚点即 Vim 锚点（D12 词动作
 *                 重锚序列 `Shift+Left, Ctrl+Shift+Right, Shift+Right` 的前提）。
 * 为什么必须有：`v` 的预选把宿主光标放到 A+1，向左越过锚点时宿主 Shift+Left
 * 会把半开选区塌成空 → `Ctrl+X` 退化成"剪切整行"（数据损坏）。 */
static int  s_v_lo;
static int  s_v_hi;
static bool s_v_end_r;
static bool s_v_abs;
static bool s_v_word_ok;

/* 字符级偏移上限（同 KV_VLINE_MAX_OFF 的理由）：单条命令的键码数受发送队列
 * EMIT_CAP=256 限制，溢出静默丢键。到上限后**拒绝继续扩展**（不发键），而不是
 * 发出错乱的重锚。计数 ≤99、|lo|/|hi| ≤100 ⇒ 单条命令 ≤~105 键。 */
#define KV_VCHAR_MAX_OFF 100

static void vchar_reset(void) {
    s_v_lo = 0; s_v_hi = 0; s_v_end_r = true; s_v_abs = true; s_v_word_ok = false;
}

/* 进入字符级 VISUAL（`v` 已发 Shift+Right）：宿主选区 = 光标下 1 字符。 */
static void vchar_enter(void) {
    s_v_lo = 0; s_v_hi = 1; s_v_end_r = true; s_v_abs = false; s_v_word_ok = true;
}

/* 重置两套可视状态。所有进入/离开可视、模式切换、kv_init、V/v 切换都经此
 * （design §4.9）：字符级偏移若不同步清掉，会把上一次的选区状态泄漏给下一次。 */
static void vline_reset(void) {
    s_vl_off = 0; s_vl_up = false; s_vl_abs = false;
    vchar_reset();
}

/* 行选移动：把 off 从 s_vl_off 变成 s_vl_off±n，并让宿主字符选区**始终覆盖整行**。
 * 重锚只在方向翻转时发生（键数受当前 |off| 限制），不做全量重建。 */
static void vline_move(bool up, int n) {
    if (n < 1) n = 1;
    if (n > 99) n = 99;
    if (s_vl_abs) {                       /* 行号未知：只做纵向扩展 + 按活动端所在边界收边 */
        kv_emit_vline_move(up, n);
        if (s_vl_up) kv_emit_vline_move_head();   /* gg 之后：活动端是上边界 → 贴行首 */
        else         kv_emit_vline_move_tail();   /* G  之后：活动端是下边界 → 贴行尾 */
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

/* ---- 字符级 VISUAL 移动（design §4.9，缺陷 D1）-------------------------------
 * 宿主半开选区 [lo,hi)（相对 A），光标在 hi（end_r）或 lo（!end_r）。
 * 向右/向左移动 n 时：
 *   同向（光标所在端继续外扩）→ 直接 Shift+方向 ×n，偏移同步 ±n。
 *   反向但**不越过锚点**   → 直接 Shift+方向 ×n（宿主会收缩选区）。
 *   反向**越过锚点**       → 必须重锚：`Esc` 把选区塌到活动端（= 光标），
 *      用不带 Shift 的方向键把光标移到"锚点外侧一格"，再 Shift+方向 扩到目标。
 *      否则宿主 Shift+方向 会把半开选区塌成空 → Ctrl+X 退化成剪切整行（数据损坏）。
 * 偏移超过 KV_VCHAR_MAX_OFF 时**拒绝扩展**（不发键、不改状态），与 vline_move 同。 */
static void vchar_move(bool right, int n) {
    if (n < 1) n = 1;
    if (n > 99) n = 99;
    if (s_v_abs) {                        /* 偏移已失效：旧的"每步一个 Shift+方向" */
        for (int i = 0; i < n; i++) kv_emit_visual_motion(right ? KV_L : KV_H);
        return;
    }
    const int lo = s_v_lo, hi = s_v_hi, w = hi - lo;
    if (right) {
        if (s_v_end_r) {                  /* 光标在右端：直接向右扩 */
            if (hi + n > KV_VCHAR_MAX_OFF) return;   /* 到上限：拒绝扩展 */
            for (int i = 0; i < n; i++) kv_emit_visual_motion(KV_L);
            s_v_hi = hi + n;
        } else if (lo + n < hi) {         /* 光标在左端、尚未越过锚点：直接收缩 */
            for (int i = 0; i < n; i++) kv_emit_visual_motion(KV_L);
            s_v_lo = lo + n;
        } else {                          /* 越过锚点：重锚到"光标在右端" */
            const int nhi = hi + n - w + 1;
            if (nhi > KV_VCHAR_MAX_OFF) return;
            kv_emit_visual_reanchor_right(w, n);
            s_v_lo = hi - 1;
            s_v_hi = nhi;
            s_v_end_r = true;
        }
    } else {
        if (!s_v_end_r) {                 /* 光标在左端：直接向左扩 */
            if (lo - n < -KV_VCHAR_MAX_OFF) return;
            for (int i = 0; i < n; i++) kv_emit_visual_motion(KV_H);
            s_v_lo = lo - n;
        } else if (hi - n > lo) {         /* 光标在右端、尚未越过锚点：直接收缩 */
            for (int i = 0; i < n; i++) kv_emit_visual_motion(KV_H);
            s_v_hi = hi - n;
        } else {                          /* 越过锚点：重锚到"光标在左端" */
            const int nlo = hi - 1 - n;
            if (nlo < -KV_VCHAR_MAX_OFF) return;
            kv_emit_visual_reanchor_left(w, n);
            s_v_hi = lo + 1;
            s_v_lo = nlo;
            s_v_end_r = false;
        }
    }
    /* 只有"光标在右端"时宿主光标才恰在 Vim 光标右侧一格（D12 词动作的前提）。 */
    s_v_word_ok = s_v_end_r;
}

/* 字符级 VISUAL 移动分派。h/l 走偏移状态机；词/纵向/0/^/$/G 的目标列依赖文本，
 * 无法用相对偏移建模 —— 按 design §4.9 回退到旧的"每步一个 Shift+方向"，并把偏移
 * 标记为失效（绝不发错误的重锚）。词前向（w/e/W/E）与 0/^/$ 在状态仍可信时做一次
 * 重锚，修正 D12 / 左端包含锚字符的差一。 */
static void vchar_motion(kv_keycode_t kc, int n) {
    switch (kc) {
        case KV_H: vchar_move(false, n); return;
        case KV_L: vchar_move(true,  n); return;
        case KV_W: case KV_E: case KV_C_W: case KV_C_E:
            if (s_v_word_ok) {
                /* D12：先 Shift+Left 把宿主光标移到 Vim 光标列，再 Ctrl+Shift+Right
                 * 从**正确列**起算词动作，最后 Shift+Right 把目标字符纳入半开选区。 */
                for (int i = 0; i < n; i++) kv_emit_visual_word_fwd_anchor();
            } else {
                for (int i = 0; i < n; i++) kv_emit_visual_motion(kc);
            }
            s_v_abs = true;   /* 目标列未知：后续 h/l 回退 */
            return;
        case KV_B: case KV_C_B:
            /* D19：后向词动作。宿主光标在**右端**时（刚 `v` 或刚向右扩选），直接
             * Ctrl+Shift+Left 会从 c+1 回到 c（= 锚点）→ 选区塌成空 → Ctrl+X 剪切整行。
             * 此时先 `Esc, Shift+Left` 把锚点挪到右端、宿主光标落到 hi−1 = Vim 光标，
             * 再 Ctrl+Shift+Left×n，与真实 Vim 逐字一致（实测 vbd/v2bd/vlbd 各列一致）。
             * 宿主光标已在**左端**时（上一动作是向左/词动作已越锚），锚点本就在右端，
             * 直接 Ctrl+Shift+Left×n 才是正确且不塌的。 */
            if (!s_v_abs && s_v_end_r) {
                kv_emit_visual_word_back_anchor(n);
                s_v_end_r = false;    /* 锚点现在在右端：活动端变成左端 */
            } else {
                for (int i = 0; i < n; i++) kv_emit_visual_motion(kc);
            }
            s_v_abs = true; s_v_word_ok = false;
            return;
        case KV_0: case KV_C_CARET:
            if (!s_v_abs && s_v_end_r) kv_emit_visual_zero_from_right(s_v_hi - s_v_lo);
            else                       kv_emit_visual_motion(kc);
            s_v_abs = true; s_v_word_ok = false;
            return;
        case KV_C_DLR:
            /* 光标在左端时锚点在右端，直接 Shift+End 会漏掉锚字符 → 先重锚。 */
            if (!s_v_abs && !s_v_end_r) kv_emit_visual_dollar_from_left(s_v_hi - s_v_lo);
            kv_emit_visual_motion(KV_C_DLR);
            s_v_abs = true; s_v_word_ok = false;
            return;
        case KV_J: case KV_K: case KV_C_G:
        default:
            for (int i = 0; i < n; i++) kv_emit_visual_motion(kc);
            s_v_abs = true; s_v_word_ok = false;
            return;
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

/* 只有「修改缓冲区」的命令才配得上 `.`（真实 Vim 的 `.` 重复上一次**修改**）：
 * 裸移动（w/j/gg/G/计数）与纯复制（y/Y）都不改变 `.` 的目标。 */
static bool rec_is_change(kv_token_t t, kv_keycode_t kc) {
    switch (t) {
        case T_OP:     return kc == KV_D || kc == KV_C;   /* y 是复制，不是修改 */
        case T_INDENT: case T_X: case T_XUP: case T_s:
        case T_C_BIG: case T_D_BIG: case T_P: case T_PUP:
        case T_JOIN: case T_S_BIG:
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
    s_rec_change = false;
}

static void rec_clear(void) { s_rec_len = 0; s_rec_change = false; }

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
    s_rec_len = 0;        /* 回放不产生新的录制（rec_push/rec_commit 在回放期已短路） */
    s_rec_change = false;
}

/* `N.` 重复 N 次（真实 Vim：dw 后 3. 连删 3 个词）。总键码数必须留在发送队列内：
 * emit 队列只有 EMIT_CAP=256 格且溢出**静默丢键**，故封顶在 99 键
 * （与 Normal 的"2 位计数 ≤99"语义一致）。
 * 封顶不能用"录制键数"估：`dd` 只录 2 键却发 5 键，`99dw` 录 3 键却发 100 键。
 * 因此先回放一次、量出本次命令**实际**发出的键数，再据此决定还能重复几次。 */
static void rec_replay_n(int n) {
    if (s_last_len <= 0 || n < 1) return;
    const int before = kv_emit_pending();
    rec_replay();
    const int per = kv_emit_pending() - before;   /* 单次回放实际发出的键数 */
    if (per < 1) return;                          /* 该命令不发键（例如被吞掉） */
    int maxrep = 99 / per;
    if (maxrep < 1) maxrep = 1;
    for (int i = 1; i < n && i < maxrep; i++) rec_replay();
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
        case T_Y_BIG:  kv_emit_line_op(KV_Y, 1); break;  /* 真实 Vim 的 Y ≡ yy（行级） */
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
                    else { kv_emit_visual_enter(); vchar_enter(); } /* v：选中光标下 1 字符 + 偏移状态 */
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
            int i;
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
                /* 先清 ctx 再回放：否则刚吃的计数会漏进被回放的命令（`3.` 会变成回放 `3dw`） */
                case T_REPEAT: reset_pending(); rec_replay_n(n); return R_CONSUMED; /* N. = 重复 N 次 */
                /* 计数作用于单键编辑命令（真实 Vim：3x 删 3 字符、3p 粘 3 次、3J 连 3 行…）。
                 * x/X/s 用**一次**选中 N 字符的版本：寄存器拿到全部 N 个（审查 P0-6），
                 * 键码 N+1 而非 3N（`99X` = 297 键会撑爆 256 格队列 = 静默丢键，审查 P0-1）。
                 * p/P 只定位一次再 Ctrl+V×N（逐个定位会把副本交错插入，审查 P0-5）。 */
                case T_X:     kv_emit_delete_char_n(n);    reset_pending(); return R_CONSUMED;
                case T_XUP:   kv_emit_backspace_char_n(n); reset_pending(); return R_CONSUMED;
                case T_s:     kv_emit_substitute_n(n);     s_mode = KV_MODE_INSERT; reset_pending(); return R_CONSUMED;
                case T_P:     kv_emit_paste_n(false, n);   reset_pending(); return R_CONSUMED;
                case T_PUP:   kv_emit_paste_n(true, n);    reset_pending(); return R_CONSUMED;
                case T_JOIN:  kv_emit_join_n(n);           reset_pending(); return R_CONSUMED; /* NJ 连 N-1 次 */
                case T_UNDO:  for (i = 0; i < n; i++) kv_emit_undo();           reset_pending(); return R_CONSUMED;
                case T_D_BIG: kv_emit_delete_to_eol_n(n);                       reset_pending(); return R_CONSUMED;
                case T_C_BIG: kv_emit_change_to_eol_n(n); s_mode = KV_MODE_INSERT; reset_pending(); return R_CONSUMED;
                case T_Y_BIG: kv_emit_line_op(KV_Y, n);                         reset_pending(); return R_CONSUMED; /* Y ≡ yy */
                case T_S_BIG:  kv_emit_line_op(KV_C, n); s_mode = KV_MODE_INSERT; reset_pending(); return R_CONSUMED;
                case T_INSERT: kv_emit_enter_insert(kc); s_mode = KV_MODE_INSERT; reset_pending(); return R_CONSUMED;
                case T_VISUAL:
                    s_mode = (kc == KV_C_V) ? KV_MODE_VISUAL_LINE : KV_MODE_VISUAL;
                    vline_reset();
                    if (s_mode == KV_MODE_VISUAL_LINE) kv_emit_visual_line_enter(); /* 选中整行 */
                    else { kv_emit_visual_enter(); vchar_enter(); } /* v：选中光标下 1 字符 + 偏移状态 */
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
        kv_emit_tap(KV_ESC);   /* 字符级也要取消宿主选区（D13）：否则下一个键会替换整个选区 */
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
                s_v_abs = true; s_v_word_ok = false;   /* gg 是绝对位置：偏移失效 */
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
        if (vline) kv_emit_vline_action(KV_D, s_vl_up); else kv_emit_visual_cut();
        s_mode = KV_MODE_NORMAL; vline_reset(); reset_pending(); return R_CONSUMED;
    }
    if (kc == KV_Y) {
        if (vline) kv_emit_vline_action(KV_Y, s_vl_up); else kv_emit_visual_yank();
        s_mode = KV_MODE_NORMAL; vline_reset(); reset_pending(); return R_CONSUMED;
    }
    if (kc == KV_C) {
        if (vline) kv_emit_vline_action(KV_C, s_vl_up); else kv_emit_visual_change();
        s_mode = KV_MODE_INSERT; vline_reset(); reset_pending(); return R_CONSUMED;
    }
    if (kc == KV_S) {
        /* 真实 Vim：V s ≡ V c（删整行 + 留一个空行 + Insert），不再只删 1 字符。 */
        if (vline) kv_emit_vline_action(KV_C, s_vl_up); else kv_emit_visual_change(); /* Vim: 字符级 s ≡ c */
        s_mode = KV_MODE_INSERT; vline_reset(); reset_pending(); return R_CONSUMED;
    }
    /* 真实 Vim 的可视模式里 `p` 与 `P` **同义**（都用寄存器覆盖选区）；旧实现只匹配小写
     * `p`，`P` 落到"非法键 → 吞掉"，`vllP`/`VjP` 什么都不做（独立审查 P1-6）。 */
    if (kc == KV_P || kc == KV_C_P) {
        if (vline) kv_emit_vline_action(KV_P, s_vl_up); else kv_emit_visual_paste();
        s_mode = KV_MODE_NORMAL; vline_reset(); reset_pending(); return R_CONSUMED;
    }
    if (t == T_VISUAL) {
        /* 真实 Vim：VISUAL 内按 V 切到行选；VISUAL_LINE 内按 v 切回字符选（不发键）。 */
        if (vline) {
            /* Vim 的 `Vv` 切回字符选并立刻选中光标下 1 字符 */
            if (kc == KV_V) { s_mode = KV_MODE_VISUAL; vline_reset(); kv_emit_visual_enter(); vchar_enter(); }
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
                vchar_motion(kc, n);  /* 字符级：偏移状态机 + 重锚（D1/D12） */
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
                vchar_motion(KV_C_G, 1);  /* G 是绝对位置：偏移失效（design §4.9） */
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
    kv_emit_reset_reg();   /* 无名寄存器类型复位（design §4.4 p/P 定位） */
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
                if (rec_is_change(kv_classify(kc), kc)) s_rec_change = true;
                rec_push(kc);
            }
            /* 命令结束（回到 Idle）才提交：只有含「修改」的录制才成为 `.` 的目标，
             * 否则**丢弃本次录制**（s_last 保留）—— 裸移动/复制不得夺走 `.` 的目标。 */
            if (s_state == ST_IDLE && s_rec_len > 0) {
                if (s_rec_change) rec_commit(); else rec_clear();
            }
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
