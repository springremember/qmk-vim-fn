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
#define REC_MAX 64
static kv_keycode_t s_rec[REC_MAX];
static bool         s_rec_iscnt[REC_MAX]; /* 该位是不是**计数位**（P2-2/D27，见 kc_is_count_digit） */
static int          s_rec_len;
static bool         s_rec_change; /* 本次录制里是否含「修改缓冲区」的命令 */
static bool         s_rec_overflow; /* 录制超 REC_MAX ⇒ 本次修改不得提交（§4.14 #6） */
static bool         s_ins_typed;    /* 本次插入里是否真的键入了字符（D26 补偿 Left 的判据） */
static int          s_ins_count;    /* 插入入口的计数（P2-3/D28，design §4.16；默认 1） */
static kv_keycode_t s_last[REC_MAX];
static bool         s_last_iscnt[REC_MAX];
static int          s_last_len;
static bool         s_replaying;
static int          s_last_cost;   /* 上一次回放实测发出的键数（P2-4：整次回放的预算判据） */

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
/* D22：宿主光标是否比 Vim 光标**右一列**。`v` 的预选把宿主光标放到 A+1；
 * 此后只把光标在垂直/词方向搬动、不重锚的动作（`j`/`k`/`w`/`e`/`$`）都保持这个 +1；
 * `0`/`^` 把宿主光标送到列 0（= Vim 光标）所以 +1 消失。Esc 只需要这一个比特
 * 就能补对落点，**不需要 lo/hi 偏移**（后者已被纵向动作作废为 s_v_abs）
 * —— 这正是 `vj<Esc>x` 此前删错字符的原因。 */
static bool s_v_rt1;

/* 字符级偏移上限（同 KV_VLINE_MAX_OFF 的理由）：单条命令的键码数受发送队列
 * EMIT_CAP=256 限制，溢出静默丢键。到上限后**拒绝继续扩展**（不发键），而不是
 * 发出错乱的重锚。计数 ≤99、|lo|/|hi| ≤100 ⇒ 单条命令 ≤~105 键。 */
#define KV_VCHAR_MAX_OFF 100

static void vchar_reset(void) {
    s_v_lo = 0; s_v_hi = 0; s_v_end_r = true; s_v_abs = true; s_v_word_ok = false;
    s_v_rt1 = false;
}

