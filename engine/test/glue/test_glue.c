/* test_glue.c — host tests for the shared QMK keymap layer (vim_glue.c +
 * vim_keymap_common.c) using qmk_stub.h.
 *
 * These tests exercise contracts that the pure engine tests cannot reach:
 * pipeline polarity, press/release pairing, modifier shadow timing, mouse
 * mode, Caps tap/hold and the myfn skeleton.  Test-only; no product code. */
#include "qmk_stub.h"
#include "emit.h" /* test helper: kv_emit_flush_now() */
#include "qmk-vim-fn/engine/include/kv.h"
#include "qmk-vim-fn/qmk/vim_glue.h"
#include "qmk-vim-fn/qmk/vim_keymap_common.h"

/* ---------------- host state ---------------- */
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
    for (int i = 0; i < s_reg_n; i++) {
        if (s_reg[i] == kc) { s_reg[i] = s_reg[--s_reg_n]; break; }
    }
}
void tap_code(uint16_t kc) { register_code(kc); unregister_code(kc); }
void tap_code16(uint16_t kc) { tap_code((uint16_t)(kc & 0xFF)); }

static uint16_t g_now;
uint16_t timer_read(void) { return g_now; }
uint16_t timer_elapsed(uint16_t since) { return (uint16_t)(g_now - since); }

static int reg_count(uint16_t kc) {
    int n = 0;
    for (int i = 0; i < s_reg_n; i++) if (s_reg[i] == kc) n++;
    return n;
}

/* ---------------- test bookkeeping ---------------- */
static int g_pass, g_fail;
#define CHECK(cond)                                                        \
    do {                                                                   \
        if (cond) { g_pass++; }                                            \
        else { g_fail++; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } \
    } while (0)
#define NOTE(...) do { printf("NOTE " __VA_ARGS__); } while (0)

/* ---------------- keyboard cfg (mirrors QK61) ---------------- */
static int  s_myfn_calls;
static bool s_fn_active;

static bool test_declared(uint16_t kc) {
    if (kc >= KC_F1 && kc <= KC_F12) return true;
    if (kc == KC_VOLD || kc == KC_VOLU) return true;
    if (kc == KC_SPC || kc == KC_CAPS || kc == KC_ESC) return true;
    if (kc == KC_T) return true;
    return false;
}
static bool test_myfn(uint16_t kc, bool pressed) {
    if (kc == KC_SPC) {          /* consume (e.g. Fn+Space battery) */
        if (pressed) s_myfn_calls++;
        return true;
    }
    return false;                /* pass (e.g. F-keys) */
}

static const vim_cfg_t g_cfg = {
    .fn_layer         = 4,
    .trigger_kc       = QK_KB_22,
    .mod_win          = KC_RALT,
    .mod_mac          = KC_RGUI,
    .is_mac           = NULL,
    .link_ok          = NULL,
    .hold_ms          = 200,
    .shift_esc_enable = true,
    .led_index        = 0,
    .hook_pre         = NULL,
    .hook_post_myfn   = NULL, /* QK61 CAD/reset are keyboard-specific */
    .myfn_declared    = test_declared,
    .myfn             = test_myfn,
    .vim_set_enabled  = NULL,
    .shortcuts        = vim_default_shortcuts,
};

static bool pipeline_cfg(uint16_t kc, bool pressed, const vim_cfg_t *cfg) {
    keyrecord_t r = {0};
    r.event.pressed = pressed;
    return vim_pipeline_process(kc, &r, cfg);
}

static bool pipeline(uint16_t kc, bool pressed) {
    keyrecord_t r = {0};
    r.event.pressed = pressed;
    return vim_pipeline_process(kc, &r, &g_cfg);
}

static void reset_engine(void) {
    g_now = 1000;
    s_mods = 0;
    s_reg_n = 0;
    s_myfn_calls = 0;
    s_fn_active = false;
    layer_state = 0;
    default_layer_state = 0;
    /* ensure a previous test cannot leave mouse mode latched */
    s_fn_active = false;
    vim_glue_init(); /* kv_init + enable + INSERT */
}

static void fn_on(void) { s_fn_active = true; layer_state = (1UL << 4); }
static void fn_off(void) { s_fn_active = false; layer_state = 0; }

/* ================= tests ================= */

