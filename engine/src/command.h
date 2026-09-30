/* command.h — emit a parsed command as a host key sequence. */
#ifndef KV_COMMAND_H
#define KV_COMMAND_H

#include "../include/kv_kc.h"
#include "ctx.h"

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
void kv_emit_change_to_eol(void);   /* C  */
void kv_emit_delete_to_eol(void);   /* D  */
void kv_emit_yank_to_eol(void);

/* 带计数的 C/D/Y：真实 Vim 的 `dN$` —— 作用范围 = [光标, **下面第 N-1 行的行尾**]。
 * `3D` 在 `abcdefgh|L2xyz|L3|L4` 上 => `L4`；`3C` => `|L4`。 */
void kv_emit_delete_to_eol_n(int n);
void kv_emit_change_to_eol_n(int n);
void kv_emit_yank_to_eol_n(int n);     /* Y  */
void kv_emit_paste(bool before);    /* p / P */
void kv_emit_join(void);            /* J  */
void kv_emit_undo(void);            /* u  */
void kv_emit_save(void);            /* ZZ */
void kv_emit_enter_insert(kv_keycode_t kc); /* i I a A o O */

/* 无名寄存器类型（行级/字符级）跟踪：由所有写宿主剪贴板的 emitter 维护，
 * `kv_emit_paste` 据此选择定位键码；`kv_init` 复位。 */
void kv_emit_reset_reg(void);
bool kv_emit_reg_linewise(void);

#endif /* KV_COMMAND_H */
