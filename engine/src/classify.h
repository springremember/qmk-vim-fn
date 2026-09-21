/* classify.h — map a keycode to a token class used by the state machine. */
#ifndef KV_CLASSIFY_H
#define KV_CLASSIFY_H

#include "kv_kc.h"

typedef enum {
    T_OTHER = 0,  /* not a vim keycode */
    T_MOTION,     /* h j k l w W b B e E */
    T_ZERO,       /* 0 */
    T_CARET,      /* ^ */
    T_DOLLAR,     /* $ */
    T_G_BIG,      /* G */
    T_g_LOWER,    /* g */
    T_Z_BIG,      /* Z */
    T_OP,         /* d y c */
    T_INDENT,     /* < > */
    T_COUNT,      /* 1..9 */
    T_DIGIT,      /* 0..9 (count-continuation states only) */
    T_S_BIG,      /* S  (== cc) */
    T_INSERT,     /* i I a A o O */
    T_VISUAL,     /* v V */
    T_REPEAT,     /* . */
    T_UNDO,       /* u */
    T_JOIN,       /* J */
    T_X,          /* x */
    T_XUP,        /* X */
    T_C_BIG,      /* C */
    T_D_BIG,      /* D */
    T_Y_BIG,      /* Y */
    T_P,          /* p */
    T_PUP,        /* P */
    T_s,          /* s */
} kv_token_t;

kv_token_t kv_classify(kv_keycode_t kc);

/* like kv_classify but 0..9 all map to T_DIGIT (count-continuation states) */
kv_token_t kv_classify_digit(kv_keycode_t kc);

/* true if kc is any vim keycode (used by strict-clear re-identification) */
bool kv_is_vim_key(kv_keycode_t kc);

#endif /* KV_CLASSIFY_H */
