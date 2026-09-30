#include "command.h"
#include "emit.h"

/* 宿主剪贴板里"无名寄存器"的类型（design §4.4/§4.9）：真实 Vim 的 `p` 对**行级**寄存器
 * 粘到下一行、对**字符级**寄存器粘到光标字符之后。宿主只有一条剪贴板，但引擎知道最近一次
 * 写剪贴板的命令是不是行级（`dd`/`yy`/`dj`/`yG`/行选动作…），故据此选择定位键码（D7）。
 * 不写剪贴板的命令（`p`/`J`/`u`/缩进…）不改它；`u` 会恢复寄存器，属已知偏差。 */
static bool s_reg_linewise;

/* 单条命令可用的键码预算。发送队列是 256 格且**溢出静默丢键**（= 数据损坏，见 design §2），
 * 故带计数的命令按实测每键成本自行截断，绝不让队列到顶。留 6 格余量。 */
#define KV_CMD_KEY_BUDGET 250

void kv_emit_reset_reg(void) { s_reg_linewise = false; }
bool kv_emit_reg_linewise(void) { return s_reg_linewise; }

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
    if (m == M_J) {                       /* 向下：Home + Shift+Down×(n+1) = n+1 行 */
        kv_emit_tap(KV_HOME);
        kv_emit_taps(KV_LSFT_KC(KV_DOWN), n + 1);
        return;
    }
    if (m == M_K) {
        /* 向上：锚点必须**越过当前行的行尾换行**（End, Right）到下一行行首，半开区间
         * [anchor,cursor) 才包含当前行的换行 —— 否则 `d` 会删掉行内容却留下一个空行。 */
        kv_emit_tap(KV_HOME);
        kv_emit_tap(KV_END);
        kv_emit_tap(KV_RGHT);
        kv_emit_taps(KV_LSFT_KC(KV_UP), n + 1);
        return;
    }
    if (m == M_G_BIG) {                   /* 到末行：Home + Ctrl+Shift+End */
        kv_emit_tap(KV_HOME);
        kv_emit_tap(KV_CS(KV_END));
        return;
    }
    if (m == M_GG) {                      /* 到首行：End,Right + Ctrl+Shift+Home（锚点同 k） */
        kv_emit_tap(KV_END);
        kv_emit_tap(KV_RGHT);
        kv_emit_tap(KV_CS(KV_HOME));
        return;
    }
    kv_emit_taps(sel, n);
}

void kv_emit_op_motion(kv_keycode_t op, kv_motion_t m, int n) {
    if (n < 1) n = 1;
    /* 行级动作（j/k/G/gg）在 Vim 里写进行级寄存器；其余为字符级。 */
    s_reg_linewise = (m == M_J || m == M_K || m == M_G_BIG || m == M_GG);
    emit_op_range(m, n);
    if (op == KV_C) {
        kv_emit_tap(KV_LCTL_KC(KV_X));
        /* 行选动作 + c：真实 Vim 与 cc 一样**留一个空行**（cj => L1||L4），补 Shift+Enter。
         * 但插入的换行会把宿主光标顶到**下一行行首**，而 Vim 把光标留在那个空行上——
         * 差一行是**缓冲区可见的**（`cjx` 会删掉接替行的首字符 = 数据损坏；独立矩阵测试的
         * "D5-residual" 组）。故插完补 `←` 把光标退回空行。 */
        if (m == M_J || m == M_K || m == M_G_BIG || m == M_GG) {
            kv_emit_tap(KV_LSFT_KC(KV_ENT));
            kv_emit_tap(KV_LEFT);
        }
        kv_emit_enter_insert(KV_I);
    } else if (op == KV_Y) {
        kv_emit_tap(KV_LCTL_KC(KV_C));
        kv_emit_tap(KV_ESC);   /* 复制后取消宿主残留选区 */
        /* 真实 Vim 的 `y` 把光标留在**被复制区间的起点**（独立审查实测：`yh`→列−1、
         * `yb`→词首、`yj`/`yk`→区间首行；只有 `yl`/`yw`/`ye`/`y$` 保持原列）。
         * 宿主的 Shift+方向 正好也把光标带到区间起点，故：
         *   h / b / B / 0 / ^ ：宿主光标已在 Vim 的位置 → **不发键**（旧实现多发回位键，
         *                       反而把光标推回原处，`yhp`/`ybp` 会粘错位置 = 缓冲区可见）。
         *   l ：Shift+Right 前进 1 → 左移 1 复位。
         *   w/e ：Ctrl+Shift+Right 落到下一个词首；只有起点在词首时 Ctrl+Left 才回到原列，
         *         词中/行尾无法恢复 → 已知偏差（design §4.4）。
         *   j/k ：Shift+Down×(n+1) → Up×(n+1) 回到区间首行（动作被夹取时会过冲，见 §4.4）。
         *   $ ：宿主在行尾、Vim 保持原列，列无法恢复 → 回到行首（已知偏差）。 */
        switch (m) {
            case M_L:      kv_emit_tap(KV_LEFT);      break;
            case M_W: case M_WBIG: case M_E: case M_EBIG:
                           kv_emit_tap(KV_LCTL_KC(KV_LEFT));  break;
            case M_DOLLAR: kv_emit_tap(KV_HOME);      break;
            case M_J: case M_K: kv_emit_taps(KV_UP, n + 1); break;
            default: break;   /* M_H / M_B / M_BBIG / M_0 / M_CARET / M_G_BIG / M_GG：不发键 */
        }
    } else {
        kv_emit_tap(KV_LCTL_KC(KV_X));
    }
}

