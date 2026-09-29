#include "command.h"
#include "emit.h"

static void emit_motion_once(kv_motion_t m) {
    switch (m) {
        case M_H:      kv_emit_tap(KV_LEFT);  break;
        case M_J:      kv_emit_tap(KV_DOWN);  break;
        case M_K:      kv_emit_tap(KV_UP);    break;
        case M_L:      kv_emit_tap(KV_RGHT);  break;
        case M_W:
        case M_WBIG:
        case M_E:
        case M_EBIG:   kv_emit_tap(KV_LCTL_KC(KV_RGHT)); break;
        case M_B:
        case M_BBIG:   kv_emit_tap(KV_LCTL_KC(KV_LEFT)); break;
        case M_ZERO:
        case M_CARET:  kv_emit_tap(KV_HOME);  break;
        case M_DOLLAR: kv_emit_tap(KV_END);   break;
        case M_G_BIG:  kv_emit_tap(KV_LCTL_KC(KV_END));  break;
        case M_GG:     kv_emit_tap(KV_LCTL_KC(KV_HOME)); break;
        default: break;
    }
}

void kv_emit_motion(kv_motion_t m, int n) {
    if (n < 1) n = 1;
    for (int i = 0; i < n; i++) emit_motion_once(m);
}

/* Emit the shifted selection keys for an operator range. */
static void emit_op_range(kv_motion_t m, int n) {
    kv_keycode_t sel;
    switch (m) {
        case M_H:      sel = KV_LSFT_KC(KV_LEFT);  break;
        case M_J:      sel = KV_LSFT_KC(KV_DOWN);  break;
        case M_K:      sel = KV_LSFT_KC(KV_UP);    break;
        case M_L:      sel = KV_LSFT_KC(KV_RGHT);  break;
        case M_W:
        case M_WBIG:
        case M_E:
        case M_EBIG:   sel = KV_CS(KV_RGHT);       break;
        case M_B:
        case M_BBIG:   sel = KV_CS(KV_LEFT);       break;
        case M_ZERO:
        case M_CARET:  sel = KV_LSFT_KC(KV_HOME);  break;
        case M_DOLLAR: sel = KV_LSFT_KC(KV_END);   break;
        case M_G_BIG:  sel = KV_CS(KV_END);        break;
        case M_GG:     sel = KV_CS(KV_HOME);       break;
        default:       return;
    }
    /* 行选动作（j/k/G/gg）在 Vim 里是**整行**操作：先把光标移到行首再扩选。
     * `dj` = 当前行 + 下一行 = 2 行（即 n+1 行），`2dj` = 3 行；`dG`/`dgg` 到文档端。
     * 旧实现从**当前列**开始 Shift+Down，实际切掉"上一行尾部 + 下一行头部"（数据损坏）。 */
    if (m == M_J || m == M_K) {
        kv_emit_tap(KV_HOME);
        kv_emit_taps(sel, n + 1);
        return;
    }
    if (m == M_G_BIG || m == M_GG) {
        kv_emit_tap(KV_HOME);
        kv_emit_taps(sel, 1);
        return;
    }
    kv_emit_taps(sel, n);
}

void kv_emit_op_motion(kv_keycode_t op, kv_motion_t m, int n) {
    if (n < 1) n = 1;
    emit_op_range(m, n);
    if (op == KV_C) {
        kv_emit_tap(KV_LCTL_KC(KV_X));
        kv_emit_enter_insert(KV_I);
    } else if (op == KV_Y) {
        kv_emit_tap(KV_LCTL_KC(KV_C));
        kv_emit_tap(KV_ESC);   /* 复制后取消宿主残留选区 */
    } else {
        kv_emit_tap(KV_LCTL_KC(KV_X));
    }
}