static void test_polarity_pairing(void) {
    /* non-vim key: pass on both edges */
    reset_engine();
    CHECK(pipeline(KC_Z, true) == true);
    CHECK(pipeline(KC_Z, false) == true);

    /* vim key in NORMAL: consumed on press; release of a consumed press is
     * swallowed and the held-motion arrow is unregistered */
    reset_engine();
    kv_set_mode(KV_MODE_NORMAL);
    CHECK(pipeline(KC_H, true) == false);
    kv_emit_flush_now(); /* drain emit -> held motion registers KC_LEFT */
    CHECK(reg_count(KC_LEFT) == 1);
    CHECK(pipeline(KC_H, false) == false); /* swallowed, arrow released */
    CHECK(reg_count(KC_LEFT) == 0);

    /* Normal idle Esc: pass-through both edges */
    reset_engine();
    kv_set_mode(KV_MODE_NORMAL);
    CHECK(pipeline(KC_ESC, true) == true);
    CHECK(pipeline(KC_ESC, false) == true);

    /* pending d + Esc: consumed press; both releases paired */
    reset_engine();
    kv_set_mode(KV_MODE_NORMAL);
    CHECK(pipeline(KC_D, true) == false);
    CHECK(pipeline(KC_ESC, true) == false);
    CHECK(pipeline(KC_ESC, false) == false);
    CHECK(pipeline(KC_D, false) == false);

    /* Insert Esc: real Esc both edges (engine never switches mode) */
    reset_engine();
    CHECK(pipeline(KC_ESC, true) == true);
    CHECK(kv_get_mode() == KV_MODE_INSERT);
    CHECK(pipeline(KC_ESC, false) == true);

    /* keymap-layer shortcut (Space => Right) consumes press AND release */
    reset_engine();
    kv_set_mode(KV_MODE_NORMAL);
    CHECK(pipeline(KC_SPC, true) == false);
    CHECK(pipeline(KC_SPC, false) == false);

    /* Shift+Esc (Insert): press changed to ~ and consumed; release paired */
    reset_engine();
    CHECK(pipeline(KC_LSFT, true) == true);   /* shadow records LSFT */
    CHECK(pipeline(KC_ESC, true) == false);   /* -> ~ , swallow */
    CHECK(pipeline(KC_ESC, false) == false);  /* paired release */
    CHECK(pipeline(KC_LSFT, false) == true);
}

static void test_shadow_before_swallow(void) {
    /* myfn swallows the modifier, yet the shadow must already have it */
    reset_engine();
    fn_on();
    CHECK(pipeline(KC_LSFT, true) == false);
    CHECK((vim_glue_mods() & MOD_BIT_LSHIFT) != 0);
    CHECK(pipeline(KC_LSFT, false) == false);
    CHECK((vim_glue_mods() & MOD_BIT_LSHIFT) == 0);
    fn_off();
}

static void test_myfn_skeleton(void) {
    reset_engine();
    fn_on();
    CHECK(pipeline(MO(4), true) == true);   /* layer key exempt */
    CHECK(pipeline(MO(4), false) == true);
    CHECK(pipeline(KC_Z, true) == false);   /* undeclared -> swallow */
    CHECK(pipeline(KC_Z, false) == false);
    CHECK(pipeline(KC_F1, true) == true);   /* declared passthrough */
    CHECK(pipeline(KC_F1, false) == true);
    CHECK(pipeline(KC_VOLU, true) == true);
    CHECK(pipeline(KC_VOLU, false) == true);
    CHECK(pipeline(KC_SPC, true) == false);  /* declared + callback consumes (battery) */
    CHECK(s_myfn_calls == 1);
    CHECK(pipeline(KC_SPC, false) == false); /* paired release consumed */
    fn_off();
}

static void test_caps(void) {
    /* short press Insert -> Normal */
    reset_engine();
    CHECK(pipeline(KC_CAPS, true) == false);
    CHECK(kv_get_mode() == KV_MODE_NORMAL);
    CHECK(pipeline(KC_CAPS, false) == false);
    CHECK(kv_get_mode() == KV_MODE_NORMAL);

    /* short press Normal -> Insert */
    CHECK(pipeline(KC_CAPS, true) == false);
    CHECK(pipeline(KC_CAPS, false) == false);
    CHECK(kv_get_mode() == KV_MODE_INSERT);

    /* long press Insert -> momentary, returns to Insert */
    reset_engine();
    g_now = 1000;
    CHECK(pipeline(KC_CAPS, true) == false);
    g_now += 250;
    CHECK(pipeline(KC_CAPS, false) == false);
    CHECK(kv_get_mode() == KV_MODE_INSERT);

    /* vim off: Caps passes through */
    reset_engine();
    kv_disable();
    CHECK(pipeline(KC_CAPS, true) == true);
    CHECK(pipeline(KC_CAPS, false) == true);

    /* Fn+Caps toggles vim (press and release both consumed) */
    reset_engine();
    fn_on();
    CHECK(kv_vim_enabled() == true);
    CHECK(pipeline(KC_CAPS, true) == false);
    CHECK(kv_vim_enabled() == false);
    CHECK(pipeline(KC_CAPS, false) == false);
    CHECK(pipeline(KC_CAPS, true) == false);
    CHECK(kv_vim_enabled() == true);
    CHECK(pipeline(KC_CAPS, false) == false);
    fn_off();
}

