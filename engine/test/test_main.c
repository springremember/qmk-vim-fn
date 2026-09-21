/* test_main.c — host unit tests for the vim engine. */
#include "kvtest.h"
#include "../src/emit.h"

/* helpers ---------------------------------------------------------------- */
/* Feed a key emulating the glue: pass-through keycodes are emitted by the
 * caller (QMK), consumed ones are handled by the engine. */
static void key(kv_keycode_t kc) {
    if (kv_kbd(kc) == KV_PASSTHROUGH) kv_emit_tap(kc);
    flush_emit();
}

static int seq_eq(const kv_keycode_t *exp, int n) {
    if (rec_count() != n) return 0;
    for (int i = 0; i < n; i++)
        if (rec_at(i) != exp[i]) return 0;
    return 1;
}

#define SEQ(...) ((const kv_keycode_t[]){__VA_ARGS__})
#define NSEQ(...) (int)(sizeof((const kv_keycode_t[]){__VA_ARGS__}) / sizeof(kv_keycode_t))
#define CHECK_SEQ(...) CHECK(seq_eq(SEQ(__VA_ARGS__), NSEQ(__VA_ARGS__)))

/* fresh NORMAL-mode engine with a clean recorder */
static void fresh(void) {
    kv_init();
    kv_enable();
    kv_set_mode(KV_MODE_NORMAL);
    rec_start();
}

/* ------------------------------------------------------------------ tests */
static void test_single(void) {
    fresh(); key(KV_H); CHECK_SEQ(KV_LEFT);
    fresh(); key(KV_J); CHECK_SEQ(KV_DOWN);
    fresh(); key(KV_K); CHECK_SEQ(KV_UP);
    fresh(); key(KV_L); CHECK_SEQ(KV_RGHT);
    fresh(); key(KV_0); CHECK_SEQ(KV_HOME);
    fresh(); key(KV_C_CARET); CHECK_SEQ(KV_HOME);
    fresh(); key(KV_C_DLR); CHECK_SEQ(KV_END);
    fresh(); key(KV_C_G); CHECK_SEQ(KV_LCTL_KC(KV_END));
    fresh(); key(KV_X); CHECK_SEQ(KV_DEL);
    fresh(); key(KV_C_X); CHECK_SEQ(KV_BSPC);
    fresh(); key(KV_C_D); CHECK_SEQ(KV_LSFT_KC(KV_END), KV_LCTL_KC(KV_X));
    fresh(); key(KV_C_Y); CHECK_SEQ(KV_LSFT_KC(KV_END), KV_LCTL_KC(KV_C));
    fresh(); key(KV_P); CHECK_SEQ(KV_LCTL_KC(KV_V));
    fresh(); key(KV_C_P); CHECK_SEQ(KV_LEFT, KV_LCTL_KC(KV_V));
    fresh(); key(KV_C_J); CHECK_SEQ(KV_END, KV_DEL);
    fresh(); key(KV_U); CHECK_SEQ(KV_LCTL_KC(KV_Z));
    fresh(); key(KV_C_Z); key(KV_C_Z); CHECK_SEQ(KV_LCTL_KC(KV_S));
    fresh(); key(KV_G); key(KV_G); CHECK_SEQ(KV_LCTL_KC(KV_HOME));
    /* s = Shift+Right, Delete, Insert */
    fresh(); key(KV_S); CHECK_SEQ(KV_LSFT_KC(KV_RGHT), KV_DEL);
}

static void test_count(void) {
    fresh();
    key(KV_3); CHECK(rec_count() == 0); /* count emits nothing */
    key(KV_W);
    CHECK_SEQ(KV_LCTL_KC(KV_RGHT), KV_LCTL_KC(KV_RGHT), KV_LCTL_KC(KV_RGHT));

    fresh(); key(KV_1); key(KV_2); key(KV_W);
    CHECK(rec_count() == 12);

    fresh(); key(KV_1); key(KV_2); key(KV_3); key(KV_W);
    CHECK(rec_count() == 12); /* 3rd digit ignored */

    fresh(); key(KV_3); CHECK(rec_count() == 0); key(KV_X); CHECK_SEQ(KV_DEL);
    fresh(); key(KV_3); key(KV_C_G); CHECK_SEQ(KV_LCTL_KC(KV_END));
    fresh(); key(KV_3); key(KV_G); key(KV_G); CHECK_SEQ(KV_LCTL_KC(KV_HOME));
    fresh(); key(KV_3); key(KV_C_S);
    CHECK_SEQ(KV_HOME, KV_HOME, KV_LSFT_KC(KV_END), KV_LSFT_KC(KV_DOWN), KV_LSFT_KC(KV_DOWN),
              KV_LCTL_KC(KV_X), KV_BSPC);
}

