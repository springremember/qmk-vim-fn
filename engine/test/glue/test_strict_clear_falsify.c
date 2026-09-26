/* test_strict_clear_falsify.c — host falsification of the "非 vim/CAG 早退前
 * 严格清空" fix (commit d9ad1c8) plus the Esc-by-physical-keycode regression
 * (MO(9) low byte 0x29 must NOT be mistaken for Esc).
 *
 * Test-only, separate TU.  It IS listed in the Makefile's GLUE_TESTS (see
 * engine/Makefile), so `make glue-test` builds and runs it alongside the rest.
 *
 * Contract:
 *   design §4.5 "严格清空": pending + non-vim -> clear pending, pass through.
 *   design §4.10 / d9ad1c8: the glue's early returns (CAG, non-vim incl. the
 *     Visual passthrough) must clear pending before returning, and Esc must be
 *     matched by the *physical* keycode so a layer key whose low byte is 0x29
 *     (real QMK MO(9) = 0x5229) is never fed to the engine as Esc.
 *
 * NOTE on keycode values: engine/test/glue/qmk_stub.h defines
 *   QK_MOMENTARY 0x5200 -> MO(9) == 0x5209 (low byte 0x09)
 * while real QMK (quantum/keycodes.h) defines QK_MOMENTARY = 0x5220, so the
 * real MO(9) == 0x5229 (low byte 0x29 == KC_ESC).  This probe therefore uses
 * the literal real value REAL_MO9 = 0x5229 to exercise the actual regression.
 */
#include "qmk_stub.h"
#include "qmk-vim-fn/engine/include/kv.h"
#include "qmk-vim-fn/qmk/vim_glue.h"
#include "qmk-vim-fn/qmk/vim_keymap_common.h"

/* Real QMK: MO(9) = QK_MOMENTARY(0x5220) | 9 = 0x5229; low byte == KC_ESC. */
#define REAL_MO9 0x5229u

/* ---------------- host state (mirrors test_pending_clear_probe.c) -------- */
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

/* ---------------- raw emit recorder (engine emits packed keycodes) ------- */
extern void kv_emit_flush_now(void); /* engine/test helper (src/emit.h) */

#define EMIT_CAP 64
static kv_keycode_t g_emit[EMIT_CAP];
static int          g_emit_n;
static void rec_emit(kv_keycode_t kc) {
    if (g_emit_n < EMIT_CAP) g_emit[g_emit_n++] = kc;
}
static void emit_flush(void) { kv_emit_flush_now(); }

/* ---------------- test bookkeeping ---------------- */
static int g_pass, g_fail;
#define CHECK(cond)                                                        \
    do {                                                                   \
        if (cond) { g_pass++; }                                            \
        else { g_fail++; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } \
    } while (0)

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

static void reset_engine(void) {
    g_now = 1000; s_mods = 0; s_reg_n = 0;
    layer_state = 0; default_layer_state = 0;
    vim_keymap_common_init();
    kv_set_emit(rec_emit); /* observe raw engine emissions, not vim_emit taps */
    g_emit_n = 0;
    kv_set_mode(KV_MODE_NORMAL);
}

/* ======================================================================
 * A. NORMAL: `d` -> non-vim key clears pending and passes through; a following
 *    `w` is a fresh standalone motion (NOT `dw`); `d`->non-vim->`d` is not `dd`.
 * ====================================================================== */
