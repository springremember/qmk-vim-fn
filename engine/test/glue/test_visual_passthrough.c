/* test_visual_passthrough.c — standalone falsification of the
 * "non-vim keys pass through in Visual / Visual-Line" fix (commit 741f4d9,
 * design.md §4.10 "非 vim 键码一律透传").
 *
 * Test-only; kept in its own translation unit so the frozen regression counts
 * (test_glue.c -> 393, test_glue_falsify.c -> 242) are untouched.
 *
 * Contract under test, per mode (NORMAL / VISUAL / VISUAL_LINE):
 *   - KC_F5 / KC_F1 / KC_LSFT / MO(4) (non-vim): pipeline passes BOTH edges;
 *   - bare x / w / a (vim keycodes): pipeline consumes BOTH edges;
 *   - VISUAL/VISUAL_LINE: Esc press is consumed, returns to NORMAL, and the
 *     paired release is swallowed (no host Esc).
 *
 * It also records the positive control that the *engine* kv_kbd() would still
 * swallow a non-vim key in VISUAL, so the pass-through is demonstrably provided
 * by the vim_glue_engine() guard rather than by the engine itself.
 */
#include "qmk_stub.h"
#include "emit.h" /* test helper: kv_emit_flush_now() */
#include "qmk-vim-fn/engine/include/kv.h"
#include "qmk-vim-fn/qmk/vim_glue.h"
#include "qmk-vim-fn/qmk/vim_keymap_common.h"

/* ---------------- host state (mirrors test_glue.c) ---------------- */
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

/* Cumulative register_code() calls per keycode (< 256).  A "tap" is
 * register+unregister, which leaves reg_count() unchanged; this counter lets a
 * test tell a real tap apart from a silently dropped emit (mirrors test_glue.c). */
#define HIT_CAP 256
static int s_hits[HIT_CAP];

void register_code(uint16_t kc) {
    if (IS_MODIFIER_KEYCODE(kc)) s_mods |= (uint8_t)(1u << (kc - KC_LCTL));
    if (kc < HIT_CAP) s_hits[kc]++;
    if (s_reg_n < REG_CAP) s_reg[s_reg_n++] = kc;
}
void unregister_code(uint16_t kc) {
    if (IS_MODIFIER_KEYCODE(kc)) s_mods &= (uint8_t)~(1u << (kc - KC_LCTL));
    for (int i = 0; i < s_reg_n; i++) {
        if (s_reg[i] == kc) { s_reg[i] = s_reg[--s_reg_n]; return; }
    }
}
void tap_code(uint16_t kc) { register_code(kc); unregister_code(kc); }
void tap_code16(uint16_t kc) { tap_code((uint16_t)(kc & 0xFF)); }

static int reg_count(uint16_t kc) {
    int n = 0;
    for (int i = 0; i < s_reg_n; i++) if (s_reg[i] == kc) n++;
    return n;
}

static uint32_t g_now;
uint16_t timer_read(void) { return (uint16_t)g_now; }
uint16_t timer_elapsed(uint16_t since) { return (uint16_t)((uint16_t)g_now - since); }
uint32_t timer_read32(void) { return g_now; }
uint32_t timer_elapsed32(uint32_t since) { return g_now - since; }

/* ---------------- test bookkeeping ---------------- */
static int g_pass, g_fail;
#define CHECK(cond)                                                        \
    do {                                                                   \
        if (cond) { g_pass++; }                                            \
        else { g_fail++; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } \
    } while (0)
#define NOTE(...) do { printf("NOTE " __VA_ARGS__); } while (0)

/* ---------------- generic test cfg ---------------- */
static bool test_declared(uint16_t kc) {
    if (kc >= KC_F1 && kc <= KC_F12) return true;
    if (kc == KC_VOLD || kc == KC_VOLU) return true;
    if (kc == KC_SPC || kc == KC_CAPS || kc == KC_ESC) return true;
    if (kc == KC_T) return true;
    return false;
}
static bool test_myfn(uint16_t kc, bool pressed) {
    (void)kc; (void)pressed;
    return false; /* pass */
}