static void test_op(void) {
    fresh(); key(KV_D); key(KV_W);
    CHECK_SEQ(KV_CS(KV_RGHT), KV_LCTL_KC(KV_X));

    fresh(); key(KV_D); key(KV_3); key(KV_W);
    CHECK_SEQ(KV_CS(KV_RGHT), KV_CS(KV_RGHT), KV_CS(KV_RGHT), KV_LCTL_KC(KV_X));

    fresh(); key(KV_2); key(KV_D); key(KV_3); key(KV_W);
    CHECK(rec_count() == 7); /* 6 x CS(RGHT) + Ctrl+X */

    fresh(); key(KV_D); key(KV_C_G); CHECK_SEQ(KV_CS(KV_END), KV_LCTL_KC(KV_X));
    fresh(); key(KV_2); key(KV_D); key(KV_C_G); CHECK_SEQ(KV_CS(KV_END), KV_LCTL_KC(KV_X));
    fresh(); key(KV_D); key(KV_2); key(KV_C_G); CHECK_SEQ(KV_CS(KV_END), KV_LCTL_KC(KV_X));
    fresh(); key(KV_D); key(KV_G); key(KV_G); CHECK_SEQ(KV_CS(KV_HOME), KV_LCTL_KC(KV_X));

    /* line ops */
    fresh(); key(KV_D); key(KV_D);
    CHECK_SEQ(KV_HOME, KV_HOME, KV_LSFT_KC(KV_END), KV_LCTL_KC(KV_X), KV_BSPC);
    fresh(); key(KV_3); key(KV_D); key(KV_D);
    CHECK_SEQ(KV_HOME, KV_HOME, KV_LSFT_KC(KV_END), KV_LSFT_KC(KV_DOWN), KV_LSFT_KC(KV_DOWN),
              KV_LCTL_KC(KV_X), KV_BSPC);
    fresh(); key(KV_Y); key(KV_Y);
    CHECK_SEQ(KV_HOME, KV_HOME, KV_LSFT_KC(KV_DOWN), KV_LCTL_KC(KV_C));
}

static void test_indent(void) {
    fresh(); key(KV_C_GT); key(KV_J);
    CHECK_SEQ(KV_LSFT_KC(KV_DOWN), KV_TAB);
    fresh(); key(KV_C_GT); key(KV_C_GT); CHECK_SEQ(KV_HOME, KV_HOME, KV_TAB);
    fresh(); key(KV_3); key(KV_C_LT); key(KV_C_LT);
    CHECK_SEQ(KV_HOME, KV_HOME, KV_LSFT_KC(KV_DOWN), KV_LSFT_KC(KV_DOWN), KV_LSFT_KC(KV_TAB));
    fresh(); key(KV_C_GT); key(KV_1); key(KV_0); key(KV_J);
    CHECK(rec_count() == 11); /* 10 x LSFT(DOWN) + TAB */
    fresh(); key(KV_2); key(KV_C_GT); key(KV_3); key(KV_J);
    CHECK(rec_count() == 7); /* 6 x LSFT(DOWN) + TAB */
    fresh(); key(KV_C_GT); key(KV_0); CHECK_SEQ(KV_TAB);
    fresh(); key(KV_C_LT); key(KV_0); CHECK_SEQ(KV_LSFT_KC(KV_TAB));
    /* > < is not >> : strict clear, < re-identified */
    fresh(); key(KV_C_GT); key(KV_C_LT); CHECK(rec_count() == 0); /* < starts indent-pending */
}