void kv_emit_line_op(kv_keycode_t op, int n) {
    if (n < 1) n = 1;
    if (op == KV_Y) {
        kv_emit_tap(KV_HOME);
        kv_emit_tap(KV_HOME);
        kv_emit_taps(KV_LSFT_KC(KV_DOWN), n);
        kv_emit_tap(KV_LCTL_KC(KV_C));
        kv_emit_tap(KV_ESC);   /* 取消宿主残留选区（否则下一个键会替换刚复制的内容） */
        return;
    }
    kv_emit_tap(KV_HOME);
    kv_emit_tap(KV_HOME);
    kv_emit_tap(KV_LSFT_KC(KV_END));
    if (n > 1) kv_emit_taps(KV_LSFT_KC(KV_DOWN), n - 1);
    kv_emit_tap(KV_LCTL_KC(KV_X));
    /* `cc`/`S` 必须**留一个空行**（真实 Vim：L1|L2|L3 上 cc => L1||L3、2cc => L1||L4）。
     * 只有 `dd` 才补 Backspace 把整行并掉（末行也因此可删）。 */
    if (op != KV_C) kv_emit_tap(KV_BSPC);
    if (op == KV_C) kv_emit_enter_insert(KV_I);
}

void kv_emit_indent_motion(kv_keycode_t ang, kv_motion_t m, int n) {
    if (n < 1) n = 1;
    if (m == M_ZERO || m == M_CARET) {
        kv_emit_tap(ang == KV_C_GT ? KV_TAB : KV_LSFT_KC(KV_TAB));
        return;
    }
    emit_op_range(m, n);
    kv_emit_tap(ang == KV_C_GT ? KV_TAB : KV_LSFT_KC(KV_TAB));
}

void kv_emit_indent_line(kv_keycode_t ang, int n) {
    if (n < 1) n = 1;
    /* select n lines (Home, Home, Shift+Down x (n-1)) then indent/outdent */
    kv_emit_tap(KV_HOME);
    kv_emit_tap(KV_HOME);
    if (n > 1) kv_emit_taps(KV_LSFT_KC(KV_DOWN), n - 1);
    kv_emit_tap(ang == KV_C_GT ? KV_TAB : KV_LSFT_KC(KV_TAB));
}

/* design §4.9 VISUAL_LINE（v2）—— 与真实 Vim 行选对齐，动作方向无关。
 * 旧版"锚点固定在被选首行行首 + 字符级 Shift 扩展"在活动端越过锚点（k/b/gg）时会退化成
 * "只选一个换行"，动作前的 Shift+Home/End 又把范围缩错（V k d 拼接两行、V gg y 丢首尾正文，
 * 属数据损坏），已废弃；现在由引擎自记行偏移 off 重建整行选区。 */

/* 进入行选：Home + Shift+End = 选中整行（只发 Shift+End 会漏光标前的半行）。 */
void kv_emit_visual_line_enter(void) {
    kv_emit_tap(KV_HOME);
    kv_emit_tap(KV_LSFT_KC(KV_END));
}

/* 纵向扩展 n 行（活动端随宿主移动 n 行）。 */
void kv_emit_vline_move(bool up, int n) {
    if (n < 1) n = 1;
    kv_emit_taps(up ? KV_LSFT_KC(KV_UP) : KV_LSFT_KC(KV_DOWN), n);
}

/* 把活动端顶到所在行行尾：宿主的 Shift+↓ 只下移一行、列不变，不补 Shift+End 时
 * 目标行更长就只选到"源行末列"，长行末字符会漏（用户实测报告）。 */
void kv_emit_vline_move_tail(void) { kv_emit_tap(KV_LSFT_KC(KV_END)); }

/* 活动端是**上边界**（`gg` 之后）时贴行首：整行选区的上边界是"行首"而不是"行尾"，
 * 用 Shift+End 收边会让选区从该行行尾开始 → 丢半行/退化成只选一个换行（`V gg j y`）。 */
void kv_emit_vline_move_head(void) { kv_emit_tap(KV_LSFT_KC(KV_HOME)); }

