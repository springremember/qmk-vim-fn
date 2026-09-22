/* test_glue_falsify.c — focused falsification of the shared-layer fixes.
 *
 * Test-only.  Deliberately kept separate from test_glue.c so the 125-count
 * regression command is unchanged.  Each test below is a minimal, faithful
 * reproduction of one scenario from the review checklist; assertions are on
 * observable host effects (register/unregister bookkeeping) and on the
 * pipeline polarity (true = QMK let-through, false = consumed).
 *
 * Design authority: vim/design.md §4.9 (mouse), §4.10 (key-up/modifiers/held
 * motion), §4.12 (pipeline / glue responsibilities); vim/testcase.md §9/§10.
 */
#include "qmk_stub.h"
#include "emit.h" /* kv_emit_flush_now() */
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

/* Cumulative number of register_code() calls per keycode (keycodes < 256).
 * A "tap" is register+unregister, which leaves reg_count() unchanged; this
 * counter lets a test tell a real tap apart from a silently dropped emit. */
#define HIT_CAP 256
static int s_hits[HIT_CAP];

/* Physical keys whose press the pipeline let through to the host.  An orphan
 * key-up is a *physical* release the pipeline lets through for a key whose
 * physical press was consumed (design §4.10 E3 mirror).  Internal
 * unregister_code() calls (emit taps, mouse_release_all's blanket clear) are
 * QMK-internal and deliberately not counted. */
static uint16_t s_phys[REG_CAP];
static int      s_phys_n;
static int      s_orphan;

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

static uint16_t g_now;
uint16_t timer_read(void) { return g_now; }
uint16_t timer_elapsed(uint16_t since) { return (uint16_t)(g_now - since); }

static int reg_count(uint16_t kc) {
    int n = 0;
    for (int i = 0; i < s_reg_n; i++) if (s_reg[i] == kc) n++;
    return n;
}

/* emulate QMK: when the pipeline lets a press/release through, the host
 * registers/unregisters it.  A release we let through for a key the host never
 * registered is an orphan key-up (design §4.10 E3 mirror). */
static void host_press(uint16_t kc) { register_code(kc); }
static void host_release(uint16_t kc) { unregister_code(kc); }

/* ---------------- test bookkeeping ---------------- */
static int g_pass, g_fail;
#define CHECK(cond)                                                        \
    do {                                                                   \
        if (cond) { g_pass++; }                                            \
        else { g_fail++; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } \
    } while (0)

/* ---------------- keyboard cfg (mirrors QK61) ---------------- */
static bool test_declared(uint16_t kc) {
    if (kc >= KC_F1 && kc <= KC_F12) return true;
    if (kc == KC_VOLD || kc == KC_VOLU) return true;
    if (kc == KC_SPC || kc == KC_CAPS || kc == KC_ESC) return true;
    if (kc == KC_T) return true;
    return false;
}
static int  s_myfn_calls;
static bool test_myfn(uint16_t kc, bool pressed) {
    if (kc == KC_SPC) {
        if (pressed) s_myfn_calls++;
        return true;
    }
    return false;
}

/* Mutable platform selector used by the mouse trigger long-press test (P2). */
static bool s_is_mac;
static bool test_is_mac(void) { return s_is_mac; }

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
    .hook_post_myfn   = NULL,
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

static void phys_add(uint16_t kc) { if (s_phys_n < REG_CAP) s_phys[s_phys_n++] = kc; }
static bool phys_take(uint16_t kc) {
    for (int i = 0; i < s_phys_n; i++) {
        if (s_phys[i] == kc) { s_phys[i] = s_phys[--s_phys_n]; return true; }
    }
    return false;
}

/* pipeline + host emulation: returns the pipeline polarity, and mirrors the
 * host effect so register/unregister can be asserted. */
static bool feed_cfg(uint16_t kc, bool pressed, const vim_cfg_t *cfg) {
    bool pass = pipeline_cfg(kc, pressed, cfg);
    if (pressed) {
        if (pass) { host_press(kc); phys_add(kc); }
    } else {
        if (pass) {
            if (!phys_take(kc)) s_orphan++;
            host_release(kc);
        }
    }
    return pass;
}