static void test_strict_clear(void) {
    fresh(); key(KV_D); CHECK(rec_count() == 0); key(KV_X); CHECK_SEQ(KV_DEL);
    fresh(); key(KV_D); key(0x3E /*F5*/); CHECK_SEQ(0x3E);
    fresh(); key(KV_3); key(KV_X); CHECK_SEQ(KV_DEL);
    fresh(); key(KV_D); key(KV_ESC); CHECK(rec_count() == 0);
    fresh(); key(KV_G); key(0x3E); CHECK_SEQ(0x3E);
    fresh(); key(KV_G); key(KV_X); CHECK_SEQ(KV_DEL);
    fresh(); key(KV_C_Z); key(KV_X); CHECK_SEQ(KV_DEL);
    /* dangling count */
    fresh(); key(KV_D); key(KV_W); CHECK(rec_count() == 2); /* dw */
    key(KV_3); CHECK(rec_count() == 2);                     /* 3 pending, no emit */
    key(KV_W); CHECK(rec_count() == 5);                     /* 3w */
}

static void test_insert(void) {
    fresh(); kv_set_mode(KV_MODE_INSERT); rec_start();
    key(KV_A); CHECK_SEQ(KV_A);
    rec_start(); key(KV_ESC); CHECK_SEQ(KV_ESC);
    CHECK(kv_get_mode() == KV_MODE_INSERT); /* Esc does not switch mode */
    fresh(); key(KV_I); CHECK(rec_count() == 0); CHECK(kv_get_mode() == KV_MODE_INSERT);
    fresh(); key(KV_C_I); CHECK_SEQ(KV_HOME); CHECK(kv_get_mode() == KV_MODE_INSERT);
    fresh(); key(KV_A); CHECK_SEQ(KV_RGHT);
    fresh(); key(KV_C_A); CHECK_SEQ(KV_END);
    fresh(); key(KV_O); CHECK_SEQ(KV_END, KV_LSFT_KC(KV_ENT));
    fresh(); key(KV_C_O); CHECK_SEQ(KV_HOME, KV_LSFT_KC(KV_ENT), KV_UP);
}

static void test_visual(void) {
    fresh(); kv_set_mode(KV_MODE_VISUAL); rec_start();
    key(KV_H); CHECK_SEQ(KV_LSFT_KC(KV_LEFT));
    rec_start(); key(KV_D); CHECK_SEQ(KV_LSFT_KC(KV_END), KV_LCTL_KC(KV_X));
    CHECK(kv_get_mode() == KV_MODE_VISUAL); /* still visual */
    rec_start(); key(KV_ESC); CHECK(kv_get_mode() == KV_MODE_NORMAL);
    /* illegal key stays in visual */
    kv_set_mode(KV_MODE_VISUAL); rec_start();
    key(KV_C_I); CHECK(rec_count() == 0); CHECK(kv_get_mode() == KV_MODE_VISUAL);
}

static void test_regress(void) {
    /* A1: count must not leak */
    fresh(); key(KV_3); key(KV_X); CHECK_SEQ(KV_DEL);
    fresh(); key(KV_J); CHECK_SEQ(KV_DOWN);
    /* E4: dd then u => single Ctrl+Z */
    fresh(); key(KV_D); key(KV_D);
    rec_start(); key(KV_U); CHECK_SEQ(KV_LCTL_KC(KV_Z));
    /* A2: repeat dd then . */
    fresh(); key(KV_D); key(KV_D);
    rec_start(); key(KV_DOT);
    CHECK_SEQ(KV_HOME, KV_HOME, KV_LSFT_KC(KV_END), KV_LCTL_KC(KV_X), KV_BSPC);
    /* E2: no modifier wrapping of plain motion output */
    fresh(); key(KV_H);
    CHECK(KV_MODS(rec_at(0)) == 0);
    /* A4: unknown key passes through in normal mode */
    fresh(); key(0x3E); CHECK_SEQ(0x3E);
}