static const vim_cfg_t g_cfg = {
    .fn_layer         = 4,
    .trigger_kc       = TEST_TRIGGER_KC,
    .mod_win          = KC_RALT,
    .mod_mac          = KC_RGUI,
    .is_mac           = NULL,
    .link_ok          = NULL,
    .hold_ms          = 200,
    .shift_esc_enable = true,
    .led_index        = 0,
    .hook_pre         = NULL,
    .hook_post_myfn   = NULL,
    .myfn_declared    = test_declared,
    .myfn             = test_myfn,
    .vim_set_enabled  = NULL,
    .shortcuts        = vim_default_shortcuts,
};

static bool pipeline(uint16_t kc, bool pressed) {
    keyrecord_t r = {0};
    r.event.pressed = pressed;
    return vim_pipeline_process(kc, &r, &g_cfg);
}

static void reset_engine(void) {
    g_now = 1000;
    s_mods = 0;
    s_reg_n = 0;
    for (int i = 0; i < HIT_CAP; i++) s_hits[i] = 0;
    layer_state = 0;
    default_layer_state = 0;
    vim_keymap_common_init(); /* shared statics + kv_init/enable/INSERT */
}

static const char *mode_name(kv_mode_t m) {
    switch (m) {
        case KV_MODE_INSERT:      return "INSERT";
        case KV_MODE_NORMAL:      return "NORMAL";
        case KV_MODE_VISUAL:      return "VISUAL";
        case KV_MODE_VISUAL_LINE: return "VISUAL_LINE";
        default:                  return "?";
    }
}

/* ======================================================================
 * Non-vim keys (F-keys / modifier / layer key) pass BOTH edges in every
 * non-Insert mode.  Insert is the baseline: everything passes there too.
 * ====================================================================== */
static void test_non_vim_passthrough(kv_mode_t mode) {
    const uint16_t keys[] = {KC_F5, KC_F1, KC_LSFT, MO(4)};
    const char   *names[] = {"KC_F5", "KC_F1", "KC_LSFT", "MO(4)"};

    for (unsigned i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        reset_engine();
        kv_set_mode(mode);
        bool press = pipeline(keys[i], true);
        if (press != true) {
            g_fail++;
            printf("FAIL %s:%d  %s in %s: press swallowed (expected pass-through)\n",
                   __FILE__, __LINE__, names[i], mode_name(mode));
        } else {
            g_pass++;
        }
        bool release = pipeline(keys[i], false);
        if (release != true) {
            g_fail++;
            printf("FAIL %s:%d  %s in %s: release swallowed (expected pass-through)\n",
                   __FILE__, __LINE__, names[i], mode_name(mode));
        } else {
            g_pass++;
        }
    }
}

/* ======================================================================
 * Bare vim keys stay consumed on both edges in every non-Insert mode.
 * ====================================================================== */
static void test_vim_keys_consumed(kv_mode_t mode) {
    /* x: NORMAL = delete char, VISUAL = cut selection; both stay in their mode. */
    reset_engine();
    kv_set_mode(mode);
    CHECK(pipeline(KC_X, true) == false);
    CHECK(kv_get_mode() == mode);
    CHECK(pipeline(KC_X, false) == false);

    /* w: motion in both NORMAL and VISUAL; stays in mode. */
    reset_engine();
    kv_set_mode(mode);
    CHECK(pipeline(KC_W, true) == false);
    CHECK(kv_get_mode() == mode);
    CHECK(pipeline(KC_W, false) == false);

    /* a: T_INSERT.  NORMAL -> enters INSERT; VISUAL -> illegal key, stays. */
    reset_engine();
    kv_set_mode(mode);
    CHECK(pipeline(KC_A, true) == false);
    if (mode == KV_MODE_NORMAL) {
        CHECK(kv_get_mode() == KV_MODE_INSERT);
    } else {
        CHECK(kv_get_mode() == mode);
    }
    CHECK(pipeline(KC_A, false) == false);
}