static bool feed(uint16_t kc, bool pressed) { return feed_cfg(kc, pressed, &g_cfg); }

static void reset_engine(void) {
    g_now = 1000;
    s_mods = 0;
    s_reg_n = 0;
    s_phys_n = 0;
    s_orphan = 0;
    s_myfn_calls = 0;
    s_is_mac = false;
    for (int i = 0; i < HIT_CAP; i++) s_hits[i] = 0;
    layer_state = 0;
    default_layer_state = 0;
    vim_glue_init(); /* kv_init + enable + INSERT */
}

static void fn_on(void) { layer_state = (1UL << 4); }
static void fn_off(void) { layer_state = 0; }

/* ======================================================================
 * A. held motion: NORMAL, hold h -> register host LEFT, release -> unregister
 * ====================================================================== */
static void test_falsify_held_motion(void) {
    reset_engine();
    kv_set_mode(KV_MODE_NORMAL);

    CHECK(feed(KC_H, true) == false); /* consumed by the engine */
    kv_emit_flush_now();              /* drain emit -> held motion registers */
    CHECK(reg_count(KC_LEFT) == 1);   /* register-hold, NOT a one-shot tap */

    CHECK(feed(KC_H, false) == false); /* release of a consumed press is paired */
    CHECK(reg_count(KC_LEFT) == 0);    /* host arrow unregistered */
    CHECK(s_orphan == 0);
}

/* ======================================================================
 * B. myfn modifier release must not get stuck
 * ====================================================================== */
static void test_falsify_shift_before_fn(void) {
    /* Shift down while Fn is OFF -> let through and registered by QMK. */
    reset_engine();
    CHECK(feed(KC_LSFT, true) == true);
    CHECK(reg_count(KC_LSFT) == 1);

    /* Fn goes down (Fn layer active) while Shift is still held. */
    fn_on();

    /* Shift up: must still reach the host (not swallowed by the myfn
     * undeclared-key branch). */
    if (feed(KC_LSFT, false) != true) {
        g_fail++;
        printf("FAIL %s:%d  Shift release after Fn taken was swallowed -> stuck Shift\n", __FILE__, __LINE__);
    } else {
        g_pass++;
    }
    CHECK(reg_count(KC_LSFT) == 0);
    CHECK(s_orphan == 0);
    fn_off();

    /* Mirror: modifier pressed while Fn is down is swallowed; its release
     * after Fn is released must be consumed (no orphan key-up). */
    reset_engine();
    fn_on();
    CHECK(feed(KC_LSFT, true) == false); /* myfn swallows undeclared modifier */
    CHECK(reg_count(KC_LSFT) == 0);
    fn_off();
    CHECK(feed(KC_LSFT, false) == false); /* paired release consumed */
    CHECK(s_orphan == 0);
}

/* ======================================================================
 * C. MOUSE: a modifier already held before entering MOUSE is unregistered
 * ====================================================================== */
static bool enter_mouse(void) {
    bool a = feed(QK_KB_22, true);
    bool b = feed(QK_KB_22, false);
    CHECK(a == false && b == false);
    CHECK(kv_get_mode() == KV_MODE_MOUSE);
    return a == false && b == false;
}