static void test_changes_enter_insert(void) {
    /* s / C / S / cc / cw / c<0> all enter INSERT */
    fresh(); key(KV_S); CHECK(kv_get_mode() == KV_MODE_INSERT);
    fresh(); key(KV_C_C); CHECK(kv_get_mode() == KV_MODE_INSERT);
    fresh(); key(KV_C_S); CHECK(kv_get_mode() == KV_MODE_INSERT);
    fresh(); key(KV_C); key(KV_C); CHECK(kv_get_mode() == KV_MODE_INSERT);
    fresh(); key(KV_C); key(KV_W); CHECK(kv_get_mode() == KV_MODE_INSERT);
    fresh(); key(KV_C); key(KV_0); CHECK(kv_get_mode() == KV_MODE_INSERT);
    fresh(); key(KV_C); key(KV_C_G); CHECK(kv_get_mode() == KV_MODE_INSERT); /* cG */
    fresh(); key(KV_C); key(KV_G); key(KV_G); CHECK(kv_get_mode() == KV_MODE_INSERT); /* cgg */
    fresh(); key(KV_C); key(KV_2); key(KV_C_G); CHECK(kv_get_mode() == KV_MODE_INSERT); /* c2G */
    /* non-change ops keep NORMAL */
    fresh(); key(KV_D); key(KV_D); CHECK(kv_get_mode() == KV_MODE_NORMAL);
    fresh(); key(KV_Y); key(KV_Y); CHECK(kv_get_mode() == KV_MODE_NORMAL);
    fresh(); key(KV_X); CHECK(kv_get_mode() == KV_MODE_NORMAL);
}

static void test_op_mismatch(void) {
    /* d y / y d / d c are not line ops: strict clear, second key re-identified */
    fresh(); key(KV_D); key(KV_Y); CHECK(rec_count() == 0); /* y is a pending operator */
    fresh(); key(KV_D); key(KV_X); CHECK_SEQ(KV_DEL);       /* d x -> x */
    fresh(); key(KV_C_GT); key(KV_C_LT); CHECK(rec_count() == 0);
}

static void test_zero_drops_count(void) {
    fresh(); key(KV_2); key(KV_D); key(KV_0);
    CHECK_SEQ(KV_LSFT_KC(KV_HOME), KV_LCTL_KC(KV_X)); /* d0, count 2 dropped */
    fresh(); key(KV_2); key(KV_C_GT); key(KV_0);
    CHECK_SEQ(KV_TAB); /* >0, count 2 dropped */
}

static void test_repeat(void) {
    /* dd then . replays dd (no recursion) */
    fresh(); key(KV_D); key(KV_D);
    rec_start(); key(KV_DOT);
    CHECK_SEQ(KV_HOME, KV_HOME, KV_LSFT_KC(KV_END), KV_LCTL_KC(KV_X), KV_BSPC);
    /* a second . replays again, does not crash */
    rec_start(); key(KV_DOT);
    CHECK_SEQ(KV_HOME, KV_HOME, KV_LSFT_KC(KV_END), KV_LCTL_KC(KV_X), KV_BSPC);
    /* x . . does not recurse */
    fresh(); key(KV_X); key(KV_DOT); key(KV_DOT);
    CHECK(rec_count() > 0);
    /* discarded prefix does not pollute repeat: g F5 then gg then . */
    fresh(); key(KV_G); key(0x3E); key(KV_G); key(KV_G); /* gg */
    rec_start(); key(KV_DOT);
    CHECK_SEQ(KV_LCTL_KC(KV_HOME)); /* replays gg, not ggg */
}

static void test_big_count(void) {
    fresh(); key(KV_9); key(KV_9); key(KV_W);
    CHECK(rec_count() == 99); /* 99 motions fit in the emit queue */
    /* products are clamped to 99 so the queue cannot overflow */
    fresh(); key(KV_9); key(KV_9); key(KV_D); key(KV_9); key(KV_9); key(KV_W);
    CHECK(rec_count() <= 99 + 1);
}

static void test_pass_through(void) {
    /* non-vim key in NORMAL passes through (caller emits it) */
    fresh(); key(0x3E); CHECK_SEQ(0x3E);
    /* Tab passes through */
    fresh(); key(KV_TAB); CHECK_SEQ(KV_TAB);
    /* Insert passes everything through */
    fresh(); kv_set_mode(KV_MODE_INSERT); rec_start();
    key(KV_A); CHECK_SEQ(KV_A);
}

int main(void) {
    test_single();
    test_count();
    test_op();
    test_indent();
    test_strict_clear();
    test_insert();
    test_visual();
    test_regress();
    test_changes_enter_insert();
    test_op_mismatch();
    test_zero_drops_count();
    test_repeat();
    test_big_count();
    test_pass_through();
    printf("pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