/* 进入字符级 VISUAL（`v` 已发 Shift+Right）：宿主选区 = 光标下 1 字符。 */
static void vchar_enter(void) {
    s_v_lo = 0; s_v_hi = 1; s_v_end_r = true; s_v_abs = false; s_v_word_ok = true;
    s_v_rt1 = true;
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
    /* 发送队列预算（design §4.4）：本函数随后至多发 n+3 键（重锚骨架 + 贴边键），
     * 按剩余预算截断 n —— 否则连续大计数行选动作会撑爆 256 格队列并静默丢键。 */
    n = kv_emit_clamp_n(n, 3, 1);
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
    /* h/l 之后光标落在**选区边界**上（右移=右端 ⇒ 仍是 Vim 光标+1；左移或向左重锚后
     * 落在最左端 = Vim 光标）。为稳妥统一按"不一定 +1"处理：右端情形由下面的
     * `!s_v_abs && s_v_end_r` 分支负责补 `←`，不依赖 s_v_rt1。 */
    s_v_rt1 = false;
    if (n < 1) n = 1;
    if (n > 99) n = 99;
    /* 发送队列预算（design §4.4）：本函数最多发 n+2 键（重锚路径的 Esc + Shift+方向），
     * 按剩余预算截断 n —— 否则连续大计数动作会把 256 格队列撑到顶并静默丢键。 */
    n = kv_emit_clamp_n(n, 2, 1);
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
                 * 从**正确列**起算词动作，最后 Shift+Right 把目标字符纳入半开选区。
                 * 每次 3 键：`v99w` = 297 键 > EMIT_CAP(256)，故按剩余预算截断计数
                 * （design §4.4：确定性截断，绝不静默丢键）。 */
                n = kv_emit_clamp_n(n, 0, 3);
                for (int i = 0; i < n; i++) kv_emit_visual_word_fwd_anchor();
            } else {
                n = kv_emit_clamp_n(n, 0, 1);
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
                n = kv_emit_clamp_n(n, 2, 1);   /* Esc, Shift+Left + 1×n */
                kv_emit_visual_word_back_anchor(n);
                s_v_end_r = false;    /* 锚点现在在右端：活动端变成左端 */
            } else {
                n = kv_emit_clamp_n(n, 0, 1);
                for (int i = 0; i < n; i++) kv_emit_visual_motion(kc);
            }
            s_v_abs = true; s_v_word_ok = false;
            return;
        case KV_0: case KV_C_CARET:
            if (!s_v_abs && s_v_end_r) kv_emit_visual_zero_from_right(s_v_hi - s_v_lo);
            else                       kv_emit_visual_motion(kc);
            s_v_abs = true; s_v_word_ok = false;
            s_v_rt1 = false;   /* 宿主光标到列 0 = Vim 光标，+1 消失 */
            return;
        case KV_C_DLR:
            /* 光标在左端时锚点在右端，直接 Shift+End 会漏掉锚字符 → 先重锚。 */
            if (!s_v_abs && !s_v_end_r) kv_emit_visual_dollar_from_left(s_v_hi - s_v_lo);
            kv_emit_visual_motion(KV_C_DLR);
            s_v_abs = true; s_v_word_ok = false;
            s_v_rt1 = true;    /* Shift+End 落在末字符**之后** = Vim $ 位置 +1 */
            return;
        case KV_K:
            /* D24：向上移动会保持宿主的 +1 列，于是选区左端（光标端）比 Vim
"
             * 右一列 ⇒ `vky`/`vkd` 的内容不符（VBLOCK，缓冲区/寄存器可见）。
"
             * 先 `Esc, Shift+Left` 把锚点翻到右端、宿主光标落到 **Vim 所在列**，
"
             * 再 Shift+Up×n 就与 Vim 完全一致。`j` 不需要：光标落在右端，宿主的
"
             * "末字符之后"约定正好等于 Vim 的闭区间。 */
            if (!s_v_abs && s_v_end_r) {
                kv_emit_tap(KV_ESC);
                kv_emit_tap(KV_LSFT_KC(KV_LEFT));
                s_v_rt1 = false;   /* 光标已在 Vim 所在列，+1 消失 */
            }
            n = kv_emit_clamp_n(n, 2, 1);   /* 至多 2 键骨架 + Shift+Up×n */
            for (int i = 0; i < n; i++) kv_emit_visual_motion(kc);
            s_v_abs = true; s_v_word_ok = false;
            return;
        case KV_J:
            /* 向下时光标落在右端：只作废 lo/hi 偏移，**保持** s_v_rt1。 */
            n = kv_emit_clamp_n(n, 0, 1);
            for (int i = 0; i < n; i++) kv_emit_visual_motion(kc);
            s_v_abs = true; s_v_word_ok = false;
            return;
        case KV_C_G:
        default:
            n = kv_emit_clamp_n(n, 0, 1);
            for (int i = 0; i < n; i++) kv_emit_visual_motion(kc);
            s_v_abs = true; s_v_word_ok = false;
            s_v_rt1 = false;   /* 绝对位置：无法保证 +1 */
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

/* 该键是不是**计数位**（P2-2/D27）：由按下它时解析器的状态决定。
 * 计数态（ST_CNT/ST_OPCNT/ST_ANGCnt）里的数字（含 `0`）都是计数；
 * ST_IDLE/ST_OP/ST_ANG 里的 `1..9` 是计数起始，而 `0` 是 `0` 动作（T_ZERO）——
 * **必须保留**：`d0` 的 `0` 若被当成计数删掉，`d03.` 会退化成待决的 `3d`（数据损坏）。 */
static bool kc_is_count_digit(kv_keycode_t kc, kv_state_t st_before) {
    switch (st_before) {
        case ST_CNT: case ST_OPCNT: case ST_ANGCnt:
            return kv_classify_digit(kc) == T_DIGIT;
        case ST_IDLE: case ST_OP: case ST_ANG:
            return kv_classify(kc) == T_COUNT;
        default:
            return false;
    }
}