/* 方向翻转时重建锚点：**直接从当前光标**（位于 A+off_before）重建，不做
 * "先按 Shift+↑/↓ 移动再重锚"的冗余移动 —— 后者会把键码数抬到 ~3n，撑爆发送队列。
 *   to_up=true  （off_after<0）：锚移到 (A+1) 行首，活动端落在 A+off_after 行首
 *     = [Up×(off_before−1) | Down×(1−off_before)] + Home + Shift+Up×(1−off_after)
 *   to_up=false （off_after>0）：锚移到 A 行首，活动端落在 A+off_after 行尾
 *     = Down×(−off_before) + Home + Shift+Down×off_after + Shift+End
 * 键码数：to_up ≤ n+3、to_down ≤ n+2（off_before 项相消），与 Normal 的 99dd(103) 同量级。 */
void kv_emit_vline_reanchor(bool to_up, int off_before, int off_after) {
    if (to_up) {
        if (off_before > 1)      kv_emit_taps(KV_UP, off_before - 1);
        else if (off_before < 1) kv_emit_taps(KV_DOWN, 1 - off_before);
        kv_emit_tap(KV_HOME);
        kv_emit_taps(KV_LSFT_KC(KV_UP), 1 - off_after);
    } else {
        if (off_before < 0) kv_emit_taps(KV_DOWN, -off_before);
        kv_emit_tap(KV_HOME);
        if (off_after > 0) kv_emit_taps(KV_LSFT_KC(KV_DOWN), off_after);
        kv_emit_tap(KV_LSFT_KC(KV_END));
    }
}

/* gg：范围 = [文首, A]。UP 态锚已在 (A+1) 行首，直接 Ctrl+Shift+Home；
 * DOWN 态先把光标移到 (A+1) 行首再扩展。 */
void kv_emit_vline_gg(bool dir_up, int off) {
    if (!dir_up) {
        if (off > 1)      kv_emit_taps(KV_UP, off - 1);
        else if (off < 1) kv_emit_taps(KV_DOWN, 1 - off);
        kv_emit_tap(KV_HOME);
    }
    kv_emit_tap(KV_CS(KV_HOME));
}

/* G：范围 = [A, 文末]。UP 态先把光标移回 A 行首（锚随之落到 A 行首）再扩展。 */
void kv_emit_vline_G(bool dir_up, int off) {
    if (dir_up) {
        kv_emit_tap(KV_END);
        if (off < 0) kv_emit_taps(KV_DOWN, -off);
        kv_emit_tap(KV_HOME);
    }
    kv_emit_tap(KV_CS(KV_END));
}

/* 行选动作：先把"行尾换行"纳入选区（仅 DOWN 态需要），再执行宿主剪贴板操作。
 * 与真实 Vim 一致：y=复制、d/x=删除整行、c/s=删整行+留一个空行+Insert（二者等价）、
 * p=用寄存器覆盖选区。Ctrl+C 后宿主通常保留高亮选区，故补 Esc 取消（Vim 也取消）。 */
void kv_emit_vline_action(kv_keycode_t op, bool dir_up) {
    if (!dir_up) kv_emit_tap(KV_LSFT_KC(KV_RGHT));   /* 纳入行尾换行 → linewise */
    switch (op) {
        case KV_Y: kv_emit_tap(KV_LCTL_KC(KV_C)); kv_emit_tap(KV_ESC); break;
        case KV_D: kv_emit_tap(KV_LCTL_KC(KV_X)); break;
        case KV_C: kv_emit_tap(KV_LCTL_KC(KV_X)); kv_emit_tap(KV_LSFT_KC(KV_ENT)); break;
        case KV_P: kv_emit_tap(KV_LCTL_KC(KV_V)); kv_emit_tap(KV_ESC); break;
        default: break;
    }
}

void kv_emit_visual_motion(kv_keycode_t kc) {
    switch (kc) {
        case KV_H:       kv_emit_tap(KV_LSFT_KC(KV_LEFT));  break;
        case KV_J:       kv_emit_tap(KV_LSFT_KC(KV_DOWN));  break;
        case KV_K:       kv_emit_tap(KV_LSFT_KC(KV_UP));    break;
        case KV_L:       kv_emit_tap(KV_LSFT_KC(KV_RGHT));  break;
        case KV_W: case KV_E:       kv_emit_tap(KV_CS(KV_RGHT)); break;
        case KV_C_W: case KV_C_E:   kv_emit_tap(KV_CS(KV_RGHT)); break;
        case KV_B:                  kv_emit_tap(KV_CS(KV_LEFT)); break;
        case KV_C_B:                kv_emit_tap(KV_CS(KV_LEFT)); break;
        case KV_0: case KV_C_CARET: kv_emit_tap(KV_LSFT_KC(KV_HOME)); break;
        case KV_C_DLR:              kv_emit_tap(KV_LSFT_KC(KV_END));  break;
        case KV_C_G:                kv_emit_tap(KV_CS(KV_END));  break;
        default: break;
    }
}