static void test_normal_strict_clear(uint16_t nv, const char *name) {
    /* --- pending is cleared and the non-vim key passes through --- */
    reset_engine();
    if (pipeline(KC_D, true) != false) { g_fail++; printf("FAIL %s: 'd' not consumed\n", name); return; }
    CHECK(kv_pending() == true);
    CHECK(pipeline(nv, true) == true);   /* non-vim: let through */
    CHECK(kv_pending() == false);        /* strict clear */
    CHECK(pipeline(KC_D, false) == false); /* paired 'd' release */
    CHECK(pipeline(nv, false) == true);  /* non-vim release passes */

    /* --- following `w` only moves --- */
    reset_engine();
    CHECK(pipeline(KC_D, true) == false);
    CHECK(pipeline(nv, true) == true);
    CHECK(pipeline(KC_D, false) == false);
    CHECK(pipeline(nv, false) == true);
    g_emit_n = 0;
    CHECK(pipeline(KC_W, true) == false);
    CHECK(kv_get_mode() == KV_MODE_NORMAL);
    CHECK(kv_pending() == false);
    emit_flush();
    {
        const kv_keycode_t exp[] = { KV_LCTL_KC(KV_RGHT) }; /* w = Ctrl+Right */
        bool ok = (g_emit_n == 1);
        if (ok) for (int i = 0; i < 1; i++) if (g_emit[i] != exp[i]) ok = false;
        if (!ok) { g_fail++; printf("FAIL %s: 'w' after clear emitted %d codes (want 1x Ctrl+Right):", name, g_emit_n); for (int i = 0; i < g_emit_n; i++) printf(" 0x%04X", g_emit[i]); printf("\n"); }
        else g_pass++;
    }
    CHECK(pipeline(KC_W, false) == false);

    /* --- `d` non-vim `d` must NOT be `dd` (line delete) --- */
    reset_engine();
    CHECK(pipeline(KC_D, true) == false);
    CHECK(pipeline(nv, true) == true);
    CHECK(pipeline(KC_D, false) == false);
    CHECK(pipeline(nv, false) == true);
    g_emit_n = 0;
    CHECK(pipeline(KC_D, true) == false); /* fresh operator-pending 'd' */
    CHECK(kv_pending() == true);
    CHECK(kv_get_mode() == KV_MODE_NORMAL);
    emit_flush();
    if (g_emit_n != 0) {
        g_fail++;
        printf("FAIL %s: 'd'+non-vim+'d' executed something (want no emit):", name);
        for (int i = 0; i < g_emit_n; i++) printf(" 0x%04X", g_emit[i]);
        printf("\n");
    } else g_pass++;
    CHECK(pipeline(KC_D, false) == false);
}

/* ======================================================================
 * B. CAG: `d` -> Ctrl+X clears pending; the combo passes through to QMK.
 * ====================================================================== */
static void test_cag_strict_clear(void) {
    reset_engine();
    CHECK(pipeline(KC_D, true) == false);
    CHECK(kv_pending() == true);

    /* A bare modifier never clears pending (design §4.12 #1: modifiers only
     * update the shadow).  `d` stays pending until a real CAG key arrives. */
    CHECK(pipeline(KC_LCTL, true) == true);
    CHECK(kv_pending() == true);
    CHECK(kv_get_mode() == KV_MODE_NORMAL);

    /* Ctrl+X (non-modifier with CAG held): early-return clears pending, passes. */
    g_emit_n = 0;
    CHECK(pipeline(KC_X, true) == true);
    CHECK(kv_pending() == false);
    CHECK(kv_get_mode() == KV_MODE_NORMAL);
    emit_flush();
    CHECK(g_emit_n == 0); /* engine must not act on the combo */

    CHECK(pipeline(KC_X, false) == true);
    CHECK(pipeline(KC_LCTL, false) == true);
}

/* ======================================================================
 * C. Esc is matched by physical keycode: real MO(9)=0x5229 (low byte 0x29)
 *    is a non-vim layer key -> pass through, never fed as Esc.
 * ====================================================================== */
static void test_mo9_not_esc(void) {
    /* C1: NORMAL idle -> pass through, no mode change. */
    reset_engine();
    CHECK(pipeline(REAL_MO9, true) == true);
    CHECK(kv_get_mode() == KV_MODE_NORMAL);
    CHECK(kv_pending() == false);
    CHECK(pipeline(REAL_MO9, false) == true);

    /* C2: NORMAL with pending `d` -> pass through (NOT swallowed as Esc);
     *     pending cleared by the non-vim early-return, not by an Esc cancel. */
    reset_engine();
    CHECK(pipeline(KC_D, true) == false);
    CHECK(kv_pending() == true);
    g_emit_n = 0;
    CHECK(pipeline(REAL_MO9, true) == true);   /* critical: not consumed */
    CHECK(kv_pending() == false);
    CHECK(kv_get_mode() == KV_MODE_NORMAL);
    emit_flush();
    CHECK(g_emit_n == 0);
    CHECK(pipeline(REAL_MO9, false) == true);
    CHECK(pipeline(KC_D, false) == false);

    /* C3: VISUAL -> MO(9) passes through and does NOT exit (an Esc would). */
    reset_engine();
    CHECK(pipeline(KC_V, true) == false);
    CHECK(kv_get_mode() == KV_MODE_VISUAL);
    CHECK(pipeline(KC_V, false) == false);
    CHECK(pipeline(REAL_MO9, true) == true);   /* critical: not treated as Esc */
    CHECK(kv_get_mode() == KV_MODE_VISUAL);    /* still selecting */
    CHECK(pipeline(REAL_MO9, false) == true);
    /* control: a genuine Esc in VISUAL is consumed and exits to NORMAL. */
    CHECK(pipeline(KC_ESC, true) == false);
    CHECK(kv_get_mode() == KV_MODE_NORMAL);
    CHECK(pipeline(KC_ESC, false) == false);
}

