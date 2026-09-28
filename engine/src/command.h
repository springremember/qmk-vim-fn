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

/* VISUAL_LINE（design §4.9 v2）：方向无关的按行语义（对齐真实 Vim）。
 * off = 光标行 − 锚行 A（A = 按 V 时所在行）；DOWN 态锚在 A 行首、UP 态锚在 A+1 行首。 */
void kv_emit_visual_line_enter(void);              /* Home, Shift+End = 选中整行 */
void kv_emit_vline_move(bool up, int n);           /* Shift+Up/Down × n */
void kv_emit_vline_move_tail(void);                /* Shift+End：活动端顶到行尾 */
void kv_emit_vline_reanchor(bool to_up, int off_before, int off_after); /* 方向翻转时重建锚点 */
void kv_emit_vline_gg(bool dir_up, int off);
void kv_emit_vline_G(bool dir_up, int off);
void kv_emit_vline_action(kv_keycode_t op, bool dir_up); /* op ∈ {KV_Y,KV_D,KV_C,KV_P} */
/* Single-key editing commands. */
void kv_emit_delete_char(void);     /* x  */
void kv_emit_backspace_char(void);  /* X  */
void kv_emit_substitute(void);      /* s  */
void kv_emit_change_to_eol(void);   /* C  */
void kv_emit_delete_to_eol(void);   /* D  */
void kv_emit_yank_to_eol(void);     /* Y  */
void kv_emit_paste(bool before);    /* p / P */
void kv_emit_join(void);            /* J  */
void kv_emit_undo(void);            /* u  */
void kv_emit_save(void);            /* ZZ */
void kv_emit_enter_insert(kv_keycode_t kc); /* i I a A o O */

#endif /* KV_COMMAND_H */
