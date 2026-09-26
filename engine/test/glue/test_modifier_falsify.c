/* test_modifier_falsify.c — host falsification of the P0 "修饰键在 step8 一律
 * 直接放行、不清 pending" fix (commit d15db52, qmk/vim_glue.c).
 *
 * Test-only, separate TU.  It IS listed in the Makefile's GLUE_TESTS, so
 * `make glue-test` builds and runs it alongside the rest.
 *
 * Contract under test (vim/design.md §4.10 / §4.12 #1, vim/testcase.md §10):
 *   - a bare modifier (Shift/Ctrl/Alt/GUI) passes through and NEVER clears a
 *     pending count/operator/prefix/indent; it only updates the physical shadow
 *     (design §4.12 #4);
 *   - therefore `d` Shift `$` still completes `d$` -> Shift+End, Ctrl+X and
 *     must NOT degrade to a plain `$` (End) by wiping the operator;
 *   - a non-modifier non-vim key (F5/`,`) still strictly clears pending;
 *   - pure Shift folds a letter to its vim command (Shift+G == `G`, ...);
 *   - held motion is limited to NORMAL (the `kv_get_mode()==NORMAL` guard):
 *     a bare Visual motion must tap, and must not leak a held arrow into a
 *     later normal command.
 */
#include "qmk_stub.h"
#include "qmk-vim-fn/engine/include/kv.h"
#include "qmk-vim-fn/qmk/vim_glue.h"
#include "qmk-vim-fn/qmk/vim_keymap_common.h"

/* ---------------- host state (mirrors test_strict_clear_falsify.c) -------- */
layer_state_t layer_state         = 0;
layer_state_t default_layer_state = 0;

static uint8_t s_mods;
uint8_t get_mods(void) { return s_mods; }
void    clear_mods(void) { s_mods = 0; }
void    set_mods(uint8_t m) { s_mods = m; }
void    register_mods(uint8_t m) { s_mods |= m; }
void    unregister_mods(uint8_t m) { s_mods &= (uint8_t)~m; }

#define REG_CAP 64
static uint16_t s_reg[REG_CAP];
static int      s_reg_n;
void register_code(uint16_t kc) {
    if (IS_MODIFIER_KEYCODE(kc)) s_mods |= (uint8_t)(1u << (kc - KC_LCTL));
    if (s_reg_n < REG_CAP) s_reg[s_reg_n++] = kc;
}
void unregister_code(uint16_t kc) {
    if (IS_MODIFIER_KEYCODE(kc)) s_mods &= (uint8_t)~(1u << (kc - KC_LCTL));
    for (int i = 0; i < s_reg_n; i++) if (s_reg[i] == kc) { s_reg[i] = s_reg[--s_reg_n]; return; }
}
void tap_code(uint16_t kc) { register_code(kc); unregister_code(kc); }
void tap_code16(uint16_t kc) { tap_code((uint16_t)(kc & 0xFF)); }

static uint32_t g_now;
uint16_t timer_read(void) { return (uint16_t)g_now; }
uint16_t timer_elapsed(uint16_t since) { return (uint16_t)((uint16_t)g_now - since); }
uint32_t timer_read32(void) { return g_now; }
uint32_t timer_elapsed32(uint32_t since) { return g_now - since; }

static int reg_count(uint16_t kc) {
    int n = 0;
    for (int i = 0; i < s_reg_n; i++) if (s_reg[i] == kc) n++;
    return n;
}

/* ---------------- raw emit recorder (engine emit callback) ---------------- */
extern void kv_emit_flush_now(void); /* engine/test helper (src/emit.h) */

#define EMIT_CAP 64
static kv_keycode_t g_emit[EMIT_CAP];
static int          g_emit_n;
static void rec_emit(kv_keycode_t kc) {
    if (g_emit_n < EMIT_CAP) g_emit[g_emit_n++] = kc;
}

/* ---------------- test bookkeeping ---------------- */
static int g_pass, g_fail;
#define CHECK(cond)                                                        \
    do {                                                                   \
        if (cond) { g_pass++; }                                            \
        else { g_fail++; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } \
    } while (0)