/* design.md §4.12 / readme.md §1: Caps long press = momentary Normal, on
 * release return to the ORIGINAL mode.  Starting from VISUAL must return to
 * VISUAL. */
static void test_caps_long_from_visual(void) {
    reset_engine();
    kv_set_mode(KV_MODE_VISUAL);
    g_now = 2000;
    CHECK(pipeline(KC_CAPS, true) == false);
    g_now += 250;
    CHECK(pipeline(KC_CAPS, false) == false);
    if (kv_get_mode() != KV_MODE_VISUAL) {
        g_fail++;
        printf("FAIL %s:%d  Caps long from VISUAL: expected VISUAL, got mode=%d\n",
               __FILE__, __LINE__, (int)kv_get_mode());
    } else {
        g_pass++;
    }
}

static void test_mouse(void) {
    /* trigger tap enters MOUSE */
    reset_engine();
    g_now = 1000;
    CHECK(pipeline(QK_KB_22, true) == false);
    CHECK(pipeline(QK_KB_22, false) == false);
    CHECK(kv_get_mode() == KV_MODE_MOUSE);

    /* Shift does NOT exit MOUSE (needed for Shift+J/K wheel) */
    CHECK(pipeline(KC_LSFT, true) == false);
    CHECK(kv_get_mode() == KV_MODE_MOUSE);

    /* Shift+J -> wheel down; releasing Shift mid-hold must still unregister
     * the actually registered code (not plain down) */
    CHECK(pipeline(KC_J, true) == false);
    CHECK(reg_count(MS_WHLD) == 1);
    CHECK(reg_count(MS_DOWN) == 0);
    CHECK(pipeline(KC_LSFT, false) == false);
    CHECK(kv_get_mode() == KV_MODE_MOUSE);
    CHECK(pipeline(KC_J, false) == false);
    CHECK(reg_count(MS_WHLD) == 0);

    /* plain j -> pointer down; other key exits + force-release + re-identify */
    CHECK(pipeline(KC_J, true) == false);
    CHECK(reg_count(MS_DOWN) == 1);
    CHECK(pipeline(KC_A, true) == true);      /* exits, 'a' re-identified in Insert */
    CHECK(reg_count(MS_DOWN) == 0);           /* forced release */
    CHECK(kv_get_mode() == KV_MODE_INSERT);
    CHECK(pipeline(KC_A, false) == true);

    /* Ctrl/Alt/GUI exit MOUSE on press and are re-identified in the entry
     * mode (pipeline continues -> true); their release then passes through. */
    CHECK(pipeline(QK_KB_22, true) == false);
    CHECK(pipeline(QK_KB_22, false) == false);
    CHECK(kv_get_mode() == KV_MODE_MOUSE);
    CHECK(pipeline(KC_LCTL, true) == true);   /* exits + re-identify */
    CHECK(kv_get_mode() == KV_MODE_INSERT);
    CHECK(pipeline(KC_LCTL, false) == true);

    /* Esc inside MOUSE exits and re-identifies (Insert -> real Esc) */
    CHECK(pipeline(KC_ESC, true) == true);
    CHECK(kv_get_mode() == KV_MODE_INSERT);
    CHECK(pipeline(KC_ESC, false) == true);

    /* trigger long press registers the Win/Mac modifier, does not toggle */
    reset_engine();
    g_now = 5000;
    CHECK(pipeline(QK_KB_22, true) == false);
    g_now += 250;
    vim_keymap_common_task(g_now);
    CHECK(reg_count(KC_RALT) == 1);
    CHECK(pipeline(QK_KB_22, false) == false);
    CHECK(reg_count(KC_RALT) == 0);
    CHECK(kv_get_mode() == KV_MODE_INSERT);
}

/* Every Ctrl/Alt/GUI (both sides) exits MOUSE on press and is re-identified;
 * Shift (both sides) never exits.  Shift + a non-J/K key still exits via the
 * generic "other key" branch. */
