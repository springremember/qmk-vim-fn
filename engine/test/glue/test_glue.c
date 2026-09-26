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

/* Physical keys whose press the pipeline let through to the host, plus the
 * orphan-key-up counter (mirrors test_glue_falsify.c, design §4.10 E3): a
 * physical release the pipeline lets through for a key whose physical press it
 * had consumed is an orphan.  Only feed_cfg() (used by the release-default
 * branch test) drives this; internal register/unregister calls are invisible. */
static uint16_t s_phys[REG_CAP];
static int      s_phys_n;
static int      s_orphan;

/* Cumulative number of register_code() calls per keycode (keycodes < 256).
 * A "tap" is register+unregister, which leaves reg_count() unchanged; this
 * counter lets a test tell a real tap apart from a silently dropped emit
 * (template copied from test_glue_falsify.c). */
#define HIT_CAP 256
static int s_hits[HIT_CAP];

/* Full 16-bit keycodes handed to tap_code16() (e.g. LSFT(KC_GRV) for Shift+Esc
 * ~).  The stub's tap_code16() only applies the low byte to the host, so the
 * high (modifier) bits are otherwise unobservable; recording the raw argument
 * preserves the existing host behaviour while allowing a content assertion. */
#define TAP16_CAP 64
static uint16_t s_tap16[TAP16_CAP];
static int      s_tap16_n;

void register_code(uint16_t kc) {
    if (IS_MODIFIER_KEYCODE(kc)) s_mods |= (uint8_t)(1u << (kc - KC_LCTL));
    if (kc < HIT_CAP) s_hits[kc]++;
    if (s_reg_n < REG_CAP) s_reg[s_reg_n++] = kc;
}
void unregister_code(uint16_t kc) {
    if (IS_MODIFIER_KEYCODE(kc)) s_mods &= (uint8_t)~(1u << (kc - KC_LCTL));
    for (int i = 0; i < s_reg_n; i++) {
        if (s_reg[i] == kc) { s_reg[i] = s_reg[--s_reg_n]; break; }
    }
}
void tap_code(uint16_t kc) { register_code(kc); unregister_code(kc); }
void tap_code16(uint16_t kc) {
    if (s_tap16_n < TAP16_CAP) s_tap16[s_tap16_n++] = kc;
    tap_code((uint16_t)(kc & 0xFF));
}

static uint16_t g_now;
uint16_t timer_read(void) { return g_now; }
uint16_t timer_elapsed(uint16_t since) { return (uint16_t)(g_now - since); }

static int reg_count(uint16_t kc) {
    int n = 0;
    for (int i = 0; i < s_reg_n; i++) if (s_reg[i] == kc) n++;
    return n;
}

static void phys_add(uint16_t kc) { if (s_phys_n < REG_CAP) s_phys[s_phys_n++] = kc; }
static bool phys_take(uint16_t kc) {
    for (int i = 0; i < s_phys_n; i++) {
        if (s_phys[i] == kc) { s_phys[i] = s_phys[--s_phys_n]; return true; }
    }
    return false;
}

/* ---------------- test bookkeeping ---------------- */
static int g_pass, g_fail;
#define CHECK(cond)                                                        \
    do {                                                                   \
        if (cond) { g_pass++; }                                            \
        else { g_fail++; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } \
    } while (0)
#define NOTE(...) do { printf("NOTE " __VA_ARGS__); } while (0)

/* ---------------- generic test cfg ---------------- */
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
    if (kc == KC_SPC) {          /* consume (e.g. a declared function key) */
        if (pressed) s_myfn_calls++;
        return true;
    }
    return false;                /* pass (e.g. F-keys) */
}

/* --- test-only callback state (hook_pre / link_ok / vim_set_enabled / order) --- */
static bool s_link_ok;
static bool test_link_ok(void) { return s_link_ok; }

static int  s_set_enabled_calls;
static bool s_set_enabled_last;
static void test_set_enabled(bool enabled) {
    s_set_enabled_calls++;
    s_set_enabled_last = enabled;
}

static int  s_hook_pre_calls;
static bool hook_pre_f1(uint16_t kc, keyrecord_t *r) {
    if (kc != KC_F1) return false;
    s_hook_pre_calls++;
    return r->event.pressed;     /* claim the press; release falls through */
}

/* hook_pre that claims BOTH edges of KC_T, to prove hook_process() deliberately
 * ignores a hook's release claim (design §4.12: releases are owned by the shared
 * press/release pairing table). */
static int  s_hook_both_calls;
static bool hook_pre_both(uint16_t kc, keyrecord_t *r) {
    (void)r;
    if (kc != KC_T) return false;
    s_hook_both_calls++;
    return true;
}

/* hook_pre that consumes the press of the MOUSE movement keys and Space.  Used
 * to reach their release-switch arms while s_move_reg[]/s_lbtn_timer are still
 * zero (the mouse FSM never saw a press).  The consumed press is paired by
 * hook_process() -> vim_glue_swallow(), so the matching release is swallowed by
 * the shared pairing table instead of leaking an orphan key-up. */
static bool hook_pre_mouse_own(uint16_t kc, keyrecord_t *r) {
    (void)r;
    switch (kc) {
        case KC_H:
        case KC_J:
        case KC_K:
        case KC_L:
        case KC_SPC:
            return true; /* claim both edges; hook_process() ignores the release */
        default:
            return false;
    }
}

/* Separate myfn used to prove pipeline ordering (hook_pre runs before myfn). */
static uint16_t s_ord_myfn_seen[16];
static int      s_ord_myfn_n;
static bool ord_declared(uint16_t kc) { return kc == KC_F1 || kc == KC_X; }
static bool ord_myfn(uint16_t kc, bool pressed) {
    if (pressed && s_ord_myfn_n < 16) s_ord_myfn_seen[s_ord_myfn_n++] = kc;
    return false;                /* pass */
}

/* Ordering probe: myfn (step 2) and hook_post_myfn (step 3) each stamp a shared
 * sequence counter so the test can prove step 3 runs after step 2. */