static void test_falsify_mouse_preheld_mod(void) {
    /* Shift held BEFORE entering MOUSE. */
    reset_engine();
    CHECK(feed(KC_LSFT, true) == true);
    CHECK(reg_count(KC_LSFT) == 1);

    enter_mouse();

    /* Releasing Shift inside MOUSE must reach the host. */
    if (feed(KC_LSFT, false) != true) {
        g_fail++;
        printf("FAIL %s:%d  Shift release inside MOUSE swallowed -> stuck Shift\n", __FILE__, __LINE__);
    } else {
        g_pass++;
    }
    CHECK(reg_count(KC_LSFT) == 0);
    CHECK(kv_get_mode() == KV_MODE_MOUSE);

    /* Variant: exit MOUSE first (via another key), then release Shift.
     * Shift is still held here, so use H (pointer-left always, not shifted
     * into a wheel). */
    reset_engine();
    CHECK(feed(KC_LSFT, true) == true);
    enter_mouse();
    CHECK(feed(KC_H, true) == false); /* pointer left */
    CHECK(reg_count(MS_LEFT) == 1);
    CHECK(feed(KC_A, true) == true);  /* other key exits MOUSE, re-identifies 'a' */
    CHECK(kv_get_mode() == KV_MODE_INSERT);
    CHECK(reg_count(MS_LEFT) == 0);
    CHECK(feed(KC_LSFT, false) == true); /* now released in INSERT */
    CHECK(reg_count(KC_LSFT) == 0);
    CHECK(s_orphan == 0);
}

/* ======================================================================
 * D. MOUSE: pointer held while another key exits -> no orphan release,
 *    no stuck pointer
 * ====================================================================== */
static void test_falsify_mouse_consume_exit(void) {
    reset_engine();
    enter_mouse();

    CHECK(feed(KC_H, true) == false); /* h = pointer left */
    CHECK(reg_count(MS_LEFT) == 1);

    /* Another key exits MOUSE and force-releases the pointer. */
    CHECK(feed(KC_A, true) == true);
    CHECK(kv_get_mode() == KV_MODE_INSERT);
    CHECK(reg_count(MS_LEFT) == 0); /* no stuck pointer */
    CHECK(feed(KC_A, false) == true);

    /* The held h is released *after* the mode change: it must be consumed by
     * the shared pairing table (no orphan h key-up). */
    CHECK(feed(KC_H, false) == false);
    CHECK(s_orphan == 0);

    /* Reverse: back in INSERT, h is a normal character, not a pointer. */
    CHECK(feed(KC_H, true) == true);
    CHECK(reg_count(MS_LEFT) == 0);
    CHECK(feed(KC_H, false) == true);
    CHECK(s_orphan == 0);
}

/* ======================================================================
 * E. CAD (Ctrl+Alt+BSPC): consume press+release with no orphan / stuck BSPC
 * ====================================================================== */
static bool cad_hook(uint16_t keycode, keyrecord_t *record) {
    if (keycode == KC_BSPC && record->event.pressed &&
        (get_mods() & MOD_BIT(KC_LCTL)) && (get_mods() & MOD_BIT(KC_LALT))) {
        vim_glue_swallow(KC_BSPC);
        tap_code(KC_DEL);
        return true;
    }
    return false;
}

static bool cad_pipeline(uint16_t kc, bool pressed) {
    keyrecord_t r = {0};
    r.event.pressed = pressed;
    vim_cfg_t cfg = g_cfg;
    cfg.hook_post_myfn = cad_hook;
    return vim_pipeline_process(kc, &r, &cfg);
}
static bool cad_feed(uint16_t kc, bool pressed) {
    bool pass = cad_pipeline(kc, pressed);
    if (pressed) { if (pass) host_press(kc); } else { if (pass) host_release(kc); }
    return pass;
}

static void test_falsify_cad(void) {
    /* (1) BSPC pressed plain, then Ctrl+Alt pressed, then BSPC released:
     * release predicate needs `pressed`, so the release must pass. */
    reset_engine();
    CHECK(cad_feed(KC_BSPC, true) == true);
    CHECK(reg_count(KC_BSPC) == 1);
    CHECK(cad_feed(KC_LCTL, true) == true);
    CHECK(cad_feed(KC_LALT, true) == true);
    if (cad_feed(KC_BSPC, false) != true) {
        g_fail++;
        printf("FAIL %s:%d  CAD: BSPC release swallowed while Ctrl+Alt held -> stuck Backspace\n",
               __FILE__, __LINE__);
    } else {
        g_pass++;
    }
    CHECK(reg_count(KC_BSPC) == 0);
    cad_feed(KC_LCTL, false);
    cad_feed(KC_LALT, false);
    CHECK(s_orphan == 0);

    /* (2) Ctrl+Alt held, BSPC press consumed; Ctrl/Alt released before BSPC:
     * the BSPC release must still be consumed (no orphan key-up). */
    reset_engine();
    CHECK(cad_feed(KC_LCTL, true) == true);
    CHECK(cad_feed(KC_LALT, true) == true);
    CHECK(cad_feed(KC_BSPC, true) == false); /* consumed by CAD */
    CHECK(reg_count(KC_BSPC) == 0);
    cad_feed(KC_LCTL, false);
    cad_feed(KC_LALT, false);
    if (cad_feed(KC_BSPC, false) != false) {
        g_fail++;
        printf("FAIL %s:%d  CAD: consumed BSPC press leaked an orphan release\n", __FILE__, __LINE__);
    } else {
        g_pass++;
    }
    CHECK(s_orphan == 0);
}

