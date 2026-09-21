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
    kv_emit_taps(sel, n);
}

void kv_emit_op_motion(kv_keycode_t op, kv_motion_t m, int n) {
    if (n < 1) n = 1;
    emit_op_range(m, n);
    if (op == KV_C) {
        kv_emit_tap(KV_LCTL_KC(KV_X));
        kv_emit_enter_insert(KV_I);
    } else {
        kv_emit_tap(KV_LCTL_KC(op == KV_D ? KV_X : KV_C));
    }
}

void kv_emit_line_op(kv_keycode_t op, int n) {
    if (n < 1) n = 1;
    if (op == KV_Y) {
        kv_emit_tap(KV_HOME);
        kv_emit_tap(KV_HOME);
        kv_emit_taps(KV_LSFT_KC(KV_DOWN), n);
        kv_emit_tap(KV_LCTL_KC(KV_C));
        return;
    }
    kv_emit_tap(KV_HOME);
    kv_emit_tap(KV_HOME);
    kv_emit_tap(KV_LSFT_KC(KV_END));
    if (n > 1) kv_emit_taps(KV_LSFT_KC(KV_DOWN), n - 1);
    kv_emit_tap(KV_LCTL_KC(KV_X));
    kv_emit_tap(KV_BSPC);
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
    for (int i = 0; i < n; i++) {
        kv_emit_tap(ang == KV_C_GT ? KV_TAB : KV_LSFT_KC(KV_TAB));
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

void kv_emit_yank_to_eol(void) {
    kv_emit_tap(KV_LSFT_KC(KV_END));
    kv_emit_tap(KV_LCTL_KC(KV_C));
}

void kv_emit_paste(bool before) {
    if (before) kv_emit_tap(KV_LEFT);
    kv_emit_tap(KV_LCTL_KC(KV_V));
}

void kv_emit_join(void) {
    kv_emit_tap(KV_END);
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
