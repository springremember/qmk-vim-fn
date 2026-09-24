/* test_main.c — host unit tests for the vim engine. */
#include "kvtest.h"
#include "../src/emit.h"
#include "../src/command.h"
#include "../src/classify.h"

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

/* fresh VISUAL / VISUAL-LINE engine with a clean recorder */
static void fresh_visual(void) {
    kv_init();
    kv_enable();
    kv_set_mode(KV_MODE_VISUAL);
    rec_start();
}

static void fresh_vline(void) {
    kv_init();
    kv_enable();
    kv_set_mode(KV_MODE_VISUAL_LINE);
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
    CHECK(kv_get_mode() == KV_MODE_NORMAL); /* Esc still emits, and leaves INSERT */
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

/* testcase.md §9 — API mode/enable transitions must drop pending and the
 * in-progress repeat recording, while preserving a completed s_last. */
static void test_mode_pending_clear(void) {
    /* mode switch drops the pending operator */
    fresh(); key(KV_2); key(KV_D);
    CHECK(kv_pending() == true);
    kv_set_mode(KV_MODE_INSERT);
    kv_set_mode(KV_MODE_NORMAL);
    rec_start(); key(KV_W);
    CHECK_SEQ(KV_LCTL_KC(KV_RGHT)); /* w only, no residual dw */
    CHECK(kv_pending() == false);

    /* disable drops pending and passes every key through */
    fresh(); key(KV_2); key(KV_D);
    CHECK(kv_pending() == true);
    kv_disable();
    CHECK(kv_pending() == false);
    CHECK(kv_kbd(KV_W) == KV_PASSTHROUGH);
    CHECK(kv_kbd(KV_D) == KV_PASSTHROUGH);

    /* enable always restarts in INSERT */
    kv_disable();
    kv_enable();
    CHECK(kv_get_mode() == KV_MODE_INSERT);

    /* a dropped prefix must not pollute repeat: 2d <switch> w . => replay w */
    fresh(); key(KV_2); key(KV_D);
    kv_set_mode(KV_MODE_INSERT);
    kv_set_mode(KV_MODE_NORMAL);
    key(KV_W);
    rec_start(); key(KV_DOT);
    CHECK_SEQ(KV_LCTL_KC(KV_RGHT)); /* w, not 2dw */

    /* a completed command survives a mode round-trip: dd <switch> . => dd */
    fresh(); key(KV_D); key(KV_D);
    kv_set_mode(KV_MODE_INSERT);
    kv_set_mode(KV_MODE_NORMAL);
    rec_start(); key(KV_DOT);
    CHECK_SEQ(KV_HOME, KV_HOME, KV_LSFT_KC(KV_END), KV_LCTL_KC(KV_X), KV_BSPC);
}

/* testcase.md §9 — Shift folds into the modifier bits (LSFT+Esc); the Esc
 * base keycode must still be recognised inside Visual. */
static void test_shift_esc_visual(void) {
    fresh();
    CHECK(kv_kbd(KV_V) == KV_CONSUMED);
    CHECK(kv_get_mode() == KV_MODE_VISUAL);
    CHECK(kv_kbd(KV_LSFT_KC(KV_ESC)) == KV_CONSUMED);
    CHECK(kv_get_mode() == KV_MODE_NORMAL);
}

/* testcase.md §9 — independent contract checks: repeat recording is normal,
 * s_last survives every abort path, modifier-folded Esc cancels a pending
 * operator, enable/disable is idempotent, keyboard-layer modes pass through,
 * and a dropped prefix never contaminates a later command or the repeat. */
static void test_contract_extra(void) {
    /* recording is normal: dd then . replays dd */
    fresh(); key(KV_D); key(KV_D);
    rec_start(); key(KV_DOT);
    CHECK_SEQ(KV_HOME, KV_HOME, KV_LSFT_KC(KV_END), KV_LCTL_KC(KV_X), KV_BSPC);

    /* recording is normal: dw then . replays dw */
    fresh(); key(KV_D); key(KV_W);
    rec_start(); key(KV_DOT);
    CHECK_SEQ(KV_CS(KV_RGHT), KV_LCTL_KC(KV_X));

    /* s_last survives a mode round-trip: dd <switch> . => dd */
    fresh(); key(KV_D); key(KV_D);
    kv_set_mode(KV_MODE_INSERT);
    kv_set_mode(KV_MODE_NORMAL);
    rec_start(); key(KV_DOT);
    CHECK_SEQ(KV_HOME, KV_HOME, KV_LSFT_KC(KV_END), KV_LCTL_KC(KV_X), KV_BSPC);

    /* half command then Esc: in-progress rec dropped, s_last kept
     * (dd, d, Esc, . => dd, not "d d d") */
    fresh(); key(KV_D); key(KV_D);
    key(KV_D);
    CHECK(kv_pending() == true);
    key(KV_ESC);
    CHECK(kv_pending() == false);
    rec_start(); key(KV_DOT);
    CHECK_SEQ(KV_HOME, KV_HOME, KV_LSFT_KC(KV_END), KV_LCTL_KC(KV_X), KV_BSPC);

    /* half command then kv_cancel(): in-progress rec dropped, s_last kept */
    fresh(); key(KV_D); key(KV_D);
    key(KV_2); key(KV_D);
    CHECK(kv_pending() == true);
    kv_cancel();
    CHECK(kv_pending() == false);
    rec_start(); key(KV_DOT);
    CHECK_SEQ(KV_HOME, KV_HOME, KV_LSFT_KC(KV_END), KV_LCTL_KC(KV_X), KV_BSPC);

    /* kv_init() drops s_last too: after dd, re-init, . replays nothing */
    fresh(); key(KV_D); key(KV_D);
    kv_init(); kv_enable(); kv_set_mode(KV_MODE_NORMAL); rec_start();
    key(KV_DOT);
    CHECK(rec_count() == 0);

    /* NORMAL idle Esc (plain and modifier-folded) passes through, no swallow */
    fresh();
    CHECK(kv_kbd(KV_ESC) == KV_PASSTHROUGH);
    CHECK(kv_kbd(KV_LSFT_KC(KV_ESC)) == KV_PASSTHROUGH);
    CHECK(kv_pending() == false);

    /* modifier-folded Esc with a pending operator: consumed, emits nothing */
    fresh(); key(KV_D);
    CHECK(kv_pending() == true);
    rec_start();
    CHECK(kv_kbd(KV_LSFT_KC(KV_ESC)) == KV_CONSUMED);
    flush_emit();
    CHECK(rec_count() == 0);
    CHECK(kv_pending() == false);
    CHECK(kv_get_mode() == KV_MODE_NORMAL);

    /* double disable / double enable is idempotent */
    fresh(); key(KV_2); key(KV_D);
    CHECK(kv_pending() == true);
    kv_disable(); kv_disable();
    CHECK(kv_vim_enabled() == false);
    CHECK(kv_pending() == false);
    CHECK(kv_kbd(KV_W) == KV_PASSTHROUGH);
    CHECK(kv_kbd(KV_DOT) == KV_PASSTHROUGH);
    kv_enable(); kv_enable();
    CHECK(kv_vim_enabled() == true);
    CHECK(kv_get_mode() == KV_MODE_INSERT);
    CHECK(kv_pending() == false);
    CHECK(kv_kbd(KV_W) == KV_PASSTHROUGH); /* INSERT passes through */

    /* MOUSE and any keyboard-layer mode pass everything through */
    fresh(); kv_set_mode(KV_MODE_MOUSE); rec_start();
    CHECK(kv_kbd(KV_H) == KV_PASSTHROUGH);
    CHECK(kv_kbd(KV_ESC) == KV_PASSTHROUGH);
    CHECK(kv_kbd(KV_LSFT_KC(KV_ESC)) == KV_PASSTHROUGH);
    CHECK(kv_kbd((kv_keycode_t)0x3E) == KV_PASSTHROUGH);
    kv_set_mode((kv_mode_t)(KV_MODE_MOUSE + 1));
    CHECK(kv_kbd(KV_H) == KV_PASSTHROUGH);

    /* reverse assertion: 2d -> switch mode -> w must NOT emit dw */
    fresh(); key(KV_2); key(KV_D);
    kv_set_mode(KV_MODE_INSERT);
    kv_set_mode(KV_MODE_NORMAL);
    rec_start();
    CHECK(kv_kbd(KV_W) == KV_CONSUMED);
    flush_emit();
    CHECK_SEQ(KV_LCTL_KC(KV_RGHT)); /* w only */
    rec_start(); key(KV_DOT);
    CHECK_SEQ(KV_LCTL_KC(KV_RGHT)); /* replay w, never 2dw */
}

/* ------------------------------------------------------------------ expanded
 * Coverage added by the test agent.  Every assertion is derived from the
 * authoritative sources (design.md §4.4/§4.8/§4.9, testcase.md) and the
 * actual engine/command implementation; nothing here weakens an existing
 * check. */

/* testcase.md §1 — the remaining word motions.  w/W/e/E all collapse to
 * Ctrl+Right, b/B to Ctrl+Left (command.c emit_motion_once M_W/WBIG/E/EBIG
 * and M_B/BBIG groups). */
static void test_motion_words(void) {
    fresh(); key(KV_W);   CHECK_SEQ(KV_LCTL_KC(KV_RGHT));
    fresh(); key(KV_C_W); CHECK_SEQ(KV_LCTL_KC(KV_RGHT));
    fresh(); key(KV_E);   CHECK_SEQ(KV_LCTL_KC(KV_RGHT));
    fresh(); key(KV_C_E); CHECK_SEQ(KV_LCTL_KC(KV_RGHT));
    fresh(); key(KV_B);   CHECK_SEQ(KV_LCTL_KC(KV_LEFT));
    fresh(); key(KV_C_B); CHECK_SEQ(KV_LCTL_KC(KV_LEFT));
    /* counts repeat the motion */
    fresh(); key(KV_3); key(KV_B);   CHECK(rec_count() == 3);
    fresh(); key(KV_2); key(KV_C_E); CHECK(rec_count() == 2);
}

/* testcase.md §2 — y + motion yanks with Ctrl+C (op != d).  Word motions
 * select with Ctrl+Shift (same range used by dw). */
static void test_yank_motion(void) {
    fresh(); key(KV_Y); key(KV_E);
    CHECK_SEQ(KV_CS(KV_RGHT), KV_LCTL_KC(KV_C));
    fresh(); key(KV_Y); key(KV_W);
    CHECK_SEQ(KV_CS(KV_RGHT), KV_LCTL_KC(KV_C));
    fresh(); key(KV_Y); key(KV_B);
    CHECK_SEQ(KV_CS(KV_LEFT), KV_LCTL_KC(KV_C));
    fresh(); key(KV_Y); key(KV_C_DLR);
    CHECK_SEQ(KV_LSFT_KC(KV_END), KV_LCTL_KC(KV_C));
    fresh(); key(KV_Y); key(KV_C_CARET);
    CHECK_SEQ(KV_LSFT_KC(KV_HOME), KV_LCTL_KC(KV_C));
    /* yy / 3yy (line yank, n lines in one selection) */
    fresh(); key(KV_Y); key(KV_Y);
    CHECK_SEQ(KV_HOME, KV_HOME, KV_LSFT_KC(KV_DOWN), KV_LCTL_KC(KV_C));
    fresh(); key(KV_3); key(KV_Y); key(KV_Y);
    CHECK_SEQ(KV_HOME, KV_HOME, KV_LSFT_KC(KV_DOWN), KV_LSFT_KC(KV_DOWN),
              KV_LSFT_KC(KV_DOWN), KV_LCTL_KC(KV_C));
}

/* testcase.md §2 — operator corners: d$/d^/d0, postfix counts, count drop on
 * G/gg, mismatched key re-identification, and c -> Ctrl+X + Insert. */
static void test_op_corners(void) {
    fresh(); key(KV_D); key(KV_C_DLR);
    CHECK_SEQ(KV_LSFT_KC(KV_END), KV_LCTL_KC(KV_X));
    fresh(); key(KV_D); key(KV_C_CARET);
    CHECK_SEQ(KV_LSFT_KC(KV_HOME), KV_LCTL_KC(KV_X));
    fresh(); key(KV_D); key(KV_0);
    CHECK_SEQ(KV_LSFT_KC(KV_HOME), KV_LCTL_KC(KV_X));

    /* d20 is pending (0 continues the count, not a line-start motion) */
    fresh(); key(KV_D); key(KV_2); key(KV_0);
    CHECK(kv_pending() == true); CHECK(rec_count() == 0);
    key(KV_W);
    CHECK(rec_count() == 21); /* 20 x Ctrl+Shift+Right + Ctrl+X */
    CHECK(kv_pending() == false);

    fresh(); key(KV_D); key(KV_2); key(KV_0); key(KV_W);
    CHECK(rec_count() == 21);
    CHECK(rec_at(0) == KV_CS(KV_RGHT));
    CHECK(rec_at(20) == KV_LCTL_KC(KV_X));

    /* G always drops counts: dG / 2dG / d2G are all dG */
    fresh(); key(KV_D); key(KV_C_G); CHECK_SEQ(KV_CS(KV_END), KV_LCTL_KC(KV_X));
    fresh(); key(KV_2); key(KV_D); key(KV_C_G); CHECK_SEQ(KV_CS(KV_END), KV_LCTL_KC(KV_X));
    fresh(); key(KV_D); key(KV_2); key(KV_C_G); CHECK_SEQ(KV_CS(KV_END), KV_LCTL_KC(KV_X));
    /* dgg / d2gg are both dgg */
    fresh(); key(KV_D); key(KV_G); key(KV_G); CHECK_SEQ(KV_CS(KV_HOME), KV_LCTL_KC(KV_X));
    fresh(); key(KV_D); key(KV_2); key(KV_G); key(KV_G); CHECK_SEQ(KV_CS(KV_HOME), KV_LCTL_KC(KV_X));
    /* mismatched key after a postfix count: d2x -> x */
    fresh(); key(KV_D); key(KV_2); key(KV_X); CHECK_SEQ(KV_DEL);
    CHECK(kv_pending() == false);
    /* postfix count multiplies the prefix (2d3w = d6w) */
    fresh(); key(KV_2); key(KV_D); key(KV_3); key(KV_W);
    CHECK(rec_count() == 7);
    CHECK(kv_pending() == false);
    /* c operator: change + Insert */
    fresh(); key(KV_C); key(KV_W);
    CHECK_SEQ(KV_CS(KV_RGHT), KV_LCTL_KC(KV_X));
    CHECK(kv_get_mode() == KV_MODE_INSERT);
    CHECK(kv_pending() == false);

    /* G1 (design §4.4 engine.c L217-225 / §4.7): c2w drives the ST_OPCNT
     * branch with op == KV_C: two word selections, then Ctrl+X and Insert. */
    fresh(); key(KV_C); key(KV_2); key(KV_W);
    CHECK_SEQ(KV_CS(KV_RGHT), KV_CS(KV_RGHT), KV_LCTL_KC(KV_X));
    CHECK(kv_get_mode() == KV_MODE_INSERT);

    /* G3 (design §4.4 / command.c emit_op_range L36): operator + k selects the
     * line above (Shift+Up), then the register op. */
    fresh(); key(KV_D); key(KV_K);
    CHECK_SEQ(KV_LSFT_KC(KV_UP), KV_LCTL_KC(KV_X));
    /* postfix count multiplies: d2k -> 2 x Shift+Up + Ctrl+X */
    fresh(); key(KV_D); key(KV_2); key(KV_K);
    CHECK(rec_count() == 3);
    CHECK(kv_pending() == false);
}

/* testcase.md §4 §6 — indent corners: >G / >gg / >2G / >2gg, the default
 * (non-expected key) path, and 2-digit postfix-count saturation. */
static void test_indent_corners(void) {
    fresh(); key(KV_C_GT); key(KV_C_G);
    CHECK_SEQ(KV_CS(KV_END), KV_TAB);
    fresh(); key(KV_2); key(KV_C_GT); key(KV_C_G);
    CHECK_SEQ(KV_CS(KV_END), KV_TAB);
    fresh(); key(KV_C_GT); key(KV_2); key(KV_C_G);
    CHECK_SEQ(KV_CS(KV_END), KV_TAB);
    fresh(); key(KV_C_GT); key(KV_G); key(KV_G);
    CHECK_SEQ(KV_CS(KV_HOME), KV_TAB);
    fresh(); key(KV_2); key(KV_C_GT); key(KV_G); key(KV_G);
    CHECK_SEQ(KV_CS(KV_HOME), KV_TAB);
    fresh(); key(KV_C_GT); key(KV_2); key(KV_G); key(KV_G);
    CHECK_SEQ(KV_CS(KV_HOME), KV_TAB);
    /* < variant keeps the >/< identity */
    fresh(); key(KV_C_LT); key(KV_2); key(KV_G); key(KV_G);
    CHECK_SEQ(KV_CS(KV_HOME), KV_LSFT_KC(KV_TAB));
    /* >^ (caret branch: indent only, no selection) */
    fresh(); key(KV_C_GT); key(KV_C_CARET); CHECK_SEQ(KV_TAB);
    /* >x: x is unexpected -> clear >, re-identify x */
    fresh(); key(KV_C_GT); key(KV_X); CHECK_SEQ(KV_DEL);
    CHECK(kv_pending() == false);
    /* >234j: postfix count stops at 2 digits (34 ignored -> 23) */
    fresh(); key(KV_C_GT); key(KV_2); key(KV_3); key(KV_4); key(KV_J);
    CHECK(rec_count() == 24); /* 23 x Shift+Down + Tab */
    CHECK(kv_pending() == false);

    /* G2 (design §4.4 engine.c L260-276, ST_ANGCnt default): >2x clears the
     * indent prefix and re-identifies x in IDLE -> delete char. */
    fresh(); key(KV_C_GT); key(KV_2); key(KV_X);
    CHECK_SEQ(KV_DEL);
    CHECK(kv_pending() == false);

    /* G3 (design §4.4 / command.c emit_op_range L36): indent + k selects the
     * line above (Shift+Up), then Tab. */
    fresh(); key(KV_C_GT); key(KV_K);
    CHECK_SEQ(KV_LSFT_KC(KV_UP), KV_TAB);
    CHECK(kv_pending() == false);
}

/* testcase.md §6 A1 — counts are discarded by every single-key command that
 * does not accept one, and no count leaks into the next command. */
static void test_count_drop(void) {
    /* insert entries drop the count */
    fresh(); key(KV_3); key(KV_I);
    CHECK(rec_count() == 0); CHECK(kv_get_mode() == KV_MODE_INSERT); CHECK(kv_pending() == false);
    fresh(); key(KV_3); key(KV_C_I); CHECK_SEQ(KV_HOME); CHECK(kv_get_mode() == KV_MODE_INSERT);
    fresh(); key(KV_3); key(KV_A);   CHECK_SEQ(KV_RGHT); CHECK(kv_get_mode() == KV_MODE_INSERT);
    fresh(); key(KV_3); key(KV_C_A); CHECK_SEQ(KV_END); CHECK(kv_get_mode() == KV_MODE_INSERT);
    fresh(); key(KV_3); key(KV_O);
    CHECK_SEQ(KV_END, KV_LSFT_KC(KV_ENT)); CHECK(kv_get_mode() == KV_MODE_INSERT);
    fresh(); key(KV_3); key(KV_C_O);
    CHECK_SEQ(KV_HOME, KV_LSFT_KC(KV_ENT), KV_UP); CHECK(kv_get_mode() == KV_MODE_INSERT);

    /* visual entries drop the count */
    fresh(); key(KV_3); key(KV_V);
    CHECK(rec_count() == 0); CHECK(kv_get_mode() == KV_MODE_VISUAL); CHECK(kv_pending() == false);
    fresh(); key(KV_3); key(KV_C_V);
    CHECK(rec_count() == 0); CHECK(kv_get_mode() == KV_MODE_VISUAL_LINE);

    /* single-key commands behave exactly as without a count */
    fresh(); key(KV_3); key(KV_S);
    CHECK_SEQ(KV_LSFT_KC(KV_RGHT), KV_DEL);
    CHECK(kv_get_mode() == KV_MODE_INSERT); CHECK(kv_pending() == false);
    fresh(); key(KV_3); key(KV_P);    CHECK_SEQ(KV_LCTL_KC(KV_V)); CHECK(kv_pending() == false);
    fresh(); key(KV_3); key(KV_C_P);  CHECK_SEQ(KV_LEFT, KV_LCTL_KC(KV_V)); CHECK(kv_pending() == false);
    fresh(); key(KV_3); key(KV_C_J);  CHECK_SEQ(KV_END, KV_DEL); CHECK(kv_pending() == false);
    fresh(); key(KV_3); key(KV_U);    CHECK_SEQ(KV_LCTL_KC(KV_Z)); CHECK(kv_pending() == false);
    fresh(); key(KV_3); key(KV_C_X);  CHECK_SEQ(KV_BSPC); CHECK(kv_pending() == false);
    fresh(); key(KV_3); key(KV_C_C);
    CHECK_SEQ(KV_LSFT_KC(KV_END), KV_LCTL_KC(KV_X));
    CHECK(kv_get_mode() == KV_MODE_INSERT); CHECK(kv_pending() == false);
    fresh(); key(KV_3); key(KV_C_D);  CHECK_SEQ(KV_LSFT_KC(KV_END), KV_LCTL_KC(KV_X)); CHECK(kv_pending() == false);
    fresh(); key(KV_3); key(KV_C_Y);  CHECK_SEQ(KV_LSFT_KC(KV_END), KV_LCTL_KC(KV_C)); CHECK(kv_pending() == false);

    /* 3. drops the count and replays the previous completed command */
    fresh(); key(KV_X); rec_start(); key(KV_3); key(KV_DOT);
    CHECK_SEQ(KV_DEL); CHECK(kv_pending() == false);

    /* 3ZZ drops the count and still saves */
    fresh(); key(KV_3); key(KV_C_Z); key(KV_C_Z);
    CHECK_SEQ(KV_LCTL_KC(KV_S)); CHECK(kv_pending() == false);

    /* 3S accepts the count (== 3cc), 3gg drops it */
    fresh(); key(KV_3); key(KV_C_S);
    CHECK_SEQ(KV_HOME, KV_HOME, KV_LSFT_KC(KV_END), KV_LSFT_KC(KV_DOWN),
              KV_LSFT_KC(KV_DOWN), KV_LCTL_KC(KV_X), KV_BSPC);
    CHECK(kv_get_mode() == KV_MODE_INSERT); CHECK(kv_pending() == false);
    fresh(); key(KV_3); key(KV_G); key(KV_G); CHECK_SEQ(KV_LCTL_KC(KV_HOME));
    /* 42G drops the count */
    fresh(); key(KV_4); key(KV_2); key(KV_C_G); CHECK_SEQ(KV_LCTL_KC(KV_END));
}

/* testcase.md §5 — Z prefix: Z + non-Z (vim key or not) clears Z and
 * re-identifies; ZZ saves. */
static void test_z_prefix(void) {
    fresh(); key(KV_C_Z); key(KV_X); CHECK_SEQ(KV_DEL); CHECK(kv_pending() == false);
    fresh(); key(KV_C_Z); key(KV_C_P); CHECK_SEQ(KV_LEFT, KV_LCTL_KC(KV_V)); CHECK(kv_pending() == false);
    fresh(); key(KV_C_Z); key(0x3E /*F5*/); CHECK_SEQ(0x3E); CHECK(kv_pending() == false);
    fresh(); key(KV_C_Z); key(KV_C_Z); CHECK_SEQ(KV_LCTL_KC(KV_S)); CHECK(kv_pending() == false);
}

/* testcase.md §7 / design §4.9 — the full Visual command set. */
static void test_visual_commands(void) {
    fresh_visual(); key(KV_L);       CHECK_SEQ(KV_LSFT_KC(KV_RGHT));
    fresh_visual(); key(KV_J);       CHECK_SEQ(KV_LSFT_KC(KV_DOWN));
    fresh_visual(); key(KV_K);       CHECK_SEQ(KV_LSFT_KC(KV_UP));
    fresh_visual(); key(KV_H);       CHECK_SEQ(KV_LSFT_KC(KV_LEFT));
    fresh_visual(); key(KV_B);       CHECK_SEQ(KV_CS(KV_LEFT));
    fresh_visual(); key(KV_C_B);     CHECK_SEQ(KV_CS(KV_LEFT));
    fresh_visual(); key(KV_W);       CHECK_SEQ(KV_CS(KV_RGHT));
    fresh_visual(); key(KV_E);       CHECK_SEQ(KV_CS(KV_RGHT));
    fresh_visual(); key(KV_C_W);     CHECK_SEQ(KV_CS(KV_RGHT));
    fresh_visual(); key(KV_C_E);     CHECK_SEQ(KV_CS(KV_RGHT));
    fresh_visual(); key(KV_0);       CHECK_SEQ(KV_LSFT_KC(KV_HOME));
    fresh_visual(); key(KV_C_CARET); CHECK_SEQ(KV_LSFT_KC(KV_HOME));
    fresh_visual(); key(KV_C_DLR);   CHECK_SEQ(KV_LSFT_KC(KV_END));
    fresh_visual(); key(KV_C_G);     CHECK_SEQ(KV_CS(KV_END));

    /* y: yank selection, stay in Visual */
    fresh_visual(); key(KV_Y);
    CHECK_SEQ(KV_LSFT_KC(KV_END), KV_LCTL_KC(KV_C));
    CHECK(kv_get_mode() == KV_MODE_VISUAL);
    CHECK(kv_pending() == false);

    /* d / x: cut selection */
    fresh_visual(); key(KV_D); CHECK_SEQ(KV_LSFT_KC(KV_END), KV_LCTL_KC(KV_X));
    fresh_visual(); key(KV_X); CHECK_SEQ(KV_LSFT_KC(KV_END), KV_LCTL_KC(KV_X));

    /* c: cut + Insert */
    fresh_visual(); key(KV_C);
    CHECK_SEQ(KV_LSFT_KC(KV_END), KV_LCTL_KC(KV_X));
    CHECK(kv_get_mode() == KV_MODE_INSERT);

    /* s: substitute + Insert */
    fresh_visual(); key(KV_S);
    CHECK_SEQ(KV_LSFT_KC(KV_RGHT), KV_DEL);
    CHECK(kv_get_mode() == KV_MODE_INSERT);

    /* p: paste */
    fresh_visual(); key(KV_P); CHECK_SEQ(KV_LCTL_KC(KV_V));

    /* illegal keys are swallowed and stay in Visual */
    fresh_visual(); key(KV_C_I);
    CHECK(rec_count() == 0); CHECK(kv_get_mode() == KV_MODE_VISUAL); CHECK(kv_pending() == false);
    fresh_visual(); key(KV_G);
    CHECK(rec_count() == 0); CHECK(kv_get_mode() == KV_MODE_VISUAL);
}

/* design §4.9 — VISUAL_LINE routes through the same feed_visual switch. */
static void test_visual_line_commands(void) {
    fresh_vline(); CHECK(kv_get_mode() == KV_MODE_VISUAL_LINE);
    key(KV_J); CHECK_SEQ(KV_LSFT_KC(KV_DOWN));
    fresh_vline(); key(KV_B);     CHECK_SEQ(KV_CS(KV_LEFT));
    fresh_vline(); key(KV_C_DLR); CHECK_SEQ(KV_LSFT_KC(KV_END));
    fresh_vline(); key(KV_Y);     CHECK_SEQ(KV_LSFT_KC(KV_END), KV_LCTL_KC(KV_C));
    fresh_vline(); key(KV_D);     CHECK_SEQ(KV_LSFT_KC(KV_END), KV_LCTL_KC(KV_X));
    fresh_vline(); key(KV_P);     CHECK_SEQ(KV_LCTL_KC(KV_V));
    fresh_vline(); key(KV_C);
    CHECK_SEQ(KV_LSFT_KC(KV_END), KV_LCTL_KC(KV_X));
    CHECK(kv_get_mode() == KV_MODE_INSERT);
    fresh_vline(); key(KV_S);
    CHECK_SEQ(KV_LSFT_KC(KV_RGHT), KV_DEL);
    CHECK(kv_get_mode() == KV_MODE_INSERT);
    fresh_vline(); key(KV_ESC); CHECK(kv_get_mode() == KV_MODE_NORMAL);
}

/* command.c §4.8 — direct mapping coverage of kv_emit_visual_motion.
 * KV_C_G ('G') is a legal Visual movement (readme §2/§7); feed_visual routes
 * T_G_BIG now, so it is reachable via the parser too (see test_visual_commands). */
static void test_visual_motion_map(void) {
    rec_start(); kv_emit_visual_motion(KV_H);       flush_emit(); CHECK_SEQ(KV_LSFT_KC(KV_LEFT));
    rec_start(); kv_emit_visual_motion(KV_J);       flush_emit(); CHECK_SEQ(KV_LSFT_KC(KV_DOWN));
    rec_start(); kv_emit_visual_motion(KV_K);       flush_emit(); CHECK_SEQ(KV_LSFT_KC(KV_UP));
    rec_start(); kv_emit_visual_motion(KV_L);       flush_emit(); CHECK_SEQ(KV_LSFT_KC(KV_RGHT));
    rec_start(); kv_emit_visual_motion(KV_W);       flush_emit(); CHECK_SEQ(KV_CS(KV_RGHT));
    rec_start(); kv_emit_visual_motion(KV_E);       flush_emit(); CHECK_SEQ(KV_CS(KV_RGHT));
    rec_start(); kv_emit_visual_motion(KV_C_W);     flush_emit(); CHECK_SEQ(KV_CS(KV_RGHT));
    rec_start(); kv_emit_visual_motion(KV_C_E);     flush_emit(); CHECK_SEQ(KV_CS(KV_RGHT));
    rec_start(); kv_emit_visual_motion(KV_B);       flush_emit(); CHECK_SEQ(KV_CS(KV_LEFT));
    rec_start(); kv_emit_visual_motion(KV_C_B);     flush_emit(); CHECK_SEQ(KV_CS(KV_LEFT));
    rec_start(); kv_emit_visual_motion(KV_0);       flush_emit(); CHECK_SEQ(KV_LSFT_KC(KV_HOME));
    rec_start(); kv_emit_visual_motion(KV_C_CARET); flush_emit(); CHECK_SEQ(KV_LSFT_KC(KV_HOME));
    rec_start(); kv_emit_visual_motion(KV_C_DLR);   flush_emit(); CHECK_SEQ(KV_LSFT_KC(KV_END));
    rec_start(); kv_emit_visual_motion(KV_C_G);     flush_emit(); CHECK_SEQ(KV_CS(KV_END));
    /* default branch: unknown key emits nothing */
    rec_start(); kv_emit_visual_motion(KV_Q);       flush_emit(); CHECK(rec_count() == 0);
}

/* design §4.8 — direct emitter guard coverage (n < 1 clamp, M_NONE, and
 * enter_insert's default branch). */
static void test_command_guards(void) {
    rec_start(); kv_emit_motion(M_H, 0);  flush_emit(); CHECK_SEQ(KV_LEFT);
    rec_start(); kv_emit_motion(M_H, -1); flush_emit(); CHECK_SEQ(KV_LEFT);
    rec_start(); kv_emit_motion(M_NONE, 1); flush_emit(); CHECK(rec_count() == 0);

    rec_start(); kv_emit_op_motion(KV_D, M_W, 0); flush_emit();
    CHECK_SEQ(KV_CS(KV_RGHT), KV_LCTL_KC(KV_X));
    rec_start(); kv_emit_op_motion(KV_C, M_W, 0); flush_emit();
    CHECK_SEQ(KV_CS(KV_RGHT), KV_LCTL_KC(KV_X));
    /* op + M_NONE: no selection, just the register op */
    rec_start(); kv_emit_op_motion(KV_D, M_NONE, 1); flush_emit(); CHECK_SEQ(KV_LCTL_KC(KV_X));

    rec_start(); kv_emit_line_op(KV_D, 0); flush_emit();
    CHECK_SEQ(KV_HOME, KV_HOME, KV_LSFT_KC(KV_END), KV_LCTL_KC(KV_X), KV_BSPC);
    rec_start(); kv_emit_line_op(KV_Y, 0); flush_emit();
    CHECK_SEQ(KV_HOME, KV_HOME, KV_LSFT_KC(KV_DOWN), KV_LCTL_KC(KV_C));
    /* cc via the emitter enters Insert */
    rec_start(); kv_emit_line_op(KV_C, 1); flush_emit();
    CHECK_SEQ(KV_HOME, KV_HOME, KV_LSFT_KC(KV_END), KV_LCTL_KC(KV_X), KV_BSPC);

    rec_start(); kv_emit_indent_line(KV_C_GT, 0); flush_emit();
    CHECK_SEQ(KV_HOME, KV_HOME, KV_TAB);
    rec_start(); kv_emit_indent_motion(KV_C_GT, M_H, 0); flush_emit();
    CHECK_SEQ(KV_LSFT_KC(KV_LEFT), KV_TAB);
    rec_start(); kv_emit_indent_motion(KV_C_LT, M_NONE, 1); flush_emit();
    CHECK_SEQ(KV_LSFT_KC(KV_TAB));

    /* enter_insert default and i both emit nothing */
    rec_start(); kv_emit_enter_insert(KV_Z); flush_emit(); CHECK(rec_count() == 0);
    rec_start(); kv_emit_enter_insert(KV_I); flush_emit(); CHECK(rec_count() == 0);
}

/* classify.c — the digit 4..8 and B/E/W-variant branches, classify_digit's
 * fall-through, and is_vim_key. */
static void test_classify_coverage(void) {
    CHECK(kv_classify(KV_4) == T_COUNT);
    CHECK(kv_classify(KV_5) == T_COUNT);
    CHECK(kv_classify(KV_6) == T_COUNT);
    CHECK(kv_classify(KV_7) == T_COUNT);
    CHECK(kv_classify(KV_8) == T_COUNT);
    CHECK(kv_classify(KV_B) == T_MOTION);
    CHECK(kv_classify(KV_E) == T_MOTION);
    CHECK(kv_classify(KV_C_W) == T_MOTION);
    CHECK(kv_classify(KV_C_B) == T_MOTION);
    CHECK(kv_classify(KV_C_E) == T_MOTION);
    /* classify_digit: every digit -> T_DIGIT, everything else delegated */
    CHECK(kv_classify_digit(KV_0) == T_DIGIT);
    CHECK(kv_classify_digit(KV_4) == T_DIGIT);
    CHECK(kv_classify_digit(KV_9) == T_DIGIT);
    CHECK(kv_classify_digit(KV_W) == T_MOTION);
    CHECK(kv_classify_digit(KV_H) == T_MOTION);
    /* is_vim_key */
    CHECK(kv_is_vim_key(KV_4) == true);
    CHECK(kv_is_vim_key(KV_C_W) == true);
    CHECK(kv_is_vim_key((kv_keycode_t)0x3E) == false);
}

/* testcase.md §11 / design §4.7 — non-blocking emit queue timing. */
static void test_emit_queue_timing(void) {
    fresh();
    kv_kbd(KV_D); kv_kbd(KV_D);
    CHECK(rec_count() == 0);        /* queued, nothing sent synchronously */
    CHECK(kv_emit_pending() == 5);  /* Home,Home,Shift+End,Ctrl+X,Bspc */

    kv_task(100);
    CHECK(rec_count() == 1);        /* first key sent immediately */
    CHECK(kv_emit_pending() == 4);

    /* same timestamp: gap not elapsed -> suppressed */
    kv_task(100);
    CHECK(rec_count() == 1);
    CHECK(kv_emit_pending() == 4);

    /* after KV_EMIT_GAP_MS the next key is released */
    kv_task(100 + KV_EMIT_GAP_MS);
    CHECK(rec_count() == 2);
    CHECK(kv_emit_pending() == 3);

    /* flush drains the remainder and resets the gap timer */
    flush_emit();
    CHECK(kv_emit_pending() == 0);
    CHECK(rec_count() == 5);

    kv_kbd(KV_W);
    CHECK(kv_emit_pending() == 1);
    kv_task(200);
    CHECK(rec_count() == 6);        /* sends immediately after flush */

    /* service with an empty queue is a no-op */
    kv_task(500);
    CHECK(rec_count() == 6);
}

/* testcase.md §11 / emit.h — queue primitives and the overflow boundary. */
static void test_emit_bounds(void) {
    rec_start();
    CHECK(kv_emit_pending() == 0);

    /* n <= 0 enqueues nothing */
    kv_emit_taps(KV_A, 0);
    CHECK(kv_emit_pending() == 0);
    kv_emit_taps(KV_A, -5);
    CHECK(kv_emit_pending() == 0);
    kv_emit_taps(KV_A, 3);
    CHECK(kv_emit_pending() == 3);
    flush_emit();
    CHECK_SEQ(KV_A, KV_A, KV_A);

    /* kv_emit_seq with n=0 is a no-op, otherwise pushes in order */
    rec_start();
    const kv_keycode_t seq3[] = { KV_A, KV_B, KV_C };
    kv_emit_seq(seq3, 0);
    CHECK(kv_emit_pending() == 0);
    kv_emit_seq(seq3, 3);
    CHECK(kv_emit_pending() == 3);
    flush_emit();
    CHECK_SEQ(KV_A, KV_B, KV_C);

    /* push drops silently once EMIT_CAP (256) is reached */
    rec_start();
    kv_emit_taps(KV_A, 300);
    CHECK(kv_emit_pending() == 256);
    kv_emit_tap(KV_B);              /* still full: dropped */
    CHECK(kv_emit_pending() == 256);
    CHECK(rec_count() == 0);
    flush_emit();
    CHECK(rec_count() == 256);
    CHECK(rec_at(0) == KV_A);
    CHECK(rec_at(255) == KV_A);

    /* empty service on a cleared queue */
    rec_start();
    kv_emit_service(0);
    kv_emit_service(1000);
    CHECK(rec_count() == 0);
}

/* b4 (audit c6) — emit.c:44 `if (s_fn)` false arm.  With no installed
 * callback, send_one() still dequeues the head (s_count--, s_head++) and
 * then silently drops the keycode: the queue is consumed, nothing is
 * delivered, and no call is made through a NULL pointer.  Asserted against
 * the source behavior (dequeue + discard, not "keep the queue"). */
static void test_emit_null_callback(void) {
    rec_start();                 /* clean recorder + install rec_cb */
    kv_set_emit(NULL);           /* clear the callback: s_fn == NULL */

    /* service path: kv_emit_service -> send_one -> if (s_fn) false */
    kv_emit_taps(KV_A, 1);
    CHECK(kv_emit_pending() == 1);
    kv_emit_service(0);          /* must not crash; dequeues and drops */
    CHECK(kv_emit_pending() == 0); /* head consumed despite no callback */
    CHECK(rec_count() == 0);       /* nothing delivered to the recorder */

    /* flush path: kv_emit_flush_now -> send_one with s_fn == NULL */
    kv_emit_taps(KV_B, 2);
    CHECK(kv_emit_pending() == 2);
    kv_emit_flush_now();
    CHECK(kv_emit_pending() == 0);
    CHECK(rec_count() == 0);

    /* restore a valid callback so later tests are not polluted */
    rec_start();
    kv_emit_tap(KV_C);
    flush_emit();
    CHECK_SEQ(KV_C);
}

/* engine.c rec_push saturation: a pending chain longer than REC_MAX (8) must
 * not corrupt state or drop the completed command. */
static void test_rec_cap(void) {
    fresh();
    key(KV_D);
    for (int i = 0; i < 9; i++) key(KV_9);
    key(KV_W);
    CHECK(rec_count() == 100); /* 99 x Ctrl+Shift+Right + Ctrl+X (count clamped) */
    CHECK(kv_pending() == false);
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
    test_mode_pending_clear();
    test_shift_esc_visual();
    test_contract_extra();
    test_motion_words();
    test_yank_motion();
    test_op_corners();
    test_indent_corners();
    test_count_drop();
    test_z_prefix();
    test_visual_commands();
    test_visual_line_commands();
    test_visual_motion_map();
    test_command_guards();
    test_classify_coverage();
    test_emit_queue_timing();
    test_emit_bounds();
    test_emit_null_callback();
    test_rec_cap();
    printf("pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