static void test_mouse_modifier_exit(void) {
    const uint16_t exiting[] = {KC_LCTL, KC_RCTL, KC_LALT, KC_RALT, KC_LGUI, KC_RGUI};
    for (unsigned i = 0; i < sizeof(exiting) / sizeof(exiting[0]); i++) {
        reset_engine();
        kv_set_mode(KV_MODE_NORMAL); /* entry mode to be restored */
        CHECK(pipeline(QK_KB_22, true) == false);
        CHECK(pipeline(QK_KB_22, false) == false);
        CHECK(kv_get_mode() == KV_MODE_MOUSE);
        CHECK(pipeline(exiting[i], true) == true);   /* exit + re-identify */
        CHECK(kv_get_mode() == KV_MODE_NORMAL);
        CHECK(pipeline(exiting[i], false) == true);  /* release passes */
    }

    const uint16_t staying[] = {KC_LSFT, KC_RSFT};
    for (unsigned i = 0; i < sizeof(staying) / sizeof(staying[0]); i++) {
        reset_engine();
        CHECK(pipeline(QK_KB_22, true) == false);
        CHECK(pipeline(QK_KB_22, false) == false);
        CHECK(kv_get_mode() == KV_MODE_MOUSE);
        CHECK(pipeline(staying[i], true) == false);  /* stays in MOUSE */
        CHECK(kv_get_mode() == KV_MODE_MOUSE);
        CHECK(pipeline(staying[i], false) == false);
        CHECK(kv_get_mode() == KV_MODE_MOUSE);
    }

    /* Shift held + a non-J/K key: the key is "other" -> exits + re-identifies
     * (entry mode INSERT, so 'a' passes through); the Shift press was paired
     * inside MOUSE, so its release is consumed by the pairing table. */
    reset_engine(); /* entry mode INSERT */
    CHECK(pipeline(QK_KB_22, true) == false);
    CHECK(pipeline(QK_KB_22, false) == false);
    CHECK(pipeline(KC_LSFT, true) == false);         /* Shift stays */
    CHECK(kv_get_mode() == KV_MODE_MOUSE);
    CHECK(pipeline(KC_A, true) == true);             /* other key exits */
    CHECK(kv_get_mode() == KV_MODE_INSERT);
    CHECK(pipeline(KC_A, false) == true);
    CHECK(pipeline(KC_LSFT, false) == false);        /* paired release consumed */
}

static void test_mode_change_releases_motion(void) {
    reset_engine();
    kv_set_mode(KV_MODE_NORMAL);
    CHECK(pipeline(KC_H, true) == false);
    kv_emit_flush_now();
    CHECK(reg_count(KC_LEFT) == 1);
    /* Caps switch (mode transition) must release the held arrow */
    CHECK(pipeline(KC_CAPS, true) == false);
    CHECK(reg_count(KC_LEFT) == 0);
    CHECK(pipeline(KC_CAPS, false) == false);
}

/* Suspected defect: shift_esc_process() is not gated on kv_vim_enabled(),
 * unlike shortcuts_process().  With vim disabled (mode left at INSERT) the
 * Shift+Esc combo is still hijacked to ~ / `.  Expected: pass through. */
static void test_shift_esc_vim_off(void) {
    reset_engine();
    kv_disable();
    CHECK(pipeline(KC_LSFT, true) == true);
    if (pipeline(KC_ESC, true) != true) {
        g_fail++;
        printf("FAIL %s:%d  Shift+Esc with vim OFF: press consumed (expected pass)\n",
               __FILE__, __LINE__);
    } else {
        g_pass++;
    }
    CHECK(pipeline(KC_ESC, false) == true);
    CHECK(pipeline(KC_LSFT, false) == true);
}

/* Faithful transcription of the QK61 CAD block (keymap.c:190-193) used as
 * cfg->hook_post_myfn, to reproduce the press/release pairing defect through
 * the real vim_pipeline_process(). */
static bool cad_hook(uint16_t keycode, keyrecord_t *record) {
    if (keycode == KC_BSPC && (get_mods() & MOD_BIT(KC_LCTL)) && (get_mods() & MOD_BIT(KC_LALT))) {
        if (record->event.pressed) tap_code(KC_DEL);
        return true;
    }
    return false;
}

