/* command.h — emit a parsed command as a host key sequence. */
#ifndef KV_COMMAND_H
#define KV_COMMAND_H

#include "../include/kv_kc.h"
#include "ctx.h"

/* 发送队列预算（design §4.4）。EMIT_CAP=256 且溢出**静默丢键**（= 数据损坏），
 * 故所有带计数的发射器都必须先按剩余预算截断计数：
 *   kv_emit_room()                 —— KV_CMD_KEY_BUDGET(250) − kv_emit_pending()，下限 0
 *   kv_emit_clamp_n(n, fix, per)   —— 把 n 截断到 fixed + per×n ≤ room 的最大值（下限 1）
 * KV_CMD_KEY_BUDGET 定义在 command.c。 */
int kv_emit_room(void);
int kv_emit_clamp_n(int n, int fixed, int per);

/* Motion kinds. */
typedef enum {
    M_NONE = 0,
    M_H, M_J, M_K, M_L,
    M_W, M_WBIG, M_B, M_BBIG, M_E, M_EBIG,
    M_ZERO, M_CARET, M_DOLLAR,
    M_G_BIG, M_GG,
} kv_motion_t;

/* Emit a standalone motion (n times). */
void kv_emit_motion(kv_motion_t m, int n);

/* Emit an operator (d/y/c) combined with a motion.  chg => change (Insert). */
void kv_emit_op_motion(kv_keycode_t op, kv_motion_t m, int n);

/* Emit a line operation (dd/yy/cc) over n lines.  cc enters Insert. */
void kv_emit_line_op(kv_keycode_t op, int n);

/* Emit an indent operation combined with a motion. */
void kv_emit_indent_motion(kv_keycode_t ang, kv_motion_t m, int n);

/* Emit a line indent (>>/<<) over n lines. */
void kv_emit_indent_line(kv_keycode_t ang, int n);

/* Emit a visual-mode motion: extend the selection by one step. */
void kv_emit_visual_motion(kv_keycode_t kc);

/* 字符级 VISUAL 重锚（design §4.9，缺陷 D1/D12）：方向翻转越过锚点时重建半开选区，
 * 避免 Shift+方向 把选区塌成空（否则 Ctrl+X 会剪切整行 = 数据损坏）。w = hi−lo。 */
void kv_emit_visual_reanchor_left(int w, int n);   /* Esc, Left×(w-1), Shift+Left×(n-w+2) */
void kv_emit_visual_reanchor_right(int w, int n);  /* Esc, Right×(w-1), Shift+Right×(n-w+2) */
void kv_emit_visual_zero_from_right(int w);        /* Esc, Left×(w-1), Shift+Home */
void kv_emit_visual_dollar_from_left(int w);       /* Esc, Right×(w-1)（随后 Shift+End,Shift+Right） */
void kv_emit_visual_word_fwd_anchor(void);         /* Shift+Left, Ctrl+Shift+Right, Shift+Right */
/* 后向词动作重锚（D19）：Esc, Shift+Left, Ctrl+Shift+Left×n */
void kv_emit_visual_word_back_anchor(int n);

/* VISUAL_LINE（design §4.9 v2）：方向无关的按行语义（对齐真实 Vim）。
 * off = 光标行 − 锚行 A（A = 按 V 时所在行）；DOWN 态锚在 A 行首、UP 态锚在 A+1 行首。 */
void kv_emit_visual_line_enter(void);              /* Home, Shift+End = 选中整行 */
/* 进入字符级 VISUAL：真实 Vim 的 `v` 立刻选中光标下的 1 个字符，故发 Shift+Right。
 * 由此 `v` + n 次移动 = n+1 个字符（vd 删 1 个、vlld 删 3 个）。 */