static void emit_flush(void) { kv_emit_flush_now(); }

static void print_codes(const char *tag) {
    printf("FAIL %s: emitted %d code(s):", tag, g_emit_n);
    for (int i = 0; i < g_emit_n; i++) printf(" 0x%04X", g_emit[i]);
    printf("\n");
}
static bool emit_is(const kv_keycode_t *exp, int n) {
    if (g_emit_n != n) return false;
    for (int i = 0; i < n; i++) if (g_emit[i] != exp[i]) return false;
    return true;
}

/* ---------------- generic test cfg ---------------- */
static bool test_declared(uint16_t kc) {
    if (kc >= KC_F1 && kc <= KC_F12) return true;
    if (kc == KC_VOLD || kc == KC_VOLU) return true;
    if (kc == KC_SPC || kc == KC_CAPS || kc == KC_ESC) return true;
    if (kc == KC_T) return true;
    return false;
}
static bool test_myfn(uint16_t kc, bool pressed) { (void)kc; (void)pressed; return false; }

static const vim_cfg_t g_cfg = {
    .fn_layer = 4, .trigger_kc = TEST_TRIGGER_KC, .mod_win = KC_RALT, .mod_mac = KC_RGUI,
    .is_mac = NULL, .link_ok = NULL, .hold_ms = 200, .shift_esc_enable = true,
    .led_index = 0, .hook_pre = NULL, .hook_post_myfn = NULL,
    .myfn_declared = test_declared, .myfn = test_myfn, .vim_set_enabled = NULL,
    .shortcuts = vim_default_shortcuts,
};

static bool pipeline(uint16_t kc, bool pressed) {
    keyrecord_t r = {0};
    r.event.pressed = pressed;
    return vim_pipeline_process(kc, &r, &g_cfg);
}

/* vim_glue_init() installs the real vim_emit(); reset_engine leaves it that way
 * so held-motion register/unregister can be observed.  use_raw_emit() overrides
 * it with the raw recorder for pure emit-sequence checks. */
static void reset_engine(void) {
    g_now = 1000; s_mods = 0; s_reg_n = 0;
    layer_state = 0; default_layer_state = 0;
    vim_keymap_common_init();
    kv_set_mode(KV_MODE_NORMAL);
    g_emit_n = 0;
}
static void use_raw_emit(void) { kv_set_emit(rec_emit); }

/* ======================================================================
 * A. A modifier pressed after an operator/prefix must not clear pending:
 *    `d` <mod> `$`/`^`/`G`, `y` <mod> `$`.  Before the fix the modifier made
 *    vim_glue_engine early-return *after* clearing pending, so `d` vanished.
 * ====================================================================== */
static void test_op_modifier_second(const char *name, uint16_t op, uint16_t mod,
                                    uint16_t second, const kv_keycode_t *exp, int exp_n) {
    reset_engine();
    use_raw_emit();

    if (pipeline(op, true) != false) {
        g_fail++; printf("FAIL %s: operator 0x%04X not consumed\n", name, op); return;
    }
    if (kv_pending() != true) {
        g_fail++; printf("FAIL %s: operator 0x%04X did not create pending\n", name, op); return;
    }

    /* critical: the bare modifier passes and leaves the operator pending.
     * Right Shift is the one documented exception: it is swallowed (lazy
     * Shift / no lone Shift) but must still leave the operator pending. */
    bool rsh = (mod == KC_RSFT);
    CHECK(pipeline(mod, true) == (rsh ? false : true));
    if (kv_pending() != true) {
        g_fail++;
        printf("FAIL %s: modifier 0x%04X cleared pending (d+Shift+$ regression)\n", name, mod);
    } else g_pass++;
    CHECK(kv_get_mode() == KV_MODE_NORMAL);

    g_emit_n = 0;
    CHECK(pipeline(second, true) == false);
    emit_flush();
    if (!emit_is(exp, exp_n)) {
        g_fail++;
        printf("FAIL %s: ", name);
        print_codes("op+mod+key");
    } else g_pass++;

    CHECK(kv_get_mode() == KV_MODE_NORMAL);
    CHECK(kv_pending() == false);
    CHECK(pipeline(second, false) == false);
    CHECK(pipeline(mod, false) == (rsh ? false : true));
    CHECK(pipeline(op, false) == false);
}