/* ======================================================================
 * F. Caps long press returns to the exact entry mode (Visual -> Visual)
 * ====================================================================== */
static void test_falsify_caps(void) {
    /* Insert -> Normal short press, stable. */
    reset_engine();
    CHECK(feed(KC_CAPS, true) == false);
    CHECK(kv_get_mode() == KV_MODE_NORMAL);
    CHECK(feed(KC_CAPS, false) == false);
    CHECK(kv_get_mode() == KV_MODE_NORMAL);

    /* Normal -> Insert short press, stable. */
    CHECK(feed(KC_CAPS, true) == false);
    CHECK(feed(KC_CAPS, false) == false);
    CHECK(kv_get_mode() == KV_MODE_INSERT);

    /* Insert -> Normal again. */
    CHECK(feed(KC_CAPS, true) == false);
    CHECK(feed(KC_CAPS, false) == false);
    CHECK(kv_get_mode() == KV_MODE_NORMAL);
    reset_engine(); /* leave the suite in a clean INSERT state */

    /* Long press from Visual -> momentary Normal -> release returns to Visual. */
    reset_engine();
    kv_set_mode(KV_MODE_VISUAL);
    g_now = 2000;
    CHECK(feed(KC_CAPS, true) == false);
    CHECK(kv_get_mode() == KV_MODE_NORMAL);
    g_now += 250;
    CHECK(feed(KC_CAPS, false) == false);
    if (kv_get_mode() != KV_MODE_VISUAL) {
        g_fail++;
        printf("FAIL %s:%d  Caps long from VISUAL: expected VISUAL, got mode=%d\n",
               __FILE__, __LINE__, (int)kv_get_mode());
    } else {
        g_pass++;
    }
}

/* ======================================================================
 * F2. Probe: the non-Fn Caps press is consumed by caps_process() without being
 *     recorded in the shared pairing table.  Design §4.10 requires every
 *     keymap-layer consumed press to be paired so its release is owned
 *     unconditionally.  If Fn goes down while Caps is held, the release is
 *     routed to the Fn+Caps branch (which relies on the pairing table), so a
 *     missing pair leaks an orphan Caps key-up.
 * ====================================================================== */
static void test_falsify_caps_normal_press_pairing(void) {
    reset_engine(); /* INSERT */
    CHECK(feed(KC_CAPS, true) == false); /* consumed by the non-Fn branch */
    CHECK(kv_get_mode() == KV_MODE_NORMAL);
    fn_on(); /* Fn pressed while Caps is still held */
    bool pass = feed(KC_CAPS, false);
    if (pass != false) {
        g_fail++;
        printf("FAIL %s:%d  Caps pressed without Fn, Fn pressed before release: "
               "orphan Caps key-up leaked (pipeline pass=%d, host orphan=%d)\n",
               __FILE__, __LINE__, (int)pass, s_orphan);
    } else {
        g_pass++;
    }
    CHECK(s_orphan == 0);
    fn_off();
}

/* ======================================================================
 * G. Shift+Esc must not be hijacked while vim is disabled
 * ====================================================================== */