void kv_emit_line_op(kv_keycode_t op, int n) {
    if (n < 1) n = 1;
    s_reg_linewise = true;      /* dd/yy/cc/Y/S 都是行级 */
    if (op == KV_Y) {
        kv_emit_tap(KV_HOME);
        kv_emit_tap(KV_HOME);
        kv_emit_taps(KV_LSFT_KC(KV_DOWN), n);
        kv_emit_tap(KV_LCTL_KC(KV_C));
        kv_emit_tap(KV_ESC);   /* 取消宿主残留选区（否则下一个键会替换刚复制的内容） */
        kv_emit_taps(KV_UP, n); /* Vim 的 y 不移动光标：把宿主光标拉回原行 */
        return;
    }
    kv_emit_tap(KV_HOME);
    kv_emit_tap(KV_HOME);
    kv_emit_tap(KV_LSFT_KC(KV_END));
    /* `dd` 必须把**行尾换行**也纳入选区（`Shift+End` 只到末字符之前）：否则删不掉换行，
     * 在**首行**会留下一个空行（数据损坏，独立审查未列出的新发现）；`Ctrl+X` 后光标正好
     * 停在接替行行首。`cc`/`S` 相反：只删行内容、**留一个空行**（Vim 语义），故不加
     * `Shift+Right`。 */
    if (op != KV_C) kv_emit_tap(KV_LSFT_KC(KV_RGHT));
    if (n > 1) kv_emit_taps(KV_LSFT_KC(KV_DOWN), n - 1);
    kv_emit_tap(KV_LCTL_KC(KV_X));
    if (op == KV_C) kv_emit_enter_insert(KV_I);
}