/* ======================================================================
 * D. Half repeat: `2d` -> non-vim -> `w` -> `.` replays only `w`.
 * ====================================================================== */
static void test_half_repeat_isolation(void) {
    reset_engine();
    CHECK(pipeline(KC_2, true) == false); /* count */
    CHECK(pipeline(KC_D, true) == false); /* operator */
    CHECK(kv_pending() == true);
    CHECK(pipeline(KC_F5, true) == true); /* non-vim cancels 2d */
    CHECK(kv_pending() == false);
    CHECK(pipeline(KC_2, false) == false);
    CHECK(pipeline(KC_D, false) == false);
    CHECK(pipeline(KC_F5, false) == true);

    /* `w` = fresh motion, recorded as last command. */
    g_emit_n = 0;
    CHECK(pipeline(KC_W, true) == false);
    emit_flush();
    CHECK(g_emit_n == 1 && g_emit[0] == KV_LCTL_KC(KV_RGHT));
    CHECK(pipeline(KC_W, false) == false);

    /* `.` replays only `w`, never `2d`/`dw`. */
    g_emit_n = 0;
    CHECK(pipeline(KC_DOT, true) == false);
    emit_flush();
    if (!(g_emit_n == 1 && g_emit[0] == KV_LCTL_KC(KV_RGHT))) {
        g_fail++;
        printf("FAIL repeat: '.' after 2d/non-vim/w emitted %d codes (want 1x Ctrl+Right):", g_emit_n);
        for (int i = 0; i < g_emit_n; i++) printf(" 0x%04X", g_emit[i]);
        printf("\n");
    } else g_pass++;
    CHECK(pipeline(KC_DOT, false) == false);
}

/* ======================================================================
 * E. A modifier pressed *after* a prefix must not clear it: `d` Shift `$`
 *    must still delete to end of line (regression fixed by the modifier
 *    early-return in vim_glue_engine).
 * ====================================================================== */
static void test_modifier_prefix(void) {
    reset_engine();
    CHECK(pipeline(KC_D, true) == false);   /* operator pending */
    CHECK(kv_pending() == true);
    CHECK(pipeline(KC_LSFT, true) == true); /* modifier: pass, keep pending */
    CHECK(kv_pending() == true);
    g_emit_n = 0;
    CHECK(pipeline(KC_4, true) == false);   /* Shift+4 = '$' (folded) */
    emit_flush();
    if (!(g_emit_n == 2 && g_emit[0] == KV_LSFT_KC(KV_END) && g_emit[1] == KV_LCTL_KC(KV_X))) {
        g_fail++;
        printf("FAIL modifier-prefix: d$ emitted %d codes (want Shift+End,Ctrl+X)\n", g_emit_n);
    } else g_pass++;
    CHECK(pipeline(KC_4, false) == false);
    CHECK(pipeline(KC_LSFT, false) == true);
    CHECK(pipeline(KC_D, false) == false);
}

int main(void) {
    test_normal_strict_clear(KC_F5, "F5");
    test_normal_strict_clear(KC_F1, "F1");
    test_normal_strict_clear(KC_COMM, ",");
    test_normal_strict_clear(MO(4), "MO(4)");
    test_cag_strict_clear();
    test_mo9_not_esc();
    test_half_repeat_isolation();
    test_modifier_prefix();

    printf("strict-clear-falsify: pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