void kv_emit_delete_char(void)    { kv_emit_tap(KV_DEL); }
void kv_emit_backspace_char(void) { kv_emit_tap(KV_BSPC); }

void kv_emit_substitute(void) {
    kv_emit_tap(KV_LSFT_KC(KV_RGHT));
    kv_emit_tap(KV_DEL);
    kv_emit_enter_insert(KV_I);
}

void kv_emit_change_to_eol(void) {
    kv_emit_tap(KV_LSFT_KC(KV_END));
    kv_emit_tap(KV_LCTL_KC(KV_X));
    kv_emit_enter_insert(KV_I);
}

void kv_emit_delete_to_eol(void) {
    kv_emit_tap(KV_LSFT_KC(KV_END));
    kv_emit_tap(KV_LCTL_KC(KV_X));
}

/* 复制后补 Esc：宿主在 Ctrl+C 后保留高亮选区，不取消则下一个键会替换刚复制的内容
 * （实测 `yy` 后按 `x` 会删掉整行 = 数据损坏）。 */
void kv_emit_yank_to_eol(void) {
    kv_emit_tap(KV_LSFT_KC(KV_END));
    kv_emit_tap(KV_LCTL_KC(KV_C));
    kv_emit_tap(KV_ESC);
}

/* 字符级 VISUAL：动作直接作用于当前选区，不再自行扩选。 */
void kv_emit_visual_cut(void)    { kv_emit_tap(KV_LCTL_KC(KV_X)); }
void kv_emit_visual_yank(void)   { kv_emit_tap(KV_LCTL_KC(KV_C)); kv_emit_tap(KV_ESC); }
void kv_emit_visual_change(void) { kv_emit_tap(KV_LCTL_KC(KV_X)); kv_emit_enter_insert(KV_I); }
void kv_emit_visual_paste(void)  { kv_emit_tap(KV_LCTL_KC(KV_V)); kv_emit_tap(KV_ESC); }

void kv_emit_paste(bool before) {
    if (before) kv_emit_tap(KV_LEFT);
    kv_emit_tap(KV_LCTL_KC(KV_V));
}

/* 真实 Vim 的 `J` 会插**一个空格**（three + four => three four）。已知偏差：Vim 还会去掉
 * 下一行的前导空白，固件读不到空白长度，故保留。 */
void kv_emit_join(void) {
    kv_emit_tap(KV_END);
    kv_emit_tap(KV_SPC);
    kv_emit_tap(KV_DEL);
}

void kv_emit_undo(void) { kv_emit_tap(KV_LCTL_KC(KV_Z)); }
void kv_emit_save(void) { kv_emit_tap(KV_LCTL_KC(KV_S)); }

void kv_emit_enter_insert(kv_keycode_t kc) {
    switch (kc) {
        case KV_I:     break;                                /* i: in place */
        case KV_C_I:   kv_emit_tap(KV_HOME); break;          /* I */
        case KV_A:     kv_emit_tap(KV_RGHT); break;          /* a */
        case KV_C_A:   kv_emit_tap(KV_END); break;           /* A */
        case KV_O:                                           /* o */
            kv_emit_tap(KV_END);
            kv_emit_tap(KV_LSFT_KC(KV_ENT));
            break;
        case KV_C_O:                                         /* O */
            kv_emit_tap(KV_HOME);
            kv_emit_tap(KV_LSFT_KC(KV_ENT));
            kv_emit_tap(KV_UP);
            break;
        default: break;
    }
}