void kv_emit_visual_enter(void);
void kv_emit_vline_move(bool up, int n);           /* Shift+Up/Down × n */
void kv_emit_vline_move_tail(void);                /* Shift+End：活动端顶到行尾 */
void kv_emit_vline_move_head(void);                /* Shift+Home：活动端贴到行首 */
void kv_emit_vline_reanchor(bool to_up, int off_before, int off_after); /* 方向翻转时重建锚点 */
void kv_emit_vline_gg(bool dir_up, int off);
void kv_emit_vline_G(bool dir_up, int off);
void kv_emit_vline_action(kv_keycode_t op, bool dir_up); /* op ∈ {KV_Y,KV_D,KV_C,KV_P} */

/* 字符级 VISUAL 的动作：直接作用于**当前选区**（design §4.9）。
 * 旧实现在动作前多发 Shift+End 把选区扩到行尾（`v l l d` 会删掉整行），复制后也不取消
 * 宿主残留选区（`yy` 后按 `x` 会删掉整行）—— 都是数据损坏级缺陷。 */
void kv_emit_visual_cut(void);     /* d/x: Ctrl+X */
void kv_emit_visual_yank(void);    /* y  : Ctrl+C + Esc */
void kv_emit_visual_change(void);  /* c/s: Ctrl+X + 进 INSERT */
void kv_emit_visual_paste(void);   /* p  : Ctrl+V + Esc */
/* Single-key editing commands. */
void kv_emit_delete_char(void);     /* x  */
void kv_emit_backspace_char(void);  /* X  */
void kv_emit_substitute(void);      /* s  */

/* 带计数的单键编辑（真实 Vim 的 `Nx`/`NX`/`Ns`）：**一次**选中 N 个字符再剪切 ——
 * 寄存器里是**全部** N 个字符（逐个删只剩最后一个，独立审查 P0-6），键码数也只有
 * N+1（逐个删是 3N，`99X` = 297 键会撑爆 256 格发送队列 → **静默丢键**，审查 P0-1）。 */
void kv_emit_delete_char_n(int n);      /* Nx */
void kv_emit_backspace_char_n(int n);   /* NX */
void kv_emit_substitute_n(int n);       /* Ns */

/* 带计数的粘贴：**只定位一次**，随后 Ctrl+V ×N（逐个"定位+粘贴"会把副本交错插入，
 * 审查 P0-5）。 */
void kv_emit_paste_n(bool before, int n);   /* Np / NP */

/* 带计数的连接：`NJ` = N−1 次连接（每次 4 键）；超出队列预算时**截断**（审查 P0-1）。 */
void kv_emit_join_n(int n);                 /* NJ */

void kv_emit_change_to_eol(void);   /* C  */
void kv_emit_delete_to_eol(void);   /* D  */

/* 带计数的 C/D/Y：真实 Vim 的 `dN$` —— 作用范围 = [光标, **下面第 N-1 行的行尾**]。
 * `3D` 在 `abcdefgh|L2xyz|L3|L4` 上 => `L4`；`3C` => `|L4`。 */
void kv_emit_delete_to_eol_n(int n);
void kv_emit_change_to_eol_n(int n);
void kv_emit_paste(bool before);    /* p / P */
void kv_emit_join(void);            /* J  */
void kv_emit_undo(void);            /* u  */
void kv_emit_save(void);            /* ZZ */
void kv_emit_enter_insert(kv_keycode_t kc); /* i I a A o O */
/* 入口键的键码成本（P2-3/D28，design §4.16 #6）：`kv_emit_enter_insert()` 会发出的键数。
 * 计数插入在提交点**重发入口**（`o`/`O`）时用它夹取额外重复次数，保证每次重复完整。 */
int  kv_emit_enter_insert_cost(kv_keycode_t kc);

/* 无名寄存器类型（行级/字符级）跟踪：由所有写宿主剪贴板的 emitter 维护，
 * `kv_emit_paste` 据此选择定位键码；`kv_init` 复位。 */
void kv_emit_reset_reg(void);
bool kv_emit_reg_linewise(void);

#endif /* KV_COMMAND_H */