/* ======================================================================
 * VISUAL / VISUAL_LINE: Esc exits to NORMAL; press consumed, paired release
 * swallowed, mode change released any held motion (checked elsewhere).
 * ====================================================================== */
static void test_visual_esc_exit(kv_mode_t mode) {
    reset_engine();
    kv_set_mode(mode);
    CHECK(pipeline(KC_ESC, true) == false);   /* consumed */
    CHECK(kv_get_mode() == KV_MODE_NORMAL);
    CHECK(pipeline(KC_ESC, false) == false);  /* paired: no host Esc */
}

/* ======================================================================
 * Positive control: the engine alone (kv_kbd) still returns KV_CONSUMED for a
 * non-vim key in VISUAL — so the pass-through comes from the glue guard.
 * ====================================================================== */
static void test_engine_alone_would_swallow(void) {
    reset_engine();
    kv_set_mode(KV_MODE_VISUAL);
    kv_result_t raw = kv_kbd((kv_keycode_t)KC_F5); /* 0x3E, T_OTHER */
    NOTE("engine kv_kbd(F5) in VISUAL = %s (KV_CONSUMED=%d KV_PASSTHROUGH=%d)\n",
         raw == KV_CONSUMED ? "CONSUMED" : "PASSTHROUGH", KV_CONSUMED, KV_PASSTHROUGH);
    CHECK(raw == KV_CONSUMED); /* baseline: engine swallows illegal Visual key */

    /* ...but the glue (the fix under test) passes it through. */
    CHECK(pipeline(KC_F5, true) == true);
    CHECK(pipeline(KC_F5, false) == true);
}

/* ======================================================================
 * G4: a bare h/j/k/l in VISUAL must TAP its (Shift+)host arrow (extending the
 * selection) — it must NEVER register-hold the host arrow.  vim_glue.c:262
 * only announces a held motion when the mode is NORMAL
 * (`mi >= 0 && !was_pending && kv_get_mode() == KV_MODE_NORMAL`); in VISUAL
 * that guard's mode test is false, so the "expected hold" slot stays clear and
 * vim_emit() falls through to the tap path.  (design §4.10 held-motion; the
 * Visual Shift+arrow extension is why a tap — not a hold — is correct here.)
 * ====================================================================== */
static void test_visual_motion_taps(void) {
    reset_engine();
    kv_set_mode(KV_MODE_VISUAL);
    CHECK(pipeline(KC_H, true) == false);   /* consumed by the engine */
    kv_emit_flush_now();                    /* drain emit -> host arrow */
    CHECK(s_hits[KC_LEFT] == 1);            /* tapped exactly once */
    CHECK(reg_count(KC_LEFT) == 0);         /* NOT register-held */
    CHECK(kv_get_mode() == KV_MODE_VISUAL); /* stays in Visual */
    CHECK(pipeline(KC_H, false) == false);  /* paired release consumed */
}

int main(void) {
    const kv_mode_t modes[] = {KV_MODE_NORMAL, KV_MODE_VISUAL, KV_MODE_VISUAL_LINE};
    for (unsigned i = 0; i < sizeof(modes) / sizeof(modes[0]); i++) {
        test_non_vim_passthrough(modes[i]);
        test_vim_keys_consumed(modes[i]);
    }

    /* NORMAL idle Esc: real Esc and back to INSERT (shared Esc toggle). */
    reset_engine();
    kv_set_mode(KV_MODE_NORMAL);
    CHECK(pipeline(KC_ESC, true) == true);
    CHECK(kv_get_mode() == KV_MODE_INSERT);
    CHECK(pipeline(KC_ESC, false) == true);

    test_visual_esc_exit(KV_MODE_VISUAL);
    test_visual_esc_exit(KV_MODE_VISUAL_LINE);

    test_engine_alone_would_swallow();
    test_visual_motion_taps();

    printf("visual-passthrough: pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