static int s_order_seq;
static int s_myfn_order_at;
static int s_post_order_at;
static int s_post_calls;
static bool order_myfn(uint16_t kc, bool pressed) {
    if (kc == KC_X && pressed) s_myfn_order_at = ++s_order_seq;
    return false;                /* declared key: pass to QMK */
}
static bool order_post(uint16_t kc, keyrecord_t *r) {
    (void)kc;
    if (r->event.pressed) {
        s_post_calls++;
        s_post_order_at = ++s_order_seq;
    }
    return false;                /* never consume */
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
    .hook_post_myfn   = NULL, /* keyboard-specific post-myfn hooks are added per keyboard */
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

/* pipeline + host emulation (mirrors test_glue_falsify.c): a passed press is
 * registered and tracked as physically down; a passed release of an untracked
 * key is an orphan key-up (design §4.10 E3). */
static bool feed_cfg(uint16_t kc, bool pressed, const vim_cfg_t *cfg) {
    bool pass = pipeline_cfg(kc, pressed, cfg);
    if (pressed) {
        if (pass) { register_code(kc); phys_add(kc); }
    } else if (pass) {
        if (!phys_take(kc)) s_orphan++;
        unregister_code(kc);
    }
    return pass;
}

static void reset_engine(void) {
    g_now = 1000;
    s_mods = 0;
    s_reg_n = 0;
    s_phys_n = 0;
    s_orphan = 0;
    s_tap16_n = 0;
    for (int i = 0; i < HIT_CAP; i++) s_hits[i] = 0;
    s_myfn_calls = 0;
    s_fn_active = false;
    s_link_ok = false;
    s_set_enabled_calls = 0;
    s_set_enabled_last = false;
    s_hook_pre_calls = 0;
    s_hook_both_calls = 0;
    s_ord_myfn_n = 0;
    s_order_seq = 0;
    s_myfn_order_at = 0;
    s_post_order_at = 0;
    s_post_calls = 0;
    layer_state = 0;
    default_layer_state = 0;
    /* ensure a previous test cannot leave mouse/Caps/grace state latched */
    vim_keymap_common_init(); /* resets shared statics + kv_init/enable/INSERT */
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

    /* Insert Esc (no grace window): swallowed, drops to NORMAL, no host Esc */
    reset_engine();
    CHECK(pipeline(KC_ESC, true) == false);
    CHECK(kv_get_mode() == KV_MODE_NORMAL);
    CHECK(pipeline(KC_ESC, false) == false);

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
    CHECK(pipeline(KC_SPC, true) == false);  /* declared + callback consumes */
    CHECK(s_myfn_calls == 1);
    CHECK(pipeline(KC_SPC, false) == false); /* paired release consumed */
    fn_off();
}

static void test_caps(void) {
    /* tap (vim on): press previews NORMAL, release toggles vim OFF */
    reset_engine();
    CHECK(pipeline(KC_CAPS, true) == false);
    CHECK(kv_vim_enabled() == true);          /* not toggled until release */
    CHECK(pipeline(KC_CAPS, false) == false);
    CHECK(kv_vim_enabled() == false);         /* tap toggled vim off */

    /* tap (vim off): release toggles vim back ON, restarting in INSERT */
    CHECK(pipeline(KC_CAPS, true) == false);
    CHECK(pipeline(KC_CAPS, false) == false);
    CHECK(kv_vim_enabled() == true);
    CHECK(kv_get_mode() == KV_MODE_INSERT);   /* enable restarts in INSERT */

    /* Normal + pending `d` then Caps: press drops the pending, release toggles */
    reset_engine();
    kv_set_mode(KV_MODE_NORMAL);
    CHECK(pipeline(KC_D, true) == false);      /* operator pending */
    CHECK(kv_pending() == true);
    CHECK(pipeline(KC_CAPS, true) == false);
    CHECK(kv_pending() == false);              /* mode preview cleared pending */
    CHECK(pipeline(KC_CAPS, false) == false);
    CHECK(kv_vim_enabled() == false);          /* release toggled off */

    /* long press Insert -> momentary NORMAL, returns to Insert, vim stays on */
    reset_engine();
    g_now = 1000;
    CHECK(pipeline(KC_CAPS, true) == false);
    g_now += 250;
    CHECK(pipeline(KC_CAPS, false) == false);
    CHECK(kv_get_mode() == KV_MODE_INSERT);
    CHECK(kv_vim_enabled() == true);

    /* vim off: Caps is still owned (it is the vim switch), never Caps Lock */
    reset_engine();
    kv_disable();
    CHECK(pipeline(KC_CAPS, true) == false);
    CHECK(pipeline(KC_CAPS, false) == false);
    CHECK(kv_vim_enabled() == true);          /* tap re-enabled vim */

    /* Fn+Caps behaves exactly like a bare Caps (no special case) */
    reset_engine();
    fn_on();
    CHECK(kv_vim_enabled() == true);
    CHECK(pipeline(KC_CAPS, true) == false);
    CHECK(pipeline(KC_CAPS, false) == false);
    CHECK(kv_vim_enabled() == false);         /* toggled off, same as bare Caps */
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
    CHECK(pipeline(TEST_TRIGGER_KC, true) == false);
    CHECK(pipeline(TEST_TRIGGER_KC, false) == false);
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
    CHECK(pipeline(TEST_TRIGGER_KC, true) == false);
    CHECK(pipeline(TEST_TRIGGER_KC, false) == false);
    CHECK(kv_get_mode() == KV_MODE_MOUSE);
    CHECK(pipeline(KC_LCTL, true) == true);   /* exits + re-identify */
    CHECK(kv_get_mode() == KV_MODE_INSERT);
    CHECK(pipeline(KC_LCTL, false) == true);

    /* Esc inside MOUSE exits (restores entry mode INSERT) and re-identifies;
     * re-fed in INSERT with no grace window, Esc is swallowed and drops to
     * NORMAL (the shared Esc-toggle rule). */
    CHECK(pipeline(KC_ESC, true) == false);
    CHECK(kv_get_mode() == KV_MODE_NORMAL);
    CHECK(pipeline(KC_ESC, false) == false);

    /* trigger long press registers the Win/Mac modifier, does not toggle */
    reset_engine();
    g_now = 5000;
    CHECK(pipeline(TEST_TRIGGER_KC, true) == false);
    g_now += 250;
    vim_keymap_common_task(g_now);
    CHECK(reg_count(KC_RALT) == 1);
    CHECK(pipeline(TEST_TRIGGER_KC, false) == false);
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
        CHECK(pipeline(TEST_TRIGGER_KC, true) == false);
        CHECK(pipeline(TEST_TRIGGER_KC, false) == false);
        CHECK(kv_get_mode() == KV_MODE_MOUSE);
        CHECK(pipeline(exiting[i], true) == true);   /* exit + re-identify */
        CHECK(kv_get_mode() == KV_MODE_NORMAL);
        CHECK(pipeline(exiting[i], false) == true);  /* release passes */
    }

    const uint16_t staying[] = {KC_LSFT, KC_RSFT};
    for (unsigned i = 0; i < sizeof(staying) / sizeof(staying[0]); i++) {
        reset_engine();
        CHECK(pipeline(TEST_TRIGGER_KC, true) == false);
        CHECK(pipeline(TEST_TRIGGER_KC, false) == false);
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
    CHECK(pipeline(TEST_TRIGGER_KC, true) == false);
    CHECK(pipeline(TEST_TRIGGER_KC, false) == false);
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

/* design §4.10 / readme §4: Shift+Esc is gated on kv_vim_enabled() inside
 * shift_esc_process() (the same gate shortcuts_process() uses).  With vim
 * disabled (mode left at INSERT) the combo must pass through unchanged. */
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

/* Sample post-myfn hook: a chord (Ctrl+Alt+BSPC) consumes its press and (on
 * the release edge) its release, used as cfg->hook_post_myfn to reproduce the
 * press/release pairing contract through the real vim_pipeline_process(). */
static bool chord_hook(uint16_t keycode, keyrecord_t *record) {
    if (keycode == KC_BSPC && (get_mods() & MOD_BIT(KC_LCTL)) && (get_mods() & MOD_BIT(KC_LALT))) {
        if (record->event.pressed) tap_code(KC_DEL);
        return true;
    }
    return false;
}

static void test_hook_pairing_repro(void) {
    vim_cfg_t cfg = g_cfg;
    cfg.hook_post_myfn = chord_hook;

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
        printf("FAIL %s:%d  chord hook: BSPC release swallowed while Ctrl+Alt held -> "
               "host Backspace stuck down (registered=%d)\n",
               __FILE__, __LINE__, reg_count(KC_BSPC));
    } else {
        g_pass++;
    }
    unregister_code(KC_LCTL);
    unregister_code(KC_LALT);
    unregister_code(KC_BSPC); /* clean up the stuck stub state */

    /* (2) Ctrl+Alt held, BSPC press consumed by the chord hook; if Ctrl/Alt are released
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
        printf("FAIL %s:%d  chord hook: consumed BSPC press leaked an orphan release\n",
               __FILE__, __LINE__);
    } else {
        g_pass++;
    }
}

/* Visual Esc and CAG handling (design §4.9 / §4.10).  The held-motion contract
 * (a bare h/j/k/l register-holds its host arrow, design §4.10) is exercised by
 * test_held_motion() below: vim_glue.c's vim_emit() now indexes the emitted
 * HOST arrow through arrow_index() (s_arrow_kc[] = {KC_LEFT,KC_DOWN,KC_UP,
 * KC_RIGHT}), so a bare motion's arrow is correctly register-held rather than
 * tapped. */
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

/* Exception: in NORMAL a *bare* h/j/k/l keeps acting as a held direction key
 * even while Ctrl/Alt/GUI is physically held (combined with that modifier),
 * instead of passing the chord through to the host.  A pending count/operator/
 * prefix still takes the old strict-clear passthrough path. */
static void test_held_motion_with_cag(void) {
    /* Win+h (GUI held): KC_LEFT is register-held, host does NOT get Win+h. */
    reset_engine();
    kv_set_mode(KV_MODE_NORMAL);
    CHECK(pipeline(KC_LGUI, true) == true);   /* shadow: GUI down */
    CHECK(pipeline(KC_H, true) == false);     /* consumed by the exception */
    kv_emit_flush_now();
    CHECK(reg_count(KC_LEFT) == 1);
    CHECK(pipeline(KC_H, false) == false);    /* tail unregisters the arrow */
    CHECK(reg_count(KC_LEFT) == 0);
    CHECK(pipeline(KC_LGUI, false) == true);

    /* Ctrl+l -> KC_RGHT held. */
    reset_engine();
    kv_set_mode(KV_MODE_NORMAL);
    CHECK(pipeline(KC_LCTL, true) == true);
    CHECK(pipeline(KC_L, true) == false);
    kv_emit_flush_now();
    CHECK(reg_count(KC_RGHT) == 1);
    CHECK(pipeline(KC_L, false) == false);
    CHECK(reg_count(KC_RGHT) == 0);
    CHECK(pipeline(KC_LCTL, false) == true);

    /* A pending prefix still wins: `d` then Ctrl+h abandons the operator and
     * passes the chord to the host (no arrow held). */
    reset_engine();
    kv_set_mode(KV_MODE_NORMAL);
    CHECK(pipeline(KC_D, true) == false);     /* operator pending */
    CHECK(pipeline(KC_LCTL, true) == true);
    CHECK(pipeline(KC_H, true) == true);      /* passthrough, not a motion */
    CHECK(reg_count(KC_LEFT) == 0);
    CHECK(kv_pending() == false);             /* strict clear dropped `d` */
    CHECK(pipeline(KC_H, false) == true);
    CHECK(pipeline(KC_LCTL, false) == true);

    /* Insert + Ctrl is untouched: h is a plain letter, passes through. */
    reset_engine(); /* INSERT */
    CHECK(pipeline(KC_LCTL, true) == true);
    CHECK(pipeline(KC_H, true) == true);
    CHECK(reg_count(KC_LEFT) == 0);
    CHECK(pipeline(KC_H, false) == true);
    CHECK(pipeline(KC_LCTL, false) == true);

    /* Visual + GUI: not the bare-NORMAL case -> chord still passes through. */
    reset_engine();
    kv_set_mode(KV_MODE_VISUAL);
    CHECK(pipeline(KC_LGUI, true) == true);
    CHECK(pipeline(KC_H, true) == true);
    CHECK(reg_count(KC_LEFT) == 0);
    CHECK(pipeline(KC_H, false) == true);
    CHECK(pipeline(KC_LGUI, false) == true);
}

/* ================= added coverage ================= */

/* Tap the mouse trigger key and report whether MOUSE was entered. */
static bool mouse_tap(void) {
    bool a = pipeline(TEST_TRIGGER_KC, true);
    bool b = pipeline(TEST_TRIGGER_KC, false);
    return a == false && b == false && kv_get_mode() == KV_MODE_MOUSE;
}

/* design §4.9 task must early-return when no cfg has been seen yet (s_cfg is
 * only assigned by vim_pipeline_process).  Runs first in main, before any
 * pipeline call leaves s_cfg non-NULL. */
static void test_task_null_cfg_guard(void) {
    vim_glue_init();
    vim_keymap_common_task(1234); /* must not dereference a NULL s_cfg */
    vim_keymap_common_task(9999);
    CHECK(kv_get_mode() == KV_MODE_INSERT);
    CHECK(reg_count(MS_BTN1) == 0);
    CHECK(reg_count(KC_RALT) == 0);
}

/* design §4.10 / §4.12: shared timer helper guards a zero timer_read() and a
 * zero start never counts as elapsed. */
static void test_timer_helpers(void) {
    reset_engine();
    g_now = 0;
    CHECK(vim_timer_start() == 1);            /* t ? t : 1 zero guard */
    g_now = 1000;
    CHECK(vim_timer_start() == 1000);
    CHECK(vim_timer_elapsed(1000, 200) == false);
    g_now = 1199;
    CHECK(vim_timer_elapsed(1000, 200) == false);  /* hold_ms - 1 */
    g_now = 1200;
    CHECK(vim_timer_elapsed(1000, 200) == true);   /* exactly hold_ms */
    CHECK(vim_timer_elapsed(0, 1) == false);       /* start == 0 -> never */
}

/* design §4.9: the pointer/wheel/button half of the mouse FSM that the existing
 * test_mouse() did not cover (K/L/Enter/plain-and-long Space + force-release). */
static void test_mouse_fsm_axes(void) {
    reset_engine();
    CHECK(mouse_tap());

    /* K -> pointer up */
    CHECK(pipeline(KC_K, true) == false);
    CHECK(reg_count(MS_UP) == 1);
    CHECK(pipeline(KC_K, false) == false);
    CHECK(reg_count(MS_UP) == 0);

    /* L -> pointer right */
    CHECK(pipeline(KC_L, true) == false);
    CHECK(reg_count(MS_RGHT) == 1);
    CHECK(pipeline(KC_L, false) == false);
    CHECK(reg_count(MS_RGHT) == 0);

    /* Shift+K -> wheel up (Shift stays in MOUSE) */
    CHECK(pipeline(KC_LSFT, true) == false);
    CHECK(kv_get_mode() == KV_MODE_MOUSE);
    CHECK(pipeline(KC_K, true) == false);
    CHECK(reg_count(MS_WHLU) == 1);
    CHECK(reg_count(MS_UP) == 0);
    CHECK(pipeline(KC_K, false) == false);
    CHECK(reg_count(MS_WHLU) == 0);
    CHECK(pipeline(KC_LSFT, false) == false);
    CHECK(kv_get_mode() == KV_MODE_MOUSE);

    /* Enter -> right button, held until release */
    CHECK(pipeline(KC_ENT, true) == false);
    CHECK(reg_count(MS_BTN2) == 1);
    CHECK(pipeline(KC_ENT, false) == false);
    CHECK(reg_count(MS_BTN2) == 0);

    /* Space short press -> single left click (tap; net registration zero) */
    int hits_before = s_hits[MS_BTN1];
    CHECK(pipeline(KC_SPC, true) == false);
    g_now += 50;
    CHECK(pipeline(KC_SPC, false) == false);
    CHECK(s_hits[MS_BTN1] == hits_before + 1);

    /* Space long press -> left button held (drag) */
    CHECK(pipeline(KC_SPC, true) == false);
    g_now += 250;
    vim_keymap_common_task(g_now);
    CHECK(reg_count(MS_BTN1) == 1);
    CHECK(pipeline(KC_SPC, false) == false);
    CHECK(reg_count(MS_BTN1) == 0);

    /* h held + another key exits -> mouse_release_all drops pointer AND both
     * buttons (design §4.9 "强制反注册全部按住的鼠标键/轴"). */
    reset_engine();
    CHECK(mouse_tap());
    CHECK(pipeline(KC_ENT, true) == false);       /* BTN2 down */
    CHECK(reg_count(MS_BTN2) == 1);
    CHECK(pipeline(KC_SPC, true) == false);       /* start left-button timer */
    g_now += 250;
    vim_keymap_common_task(g_now);                /* drag: BTN1 held */
    CHECK(reg_count(MS_BTN1) == 1);
    CHECK(pipeline(KC_H, true) == false);         /* pointer left held */
    CHECK(reg_count(MS_LEFT) == 1);
    CHECK(pipeline(KC_A, true) == true);          /* other key exits + re-identifies */
    CHECK(kv_get_mode() == KV_MODE_INSERT);
    CHECK(reg_count(MS_LEFT) == 0);
    CHECK(reg_count(MS_BTN1) == 0);
    CHECK(reg_count(MS_BTN2) == 0);
    CHECK(pipeline(KC_A, false) == true);
    CHECK(pipeline(KC_H, false) == false);        /* paired release consumed */
    CHECK(pipeline(KC_SPC, false) == false);      /* paired release consumed */
    CHECK(pipeline(KC_ENT, false) == false);      /* paired release consumed */
}

/* design §4.9: mouse_link_ok() gates entry (cfg->link_ok). */
static void test_mouse_link_gate(void) {
    vim_cfg_t cfg = g_cfg;
    cfg.link_ok = test_link_ok;

    reset_engine();
    s_link_ok = false;
    CHECK(pipeline_cfg(TEST_TRIGGER_KC, true, &cfg) == false);
    CHECK(pipeline_cfg(TEST_TRIGGER_KC, false, &cfg) == false);
    CHECK(s_link_ok == false);
    CHECK(kv_get_mode() == KV_MODE_INSERT);      /* gate refused entry */

    reset_engine();
    s_link_ok = true;
    CHECK(pipeline_cfg(TEST_TRIGGER_KC, true, &cfg) == false);
    CHECK(pipeline_cfg(TEST_TRIGGER_KC, false, &cfg) == false);
    CHECK(kv_get_mode() == KV_MODE_MOUSE);       /* gate allowed entry */
}

/* design §4.9: with vim OFF the trigger must still be owned (consumed on both
 * edges) but must never enter MOUSE.  Polarity note: mouse_process() returning
 * true = consumed => vim_pipeline_process() returns false. */
static void test_mouse_trigger_vim_off(void) {
    reset_engine();
    kv_disable();
    CHECK(pipeline(TEST_TRIGGER_KC, true) == false);    /* consumed, not passed */
    CHECK(pipeline(TEST_TRIGGER_KC, false) == false);   /* consumed, not passed */
    CHECK(kv_get_mode() == KV_MODE_INSERT);      /* never entered MOUSE */
}

/* design §4.9: MOUSE restores the exact mode it was entered from (not only
 * INSERT/NORMAL, which test_mouse() covered). */
static void test_mouse_entry_mode_restore(void) {
    reset_engine();
    kv_set_mode(KV_MODE_VISUAL);
    CHECK(mouse_tap());
    CHECK(pipeline(KC_F5, true) == true);        /* non-vim key exits MOUSE */
    CHECK(kv_get_mode() == KV_MODE_VISUAL);
    CHECK(pipeline(KC_F5, false) == true);

    reset_engine();
    kv_set_mode(KV_MODE_VISUAL_LINE);
    CHECK(mouse_tap());
    CHECK(pipeline(KC_F5, true) == true);
    CHECK(kv_get_mode() == KV_MODE_VISUAL_LINE);
    CHECK(pipeline(KC_F5, false) == true);
}

/* design §4.9: a trigger release with no live hold timer (s_mouse_timer == 0)
 * is a consumed no-op and must not toggle MOUSE. */
static void test_mouse_trigger_release_no_timer(void) {
    reset_engine();
    CHECK(mouse_tap());                          /* tap leaves s_mouse_timer == 0 */
    CHECK(pipeline(TEST_TRIGGER_KC, false) == false);   /* timer==0 branch: consumed no-op */
    CHECK(kv_get_mode() == KV_MODE_MOUSE);
}

/* design §4.10 / readme §4: Shift+Esc (Insert only) => ~ / `, gated on vim and
 * on cfg->shift_esc_enable, and never in a CAG combo. */
static void test_shift_esc_variants(void) {
    /* RSHIFT+Esc -> bare ` (KC_GRV).  RSHIFT itself is never registered (lazy
     * Shift), so both of its edges are consumed. */
    reset_engine();
    int grv_before = s_hits[KC_GRV];
    CHECK(pipeline(KC_RSFT, true) == false);   /* swallowed, never a lone Shift */
    CHECK((vim_glue_mods() & MOD_BIT_RSHIFT) != 0); /* but the shadow records it */
    CHECK(pipeline(KC_ESC, true) == false);
    CHECK(s_hits[KC_GRV] == grv_before + 1);
    CHECK(s_tap16_n >= 1 && s_tap16[s_tap16_n - 1] == KC_GRV);
    CHECK(pipeline(KC_ESC, false) == false);
    CHECK(pipeline(KC_RSFT, false) == false);

    /* LSHIFT+Esc -> ~ content LSFT(KC_GRV) */
    reset_engine();
    grv_before = s_hits[KC_GRV];
    CHECK(pipeline(KC_LSFT, true) == true);
    CHECK(pipeline(KC_ESC, true) == false);
    CHECK(s_hits[KC_GRV] == grv_before + 1);
    CHECK(s_tap16_n >= 1 && s_tap16[s_tap16_n - 1] == LSFT(KC_GRV));
    CHECK((LSFT(KC_GRV) & 0xFF) == KC_GRV);
    CHECK(pipeline(KC_ESC, false) == false);
    CHECK(pipeline(KC_LSFT, false) == true);

    /* Ctrl+Shift+Esc (CAG) is not hijacked: the real Esc passes through */
    reset_engine();
    grv_before = s_hits[KC_GRV];
    CHECK(pipeline(KC_LCTL, true) == true);
    CHECK(pipeline(KC_LSFT, true) == true);
    CHECK(pipeline(KC_ESC, true) == true);
    CHECK(s_hits[KC_GRV] == grv_before);
    CHECK(pipeline(KC_ESC, false) == true);
    CHECK(pipeline(KC_LSFT, false) == true);
    CHECK(pipeline(KC_LCTL, false) == true);

    /* cfg->shift_esc_enable == false disables the ~/` combo; the Esc then falls
     * through to the shared Esc toggle (no window -> swallowed, drops NORMAL). */
    vim_cfg_t cfg = g_cfg;
    cfg.shift_esc_enable = false;
    reset_engine();
    grv_before = s_hits[KC_GRV];
    CHECK(pipeline_cfg(KC_LSFT, true, &cfg) == true);
    CHECK(pipeline_cfg(KC_ESC, true, &cfg) == false);
    CHECK(s_hits[KC_GRV] == grv_before);
    CHECK(kv_get_mode() == KV_MODE_NORMAL);
    CHECK(pipeline_cfg(KC_ESC, false, &cfg) == false);
    CHECK(pipeline_cfg(KC_LSFT, false, &cfg) == true);
}

/* fn_layer_active() reads (layer_state | default_layer_state); the fn layer may
 * be the active default layer with no momentary layer_state bit set. */
static void test_myfn_default_layer(void) {
    reset_engine();                              /* layer_state == 0 */
    default_layer_state = (1UL << 4);            /* fn_layer == 4 */
    CHECK(pipeline(KC_Z, true) == false);        /* fn active -> undeclared swallowed */
    CHECK(pipeline(KC_Z, false) == false);
    CHECK(pipeline(MO(4), true) == true);        /* layer keys stay exempt */
    CHECK(pipeline(MO(4), false) == true);

    default_layer_state = 0;                     /* not active -> Z types through */
    CHECK(pipeline(KC_Z, true) == true);
    CHECK(pipeline(KC_Z, false) == true);
}

/* cfg->myfn_declared == NULL: every key under Fn is treated as undeclared and
 * swallowed; cfg->myfn() is never dispatched. */
static void test_myfn_declared_null(void) {
    vim_cfg_t cfg = g_cfg;
    cfg.myfn_declared = NULL;
    reset_engine();
    fn_on();
    CHECK(pipeline_cfg(KC_Z, true, &cfg) == false);
    CHECK(pipeline_cfg(KC_Z, false, &cfg) == false);
    CHECK(pipeline_cfg(KC_F1, true, &cfg) == false); /* previously declared */
    CHECK(s_myfn_calls == 0);                        /* myfn never dispatched */
    fn_off();
}

/* cfg->vim_set_enabled != NULL: the callback owns the switch; the shared layer
 * must call it with the inverse of kv_vim_enabled() and not touch the engine. */
static void test_vim_set_enabled_callback(void) {
    vim_cfg_t cfg = g_cfg;
    cfg.vim_set_enabled = test_set_enabled;

    reset_engine();                              /* vim on */
    CHECK(kv_vim_enabled() == true);
    fn_on();
    CHECK(pipeline_cfg(KC_CAPS, true, &cfg) == false);
    CHECK(s_set_enabled_calls == 0);             /* toggle happens on release */
    CHECK(kv_vim_enabled() == true);             /* callback did not change engine */
    CHECK(pipeline_cfg(KC_CAPS, false, &cfg) == false);
    CHECK(s_set_enabled_calls == 1);
    CHECK(s_set_enabled_last == false);          /* toggling off */
    fn_off();

    reset_engine();
    kv_disable();
    CHECK(kv_vim_enabled() == false);
    fn_on();
    CHECK(pipeline_cfg(KC_CAPS, true, &cfg) == false);
    CHECK(s_set_enabled_calls == 0);
    CHECK(pipeline_cfg(KC_CAPS, false, &cfg) == false);
    CHECK(s_set_enabled_calls == 1);
    CHECK(s_set_enabled_last == true);           /* toggling on */
    fn_off();
}

/* cfg->hook_pre: runs before myfn (a press it consumes never reaches myfn) and
 * its consumed press is owned by the shared pairing table on release. */
static void test_hook_pre_order_and_pairing(void) {
    vim_cfg_t cfg = g_cfg;
    cfg.hook_pre = hook_pre_f1;
    cfg.myfn_declared = ord_declared;
    cfg.myfn = ord_myfn;

    reset_engine();
    fn_on();
    CHECK(pipeline_cfg(KC_F1, true, &cfg) == false); /* hook_pre consumes */
    CHECK(s_hook_pre_calls == 1);
    CHECK(s_ord_myfn_n == 0);                        /* myfn not reached -> order proven */
    CHECK(pipeline_cfg(KC_F1, false, &cfg) == false);/* paired release consumed */

    /* A declared key the hook does not claim reaches myfn and passes to QMK. */
    CHECK(pipeline_cfg(KC_X, true, &cfg) == true);
    CHECK(s_ord_myfn_n == 1 && s_ord_myfn_seen[0] == KC_X);
    CHECK(pipeline_cfg(KC_X, false, &cfg) == true);
    fn_off();
}

/* design §4.9 / §4.12: Caps tap/hold entry-mode handling for VISUAL /
 * VISUAL_LINE / NORMAL and the exact hold_ms boundary. */
static void test_caps_entry_modes(void) {
    /* short press from VISUAL -> NORMAL */
    reset_engine();
    kv_set_mode(KV_MODE_VISUAL);
    CHECK(pipeline(KC_CAPS, true) == false);
    CHECK(kv_get_mode() == KV_MODE_NORMAL);
    CHECK(pipeline(KC_CAPS, false) == false);
    CHECK(kv_get_mode() == KV_MODE_NORMAL);

    /* short press from VISUAL_LINE -> NORMAL */
    reset_engine();
    kv_set_mode(KV_MODE_VISUAL_LINE);
    CHECK(pipeline(KC_CAPS, true) == false);
    CHECK(kv_get_mode() == KV_MODE_NORMAL);
    CHECK(pipeline(KC_CAPS, false) == false);
    CHECK(kv_get_mode() == KV_MODE_NORMAL);

    /* long press from NORMAL restores NORMAL */
    reset_engine();
    kv_set_mode(KV_MODE_NORMAL);
    g_now = 3000;
    CHECK(pipeline(KC_CAPS, true) == false);
    CHECK(kv_get_mode() == KV_MODE_NORMAL);
    g_now += 250;
    CHECK(pipeline(KC_CAPS, false) == false);
    CHECK(kv_get_mode() == KV_MODE_NORMAL);
}

static void test_caps_hold_boundary(void) {
    /* elapsed == hold_ms => held => restore the entry mode (INSERT) */
    reset_engine();
    g_now = 4000;
    CHECK(pipeline(KC_CAPS, true) == false);
    CHECK(kv_get_mode() == KV_MODE_NORMAL);      /* momentary Normal on press */
    g_now += 200;                                /* hold_ms */
    CHECK(pipeline(KC_CAPS, false) == false);
    CHECK(kv_get_mode() == KV_MODE_INSERT);      /* held -> entry mode restored */

    /* elapsed == hold_ms - 1 => tap => stays Normal (Insert -> Normal toggle) */
    reset_engine();
    g_now = 4000;
    CHECK(pipeline(KC_CAPS, true) == false);
    g_now += 199;
    CHECK(pipeline(KC_CAPS, false) == false);
    CHECK(kv_get_mode() == KV_MODE_NORMAL);
}

/* design §4.12: vim_keymap_common_task() long-press timing for the mouse
 * trigger — below the threshold no modifier, at/above it one registration, and
 * an already-held trigger is never re-registered. */
static void test_mouse_task_threshold(void) {
    reset_engine();
    g_now = 5000;
    CHECK(pipeline(TEST_TRIGGER_KC, true) == false);
    g_now += 100;                                /* below hold_ms */
    vim_keymap_common_task(g_now);
    CHECK(reg_count(KC_RALT) == 0);
    g_now += 150;                                /* now 250 ms elapsed */
    vim_keymap_common_task(g_now);
    CHECK(reg_count(KC_RALT) == 1);
    vim_keymap_common_task(g_now);               /* still held: no re-register */
    CHECK(reg_count(KC_RALT) == 1);
    CHECK(pipeline(TEST_TRIGGER_KC, false) == false);
    CHECK(reg_count(KC_RALT) == 0);
    CHECK(kv_get_mode() == KV_MODE_INSERT);
}

/* design §4.12 / fn readme §3: every QK layer-key flavour (MO() is already
 * covered by test_myfn_skeleton) is exempt under Fn on BOTH edges — 
 * vim_is_layer_key() must report true and the pipeline must pass it so QMK can
 * latch the layer (a layer key must never be swallowed by myfn). */
static void test_layer_key_exempt(void) {
    const uint16_t keys[] = {LT(1, KC_A), LM(1, MOD_BIT_LCTRL), TT(1), OSL(1)};
    for (unsigned i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        reset_engine();
        fn_on();
        CHECK(vim_is_layer_key(keys[i]) == true);
        CHECK(pipeline(keys[i], true) == true);
        CHECK(pipeline(keys[i], false) == true);
        fn_off();
    }

    /* (c5) Each IS_QK_* predicate must select exactly its own layer-key flavour,
     * otherwise vim_is_layer_key()'s corresponding arm is unreachable.  The
     * stub previously used 0xF000 for IS_QK_LAYER_MOD (which also swallowed
     * TT(1) = 0x5801) and 0xFF00 for IS_QK_ONE_SHOT_LAYER (which never matched
     * OSL(l)); assert both the positive arm and the absence of cross-hits. */
    CHECK(IS_QK_LAYER_TAP(LT(1, KC_A)) == true);
    CHECK(IS_QK_LAYER_MOD(LM(1, MOD_BIT_LCTRL)) == true);
    CHECK(IS_QK_LAYER_TAP_TOGGLE(TT(1)) == true);
    CHECK(IS_QK_ONE_SHOT_LAYER(OSL(1)) == true);
    /* No cross-hits: the old masks let LAYER_MOD claim TT, and LAYER_TAP /
     * LAYER_TAP_TOGGLE / ONE_SHOT_LAYER never claimed each other. */
    CHECK(IS_QK_LAYER_MOD(TT(1)) == false);
    CHECK(IS_QK_LAYER_TAP_TOGGLE(OSL(1)) == false);
    CHECK(IS_QK_LAYER_TAP(LM(1, MOD_BIT_LCTRL)) == false);
    CHECK(IS_QK_LAYER_TAP(TT(1)) == false);
    CHECK(IS_QK_LAYER_TAP_TOGGLE(LT(1, KC_A)) == false);
    CHECK(IS_QK_ONE_SHOT_LAYER(LT(1, KC_A)) == false);

    /* (c5-v) The MO/LT/LM/TT/OSL arms of vim_is_layer_key() are a short-circuit
     * `||` chain; an arm is only *reachable* if every earlier predicate is false
     * for the key it is supposed to recognise.  Assert each arm's own positive
     * AND that all earlier arms reject it, so no arm can be dead.  The OSL arm
     * was the regression: with the old masks OSL(1)=0x5281 was claimed by both
     * IS_QK_MOMENTARY (0x5281 & 0xFF00 == 0x5200) and IS_QK_LAYER_MOD
     * (0x5281 & 0xF800 == 0x5000), so vim_is_layer_key() never needed the OSL
     * predicate.  OSL(1) must be the ONLY flavour that matches. */
    const uint16_t osl = OSL(1);
    CHECK(IS_QK_MOMENTARY(MO(4)) == true);
    CHECK(IS_QK_LAYER_TAP(LT(1, KC_A)) == true);
    CHECK(IS_QK_LAYER_MOD(LM(1, MOD_BIT_LCTRL)) == true);
    CHECK(IS_QK_LAYER_TAP_TOGGLE(TT(1)) == true);
    CHECK(IS_QK_ONE_SHOT_LAYER(osl) == true);

    /* MO arm: first in the chain, no earlier predicate. */
    CHECK(IS_QK_LAYER_TAP(MO(4)) == false);
    CHECK(IS_QK_LAYER_MOD(MO(4)) == false);
    CHECK(IS_QK_LAYER_TAP_TOGGLE(MO(4)) == false);
    CHECK(IS_QK_ONE_SHOT_LAYER(MO(4)) == false);

    /* LT arm: only MOMENTARY precedes it. */
    CHECK(IS_QK_MOMENTARY(LT(1, KC_A)) == false);

    /* LM arm: MOMENTARY + LAYER_TAP precede it. */
    CHECK(IS_QK_MOMENTARY(LM(1, MOD_BIT_LCTRL)) == false);
    CHECK(IS_QK_LAYER_TAP(LM(1, MOD_BIT_LCTRL)) == false);
    CHECK(IS_QK_LAYER_TAP_TOGGLE(LM(1, MOD_BIT_LCTRL)) == false);
    CHECK(IS_QK_ONE_SHOT_LAYER(LM(1, MOD_BIT_LCTRL)) == false);

    /* TT arm: MOMENTARY + LAYER_TAP + LAYER_MOD precede it (the old 0xF000
     * LAYER_MOD mask used to steal it). */
    CHECK(IS_QK_MOMENTARY(TT(1)) == false);
    CHECK(IS_QK_LAYER_TAP(TT(1)) == false);
    CHECK(IS_QK_LAYER_MOD(TT(1)) == false);
    CHECK(IS_QK_ONE_SHOT_LAYER(TT(1)) == false);

    /* OSL arm is isolated: every other flavour must reject OSL(1). */
    CHECK(IS_QK_MOMENTARY(osl) == false);
    CHECK(IS_QK_LAYER_TAP(osl) == false);
    CHECK(IS_QK_LAYER_MOD(osl) == false);
    CHECK(IS_QK_LAYER_TAP_TOGGLE(osl) == false);
    CHECK(IS_QK_TO(osl) == false);
    CHECK(IS_QK_TOGGLE_LAYER(osl) == false);
    CHECK(IS_QK_DEF_LAYER(osl) == false);
    CHECK(vim_is_layer_key(osl) == true); /* reached via the OSL arm alone */

    /* G8: the TO/TG/DF arms of vim_is_layer_key() must be reachable.  First
     * prove the stub's new ranges are not shadowed by the earlier IS_QK_*
     * predicates (otherwise the TO/TG/DF branch would be dead code). */
    CHECK(IS_QK_TO(TO(4)) == true);
    CHECK(IS_QK_TOGGLE_LAYER(TG(4)) == true);
    CHECK(IS_QK_DEF_LAYER(DF(4)) == true);
    CHECK(IS_QK_MOMENTARY(TO(4)) == false);
    CHECK(IS_QK_LAYER_TAP(TO(4)) == false);
    CHECK(IS_QK_LAYER_MOD(TO(4)) == false);
    CHECK(IS_QK_LAYER_TAP_TOGGLE(TO(4)) == false);
    CHECK(IS_QK_ONE_SHOT_LAYER(TO(4)) == false);
    CHECK(IS_QK_MOMENTARY(TG(4)) == false);
    CHECK(IS_QK_LAYER_TAP(TG(4)) == false);
    CHECK(IS_QK_LAYER_MOD(TG(4)) == false);
    CHECK(IS_QK_LAYER_TAP_TOGGLE(TG(4)) == false);
    CHECK(IS_QK_ONE_SHOT_LAYER(TG(4)) == false);
    CHECK(IS_QK_MOMENTARY(DF(4)) == false);
    CHECK(IS_QK_LAYER_TAP(DF(4)) == false);
    CHECK(IS_QK_LAYER_MOD(DF(4)) == false);
    CHECK(IS_QK_LAYER_TAP_TOGGLE(DF(4)) == false);
    CHECK(IS_QK_ONE_SHOT_LAYER(DF(4)) == false);

    /* Same exempt contract as MO/LT/LM/TT/OSL: recognised and passed on both
     * edges so QMK can latch the layer. */
    const uint16_t to_tg_df[] = {TO(4), TG(4), DF(4)};
    for (unsigned i = 0; i < sizeof(to_tg_df) / sizeof(to_tg_df[0]); i++) {
        reset_engine();
        fn_on();
        CHECK(vim_is_layer_key(to_tg_df[i]) == true);
        CHECK(pipeline(to_tg_df[i], true) == true);
        CHECK(pipeline(to_tg_df[i], false) == true);
        fn_off();
    }
}

/* design §4.12 hook_process(): a hook may consume a press, but its claim to
 * consume the *release* is deliberately ignored — releases are governed solely
 * by the shared press/release pairing table. */
static void test_hook_release_claim_ignored(void) {
    vim_cfg_t cfg = g_cfg;
    cfg.hook_pre = hook_pre_both;

    /* Isolated release (no prior press): the ignored claim falls through to the
     * pairing table, which has no entry, so QMK still gets the key-up. */
    reset_engine();
    CHECK(pipeline_cfg(KC_T, false, &cfg) == true);
    CHECK(s_hook_both_calls == 1);

    /* With a consumed press paired, the same release is consumed by the table. */
    CHECK(pipeline_cfg(KC_T, true, &cfg) == false);  /* hook claims the press */
    CHECK(s_hook_both_calls == 2);
    CHECK(pipeline_cfg(KC_T, false, &cfg) == false); /* release claim ignored; paired */
    CHECK(s_hook_both_calls == 3);
}

/* design §4.9 mouse_process() release default: a non-mouse key whose press was
 * consumed by a pre-hook never exits MOUSE, so its release reaches the mouse
 * release switch's default arm (not consumed there, then paired by step 8 — no
 * orphan key-up). */
static void test_mouse_release_default_branch(void) {
    vim_cfg_t cfg = g_cfg;
    cfg.hook_pre = hook_pre_both;

    reset_engine();
    CHECK(mouse_tap());                          /* enter MOUSE */
    CHECK(feed_cfg(KC_T, true, &cfg) == false);  /* hook eats press; MOUSE stays */
    CHECK(kv_get_mode() == KV_MODE_MOUSE);
    CHECK(feed_cfg(KC_T, false, &cfg) == false); /* mouse default arm -> paired */
    CHECK(kv_get_mode() == KV_MODE_MOUSE);
    CHECK(s_orphan == 0);
}

/* (a1) design §4.9 mouse_process() L184: the "above the modifier range" arm of
 * IS_MODIFIER_KEYCODE().  A key >= KC_LCTL but > KC_RGUI (here MO(4)=0x5204) is
 * not a modifier, so it takes the generic "other key" branch: exit MOUSE and
 * re-identify the key in the entry mode (the layer key then passes to QMK). */
static void test_mouse_nonmod_above_upper(void) {
    reset_engine();
    CHECK(mouse_tap());                                /* enter MOUSE (INSERT) */
    CHECK(IS_MODIFIER_KEYCODE(MO(4)) == false);        /* upper bound is exclusive */
    CHECK(MO(4) >= KC_LCTL && MO(4) > KC_RGUI);
    CHECK(pipeline(MO(4), true) == true);              /* exits + re-identify */
    CHECK(kv_get_mode() == KV_MODE_INSERT);
    CHECK(pipeline(MO(4), false) == true);             /* then passes through */
    CHECK(kv_get_mode() == KV_MODE_INSERT);
}

/* (a2) design §4.9: the four release arms whose s_move_reg[MV_*] is still zero.
 * Enter MOUSE, let a pre-hook consume each movement key's press (so the mouse
 * FSM never stores a registration), then release it: the release switch hits
 * the `if (s_move_reg[...])` false arm, returns "not consumed", and step 8's
 * pairing table swallows the paired release.  No host axis is ever touched and
 * no orphan key-up escapes. */
static void test_mouse_move_release_unset(void) {
    vim_cfg_t cfg = g_cfg;
    cfg.hook_pre = hook_pre_mouse_own;

    const uint16_t move[] = {KC_H, KC_J, KC_K, KC_L};
    const uint16_t axis[] = {MS_LEFT, MS_DOWN, MS_UP, MS_RGHT};
    for (unsigned i = 0; i < sizeof(move) / sizeof(move[0]); i++) {
        reset_engine();
        CHECK(pipeline_cfg(TEST_TRIGGER_KC, true, &cfg) == false);   /* enter MOUSE */
        CHECK(pipeline_cfg(TEST_TRIGGER_KC, false, &cfg) == false);
        CHECK(kv_get_mode() == KV_MODE_MOUSE);
        CHECK(feed_cfg(move[i], true, &cfg) == false);        /* hook eats the press */
        CHECK(kv_get_mode() == KV_MODE_MOUSE);                /* MOUSE untouched */
        CHECK(feed_cfg(move[i], false, &cfg) == false);       /* false arm; paired */
        CHECK(reg_count(axis[i]) == 0);                       /* host axis untouched */
        CHECK(s_orphan == 0);
        CHECK(kv_get_mode() == KV_MODE_MOUSE);
    }
}

/* (a3) design §4.9 mouse_process() L263-271: releasing Space with no live hold
 * timer (s_lbtn_timer == 0) takes the `else if (s_lbtn_timer)` false arm and
 * must not emit a left-click tap. */
static void test_mouse_space_release_no_timer(void) {
    vim_cfg_t cfg = g_cfg;
    cfg.hook_pre = hook_pre_mouse_own;

    reset_engine();
    CHECK(pipeline_cfg(TEST_TRIGGER_KC, true, &cfg) == false);
    CHECK(pipeline_cfg(TEST_TRIGGER_KC, false, &cfg) == false);
    CHECK(kv_get_mode() == KV_MODE_MOUSE);

    int hits_before = s_hits[MS_BTN1];
    CHECK(feed_cfg(KC_SPC, true, &cfg) == false);   /* hook eats press; no timer */
    CHECK(feed_cfg(KC_SPC, false, &cfg) == false);  /* s_lbtn_timer == 0 arm */
    CHECK(s_hits[MS_BTN1] == hits_before);          /* never tapped MS_BTN1 */
    CHECK(reg_count(MS_BTN1) == 0);
    CHECK(s_orphan == 0);
    CHECK(kv_get_mode() == KV_MODE_MOUSE);
}

/* (a4) design §4.9 mouse_process() L161: the `if (s_mouse_mod_reg)` false arm
 * when neither Win nor Mac modifier is configured (mod_win == mod_mac == 0).
 * Not a real keyboard cfg, but constructible: a long-press records 0 as the
 * registered code, so release must skip the unregister and still not toggle. */
static void test_mouse_trigger_mod_zero(void) {
    vim_cfg_t cfg = g_cfg;
    cfg.mod_win = 0;
    cfg.mod_mac = 0;

    reset_engine();
    g_now = 7100;
    CHECK(pipeline_cfg(TEST_TRIGGER_KC, true, &cfg) == false);
    g_now += 250;
    vim_keymap_common_task(g_now);                    /* s_mouse_held, s_mouse_mod_reg = 0 */
    CHECK(kv_get_mode() == KV_MODE_INSERT);           /* long press does not enter MOUSE */
    CHECK(pipeline_cfg(TEST_TRIGGER_KC, false, &cfg) == false); /* s_mouse_mod_reg == 0 arm */
    CHECK(reg_count(KC_RALT) == 0);
    CHECK(kv_get_mode() == KV_MODE_INSERT);
}

/* design §4.9: once the Space long-press has registered MS_BTN1 (s_lbtn_held),
 * vim_keymap_common_task() must never register it again while the key is held. */
static void test_mouse_lbtn_idempotent(void) {
    reset_engine();
    CHECK(mouse_tap());
    g_now = 6000;
    CHECK(pipeline(KC_SPC, true) == false);      /* start the left-button timer */
    g_now += 250;
    vim_keymap_common_task(g_now);               /* threshold -> BTN1 held once */
    CHECK(reg_count(MS_BTN1) == 1);
    vim_keymap_common_task(g_now);               /* !s_lbtn_held false -> no-op */
    CHECK(reg_count(MS_BTN1) == 1);
    vim_keymap_common_task(g_now + 50);          /* still held -> no-op */
    CHECK(reg_count(MS_BTN1) == 1);
    CHECK(pipeline(KC_SPC, false) == false);
    CHECK(reg_count(MS_BTN1) == 0);
    CHECK(kv_get_mode() == KV_MODE_MOUSE);
}

/* G5 — design §4.9: the H press/release pairing inside MOUSE.  Pressing H
 * registers the host pointer-left; releasing H unregisters exactly it, stays in
 * MOUSE, and is swallowed by the shared pairing table (so QMK never receives an
 * orphan key-up for a press it never saw). */
static void test_mouse_h_release(void) {
    reset_engine();
    CHECK(mouse_tap());                              /* enter MOUSE */
    CHECK(feed_cfg(KC_H, true, &g_cfg) == false);    /* mouse consumes the press */
    CHECK(reg_count(MS_LEFT) == 1);
    CHECK(kv_get_mode() == KV_MODE_MOUSE);
    CHECK(feed_cfg(KC_H, false, &g_cfg) == false);   /* paired release: no orphan */
    CHECK(reg_count(MS_LEFT) == 0);
    CHECK(kv_get_mode() == KV_MODE_MOUSE);
    CHECK(s_orphan == 0);
}

/* G7 — design §4.9/§4.12: the Space long-press inside MOUSE registers nothing
 * below hold_ms; at/after the threshold task() holds MS_BTN1 (drag). */
static void test_mouse_lbtn_threshold(void) {
    reset_engine();
    CHECK(mouse_tap());                              /* enter MOUSE */
    g_now = 7000;
    CHECK(pipeline(KC_SPC, true) == false);          /* start the lbtn timer */
    g_now += 100;                                    /* < hold_ms (200) */
    vim_keymap_common_task(g_now);
    CHECK(reg_count(MS_BTN1) == 0);
    g_now += 150;                                    /* now 250 ms elapsed */
    vim_keymap_common_task(g_now);
    CHECK(reg_count(MS_BTN1) == 1);
    CHECK(pipeline(KC_SPC, false) == false);         /* paired release */
    CHECK(reg_count(MS_BTN1) == 0);
    CHECK(kv_get_mode() == KV_MODE_MOUSE);
}

/* design §4.10: Shift+Esc is hijacked only in INSERT.  In NORMAL the ~/` combo
 * is skipped; the Esc then takes the shared Esc-toggle path (real Esc back to
 * INSERT) and no ~ / ` is emitted. */
static void test_shift_esc_normal_passthrough(void) {
    reset_engine();
    kv_set_mode(KV_MODE_NORMAL);
    int grv_before = s_hits[KC_GRV];
    CHECK(pipeline(KC_LSFT, true) == true);
    CHECK(pipeline(KC_ESC, true) == true);       /* NORMAL idle -> real Esc, to INSERT */
    CHECK(kv_get_mode() == KV_MODE_INSERT);
    CHECK(s_hits[KC_GRV] == grv_before);
    CHECK(pipeline(KC_ESC, false) == true);
    CHECK(pipeline(KC_LSFT, false) == true);
}

/* design §4.12 / test_caps_long_from_visual: Caps long press restores the exact
 * entry mode; from VISUAL_LINE it must return to VISUAL_LINE. */
static void test_caps_long_from_visual_line(void) {
    reset_engine();
    kv_set_mode(KV_MODE_VISUAL_LINE);
    g_now = 2000;
    CHECK(pipeline(KC_CAPS, true) == false);
    g_now += 250;
    CHECK(pipeline(KC_CAPS, false) == false);
    CHECK(kv_get_mode() == KV_MODE_VISUAL_LINE);
}

/* design §2.1: the `/` -> Ctrl+F shortcut requires no Shift; a physically held
 * Shift must disable it (mods_req == 0 => no masked modifier may be down). */
static void test_shortcut_shift_slash(void) {
    /* bare `/` triggers sc_find (LCTL(KC_F)) and consumes both edges */
    reset_engine();
    kv_set_mode(KV_MODE_NORMAL);
    int find_before = s_hits[KC_F];
    CHECK(pipeline(KC_SLSH, true) == false);
    CHECK(s_hits[KC_F] == find_before + 1);
    CHECK(pipeline(KC_SLSH, false) == false);

    /* Shift+/ must NOT trigger it: both edges pass through to QMK */
    reset_engine();
    kv_set_mode(KV_MODE_NORMAL);
    find_before = s_hits[KC_F];
    CHECK(pipeline(KC_LSFT, true) == true);
    CHECK(pipeline(KC_SLSH, true) == true);
    CHECK(s_hits[KC_F] == find_before);
    CHECK(pipeline(KC_SLSH, false) == true);
    CHECK(pipeline(KC_LSFT, false) == true);
}

/* design §4.12: pipeline steps are ordered myfn (2) before hook_post_myfn (3).
 * A declared key myfn passes must still reach the post hook; a key myfn
 * consumes must never reach it. */
static void test_hook_post_after_myfn(void) {
    vim_cfg_t cfg = g_cfg;
    cfg.myfn_declared  = ord_declared; /* KC_X declared */
    cfg.myfn           = order_myfn;   /* passes */
    cfg.hook_post_myfn = order_post;   /* never consumes */

    reset_engine();
    fn_on();
    CHECK(pipeline_cfg(KC_X, true, &cfg) == true);
    CHECK(s_myfn_order_at == 1);               /* myfn ran first */
    CHECK(s_post_calls == 1);
    CHECK(s_post_order_at == 2);               /* post_myfn ran after */
    CHECK(s_post_order_at > s_myfn_order_at);
    CHECK(pipeline_cfg(KC_X, false, &cfg) == true);

    /* A key myfn consumes must never reach the post hook. */
    vim_cfg_t consuming = g_cfg;
    consuming.myfn_declared  = test_declared; /* KC_SPC declared */
    consuming.myfn           = test_myfn;     /* consumes KC_SPC */
    consuming.hook_post_myfn = order_post;
    reset_engine();
    fn_on();
    CHECK(pipeline_cfg(KC_SPC, true, &consuming) == false);
    CHECK(s_post_calls == 0);                  /* step 3 not reached */
    fn_off();
}

/* ================= Esc toggle + grace window ================= */

/* INSERT with no window: Esc is swallowed and drops to NORMAL (no host Esc). */
static void test_esc_insert_no_window(void) {
    reset_engine(); /* INSERT, no grace window */
    CHECK(pipeline(KC_ESC, true) == false);
    CHECK(kv_get_mode() == KV_MODE_NORMAL);
    CHECK(pipeline(KC_ESC, false) == false);
}

/* NORMAL idle Esc: real Esc and back to INSERT, opening the grace window. */
static void test_esc_normal_to_insert(void) {
    reset_engine();
    kv_set_mode(KV_MODE_NORMAL);
    CHECK(pipeline(KC_ESC, true) == true);     /* passes -> host Esc */
    CHECK(kv_get_mode() == KV_MODE_INSERT);
    CHECK(pipeline(KC_ESC, false) == true);
}

/* Grace window: within 3 s of a Normal->Insert Esc, Insert Esc stays a real
 * Esc (and resets the window); past 3 s it toggles to NORMAL again. */
static void test_esc_grace_window(void) {
    reset_engine();
    kv_set_mode(KV_MODE_NORMAL);
    g_now = 1000;
    CHECK(pipeline(KC_ESC, true) == true);     /* open window at t=1000 */
    CHECK(kv_get_mode() == KV_MODE_INSERT);
    CHECK(pipeline(KC_ESC, false) == true);

    /* 2999 ms later: still in window -> real Esc, stays INSERT */
    g_now = 1000 + 2999;
    CHECK(pipeline(KC_ESC, true) == true);
    CHECK(kv_get_mode() == KV_MODE_INSERT);    /* real Esc, mode unchanged */
    CHECK(pipeline(KC_ESC, false) == true);

    /* exactly 3000 ms after the reset: window expired -> swallow, NORMAL */
    g_now += 3000;
    CHECK(pipeline(KC_ESC, true) == false);
    CHECK(kv_get_mode() == KV_MODE_NORMAL);
    CHECK(pipeline(KC_ESC, false) == false);
}

/* Entering INSERT another way (Caps enable) grants no window. */
static void test_esc_no_window_other_paths(void) {
    reset_engine();
    kv_disable();
    CHECK(pipeline(KC_CAPS, true) == false);   /* vim off: press owned */
    CHECK(pipeline(KC_CAPS, false) == false);  /* tap re-enables vim -> INSERT */
    CHECK(kv_get_mode() == KV_MODE_INSERT);
    /* No window: Esc immediately toggles to NORMAL. */
    CHECK(pipeline(KC_ESC, true) == false);
    CHECK(kv_get_mode() == KV_MODE_NORMAL);
    CHECK(pipeline(KC_ESC, false) == false);
}

/* ================= Right Shift lazy send ================= */

static bool lshift_down(void) { return (get_mods() & MOD_BIT_LSHIFT) != 0; }

/* RShift alone: never registered, both edges consumed, no lone Shift. */
static void test_rshift_alone_silent(void) {
    reset_engine(); /* INSERT */
    CHECK(pipeline(KC_RSFT, true) == false);   /* swallowed */
    CHECK(!lshift_down());                     /* no lazy Shift yet */
    CHECK(pipeline(KC_RSFT, false) == false);  /* paired release */
    CHECK(!lshift_down());
    CHECK(s_orphan == 0);
}

/* RShift+a in INSERT: host gets Shift+a (A), never a lone Shift. */
static void test_rshift_letter_uppercase(void) {
    reset_engine(); /* INSERT */
    CHECK(pipeline(KC_RSFT, true) == false);
    CHECK(!lshift_down());
    CHECK(pipeline(KC_A, true) == true);       /* passes (a) with lazy Shift added */
    CHECK(lshift_down());                      /* lazily asserted */
    CHECK(pipeline(KC_A, false) == true);
    CHECK(lshift_down());                      /* still held for the combo */
    CHECK(pipeline(KC_RSFT, false) == false);
    CHECK(!lshift_down());                     /* dropped on RShift release */
    CHECK(s_orphan == 0);
}

/* RShift + Ctrl + C: lazy Shift adds to the chord -> Ctrl+Shift+C. */
static void test_rshift_with_ctrl(void) {
    reset_engine(); /* INSERT */
    CHECK(pipeline(KC_RSFT, true) == false);
    CHECK(pipeline(KC_LCTL, true) == true);    /* modifier passes, no lazy yet */
    CHECK(!lshift_down());
    CHECK(pipeline(KC_C, true) == true);       /* Ctrl+C with lazy Shift */
    CHECK(lshift_down());
    CHECK(pipeline(KC_C, false) == true);
    CHECK(pipeline(KC_LCTL, false) == true);
    CHECK(pipeline(KC_RSFT, false) == false);
    CHECK(!lshift_down());
}

/* Modifiers / Esc / layer keys are exempt: RShift never wraps them. */
static void test_rshift_exempt_keys(void) {
    reset_engine(); /* INSERT */
    CHECK(pipeline(KC_RSFT, true) == false);
    CHECK(pipeline(KC_LALT, true) == true);    /* modifier: exempt */
    CHECK(!lshift_down());
    CHECK(pipeline(KC_LALT, false) == true);
    CHECK(pipeline(KC_RSFT, false) == false);
    CHECK(!lshift_down());
}

/* With vim off, Right Shift is an ordinary modifier. */
static void test_rshift_normal_when_vim_off(void) {
    reset_engine();
    kv_disable();
    CHECK(pipeline(KC_RSFT, true) == true);    /* passes as a normal modifier */
    CHECK(pipeline(KC_RSFT, false) == true);
}

int main(void) {
    /* Must run before any pipeline call so s_cfg is still NULL. */
    test_task_null_cfg_guard();
    test_timer_helpers();
    test_polarity_pairing();
    test_shadow_before_swallow();
    test_myfn_skeleton();
    test_caps();
    test_caps_long_from_visual();
    test_mouse();
    test_mouse_modifier_exit();
    test_mode_change_releases_motion();
    test_shift_esc_vim_off();
    test_hook_pairing_repro();
    test_held_motion();
    test_held_motion_with_cag();
    test_visual_esc_and_cag();
    /* added coverage */
    test_mouse_fsm_axes();
    test_mouse_link_gate();
    test_mouse_trigger_vim_off();
    test_mouse_entry_mode_restore();
    test_mouse_trigger_release_no_timer();
    test_shift_esc_variants();
    test_myfn_default_layer();
    test_myfn_declared_null();
    test_vim_set_enabled_callback();
    test_hook_pre_order_and_pairing();
    test_caps_entry_modes();
    test_caps_hold_boundary();
    test_mouse_task_threshold();
    test_mouse_h_release();              /* G5 */
    test_mouse_lbtn_threshold();         /* G7 */
    /* second-round audit branch coverage */
    test_layer_key_exempt();             /* + G8 TO/TG/DF */
    test_hook_release_claim_ignored();
    test_mouse_release_default_branch();
    test_mouse_lbtn_idempotent();
    test_shift_esc_normal_passthrough();
    test_caps_long_from_visual_line();
    test_shortcut_shift_slash();
    test_hook_post_after_myfn();
    /* fourth-round audit: (a) mouse branch coverage + (c) layer-key predicates */
    test_mouse_nonmod_above_upper();     /* (a1) L184 upper-bound arm */
    test_mouse_move_release_unset();     /* (a2) L238/246/252/258 false arms */
    test_mouse_space_release_no_timer(); /* (a3) L267 false arm */
    test_mouse_trigger_mod_zero();       /* (a4) L161 false arm */
    /* Esc toggle + grace window */
    test_esc_insert_no_window();
    test_esc_normal_to_insert();
    test_esc_grace_window();
    test_esc_no_window_other_paths();
    /* Right Shift lazy send */
    test_rshift_alone_silent();
    test_rshift_letter_uppercase();
    test_rshift_with_ctrl();
    test_rshift_exempt_keys();
    test_rshift_normal_when_vim_off();
    printf("glue: pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