static void test_falsify_shift_esc_vim_off(void) {
    reset_engine();
    kv_disable();
    CHECK(feed(KC_LSFT, true) == true);
    if (feed(KC_ESC, true) != true) {
        g_fail++;
        printf("FAIL %s:%d  Shift+Esc with vim OFF: press consumed (expected pass)\n", __FILE__, __LINE__);
    } else {
        g_pass++;
    }
    CHECK(feed(KC_ESC, false) == true);
    CHECK(feed(KC_LSFT, false) == true);
    CHECK(s_orphan == 0);
}

/* ======================================================================
 * H. Fn+Caps: press swallowed and paired; release swallowed via the pairing
 *    table even if Fn is released first
 * ====================================================================== */
static void test_falsify_fn_caps(void) {
    reset_engine();
    fn_on();
    CHECK(kv_vim_enabled() == true);

    /* Caps released AFTER Fn: both edges consumed. */
    CHECK(feed(KC_CAPS, true) == false);
    CHECK(kv_vim_enabled() == false);
    CHECK(feed(KC_CAPS, false) == false); /* Fn still down; step 8 pair */
    CHECK(s_orphan == 0);
    fn_off();

    /* Now turn vim back on, but release Fn BEFORE Caps. */
    fn_on();
    CHECK(feed(KC_CAPS, true) == false);
    CHECK(kv_vim_enabled() == true);
    fn_off(); /* Fn released first */
    if (feed(KC_CAPS, false) != false) {
        g_fail++;
        printf("FAIL %s:%d  Fn+Caps: paired release leaked after Fn was released first\n",
               __FILE__, __LINE__);
    } else {
        g_pass++;
    }
    CHECK(s_orphan == 0);
}

/* ======================================================================
 * I. held-motion overreach (P1-1 expected-slot regression).
 *
 * A bare h/j/k/l may register-hold its host arrow, but an arrow emitted by an
 * unrelated key while that motion is held (e.g. `a` -> RIGHT) must be a normal
 * tap.  Counts with a pending operator/count (`3l`, `dl`) must also tap.
 * ====================================================================== */
static void test_falsify_held_overreach(void) {
    /* bare l hold: exactly one persistent RIGHT registration */
    reset_engine();
    kv_set_mode(KV_MODE_NORMAL);
    CHECK(feed(KC_L, true) == false);
    kv_emit_flush_now();
    CHECK(reg_count(KC_RGHT) == 1);

    /* while l is held, `a` emits RIGHT but must TAP (not reuse/extend the
     * hold): net registration unchanged, and a real register did happen. */
    int before = reg_count(KC_RGHT);
    int hits_before = s_hits[KC_RGHT];
    CHECK(feed(KC_A, true) == false); /* consumed: -> then Insert */
    kv_emit_flush_now();
    CHECK(reg_count(KC_RGHT) == before);          /* no persistent 2nd hold */
    CHECK(s_hits[KC_RGHT] == hits_before + 1);    /* the tap was not dropped */
    CHECK(kv_get_mode() == KV_MODE_INSERT);
    CHECK(feed(KC_A, false) == false);            /* paired release */
    CHECK(reg_count(KC_RGHT) == 1);               /* l's hold is untouched */

    /* release l -> arrow unregistered */
    CHECK(feed(KC_L, false) == false);
    CHECK(reg_count(KC_RGHT) == 0);
    CHECK(s_orphan == 0);

    /* 3l must tap, not hold */
    reset_engine();
    kv_set_mode(KV_MODE_NORMAL);
    CHECK(feed(KC_3, true) == false);
    CHECK(feed(KC_L, true) == false);
    kv_emit_flush_now();
    CHECK(reg_count(KC_RGHT) == 0);
    CHECK(s_hits[KC_RGHT] == 3);                  /* three real taps, not a hold */
    CHECK(feed(KC_L, false) == false);
    CHECK(feed(KC_3, false) == false);
    CHECK(s_orphan == 0);

    /* dl must tap, not hold */
    reset_engine();
    kv_set_mode(KV_MODE_NORMAL);
    CHECK(feed(KC_D, true) == false);
    CHECK(feed(KC_L, true) == false);
    kv_emit_flush_now();
    CHECK(reg_count(KC_RGHT) == 0);
    CHECK(s_hits[KC_RGHT] == 1);                  /* one real tap, not a hold */
    CHECK(feed(KC_L, false) == false);
    CHECK(feed(KC_D, false) == false);
    CHECK(s_orphan == 0);
}