static void test_op_modifier_family(void) {
    const kv_keycode_t d_dlr[]   = { KV_LSFT_KC(KV_END),  KV_LCTL_KC(KV_X) };
    const kv_keycode_t d_caret[] = { KV_LSFT_KC(KV_HOME), KV_LCTL_KC(KV_X) };
    const kv_keycode_t d_G[]     = { KV_CS(KV_END),       KV_LCTL_KC(KV_X) };
    const kv_keycode_t y_dlr[]   = { KV_LSFT_KC(KV_END),  KV_LCTL_KC(KV_C) };

    test_op_modifier_second("d $", KC_D, KC_LSFT, KC_4, d_dlr,   2);
    test_op_modifier_second("d ^", KC_D, KC_RSFT, KC_6, d_caret, 2);
    test_op_modifier_second("d G", KC_D, KC_LSFT, KC_G, d_G,     2);
    test_op_modifier_second("y $", KC_Y, KC_LSFT, KC_4, y_dlr,   2);
}

/* ======================================================================
 * B. A bare modifier alone (Shift/Ctrl/Alt/GUI) passes through, records only
 *    the shadow, and does not clear a pending count/operator/prefix/indent.
 * ====================================================================== */
static void test_mod_alone(uint16_t prefix_a, uint16_t prefix_b,
                           uint16_t mod, const char *mname) {
    reset_engine();
    use_raw_emit();

    /* Build the pending prefix.  A shifted prefix (Z / >) uses LSFT; release it
     * before the test modifier so the shadow transition is clean. */
    if (prefix_b) {
        CHECK(pipeline(prefix_a, true) == true); /* e.g. LSFT */
        CHECK(pipeline(prefix_b, true) == false);
        CHECK(pipeline(prefix_a, false) == true);
    } else {
        CHECK(pipeline(prefix_a, true) == false);
    }
    if (kv_pending() != true) {
        g_fail++;
        printf("FAIL mod-alone(%s): prefix not pending\n", mname);
        pipeline(prefix_a, false);
        return;
    }

    g_emit_n = 0;
    bool rsh = (mod == KC_RSFT); /* Right Shift is swallowed (lazy Shift) */
    if (pipeline(mod, true) != (rsh ? false : true)) {
        g_fail++;
        printf("FAIL mod-alone(%s): bare modifier 0x%04X wrong pass/consume\n", mname, mod);
    } else g_pass++;
    CHECK(kv_pending() == true);                                 /* never cleared */
    CHECK((vim_glue_mods() & MOD_BIT(mod)) != 0);                /* shadow only */
    CHECK(g_emit_n == 0);                                        /* no emission */

    CHECK(pipeline(mod, false) == (rsh ? false : true));
    CHECK(kv_pending() == true);
    CHECK((vim_glue_mods() & MOD_BIT(mod)) == 0);

    /* clean up the still-pending prefix */
    pipeline(prefix_b ? prefix_b : prefix_a, false);
}

static void test_bare_modifiers_keep_pending(void) {
    const uint16_t mods[]  = { KC_LSFT, KC_LCTL, KC_LALT, KC_LGUI,
                               KC_RSFT, KC_RCTL, KC_RALT, KC_RGUI };
    const char    *mname[] = { "LSFT", "LCTL", "LALT", "LGUI",
                               "RSFT", "RCTL", "RALT", "RGUI" };
    for (int i = 0; i < 8; i++) {
        char tag[64];
        snprintf(tag, sizeof tag, "d+%s", mname[i]);     test_mod_alone(KC_D, 0, mods[i], tag);
        snprintf(tag, sizeof tag, "3+%s", mname[i]);     test_mod_alone(KC_3, 0, mods[i], tag);
        snprintf(tag, sizeof tag, "g+%s", mname[i]);     test_mod_alone(KC_G, 0, mods[i], tag);
        snprintf(tag, sizeof tag, "Z+%s", mname[i]);     test_mod_alone(KC_LSFT, KC_Z, mods[i], tag);
        snprintf(tag, sizeof tag, ">+%s", mname[i]);     test_mod_alone(KC_LSFT, KC_DOT, mods[i], tag);
    }
}