void kv_emit_indent_motion(kv_keycode_t ang, kv_motion_t m, int n) {
    if (n < 1) n = 1;
    kv_keycode_t tab = (ang == KV_C_GT) ? KV_TAB : KV_LSFT_KC(KV_TAB);
    /* `>`/`<` 恒为**整行**：单行动作（h/l/0/^/$）只缩进**当前行**，计数不改变行范围
     * （`2>0`/`2>^`/`2>h`/`2>l` 均 1 行）；`h` 在列 0 **不跨行**——旧实现按字符级发
     * Shift+Left×n，宿主会在列 0 回绕到上一行行尾、把上一行缩进（D4，数据损坏）。 */
    if (m == M_H || m == M_L || m == M_ZERO || m == M_CARET) {
        kv_emit_indent_line(ang, 1);
        return;
    }
    if (m == M_DOLLAR) {          /* `N$` 下移 N−1 行 ⇒ 共 N 行（实测 `2>$` = 2 行） */
        kv_emit_indent_line(ang, n);
        return;
    }
    if (m == M_J) {               /* `>j` = 当前行 + 下 1 行 = 2 行（`N>j` = N+1 行） */
        kv_emit_indent_line(ang, n + 1);
        return;
    }
    if (m == M_K) {
        /* 向上：锚点越过当前行行尾换行（同 op+动作），活动端落在范围内**首行**；
         * 宿主 Tab 的插入点就在光标处 ⇒ 光标已在"首个非空白"，只需 Esc 取消残留选区。 */
        kv_emit_tap(KV_HOME);
        kv_emit_tap(KV_END);
        kv_emit_tap(KV_RGHT);
        kv_emit_taps(KV_LSFT_KC(KV_UP), n + 1);
        kv_emit_tap(tab);
        kv_emit_tap(KV_ESC);
        return;
    }
    if (m == M_GG) {              /* 到文首：光标落在第 1 行 ⇒ 插入后即首个非空白 */
        kv_emit_tap(KV_END);
        kv_emit_tap(KV_RGHT);
        kv_emit_tap(KV_CS(KV_HOME));
        kv_emit_tap(tab);
        kv_emit_tap(KV_ESC);
        return;
    }
    if (m == M_G_BIG) {           /* 到文末：光标停在文末（design §4.4 已知偏差①） */
        kv_emit_tap(KV_HOME);
        kv_emit_tap(KV_CS(KV_END));
        kv_emit_tap(tab);
        kv_emit_tap(KV_ESC);
        return;
    }
    /* 词动作（w/e/b/W/E/B）：**行范围**由宿主半开选区决定，与真实 Vim 的整行展开一致
     * （`>w` 只缩进当前行、`>e` 跨行时缩进两行）；光标停在移动目标（已知偏差②）。 */
    emit_op_range(m, n);
    kv_emit_tap(tab);
    kv_emit_tap(KV_ESC);          /* 取消宿主 Tab 后残留的高亮选区（否则下一个键替换整段） */
}

void kv_emit_indent_line(kv_keycode_t ang, int n) {
    if (n < 1) n = 1;
    kv_keycode_t tab = (ang == KV_C_GT) ? KV_TAB : KV_LSFT_KC(KV_TAB);
    /* 真实 Vim：`>>` 1 行、`2>>` 2 行、`3>>` 3 行；旧实现用 ×(n-1) 会少缩进一行。
     * 宿主选区是半开区间：n 行 = Shift+Down×n。 */
    if (n == 1) {
        /* 单行：`Home, Tab`（**无选区** = 在行首插入一个 Tab）。宿主插入后光标停在插入点
         * 之后 = 未缩进行的"首个非空白"，与真实 Vim 的 `>>x` ⇒ `\t2` 完全一致；且不会
         * 因 Shift+Down 在末行被夹取而多缩进。 */
        kv_emit_tap(KV_HOME);
        kv_emit_tap(tab);
        return;
    }
    kv_emit_tap(KV_HOME);
    kv_emit_tap(KV_HOME);
    kv_emit_taps(KV_LSFT_KC(KV_DOWN), n);
    kv_emit_tap(tab);
    kv_emit_tap(KV_ESC);          /* 取消宿主残留选区（D6：否则 x/p/. 替换整段 = 数据损坏） */
    kv_emit_taps(KV_UP, n);       /* 回到范围内首行 */
    kv_emit_tap(KV_HOME);
    if (ang == KV_C_GT) kv_emit_tap(KV_RGHT);  /* 首个非空白（刚插入的 Tab 之后） */
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
    if (op == KV_Y || op == KV_D || op == KV_C) s_reg_linewise = true; /* 行选动作写行级寄存器 */
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
        case KV_C_DLR:
            /* 真实 Vim 的 visual `$` 把行尾**换行**也纳入选区（v$d 会删掉换行），
             * 故 Shift+End 之后要再 Shift+Right 越过换行。 */
            kv_emit_tap(KV_LSFT_KC(KV_END));
            kv_emit_tap(KV_LSFT_KC(KV_RGHT));
            break;
        case KV_C_G:                kv_emit_tap(KV_CS(KV_END));  break;
        default: break;
    }
}

/* 字符级 VISUAL 的重锚（design §4.9，缺陷 D1）：宿主半开选区 [lo,hi)、w=hi−lo。
 * 光标在 hi 端向左越过锚点（或光标在 lo 端向右越过锚点）时，直接 Shift+方向 会把
 * 选区塌成空 → `Ctrl+X` 退化成"剪切整行"（数据损坏）。
 *   Esc 把选区塌到活动端（= 宿主光标）；随后用**不带 Shift** 的方向键把光标移到
 *   "锚点外侧一格"，再 Shift+方向 扩到目标。全部相对运算，无需知道绝对列。 */