/* ======================================================================
 * J. Caps / Fn release ordering must not strand a Caps pair entry or leak an
 *    orphan Caps key-up, and a following normal Caps edge pair stays paired.
 * ====================================================================== */
/* Detects a leftover CAPS entry in the shared pairing table: with vim OFF a
 * Caps press/release must pass through both edges.  A stale pair entry would
 * swallow the release (orphan key-up / stuck Caps). */
static void check_caps_table_clean(void) {
    kv_disable();
    if (feed(KC_CAPS, true) != true) {
        g_fail++;
        printf("FAIL %s:%d  residual Caps state: pass-through press was consumed\n",
               __FILE__, __LINE__);
    } else {
        g_pass++;
    }
    if (feed(KC_CAPS, false) != true) {
        g_fail++;
        printf("FAIL %s:%d  residual Caps pair: pass-through release swallowed -> stuck Caps\n",
               __FILE__, __LINE__);
    } else {
        g_pass++;
    }
    CHECK(s_orphan == 0);
    CHECK(reg_count(KC_CAPS) == 0); /* host Caps released */
}

static void test_falsify_caps_fn_ordering(void) {
    /* (1) Caps press -> Fn down -> Fn up -> Caps release */
    reset_engine(); /* INSERT, vim on */
    CHECK(feed(KC_CAPS, true) == false);
    CHECK(kv_get_mode() == KV_MODE_NORMAL);
    fn_on();
    fn_off(); /* Fn released first */
    CHECK(feed(KC_CAPS, false) == false); /* paired release consumed */
    CHECK(s_orphan == 0);
    check_caps_table_clean();

    /* (2) Caps press -> Fn down -> Caps release (Fn still held) -> Fn up */
    reset_engine();
    CHECK(feed(KC_CAPS, true) == false);
    CHECK(kv_get_mode() == KV_MODE_NORMAL);
    fn_on();
    CHECK(feed(KC_CAPS, false) == false); /* paired release consumed */
    CHECK(s_orphan == 0);
    fn_off();
    check_caps_table_clean();

    /* (3) same as (2) but long-pressed across Fn: release must still pair. */
    reset_engine();
    g_now = 1000;
    CHECK(feed(KC_CAPS, true) == false);
    fn_on();
    g_now += 250; /* exceed hold_ms */
    CHECK(feed(KC_CAPS, false) == false);
    CHECK(kv_get_mode() == KV_MODE_INSERT); /* long press restores entry mode */
    CHECK(s_orphan == 0);
    fn_off();
    check_caps_table_clean();

    /* A following normal Caps press/release must stay paired (no stuck Caps):
     * Insert -> Normal short press stays Normal; the next one returns to Insert. */
    reset_engine();
    CHECK(feed(KC_CAPS, true) == false);
    CHECK(kv_get_mode() == KV_MODE_NORMAL);
    CHECK(feed(KC_CAPS, false) == false);
    CHECK(kv_get_mode() == KV_MODE_NORMAL);
    CHECK(feed(KC_CAPS, true) == false);
    CHECK(feed(KC_CAPS, false) == false);
    CHECK(kv_get_mode() == KV_MODE_INSERT);
    CHECK(reg_count(KC_CAPS) == 0);
    CHECK(s_orphan == 0);

    /* Repeated Fn+Caps enable/disable cycles must always pair the Caps edges;
     * afterwards a normal (vim-off) Caps press/release passes through. */
    reset_engine();
    for (int i = 0; i < 3; i++) {
        fn_on();
        CHECK(feed(KC_CAPS, true) == false);
        CHECK(kv_vim_enabled() == (i % 2 != 0)); /* 1 -> off -> on -> off */
        fn_off();
        CHECK(feed(KC_CAPS, false) == false); /* paired release */
        CHECK(s_orphan == 0);
    }
    CHECK(kv_vim_enabled() == false);
    check_caps_table_clean();
}