/* 数字键码：1..9 → KV_1..KV_9，0 → KV_0（`N.` 的 N ≤ 99，最多两位）。 */
static kv_keycode_t digit_kc(int d) {
    return (d == 0) ? KV_0 : (kv_keycode_t)(KV_1 + d - 1);
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

static void rec_push(kv_keycode_t kc, bool is_cnt) {
    if (s_replaying) return;
    if (s_rec_len < REC_MAX) {
        s_rec[s_rec_len] = kc;
        s_rec_iscnt[s_rec_len] = is_cnt;
        s_rec_len++;
    } else {
        s_rec_overflow = true; /* 静默丢弃会损坏数据（P0-2）：记下溢出，提交时拒绝 */
    }
}

/* Only "change-like" commands are worth replaying with '.'.  Insert/visual
 * entries, prefixes that never complete, and '.' itself are excluded. */
static bool rec_should_record(kv_token_t t) {
    switch (t) {
        case T_COUNT: case T_OP: case T_INDENT: case T_g_LOWER: case T_Z_BIG:
        case T_MOTION: case T_ZERO: case T_CARET: case T_DOLLAR: case T_G_BIG:
        case T_S_BIG: case T_X: case T_XUP: case T_s: case T_C_BIG:
        case T_D_BIG: case T_Y_BIG: case T_P: case T_PUP: case T_JOIN:
        case T_INSERT: /* i I a A o O（design §4.14） */
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
        case T_JOIN: case T_S_BIG: case T_INSERT:
            /* T_INSERT 也算修改：实测 `xi<Esc>.` 时 `.` **没有**重复 x，说明空插入
             * 确实会成为 `.` 的目标（一个空操作）。design §4.14 订正。 */
            return true;
        default:
            return false;
    }
}

static void rec_commit(void) {
    if (s_replaying) return;
    if (s_rec_len > 0) {
        memcpy(s_last, s_rec, sizeof(kv_keycode_t) * (size_t)s_rec_len);
        memcpy(s_last_iscnt, s_rec_iscnt, sizeof(bool) * (size_t)s_rec_len);
        s_last_len = s_rec_len;
    }
    s_rec_len = 0;
    s_rec_change = false;
    s_rec_overflow = false;
    s_last_cost = 0;   /* 新目标 ⇒ 成本未知，下一次回放重新实测（P2-4） */
}

static void rec_clear(void) {
    s_rec_len = 0;
    s_rec_change = false;
    s_rec_overflow = false;
    s_ins_typed = false;
    s_ins_count = 1;
}

/* P2-3/D28（design §4.16 #5）：光标移动键。Vim 在插入期发生**真实移动**时取消计数重复
 * （`2iA<Left>B<Esc>`=`BA`），而 no-op 移动不取消（列 0 的 `2i<Left>AB<Esc>`=`ABAB`）。
 * 引擎读不到列号/行号，无法区分二者，故采用保守判据：录制里出现任一移动键就不重复
 * （残余 `ARROWCNT`）。`<BS>`/`<CR>`/`Tab`/`Del` 是编辑键，随文本重复。 */
static bool kc_is_cursor_move(kv_keycode_t kc) {
    switch (KV_BASIC(kc)) {
        case KV_LEFT: case KV_RGHT: case KV_UP: case KV_DOWN:
        case KV_HOME: case KV_END: case KV_PGUP: case KV_PGDN:
            return true;
        default:
            return false;
    }
}

/* 取出本次插入的「入口 + 键入文本」：活插入在 s_rec（提交前），回放在 s_last
 * （回放期 rec_push 短路、s_rec 为空）。跳过前缀计数位后，第一个非计数位就是入口。
 * 成功返回入口下标（文本 = keys[entry+1 .. *len-1]），失败返回 -1；*move 收是否含移动键。 */
static int rec_insert_entry(const kv_keycode_t **keys, int *len, bool *move) {
    const bool *iscnt;
    if (s_rec_len > 0) { *keys = s_rec; iscnt = s_rec_iscnt; *len = s_rec_len; }
    else if (s_last_len > 0) { *keys = s_last; iscnt = s_last_iscnt; *len = s_last_len; }
    else return -1;
    int e = 0;
    while (e < *len && iscnt[e]) e++;      /* 跳过前缀计数位（`Ni` 的 N） */
    if (e >= *len) return -1;
    *move = false;
    for (int i = e + 1; i < *len; i++) {
        if (kc_is_cursor_move((*keys)[i])) { *move = true; break; }
    }
    return e;
}

/* P2-3/D28（design §4.16 #6）：在提交点补发 (N-1) 次额外重复。
 * 每次重复的成本 per = 文本键数（`i/a/I/A`，入口不重发）或 入口键数 + 文本键数
 * （`o`/`O`，入口每次重发 = 每行一次）；按剩余预算夹取并给末尾那个 D26 `Left` 预留 1 格。
 * 每次重复都是**完整**的（绝不半截文本）；`pending ≤ 250 < 256`。 */
static void rec_emit_extra_insert_reps(int n) {
    const kv_keycode_t *keys;
    int len, entry;
    bool move;
    entry = rec_insert_entry(&keys, &len, &move);
    if (entry < 0 || move) return;         /* ARROWCNT：保守不重复 */
    const kv_keycode_t ekc = keys[entry];
    const bool line_open = (KV_BASIC(ekc) == KV_O);   /* o / O：每次重复重开一行 */
    const int ekeys = kv_emit_enter_insert_cost(ekc);
    const int text_len = len - entry - 1;
    if (text_len < 1) return;              /* 空文本：no-op */
    const int per = ekeys + text_len;
    int extra = n - 1;
    int room = kv_emit_room() - 1;         /* 预留末尾的 D26 Left */
    if (room < 0) room = 0;
    int maxextra = room / per;
    if (extra > maxextra) extra = maxextra;
    for (int r = 0; r < extra; r++) {
        if (line_open) kv_emit_enter_insert(ekc);
        for (int i = entry + 1; i < len; i++) kv_emit_tap(keys[i]);
    }
}

/* 离开 INSERT 时提交：整段插入（入口 + 键入）成为 `.` 的目标。
 * glue 的顺序是 kv_cancel(); kv_set_mode(); 而 kv_cancel() 会 rec_clear()
 * ⇒ 提交必须也在 kv_cancel() 里发生，否则录制在提交前被抹掉（D25 真因 B）。
 *
 * D26（design §4.15）：宿主没有模态，插入期宿主光标停在**插入文本之后**，而真实 Vim 离开插入时
 * 停在**最后一个插入字符**上 ⇒ 这里补**恰好一个** `Left`，否则紧跟的命令（含 `.` 的回放）会从
 * 右一列开始、把缓冲区改坏。只在 `s_ins_typed`（本次真的键入了非 Esc 字符）时补：宿主 `Left`
 * 在列 0 会**回绕到上一行行尾**，而空插入无从知道列号。补偿与「能否回放」无关：录制溢出导致
 * 不提交时也要补。rec_commit()/rec_clear() 都会清掉 s_ins_typed ⇒ glue 的
 * `kv_cancel(); kv_set_mode();` 两个调用点**只补一次**。
 *
 * P2-3/D28（design §4.16）：计数 `N` 加在插入入口上 ⇒ 先补发 (N-1) 次键入文本，再补那**一个**
 * `Left`（重复之间不补）。`REC_MAX` 溢出时文本已被截断，重复它会插入错误内容 ⇒ 不重复
 * （残余 `RECMAXCNT`），但 `Left` 仍补。 */
static void rec_commit_insert(void) {
    if (s_replaying) return;
    if (s_mode != KV_MODE_INSERT) return;
    if (s_ins_typed && s_ins_count > 1 && !s_rec_overflow)
        rec_emit_extra_insert_reps(s_ins_count);
    if (s_ins_typed) { kv_emit_tap(KV_LEFT); s_ins_typed = false; }
    if (s_rec_len > 0 && s_rec_change && !s_rec_overflow) rec_commit();
    else rec_clear();
    s_ins_count = 1;
}

/* Shared abort path for every mode/enable transition (design #4.7):
 * drop the in-progress state machine AND the in-progress repeat recording.
 * s_last is deliberately preserved so '.' can replay a completed command
 * across a mode round-trip. */
static void abort_input(void) {
    reset_pending();
    rec_clear();
}

/* 回放录制的键。strip_counts=true 时跳过计数位（P2-2/D27 的计数类 `N.` 用）。 */
static void rec_replay_keys(bool strip_counts) {
    for (int i = 0; i < s_last_len; i++) {
        if (strip_counts && s_last_iscnt[i]) continue;
        /* 真因 A：插入期录下的键必须由引擎**直接发进发射队列**。正常路径靠 kv_kbd() 返回
         * KV_PASSTHROUGH 让 glue 转发，但回放是引擎内部循环、没有 glue 参与，返回值会被
         * 丢掉 ⇒ 插入文本永远到不了宿主。 */
        if (s_mode == KV_MODE_INSERT) {
            /* D26：回放里被 tap 的插入文本也是「键入了字符」⇒ 回放结束（下面的
             * kv_set_mode(NORMAL)）自己补一个 `Left`，与首次执行逐键等价。 */
            if (s_rec_change) s_ins_typed = true;
            kv_emit_tap(s_last[i]);
            continue;
        }
        kv_kbd(s_last[i]);
    }
}

/* 回放一次录制（P2-3/D28 起插入类与计数类共用这一条路径）。返回本次回放是否键入了字符。 */
static bool rec_replay_ex(void) {
    if (s_replaying || s_last_len == 0) return false; /* never re-enter '.' */
    /* 预算（design §4.4 不变式 b）：**整次回放**也要受剩余预算约束。只靠各发射器各自的夹取
     * 不够 —— 一条命令由多个发射器组成，夹取之和仍可能超过 room（实测 `99dw` + 连续 `.`
     * 把 256 格队列顶满并静默丢键）。判据是确定性截断：上次实测成本放不下就**不重放**。 */
    if (s_last_cost > 0 && kv_emit_room() < s_last_cost) return false;
    const int before = kv_emit_pending();
    rec_clear();  /* 丢弃残留录制（例如 `. ` 前的计数），否则会在回放结束时被提交（P0-1） */
    s_replaying = true;
    rec_replay_keys(false);
    s_replaying = false;
    bool typed = false;
    /* 回放完若仍在 INSERT 必须回 NORMAL（真实 Vim 的 `.` 结束后停在 Normal）。
     * P2-3：计数插入的额外重复由 rec_commit_insert() 在这里发射，末尾恰好一个 Left。 */
    if (s_mode == KV_MODE_INSERT) {
        typed = s_ins_typed;
        kv_set_mode(KV_MODE_NORMAL);
    }
    s_last_cost = kv_emit_pending() - before;  /* 实测本次成本，供下一次回放夹取（P2-4） */
    s_rec_len = 0;        /* 回放不产生新的录制（rec_push/rec_commit 在回放期已短路） */
    s_rec_change = false;
    return typed;
}

static void rec_replay(void) { (void)rec_replay_ex(); }

/* `N.` 的精确语义（design §4.14 #3，P2-2/D27 + P2-3/D28）。插入类与计数类**统一**为：
 *   删掉录制里**所有计数位**，前缀 N，重放**一次** —— 等价于把原命令的计数换成 N 再执行一遍
 *   （`2x3.` ≡ `3x`，寄存器是 `cde`；`iAB<Esc>2.` ≡ `2iAB` = `AABABB`）。
 *   插入类的额外重复（N 次键入文本）由 `rec_commit_insert()` 在回放的提交点发射，末尾恰好一个
 *   D26 `Left`；`o`/`O` 每次重复重发入口。键码预算（design §4.4）：整条退化成一条 ≤99 的单命令，
 *   由提交点的夹取与各发射器共同保证 256 格队列不到顶。 */
/* P1-3：`N.` 执行后要把 `s_last` 的计数改成 N。真实 Vim 里 `.` 重复的是「上一次**修改**」，
 * 而 `3.` 本身已经是一次**带计数 3** 的修改 ⇒ 紧跟的裸 `.` 应重复 `3x`，不是原来的 `2x`。
 * 实测：`2x3..` = 8 删（2+3+3），修前引擎 7（2+3+2）；`x3..` = 7 vs 5；`iAB<Esc>2..` = 5 次插入。 */
static void rec_set_last_count(int n) {
    if (n < 1 || s_last_len <= 0) return;
    kv_keycode_t buf[REC_MAX];
    bool         cnt[REC_MAX];
    int len = 0;
    if (n >= 10) { buf[len] = digit_kc(n / 10); cnt[len++] = true; }
    if (n > 1)   { buf[len] = digit_kc(n % 10); cnt[len++] = true; }  /* n==1：裸命令，不写计数 */
    for (int i = 0; i < s_last_len && len < REC_MAX; i++) {
        if (s_last_iscnt[i]) continue;            /* 丢掉旧计数位 */
        buf[len] = s_last[i];
        cnt[len++] = false;
    }
    memcpy(s_last, buf, sizeof(kv_keycode_t) * (size_t)len);
    memcpy(s_last_iscnt, cnt, sizeof(bool) * (size_t)len);
    s_last_len  = len;
    /* 注意：**不要**在这里清 s_last_cost —— 清了就等于关掉 P2-4 的整次回放预算守卫
     * （实测会让 `99dw` 连按 `. ` 的 pending 重新越过 250）。成本由调用方按本次回放实测更新。 */
}

static void rec_replay_n(int n) {
    if (s_last_len <= 0 || n < 1) return;
    /* 插入类与计数类共用：喂入 N 作为新的前缀计数，再回放**去掉计数位**的录制（重放一次）。
     * 预算（design §4.4 不变式 b，P1-1）：**整次回放**同样受剩余预算约束 —— 只靠各发射器
     * 各自的夹取不够（一条命令由多个发射器组成，夹取之和仍可超 room；实测从空队列
     * `x` 后连按 `99.` 可把 256 格队列顶满并丢键）。与裸 `. ` 同一判据：放不下就不重放。 */
    if (s_last_cost > 0 && kv_emit_room() < s_last_cost) return;
    const int before = kv_emit_pending();
    rec_clear();
    s_replaying = true;
    if (n >= 10) kv_kbd(digit_kc(n / 10));
    kv_kbd(digit_kc(n % 10));
    rec_replay_keys(true);
    s_replaying = false;
    if (s_mode == KV_MODE_INSERT) kv_set_mode(KV_MODE_NORMAL);
    if (kv_emit_pending() - before > s_last_cost) s_last_cost = kv_emit_pending() - before; /* 取最大，保守 */
    rec_set_last_count(n);   /* P1-3：`2x3..` ≡ `2x` + `3x` + `3x` = 8 删 */
    s_rec_len = 0;
    s_rec_change = false;
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

    /* 真实 Vim：`.` 不是合法的 motion/文本对象 ⇒ 待决的操作符/前缀被**中止**，且这个 `.`
     * 被**丢弃**（不重复上一次修改）。若走 §4.5 的严格清空→在 IDLE 重新识别，`. ` 会被当成
     * 重复命令执行；D25 让插入类也能回放后，这会把缓冲区改坏（实测 `iAB<Esc>d.`：引擎
     * `AABB…`、Vim `AB…`；`y./g./>./<./Z./c./2d./d2.` 同类共 9 例）。
     * 纯计数态（ST_CNT）除外：`2.` 是合法的「带计数的重复」，必须继续走重新识别。 */
    /* 用 kv_classify（**不**经计数态的 digit 覆盖）判「是不是 `.`」：`>` 在 QMK 里是
     * Shift+`.`，KV_BASIC 会把它也认成 KV_DOT，用 KV_BASIC 会误伤 `>>`（实测）。 */
    if (kv_classify(kc) == T_REPEAT && s_state != ST_IDLE && s_state != ST_CNT) {
        reset_pending();
        return R_CONSUMED;
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
                case T_INSERT: kv_emit_enter_insert(kc); s_mode = KV_MODE_INSERT; s_ins_count = 1; return R_CONSUMED;
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
                case T_UNDO: { /* 每次 1 键；按剩余预算截断（design §4.4） */
                    n = kv_emit_clamp_n(n, 0, 1);
                    for (i = 0; i < n; i++) kv_emit_undo();
                    reset_pending(); return R_CONSUMED;
                }
                case T_D_BIG: kv_emit_delete_to_eol_n(n);                       reset_pending(); return R_CONSUMED;
                case T_C_BIG: kv_emit_change_to_eol_n(n); s_mode = KV_MODE_INSERT; reset_pending(); return R_CONSUMED;
                case T_Y_BIG: kv_emit_line_op(KV_Y, n);                         reset_pending(); return R_CONSUMED; /* Y ≡ yy */
                case T_S_BIG:  kv_emit_line_op(KV_C, n); s_mode = KV_MODE_INSERT; reset_pending(); return R_CONSUMED;
                /* P2-3/D28（design §4.16）：插入入口的计数 N ⇒ 键入文本重复 N 次。 */
                case T_INSERT: kv_emit_enter_insert(kc); s_mode = KV_MODE_INSERT; s_ins_count = n; reset_pending(); return R_CONSUMED;
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
        /* D20：真实 Vim 的可视 Esc 取消选区后把光标留在**活动端**（= 它原本所在
         * 的那个字符上）；而宿主的光标在最后一个选中字符**之后**，
         * 所以只差一格（cur_end=R 时补一个 `←`；cur_end=L 时两者已重合）。
         * 不补的话 `vll<Esc>x` 会删到下一个字符（缓冲区可见）。 */
        if (vline) {
            /* D23：Vim 的**行可视** Esc 把光标送到**选区起点的行首**（最上行、列 0）；
             * 宿主停在活动端行的**行尾**，紧接 `x`/`s` 会删掉换行并**并行**
             * （`V<Esc>x`@L1 模型 `L1L2\n…`、Vim `1\nL2…`，缓冲区可见）。
             * DOWN 态（锚在 A 行）需 `Up×off`；UP 态光标本就在最上行。 */
            if (!s_vl_abs) {
                /* 预算（design §4.4）：Up×off 至多 100 键；这里可以**少发到 0**（不是带计数
                 * 命令的"至少 1 次"），预算里预扣随后的 Home 与 Esc 两个固定键。 */
                if (!s_vl_up && s_vl_off > 0) {
                    int up = s_vl_off, room = kv_emit_room() - 2;   /* 预留 Home, Esc */
                    if (room < 0) room = 0;
                    if (up > room) up = room;
                    if (up > 0) kv_emit_taps(KV_UP, up);
                }
                kv_emit_tap(KV_HOME);
            }
        } else if (s_v_rt1 || (!s_v_abs && s_v_end_r)) kv_emit_tap(KV_LEFT);
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
        if (vline) kv_emit_vline_action(KV_Y, s_vl_up);
        else {
            kv_emit_visual_yank();
            /* D20：真实 Vim 的可视 `y` 把光标留在选区起点（最左端），
             * 否则 `vlyp` 会粘到错位置（缓冲区可见）。 */
            if (!s_v_abs && s_v_end_r) kv_emit_taps(KV_LEFT, s_v_hi - s_v_lo);
        }
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
            /* 插入期键入的每个键都是这次「修改」的一部分（`. ` 重放整段插入）。 */
            /* D26：只有**引擎发起**的插入（`s_rec_change` = 录制里有插入入口，即 `i/a/o/s/C/cc…`）
             * 才需要补偿 `Left`；经 Esc/开机路径进入的透传插入不是 Vim 插入，补 `Left` 会把一个
             * 光标键发给宿主（shell/编辑器），故不置位。Esc 本身也不算键入字符。 */
            if (KV_BASIC(kc) != KV_ESC && s_rec_change) s_ins_typed = true;
            rec_push(kc, false);   /* 插入期键入的字符不是计数位 */
            /* 这里**不置** s_rec_change：「是不是修改」由插入入口（T_INSERT）决定。
             * 若在此置位，经 Esc/开机路径进入的插入（录制里没有入口标记）会被提交，
             * 回放时那些字符会被当成**普通模式命令**执行（例如 `X` 变成删字符）——
             * 那是数据损坏，比"不回放"更糟。故该类插入不成为 `.` 目标（已知限制）。 */
            return KV_PASSTHROUGH;
        }
        if (s_mode >= KV_MODE_MOUSE) return KV_PASSTHROUGH;

        if (s_mode == KV_MODE_VISUAL || s_mode == KV_MODE_VISUAL_LINE) {
            feed_visual(kc);
            return KV_CONSUMED;
        }

        const kv_state_t st_before = s_state;   /* 判定本键是不是计数位（P2-2/D27） */
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
                rec_push(kc, kc_is_count_digit(kc, st_before));
            }
            /* 命令结束（回到 Idle）才提交：只有含「修改」的录制才成为 `.` 的目标，
             * 否则**丢弃本次录制**（s_last 保留）—— 裸移动/复制不得夺走 `.` 的目标。 */
            if (s_state == ST_IDLE && s_rec_len > 0 && s_mode != KV_MODE_INSERT) {
                if (s_rec_change) rec_commit(); else rec_clear();
            }
            /* 进入 INSERT 时**不**清录制：留到退出插入时提交（design §4.14）。 */
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

void kv_set_mode(kv_mode_t m) {
    /* 只在**明确回到 NORMAL** 时提交：切到 MOUSE（鼠标层）不得把插入从中间劈开并
     * 吞掉后半段（P1-2）。design §4.14：提交点是 INSERT→NORMAL 转换。 */
    if (m == KV_MODE_NORMAL) rec_commit_insert();
    s_mode = m;
    vline_reset();
    abort_input();
}
void kv_enable(void) { s_enabled = true; vline_reset(); abort_input(); s_mode = KV_MODE_INSERT; }
void kv_disable(void) { s_enabled = false; abort_input(); kv_emit_clear(); }

void kv_cancel(void) {
    rec_commit_insert(); /* glue 先调 kv_cancel() 再 kv_set_mode()：提交必须在这里发生 */
    abort_input();
}