/* ======================================================================
 * C. A non-modifier non-vim key still strictly clears pending.
 * ====================================================================== */
static void test_nonvim_clears(uint16_t nv, const char *name) {
    reset_engine();

    /* operator d + non-vim */
    CHECK(pipeline(KC_D, true) == false);
    CHECK(kv_pending() == true);
    CHECK(pipeline(nv, true) == true);   /* pass through */
    CHECK(kv_pending() == false);        /* strict clear */
    CHECK(pipeline(KC_D, false) == false);
    CHECK(pipeline(nv, false) == true);

    /* count 3 + non-vim */
    CHECK(pipeline(KC_3, true) == false);
    CHECK(kv_pending() == true);
    CHECK(pipeline(nv, true) == true);
    CHECK(kv_pending() == false);
    CHECK(pipeline(KC_3, false) == false);
    CHECK(pipeline(nv, false) == true);

    /* prefix g + non-vim */
    CHECK(pipeline(KC_G, true) == false);
    CHECK(kv_pending() == true);
    CHECK(pipeline(nv, true) == true);
    CHECK(kv_pending() == false);
    CHECK(pipeline(KC_G, false) == false);
    CHECK(pipeline(nv, false) == true);

    (void)name;
}

/* ======================================================================
 * D. Pure Shift folding in NORMAL: Shift+<letter> == the shifted vim command.
 * ====================================================================== */
static void test_shift_fold(uint16_t letter, const kv_keycode_t *exp, int n, const char *name) {
    reset_engine();
    use_raw_emit();

    CHECK(pipeline(KC_LSFT, true) == true);       /* modifier passes, shadow set */
    g_emit_n = 0;
    CHECK(pipeline(letter, true) == false);       /* folded command consumed */
    emit_flush();
    if (!emit_is(exp, n)) {
        g_fail++;
        printf("FAIL shift-fold(%s): ", name);
        print_codes("shift+letter");
    } else g_pass++;
    CHECK(pipeline(letter, false) == false);
    CHECK(pipeline(KC_LSFT, false) == true);
}

static void test_pure_shift_folding(void) {
    const kv_keycode_t g_big[] = { KV_LCTL_KC(KV_END) };
    const kv_keycode_t dlr[]   = { KV_END };
    const kv_keycode_t caret[] = { KV_HOME };
    const kv_keycode_t x_up[]  = { KV_BSPC };
    const kv_keycode_t D[]     = { KV_LSFT_KC(KV_END), KV_LCTL_KC(KV_X) };
    const kv_keycode_t Y[]     = { KV_LSFT_KC(KV_END), KV_LCTL_KC(KV_C) };
    /* cc / S contract (engine test_main.c): line select + delete + BSPC, insert */
    const kv_keycode_t S[]     = { KV_HOME, KV_HOME, KV_LSFT_KC(KV_END),
                                   KV_LCTL_KC(KV_X), KV_BSPC };

    test_shift_fold(KC_G, g_big, 1, "G");
    test_shift_fold(KC_4, dlr,   1, "$");
    test_shift_fold(KC_6, caret, 1, "^");
    test_shift_fold(KC_X, x_up,  1, "X");
    test_shift_fold(KC_D, D,     2, "D");
    test_shift_fold(KC_Y, Y,     2, "Y");
    test_shift_fold(KC_S, S,     5, "S");

    /* Shift+Z enters the Z prefix (pending, no emit) */
    reset_engine();
    use_raw_emit();
    CHECK(pipeline(KC_LSFT, true) == true);
    g_emit_n = 0;
    CHECK(pipeline(KC_Z, true) == false);
    CHECK(kv_pending() == true);
    CHECK(g_emit_n == 0);
    CHECK(pipeline(KC_Z, false) == false);
    CHECK(pipeline(KC_LSFT, false) == true);
}