void kv_emit_visual_reanchor_left(int w, int n) {
    /* 光标在 hi：Esc 后 Left×(w−1) 到 lo+1，再 Shift+Left×(n−w+2) 到目标。 */
    kv_emit_tap(KV_ESC);
    kv_emit_taps(KV_LEFT, w - 1);
    kv_emit_taps(KV_LSFT_KC(KV_LEFT), n - w + 2);
}

void kv_emit_visual_reanchor_right(int w, int n) {
    /* 光标在 lo：Esc 后 Right×(w−1) 到 hi−1（= Vim 锚点），再 Shift+Right×(n−w+2)。 */
    kv_emit_tap(KV_ESC);
    kv_emit_taps(KV_RGHT, w - 1);
    kv_emit_taps(KV_LSFT_KC(KV_RGHT), n - w + 2);
}

/* 字符级 VISUAL 的 `0`/`^`（光标在右端）：目标列 0 一定 ≤ 锚列，故需重锚到
 * [行首, 锚字符] 的半开表示：Esc, Left×(w−1), Shift+Home。 */
void kv_emit_visual_zero_from_right(int w) {
    kv_emit_tap(KV_ESC);
    kv_emit_taps(KV_LEFT, w - 1);
    kv_emit_tap(KV_LSFT_KC(KV_HOME));
}

/* 字符级 VISUAL 的 `$`（光标在左端）：锚点在右端（= Vim 锚字符 +1），先把光标移回
 * 锚字符处，再由调用方发 Shift+End, Shift+Right（含行尾换行）。 */
void kv_emit_visual_dollar_from_left(int w) {
    kv_emit_tap(KV_ESC);
    kv_emit_taps(KV_RGHT, w - 1);
}

/* 字符级 VISUAL 前向词动作的重锚（design §4.9，缺陷 D12）：`v` 的预选把宿主光标
 * 放到了 Vim 光标右侧一格，直接 Ctrl+Shift+Right 会从 c+1 起算（差一列）。
 * Shift+Left 把光标移回 c（锚点不变），Ctrl+Shift+Right 从 c 起算，
 * Shift+Right 把目标字符纳入半开选区。可连续使用（锚点始终不变）。 */
void kv_emit_visual_word_fwd_anchor(void) {
    kv_emit_tap(KV_LSFT_KC(KV_LEFT));
    kv_emit_tap(KV_CS(KV_RGHT));
    kv_emit_tap(KV_LSFT_KC(KV_RGHT));
}

/* 后向词动作的重锚（design §4.9，缺陷 D19）：`v` 的预选把宿主光标放在 c+1，而 Vim 光标在 c。
 * 当 Vim 光标恰在**词首**时，直接 `Ctrl+Shift+Left` 会从 c+1 回到 c（即锚点）
 * → 半开选区塔成空 → 随后 `Ctrl+X` 退化成"剪切整行"（**数据损坏**）。
 * 修：`Esc` 取消选区后 `Shift+Left` 把**锚点放到 hi**（右端）、宿主光标落到 hi−1
 * （= Vim 光标），再 `Ctrl+Shift+Left×n` 就与 Vim 的 `b` 逐字对齐（含"b 是 no-op"的情形）。
 * 实测：`vbd`/`v2bd`在各列与真实 Vim 的**缓冲区与寄存器**逐字一致。
 * 已知偏差：若 `b` 落在**已选区域内部**（宽选区且目标 > 左端），Vim 保持选区不变，
 * 本实现会以 hi 为锚点重建——少选左端到目标之间那一段。 */
void kv_emit_visual_word_back_anchor(int n) {
    if (n < 1) n = 1;
    kv_emit_tap(KV_ESC);
    kv_emit_tap(KV_LSFT_KC(KV_LEFT));
    kv_emit_taps(KV_CS(KV_LEFT), n);
}

/* `x`：真实 Vim 删光标下 1 字符**并写无名寄存器**（`xp` 交换字符）。旧实现只发 Delete，
 * 剪贴板不更新 ⇒ `xp` 变成"删一个字符"（独立审查 D7）。Shift+Right 选中的就是光标下 1 字符
 * （宿主 Shift+→ 按**字符**前进，行尾时选中的是末字符本身而非换行）。 */