/* ======================================================================
 * K. Mouse trigger long-press (P2): if is_mac() flips while the trigger is
 *    held, the release must unregister the exact modifier that was registered.
 * ====================================================================== */
static void test_falsify_mouse_trigger_mod_switch(void) {
    vim_cfg_t cfg = g_cfg;
    cfg.is_mac = test_is_mac;

    /* Start on Win (RALT); flip to Mac before release. */
    reset_engine();
    s_is_mac = false;
    g_now = 3000;
    CHECK(feed_cfg(QK_KB_22, true, &cfg) == false);
    g_now += 250;
    vim_keymap_common_task(g_now);
    CHECK(reg_count(KC_RALT) == 1);
    CHECK(reg_count(KC_RGUI) == 0);
    s_is_mac = true; /* platform flips mid-hold */
    CHECK(feed_cfg(QK_KB_22, false, &cfg) == false);
    CHECK(reg_count(KC_RALT) == 0); /* the actually registered one is removed */
    CHECK(reg_count(KC_RGUI) == 0);
    CHECK(kv_get_mode() == KV_MODE_INSERT);
    CHECK(s_orphan == 0);

    /* Start on Mac (RGUI); flip to Win before release. */
    reset_engine();
    s_is_mac = true;
    g_now = 4000;
    CHECK(feed_cfg(QK_KB_22, true, &cfg) == false);
    g_now += 250;
    vim_keymap_common_task(g_now);
    CHECK(reg_count(KC_RGUI) == 1);
    CHECK(reg_count(KC_RALT) == 0);
    s_is_mac = false;
    CHECK(feed_cfg(QK_KB_22, false, &cfg) == false);
    CHECK(reg_count(KC_RGUI) == 0);
    CHECK(reg_count(KC_RALT) == 0);
    CHECK(kv_get_mode() == KV_MODE_INSERT);
    CHECK(s_orphan == 0);

    /* Long-press the trigger *while already in MOUSE*: the platform flips and
     * the release must unregister the actually registered modifier, and a long
     * press must never exit MOUSE (only the following tap does). */
    reset_engine();
    s_is_mac = false;
    g_now = 5000;
    CHECK(feed_cfg(QK_KB_22, true, &cfg) == false);
    CHECK(feed_cfg(QK_KB_22, false, &cfg) == false);
    CHECK(kv_get_mode() == KV_MODE_MOUSE);
    s_is_mac = true;
    g_now = 6000;
    CHECK(feed_cfg(QK_KB_22, true, &cfg) == false);
    g_now += 250;
    vim_keymap_common_task(g_now);
    CHECK(reg_count(KC_RGUI) == 1);
    CHECK(reg_count(KC_RALT) == 0);
    s_is_mac = false; /* flip before release */
    CHECK(feed_cfg(QK_KB_22, false, &cfg) == false);
    CHECK(reg_count(KC_RGUI) == 0);
    CHECK(reg_count(KC_RALT) == 0);
    CHECK(kv_get_mode() == KV_MODE_MOUSE); /* long press is not a tap */
    CHECK(feed_cfg(QK_KB_22, true, &cfg) == false);
    CHECK(feed_cfg(QK_KB_22, false, &cfg) == false);
    CHECK(kv_get_mode() == KV_MODE_INSERT); /* tap exits back to entry mode */
    CHECK(s_orphan == 0);
}

/* ======================================================================
 * L. held motion on every axis + mode switch / disable during a hold.
 * ====================================================================== */