static void test_cad_pairing_repro(void) {
    vim_cfg_t cfg = g_cfg;
    cfg.hook_post_myfn = cad_hook;

    /* (1) BSPC pressed plain, then Ctrl+Alt pressed while BSPC is still held,
     * then BSPC released: the release predicate now matches and swallows the
     * release, leaving the host Backspace stuck down. */
    reset_engine();
    CHECK(pipeline_cfg(KC_BSPC, true, &cfg) == true); /* plain: host gets BSPC down */
    register_code(KC_BSPC);                           /* emulate QMK registering it */
    CHECK(pipeline_cfg(KC_LCTL, true, &cfg) == true);
    register_code(KC_LCTL);
    CHECK(pipeline_cfg(KC_LALT, true, &cfg) == true);
    register_code(KC_LALT);
    if (pipeline_cfg(KC_BSPC, false, &cfg) != true) {
        g_fail++;
        printf("FAIL %s:%d  CAD: BSPC release swallowed while Ctrl+Alt held -> "
               "host Backspace stuck down (registered=%d)\n",
               __FILE__, __LINE__, reg_count(KC_BSPC));
    } else {
        g_pass++;
    }
    unregister_code(KC_LCTL);
    unregister_code(KC_LALT);
    unregister_code(KC_BSPC); /* clean up the stuck stub state */

    /* (2) Ctrl+Alt held, BSPC press consumed by CAD; if Ctrl/Alt are released
     * before BSPC, the BSPC release is let through as an orphan key-up. */
    reset_engine();
    register_code(KC_LCTL);
    register_code(KC_LALT);
    CHECK(pipeline_cfg(KC_BSPC, true, &cfg) == false); /* consumed */
    CHECK(reg_count(KC_BSPC) == 0);
    unregister_code(KC_LCTL);
    unregister_code(KC_LALT);
    if (pipeline_cfg(KC_BSPC, false, &cfg) != false) {
        g_fail++;
        printf("FAIL %s:%d  CAD: consumed BSPC press leaked an orphan release\n",
               __FILE__, __LINE__);
    } else {
        g_pass++;
    }
}

/* The held-motion contract (design §4.10): a held h/j/k/l must REGISTER the
 * host arrow (auto-repeat) and keep it registered until the physical key is
 * released.  vim_glue.c's vim_emit() looks up the emitted keycode (KC_LEFT)
 * against s_motion_kc[] = {KC_H,KC_J,KC_K,KC_L}, so it can never match and
 * falls back to a one-shot tap. */
static void test_visual_esc_and_cag(void) {
    /* Visual Esc: consumed, returns to NORMAL, release paired (no real Esc) */
    reset_engine();
    kv_set_mode(KV_MODE_NORMAL);
    CHECK(pipeline(KC_V, true) == false);
    CHECK(kv_get_mode() == KV_MODE_VISUAL);
    CHECK(pipeline(KC_ESC, true) == false);
    CHECK(kv_get_mode() == KV_MODE_NORMAL);
    CHECK(pipeline(KC_ESC, false) == false); /* paired: never a host Esc */

    /* testcase §9: Shift+Esc inside Visual exits (not swallowed dead).
     * Non-vim keys (here the LSFT modifier) pass through even in Visual
     * (design §4.10); the engine still sees Esc and exits Visual. */
    reset_engine();
    kv_set_mode(KV_MODE_NORMAL);
    CHECK(pipeline(KC_V, true) == false);
    CHECK(pipeline(KC_LSFT, true) == true);  /* non-vim: passes to QMK in Visual too */
    CHECK(pipeline(KC_ESC, true) == false);
    CHECK(kv_get_mode() == KV_MODE_NORMAL);
    CHECK(pipeline(KC_ESC, false) == false);
    CHECK(pipeline(KC_LSFT, false) == true); /* passed through */

    /* CAG (Ctrl/Alt/Gui) held: engine not fed, key passes through */
    reset_engine();
    kv_set_mode(KV_MODE_NORMAL);
    CHECK(pipeline(KC_LCTL, true) == true); /* shadow records Ctrl */
    CHECK(pipeline(KV_D, true) == true);    /* d not fed -> pass */
    CHECK(pipeline(KV_D, false) == true);
    CHECK(pipeline(KC_LCTL, false) == true);
}

static void test_held_motion(void) {
    reset_engine();
    kv_set_mode(KV_MODE_NORMAL);
    CHECK(pipeline(KC_H, true) == false);
    kv_emit_flush_now();
    if (reg_count(KC_LEFT) != 1) {
        g_fail++;
        printf("FAIL %s:%d  held h: expected KC_LEFT registered (held), got %d "
               "(emit fell back to tap)\n",
               __FILE__, __LINE__, reg_count(KC_LEFT));
    } else {
        g_pass++;
    }
    CHECK(pipeline(KC_H, false) == false);
    CHECK(reg_count(KC_LEFT) == 0);
}

int main(void) {
    test_polarity_pairing();
    test_shadow_before_swallow();
    test_myfn_skeleton();
    test_caps();
    test_caps_long_from_visual();
    test_mouse();
    test_mouse_modifier_exit();
    test_mode_change_releases_motion();
    test_shift_esc_vim_off();
    test_cad_pairing_repro();
    test_held_motion();
    test_visual_esc_and_cag();
    printf("glue: pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