/* `x`/`Nx`：真实 Vim 删光标下 N 字符**并写无名寄存器**（`xp` 交换字符）。一次选中 N 个字符
 * 再剪切 —— 寄存器拿到全部 N 个（审查 P0-6），键码 N+1（逐个删是 3N，`99X` = 297 键会撑爆
 * 256 格发送队列 → **静默丢键** = 数据损坏，审查 P0-1）。Shift+→ 按**字符**前进，行尾时
 * 选中的是末字符本身而非换行。 */
void kv_emit_delete_char_n(int n) {
    if (n < 1) n = 1;
    s_reg_linewise = false;
    kv_emit_taps(KV_LSFT_KC(KV_RGHT), n);
    kv_emit_tap(KV_LCTL_KC(KV_X));
}
void kv_emit_delete_char(void) { kv_emit_delete_char_n(1); }

/* `X`/`NX`：删光标**前** N 字符并写寄存器。列 0 时 Vim 是 no-op；若用 Shift+Left,Ctrl+X，
 * 宿主空选区会被当成"剪切整行"（数据损坏），故先 Ctrl+C 复制选区再 BSPC 删选区。
 * **已知偏差**：列 0 且**不在缓冲区开头**时，宿主 `Shift+Left` 会回绕选中上一行的换行，
 * BSPC 就把它删掉 → 两行被拼接（Vim 是 no-op）。引擎读不到列号，固有。 */
void kv_emit_backspace_char_n(int n) {
    if (n < 1) n = 1;
    s_reg_linewise = false;
    kv_emit_taps(KV_LSFT_KC(KV_LEFT), n);
    kv_emit_tap(KV_LCTL_KC(KV_C));
    kv_emit_tap(KV_BSPC);
}
void kv_emit_backspace_char(void) { kv_emit_backspace_char_n(1); }

/* `s`/`Ns`：真实 Vim 的 `s` = `cl`（删光标下 1 字符 + Insert），寄存器是字符级；
 * `Ns` 一次删 N 个字符（寄存器同样拿到全部 N 个）。 */
void kv_emit_substitute_n(int n) {
    if (n < 1) n = 1;
    s_reg_linewise = false;
    kv_emit_taps(KV_LSFT_KC(KV_RGHT), n);
    kv_emit_tap(KV_LCTL_KC(KV_X));
    kv_emit_enter_insert(KV_I);
}
void kv_emit_substitute(void) { kv_emit_substitute_n(1); }

void kv_emit_change_to_eol(void) {
    s_reg_linewise = false;
    kv_emit_tap(KV_LSFT_KC(KV_END));
    kv_emit_tap(KV_LCTL_KC(KV_X));
    kv_emit_enter_insert(KV_I);
}

void kv_emit_delete_to_eol(void) {
    s_reg_linewise = false;
    kv_emit_tap(KV_LSFT_KC(KV_END));
    kv_emit_tap(KV_LCTL_KC(KV_X));
}

/* 复制后补 Esc：宿主在 Ctrl+C 后保留高亮选区，不取消则下一个键会替换刚复制的内容
 * （实测 `yy` 后按 `x` 会删掉整行 = 数据损坏）。 */
void kv_emit_yank_to_eol(void) {
    s_reg_linewise = true;  /* 真实 Vim 的 `Y` ≡ `yy`（行级） */
    kv_emit_tap(KV_LSFT_KC(KV_END));
    kv_emit_tap(KV_LCTL_KC(KV_C));
    kv_emit_tap(KV_ESC);
    kv_emit_tap(KV_HOME);   /* Vim 的 y$ 不移动光标（列无法恢复，回到列 0） */
}

/* C/D/Y 带计数：选区 = [光标, 下面第 n-1 行的行尾]（真实 Vim 的 `dN$`）。 */
static void emit_eol_range(int n) {
    kv_emit_tap(KV_LSFT_KC(KV_END));
    if (n > 1) {
        kv_emit_taps(KV_LSFT_KC(KV_DOWN), n - 1);
        kv_emit_tap(KV_LSFT_KC(KV_END));
    }
}

