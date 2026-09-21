#include "classify.h"

kv_token_t kv_classify(kv_keycode_t kc) {
    switch (kc) {
        case KV_H: case KV_J: case KV_K: case KV_L:
        case KV_W: case KV_C_W:
        case KV_B: case KV_C_B:
        case KV_E: case KV_C_E:
            return T_MOTION;
        case KV_0:         return T_ZERO;
        case KV_C_CARET:   return T_CARET;
        case KV_C_DLR:     return T_DOLLAR;
        case KV_C_G:       return T_G_BIG;
        case KV_G:         return T_g_LOWER;
        case KV_C_Z:       return T_Z_BIG;
        case KV_D: case KV_Y: case KV_C:
            return T_OP;
        case KV_C_LT: case KV_C_GT:
            return T_INDENT;
        case KV_1: case KV_2: case KV_3: case KV_4: case KV_5:
        case KV_6: case KV_7: case KV_8: case KV_9:
            return T_COUNT;
        case KV_C_S:       return T_S_BIG;
        case KV_I: case KV_C_I:
        case KV_A: case KV_C_A:
        case KV_O: case KV_C_O:
            return T_INSERT;
        case KV_V: case KV_C_V:
            return T_VISUAL;
        case KV_DOT:       return T_REPEAT;
        case KV_U:         return T_UNDO;
        case KV_C_J:       return T_JOIN;
        case KV_X:         return T_X;
        case KV_C_X:       return T_XUP;
        case KV_C_C:       return T_C_BIG;
        case KV_C_D:       return T_D_BIG;
        case KV_C_Y:       return T_Y_BIG;
        case KV_P:         return T_P;
        case KV_C_P:       return T_PUP;
        case KV_S:         return T_s;
        default:           return T_OTHER;
    }
}

kv_token_t kv_classify_digit(kv_keycode_t kc) {
    switch (kc) {
        case KV_0: case KV_1: case KV_2: case KV_3: case KV_4:
        case KV_5: case KV_6: case KV_7: case KV_8: case KV_9:
            return T_DIGIT;
        default:
            return kv_classify(kc);
    }
}

bool kv_is_vim_key(kv_keycode_t kc) {
    return kv_classify(kc) != T_OTHER;
}