static void test_falsify_held_axes_and_mode(void) {
    /* bare j -> DOWN */
    reset_engine();
    kv_set_mode(KV_MODE_NORMAL);
    CHECK(feed(KC_J, true) == false);
    kv_emit_flush_now();
    CHECK(reg_count(KC_DOWN) == 1);
    CHECK(feed(KC_J, false) == false);
    CHECK(reg_count(KC_DOWN) == 0);
    CHECK(s_orphan == 0);

    /* bare k -> UP */
    reset_engine();
    kv_set_mode(KV_MODE_NORMAL);
    CHECK(feed(KC_K, true) == false);
    kv_emit_flush_now();
    CHECK(reg_count(KC_UP) == 1);
    CHECK(feed(KC_K, false) == false);
    CHECK(reg_count(KC_UP) == 0);
    CHECK(s_orphan == 0);

    /* diagonal h + j held simultaneously: independent axis registrations. */
    reset_engine();
    kv_set_mode(KV_MODE_NORMAL);
    CHECK(feed(KC_H, true) == false);
    kv_emit_flush_now();
    CHECK(reg_count(KC_LEFT) == 1);
    CHECK(feed(KC_J, true) == false);
    kv_emit_flush_now();
    CHECK(reg_count(KC_DOWN) == 1);
    CHECK(reg_count(KC_LEFT) == 1);
    CHECK(feed(KC_H, false) == false);
    CHECK(reg_count(KC_LEFT) == 0);
    CHECK(reg_count(KC_DOWN) == 1);
    CHECK(feed(KC_J, false) == false);
    CHECK(reg_count(KC_DOWN) == 0);
    CHECK(s_orphan == 0);

    /* 2l taps (counted motion) */
    reset_engine();
    kv_set_mode(KV_MODE_NORMAL);
    CHECK(feed(KC_2, true) == false);
    CHECK(feed(KC_L, true) == false);
    kv_emit_flush_now();
    CHECK(reg_count(KC_RGHT) == 0);
    CHECK(feed(KC_L, false) == false);
    CHECK(feed(KC_2, false) == false);
    CHECK(s_orphan == 0);

    /* held l, then a direct engine mode switch: release must not stick. */
    reset_engine();
    kv_set_mode(KV_MODE_NORMAL);
    CHECK(feed(KC_L, true) == false);
    kv_emit_flush_now();
    CHECK(reg_count(KC_RGHT) == 1);
    kv_set_mode(KV_MODE_INSERT);
    CHECK(feed(KC_L, false) == false);
    CHECK(reg_count(KC_RGHT) == 0);
    CHECK(s_orphan == 0);

    /* held l, then kv_disable(): release must not stick. */
    reset_engine();
    kv_set_mode(KV_MODE_NORMAL);
    CHECK(feed(KC_L, true) == false);
    kv_emit_flush_now();
    CHECK(reg_count(KC_RGHT) == 1);
    kv_disable();
    CHECK(feed(KC_L, false) == false);
    CHECK(reg_count(KC_RGHT) == 0);
    CHECK(s_orphan == 0);

    /* held l, then Fn+Caps toggles vim off: the real disable path force-
     * releases immediately and still consumes the physical release. */
    reset_engine();
    kv_set_mode(KV_MODE_NORMAL);
    CHECK(feed(KC_L, true) == false);
    kv_emit_flush_now();
    CHECK(reg_count(KC_RGHT) == 1);
    fn_on();
    CHECK(feed(KC_CAPS, true) == false);
    CHECK(kv_vim_enabled() == false);
    CHECK(reg_count(KC_RGHT) == 0); /* released by set_vim_enabled -> release_all */
    fn_off();
    CHECK(feed(KC_CAPS, false) == false);
    CHECK(feed(KC_L, false) == false);
    CHECK(reg_count(KC_RGHT) == 0);
    CHECK(s_orphan == 0);
}

int main(void) {
    test_falsify_held_motion();
    test_falsify_shift_before_fn();
    test_falsify_mouse_preheld_mod();
    test_falsify_mouse_consume_exit();
    test_falsify_cad();
    test_falsify_caps();
    test_falsify_caps_normal_press_pairing();
    test_falsify_shift_esc_vim_off();
    test_falsify_fn_caps();
    test_falsify_held_overreach();
    test_falsify_caps_fn_ordering();
    test_falsify_mouse_trigger_mod_switch();
    test_falsify_held_axes_and_mode();
    printf("glue-falsify: pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