void kv_emit_delete_to_eol_n(int n) { if (n < 1) n = 1; s_reg_linewise = false; emit_eol_range(n); kv_emit_tap(KV_LCTL_KC(KV_X)); }
void kv_emit_change_to_eol_n(int n) { if (n < 1) n = 1; s_reg_linewise = false; emit_eol_range(n); kv_emit_tap(KV_LCTL_KC(KV_X)); kv_emit_enter_insert(KV_I); }
void kv_emit_yank_to_eol_n(int n)   { if (n < 1) n = 1; s_reg_linewise = true;  emit_eol_range(n); kv_emit_tap(KV_LCTL_KC(KV_C)); kv_emit_tap(KV_ESC); kv_emit_tap(KV_HOME); }

void kv_emit_visual_enter(void) { kv_emit_tap(KV_LSFT_KC(KV_RGHT)); }

/* 字符级 VISUAL：动作直接作用于当前选区，不再自行扩选。寄存器是**字符级**。 */
void kv_emit_visual_cut(void)    { s_reg_linewise = false; kv_emit_tap(KV_LCTL_KC(KV_X)); }
void kv_emit_visual_yank(void)   { s_reg_linewise = false; kv_emit_tap(KV_LCTL_KC(KV_C)); kv_emit_tap(KV_ESC); }
void kv_emit_visual_change(void) { s_reg_linewise = false; kv_emit_tap(KV_LCTL_KC(KV_X)); kv_emit_enter_insert(KV_I); }
/* 可视 `p` 会用被替换的文本覆盖寄存器（Vim 语义），本引擎无法同时保存两份 → 已知偏差⑧，
 * 故**不改** s_reg_linewise。 */
void kv_emit_visual_paste(void)  { kv_emit_tap(KV_LCTL_KC(KV_V)); kv_emit_tap(KV_ESC); }

/* `p`/`P` 的定位（design §4.4，独立审查 D7）：
 *   字符级寄存器：Vim 插在**光标字符之后** ⇒ 先 `→` 再 Ctrl+V（`P` 插在光标字符之前 ⇒ 直接粘）。
 *   行级寄存器：Vim 在**下一行**新建一行粘贴 ⇒ 先 `End, →`（越过行尾换行到下一行行首）再 Ctrl+V；
 *               末行无换行时 `→` 夹取到缓冲末尾，正好在末尾追加一行。
 * 旧实现一律 Ctrl+V（`P` 先 `←`）：`xp`/`ylp`/`ddp` 都会粘错位置（行级 `yyp` 恰好蒙对）。 */
void kv_emit_paste_n(bool before, int n) {
    if (n < 1) n = 1;
    /* **只定位一次**：逐个"定位 + 粘贴"会把副本交错插到不同位置（审查 P0-5）。 */
    if (!before) {
        if (s_reg_linewise) { kv_emit_tap(KV_END); kv_emit_tap(KV_RGHT); }
        else                { kv_emit_tap(KV_RGHT); }
    }
    kv_emit_taps(KV_LCTL_KC(KV_V), n);
}
void kv_emit_paste(bool before) { kv_emit_paste_n(before, 1); }

/* 真实 Vim 的 `J` 会插**一个空格**（three + four => three four），并把光标留在那个空格上
 * （故末尾补 `←`；否则紧随的 `x` 会删到下一行的首字符 = 数据损坏，审查 P2-1）。
 * 已知偏差：Vim 还会去掉下一行的前导空白，固件读不到空白长度，故保留。 */
void kv_emit_join(void) {
    kv_emit_tap(KV_END);
    kv_emit_tap(KV_SPC);
    kv_emit_tap(KV_DEL);
    kv_emit_tap(KV_LEFT);
}

/* `NJ` = N−1 次连接，每次 4 键。超出发送队列预算时**截断**（溢出会静默丢键 = 数据损坏，
 * 审查 P0-1）；截断量已在 design §4.4 声明。 */
void kv_emit_join_n(int n) {
    if (n < 1) n = 1;
    int reps = (n > 1) ? n - 1 : 1;
    int room = KV_CMD_KEY_BUDGET - kv_emit_pending();
    int maxr = room / 4;                       /* J = End, Space, Delete, Left */
    if (maxr < 1) maxr = 1;
    if (reps > maxr) reps = maxr;
    for (int i = 0; i < reps; i++) kv_emit_join();
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