/* ======================================================================
 * E. Held motion is NORMAL-only (the `kv_get_mode()==NORMAL` guard).
 *    A Visual motion must tap (never leave an arrow registered), and must not
 *    leak `s_held_expect` into a later plain-arrow emit from another command.
 * ====================================================================== */
static void test_held_motion_normal(void) {
    reset_engine(); /* real vim_emit */
    CHECK(pipeline(KC_H, true) == false);
    kv_emit_flush_now();
    if (reg_count(KC_LEFT) != 1) {
        g_fail++;
        printf("FAIL held-normal: 'h' expected KC_LEFT held (registered), got %d\n",
               reg_count(KC_LEFT));
    } else g_pass++;
    CHECK(pipeline(KC_H, false) == false);
    CHECK(reg_count(KC_LEFT) == 0);
}

static void test_visual_motion_no_leak(void) {
    reset_engine(); /* real vim_emit */

    /* Visual 'h' emits Shift+Left: must be a TAP, not a held arrow. */
    CHECK(pipeline(KC_V, true) == false);
    CHECK(kv_get_mode() == KV_MODE_VISUAL);
    CHECK(pipeline(KC_H, true) == false);
    kv_emit_flush_now();
    if (reg_count(KC_LEFT) != 0) {
        g_fail++;
        printf("FAIL visual-held: Visual 'h' left KC_LEFT registered=%d (want tap)\n",
               reg_count(KC_LEFT));
    } else g_pass++;

    /* Keep 'h' held (no release), exit Visual, then run a normal command that
     * emits a plain Left arrow: uppercase P (paste-before char) = Left, Ctrl+V.
     * Before the fix, Visual 'h' set s_held_expect[LEFT]; P's plain Left then
     * register-held KC_LEFT and it never got a release (P's key-up is not a
     * motion key). */
    CHECK(pipeline(KC_ESC, true) == false);
    CHECK(kv_get_mode() == KV_MODE_NORMAL);
    CHECK(pipeline(KC_ESC, false) == false);

    CHECK(pipeline(KC_LSFT, true) == true);   /* Shift folds P -> KV_C_P (paste-before) */
    CHECK(pipeline(KC_P, true) == false);
    kv_emit_flush_now();
    if (reg_count(KC_LEFT) != 0) {
        g_fail++;
        printf("FAIL visual-leak: Normal 'P' after Visual 'h' register-held KC_LEFT=%d "
               "(want plain tap)\n", reg_count(KC_LEFT));
    } else g_pass++;
    CHECK(pipeline(KC_P, false) == false);
    CHECK(pipeline(KC_LSFT, false) == true);

    /* release the still-held Visual 'h' (clears held_expect, releases nothing) */
    CHECK(pipeline(KC_H, false) == false);
    CHECK(reg_count(KC_LEFT) == 0);
}

static void test_count_and_op_motion_tap(void) {
    reset_engine(); /* real vim_emit */

    /* `3l` is not a bare motion: all three arrows tap, none stay registered. */
    CHECK(pipeline(KC_3, true) == false);
    CHECK(pipeline(KC_L, true) == false);
    kv_emit_flush_now();
    CHECK(reg_count(KC_RGHT) == 0);

    /* `dl` in NORMAL -> operator selection, tap only. */
    reset_engine();
    CHECK(pipeline(KC_D, true) == false);
    CHECK(pipeline(KC_L, true) == false);
    kv_emit_flush_now();
    CHECK(reg_count(KC_RGHT) == 0);
}

int main(void) {
    test_op_modifier_family();
    test_bare_modifiers_keep_pending();
    test_nonvim_clears(KC_F5, "F5");
    test_nonvim_clears(KC_COMM, ",");
    test_pure_shift_folding();
    test_held_motion_normal();
    test_visual_motion_no_leak();
    test_count_and_op_motion_tap();

    printf("modifier-falsify: pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
