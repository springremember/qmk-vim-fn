/* test_modifiers.c — host tests for the modifier / shortcut / emit-modifier
 * surface of the shared QMK keymap layer (vim_glue.c + vim_keymap_common.c).
 *
 * Test-only.  The harness is derived from test/glue/test_glue_falsify.c and
 * extended (as required for modifier coverage) with:
 *   - register_mods()/unregister_mods() call logs   (vim_emit add/drop),
 *   - a chronological register_code() click log carrying the get_mods()
 *     snapshot at the moment of the register (the "tap" modifier state),
 *   - full 16-bit tap_code16() capture (low byte + modifier cap bits),
 *   - the get_mods() snapshot taken while a send_plain_tap() tap is in flight
 *     (to prove the physical modifiers are stripped for the tap and restored).
 *
 * Design authority: vim/design.md §2.1 (keyboard-layer shortcut table),
 * §4.7 (mode/enable clear), §4.10 (modifiers / key-up), §4.12 (glue duties);
 * vim/testcase.md §10; vim/readme.md §9.
 *
 * SCOPE OF THE RIGHT-SIDE MODIFIER BRANCH: command.c only ever emits left-side
 * modifiers (KV_MOD_LCTL / KV_MOD_LSFT via KV_LCTL_KC / KV_LSFT_KC / KV_CS),
 * so the right-side arm of packed_mods_to_hid() (m & 0x10 -> RCTL/RSHIFT/
 * RALT/RGUI) cannot be reached through a *physical* key press.  It is not dead
 * code, though: the emit callback is a white-box seam and §4.12 #5 defines it
 * as a pure keycode -> HID decoder, so test I injects right-side packed values
 * directly with kv_emit_tap()/kv_emit_flush_now() to pin the decoder down
 * (right-side flag, its 0x01/0x02/0x04/0x08 cases, the switch `default:` arm,
 * and the left LALT/LGUI arms).
 */
#include "qmk_stub.h"
#include "emit.h" /* kv_emit_flush_now() */
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

/* register_mods()/unregister_mods() call logs — only vim_emit() uses them
 * (send_plain_tap uses clear_mods/set_mods), so these capture the add/drop
 * quadrants exactly. */
#define MODLOG_CAP 128
static uint8_t s_add[MODLOG_CAP];
static int     s_add_n;
static uint8_t s_del[MODLOG_CAP];
static int     s_del_n;

void register_mods(uint8_t m) {
    s_mods |= m;
    if (s_add_n < MODLOG_CAP) s_add[s_add_n++] = m;
}
void unregister_mods(uint8_t m) {
    s_mods &= (uint8_t)~m;
    if (s_del_n < MODLOG_CAP) s_del[s_del_n++] = m;
}

#define REG_CAP 64
static uint16_t s_reg[REG_CAP];
static int      s_reg_n;

/* Chronological register_code() log with the get_mods() snapshot at entry. */
#define CLICK_CAP 512
static uint16_t s_click[CLICK_CAP];
static uint8_t  s_click_mods[CLICK_CAP];
static int      s_click_n;

/* Cumulative register_code() calls per keycode (< 256): a tap is register+
 * unregister, leaving reg_count() unchanged; this counter distinguishes a real
 * tap from a silently dropped emit. */
#define HIT_CAP 256
static int s_hits[HIT_CAP];

/* Physical keys the pipeline let through to the host.  An orphan key-up is a
 * physical release let through for a key whose press was consumed. */
static uint16_t s_phys[REG_CAP];
static int      s_phys_n;
static int      s_orphan;

void register_code(uint16_t kc) {
    if (s_click_n < CLICK_CAP) {
        s_click[s_click_n]      = kc;
        s_click_mods[s_click_n] = s_mods;
        s_click_n++;
    }
    if (IS_MODIFIER_KEYCODE(kc)) s_mods |= (uint8_t)(1u << (kc - KC_LCTL));
    if (s_reg_n < REG_CAP) s_reg[s_reg_n++] = kc;
    if (kc < HIT_CAP) s_hits[kc]++;
}
void unregister_code(uint16_t kc) {
    if (IS_MODIFIER_KEYCODE(kc)) s_mods &= (uint8_t)~(1u << (kc - KC_LCTL));
    for (int i = 0; i < s_reg_n; i++) {
        if (s_reg[i] == kc) { s_reg[i] = s_reg[--s_reg_n]; return; }
    }
}
void tap_code(uint16_t kc) { register_code(kc); unregister_code(kc); }

/* Full 16-bit capture: the low byte is tapped, but the raw argument (including
 * the modifier cap bits) and the in-flight get_mods() are recorded. */
#define TAP16_CAP 64
static uint16_t s_tap16[TAP16_CAP];
static uint8_t  s_tap16_mods[TAP16_CAP];
static int      s_tap16_n;
void tap_code16(uint16_t kc) {
    if (s_tap16_n < TAP16_CAP) {
        s_tap16[s_tap16_n]      = kc;
        s_tap16_mods[s_tap16_n] = get_mods();
        s_tap16_n++;
    }
    tap_code((uint16_t)(kc & 0xFF));
}

static uint16_t g_now;
uint16_t timer_read(void) { return g_now; }
uint16_t timer_elapsed(uint16_t since) { return (uint16_t)(g_now - since); }

/* ---------------- host emulation helpers ---------------- */
static int reg_count(uint16_t kc) {
    int n = 0;
    for (int i = 0; i < s_reg_n; i++) if (s_reg[i] == kc) n++;
    return n;
}
static bool log_has(const uint8_t *a, int n, uint8_t v) {
    for (int i = 0; i < n; i++) if (a[i] == v) return true;
    return false;
}
/* modifier snapshot at the first register_code(kc); -1 if never registered */
static int click_mods_of(uint16_t kc) {
    for (int i = 0; i < s_click_n; i++) if (s_click[i] == kc) return s_click_mods[i];
    return -1;
}
static bool tap16_has(uint16_t kc) {
    for (int i = 0; i < s_tap16_n; i++) if (s_tap16[i] == kc) return true;
    return false;
}
static int tap16_mods_of(uint16_t kc) {
    for (int i = 0; i < s_tap16_n; i++) if (s_tap16[i] == kc) return s_tap16_mods[i];
    return -1;
}
/* modifier snapshot at the first register_code(kc) that is >= after_index */
static int click_mods_after(uint16_t kc, int from) {
    for (int i = from; i < s_click_n; i++) if (s_click[i] == kc) return s_click_mods[i];
    return -1;
}

static void host_press(uint16_t kc) { register_code(kc); }
static void host_release(uint16_t kc) { unregister_code(kc); }

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
static int  s_myfn_calls;
static bool test_myfn(uint16_t kc, bool pressed) {
    if (kc == KC_SPC) {
        if (pressed) s_myfn_calls++;
        return true;
    }
    return false;
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

static bool pipeline_cfg(uint16_t kc, bool pressed, const vim_cfg_t *cfg) {
    keyrecord_t r = {0};
    r.event.pressed = pressed;
    return vim_pipeline_process(kc, &r, cfg);
}
static bool feed_cfg(uint16_t kc, bool pressed, const vim_cfg_t *cfg) {
    bool pass = pipeline_cfg(kc, pressed, cfg);
    if (pressed) {
        if (pass) { host_press(kc); if (s_phys_n < REG_CAP) s_phys[s_phys_n++] = kc; }
    } else {
        if (pass) {
            bool known = false;
            for (int i = 0; i < s_phys_n; i++)
                if (s_phys[i] == kc) { s_phys[i] = s_phys[--s_phys_n]; known = true; break; }
            if (!known) s_orphan++;
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
    s_add_n = 0;
    s_del_n = 0;
    s_tap16_n = 0;
    s_click_n = 0;
    s_myfn_calls = 0;
    for (int i = 0; i < HIT_CAP; i++) s_hits[i] = 0;
    layer_state = 0;
    default_layer_state = 0;
    vim_glue_init(); /* kv_init + enable + INSERT */
}

static void fn_on(void) { layer_state = (1UL << 4); }
static void fn_off(void) { layer_state = 0; }

/* ======================================================================
 * A. shadow_bit_of: all 8 physical modifiers (press/release/idempotent) and
 *    the non-modifier no-op branch.
 * ====================================================================== */
static void test_shadow_bits(void) {
    static const struct { uint16_t kc; uint8_t bit; } kMods[] = {
        {KC_LCTL, MOD_BIT_LCTRL}, {KC_LSFT, MOD_BIT_LSHIFT},
        {KC_LALT, MOD_BIT_LALT}, {KC_LGUI, MOD_BIT_LGUI},
        {KC_RCTL, MOD_BIT_RCTRL}, {KC_RSFT, MOD_BIT_RSHIFT},
        {KC_RALT, MOD_BIT_RALT}, {KC_RGUI, MOD_BIT_RGUI},
    };
    for (unsigned i = 0; i < sizeof(kMods) / sizeof(kMods[0]); i++) {
        reset_engine();
        vim_glue_mod_update(kMods[i].kc, true);
        CHECK(vim_glue_mods() == kMods[i].bit);   /* exactly that bit */
        vim_glue_mod_update(kMods[i].kc, true);   /* idempotent press */
        CHECK(vim_glue_mods() == kMods[i].bit);
        vim_glue_mod_update(kMods[i].kc, false);  /* release clears it */
        CHECK(vim_glue_mods() == 0);
        vim_glue_mod_update(kMods[i].kc, false);  /* already clear */
        CHECK(vim_glue_mods() == 0);
    }

    /* combination + release of one side only */
    reset_engine();
    vim_glue_mod_update(KC_LCTL, true);
    vim_glue_mod_update(KC_RCTL, true);
    CHECK(vim_glue_mods() == (MOD_BIT_LCTRL | MOD_BIT_RCTRL));
    vim_glue_mod_update(KC_RCTL, false);
    CHECK(vim_glue_mods() == MOD_BIT_LCTRL);
    vim_glue_mod_update(KC_LCTL, false);
    CHECK(vim_glue_mods() == 0);

    /* non-modifier keys never touch the shadow (both edges) */
    reset_engine();
    vim_glue_mod_update(KC_LSFT, true);
    vim_glue_mod_update(KC_A, true);
    CHECK(vim_glue_mods() == MOD_BIT_LSHIFT);
    vim_glue_mod_update(KC_A, false);
    CHECK(vim_glue_mods() == MOD_BIT_LSHIFT);
    vim_glue_mod_update(KC_LSFT, false);
    CHECK(vim_glue_mods() == 0);
}

/* ======================================================================
 * B. §2.1 shortcut table — 7 entries, positive and negative.
 * ====================================================================== */
static void test_shortcut_nomod(void) {
    /* BSPC -> LEFT */
    reset_engine();
    kv_set_mode(KV_MODE_NORMAL);
    CHECK(feed(KC_BSPC, true) == false);
    CHECK(s_hits[KC_LEFT] == 1);
    CHECK(feed(KC_BSPC, false) == false); /* swallowed release (paired) */
    CHECK(s_orphan == 0);
    CHECK(get_mods() == 0);

    /* SPC -> RIGHT */
    reset_engine();
    kv_set_mode(KV_MODE_NORMAL);
    CHECK(feed(KC_SPC, true) == false);
    CHECK(s_hits[KC_RGHT] == 1);
    CHECK(feed(KC_SPC, false) == false);

    /* MINS -> UP + HOME */
    reset_engine();
    kv_set_mode(KV_MODE_NORMAL);
    CHECK(feed(KC_MINS, true) == false);
    CHECK(s_hits[KC_UP] == 1);
    CHECK(s_hits[KC_HOME] == 1);
    CHECK(feed(KC_MINS, false) == false);

    /* SLSH -> Ctrl+F (modifier cap carried as a full 16-bit tap_code16 arg) */
    reset_engine();
    kv_set_mode(KV_MODE_NORMAL);
    CHECK(feed(KC_SLSH, true) == false);
    CHECK(s_hits[KC_F] == 1);
    CHECK(tap16_has(LCTL(KC_F)));          /* 0x0109, full 16-bit */
    CHECK(tap16_mods_of(LCTL(KC_F)) == 0); /* physical mods stripped in-flight */
    CHECK(feed(KC_SLSH, false) == false);

    /* negative path: a bare release never triggers the table */
    reset_engine();
    kv_set_mode(KV_MODE_NORMAL);
    CHECK(feed(KC_BSPC, false) == true);
    CHECK(s_hits[KC_LEFT] == 0);
}

static void test_shortcut_shift_eql(void) {
    /* LSFT+EQL -> DOWN + HOME */
    reset_engine();
    kv_set_mode(KV_MODE_NORMAL);
    CHECK(feed(KC_LSFT, true) == true);
    CHECK(feed(KC_EQL, true) == false);
    CHECK(s_hits[KC_DOWN] == 1);
    CHECK(s_hits[KC_HOME] == 1);
    /* G9: Shift is stripped for the whole tap (send_plain_tap clears mods), so
     * the in-flight get_mods() snapshot at each tap_code16 is 0. */
    CHECK(tap16_mods_of(KC_DOWN) == 0);
    CHECK(tap16_mods_of(KC_HOME) == 0);
    CHECK(feed(KC_EQL, false) == false);
    CHECK(feed(KC_LSFT, false) == true);
    CHECK(s_orphan == 0);

    /* RSFT+EQL -> DOWN + HOME (side-agnostic) */
    reset_engine();
    kv_set_mode(KV_MODE_NORMAL);
    CHECK(feed(KC_RSFT, true) == true);
    CHECK(feed(KC_EQL, true) == false);
    CHECK(s_hits[KC_DOWN] == 1);
    CHECK(s_hits[KC_HOME] == 1);
    CHECK(tap16_mods_of(KC_DOWN) == 0);
    CHECK(tap16_mods_of(KC_HOME) == 0);
    CHECK(feed(KC_EQL, false) == false);
    CHECK(feed(KC_RSFT, false) == true);

    /* LSFT+RSFT+EQL -> still a hit */
    reset_engine();
    kv_set_mode(KV_MODE_NORMAL);
    CHECK(feed(KC_LSFT, true) == true);
    CHECK(feed(KC_RSFT, true) == true);
    CHECK(feed(KC_EQL, true) == false);
    CHECK(s_hits[KC_DOWN] == 1);
    CHECK(s_hits[KC_HOME] == 1);
    CHECK(tap16_mods_of(KC_DOWN) == 0);
    CHECK(tap16_mods_of(KC_HOME) == 0);
    CHECK(feed(KC_EQL, false) == false);
    CHECK(feed(KC_RSFT, false) == true);
    CHECK(feed(KC_LSFT, false) == true);

    /* bare EQL must NOT hit (mods_req = SHIFT) and passes as a non-vim key */
    reset_engine();
    kv_set_mode(KV_MODE_NORMAL);
    CHECK(feed(KC_EQL, true) == true);
    CHECK(s_hits[KC_DOWN] == 0);
    CHECK(feed(KC_EQL, false) == true);
}

static void test_shortcut_ctrl_fb(void) {
    /* LCTL+F -> PGDN */
    reset_engine();
    kv_set_mode(KV_MODE_NORMAL);
    CHECK(feed(KC_LCTL, true) == true);
    CHECK(feed(KC_F, true) == false);
    CHECK(s_hits[KC_PGDN] == 1);
    /* G9: the Ctrl is stripped for the tap (send_plain_tap clears mods) */
    CHECK(tap16_mods_of(KC_PGDN) == 0);
    CHECK(feed(KC_F, false) == false);
    CHECK(feed(KC_LCTL, false) == true);

    /* RCTL+F -> PGDN */
    reset_engine();
    kv_set_mode(KV_MODE_NORMAL);
    CHECK(feed(KC_RCTL, true) == true);
    CHECK(feed(KC_F, true) == false);
    CHECK(s_hits[KC_PGDN] == 1);
    CHECK(tap16_mods_of(KC_PGDN) == 0);
    CHECK(feed(KC_F, false) == false);
    CHECK(feed(KC_RCTL, false) == true);

    /* LCTL+RCTL+F -> PGDN */
    reset_engine();
    kv_set_mode(KV_MODE_NORMAL);
    CHECK(feed(KC_LCTL, true) == true);
    CHECK(feed(KC_RCTL, true) == true);
    CHECK(feed(KC_F, true) == false);
    CHECK(s_hits[KC_PGDN] == 1);
    CHECK(tap16_mods_of(KC_PGDN) == 0);
    CHECK(feed(KC_F, false) == false);
    CHECK(feed(KC_RCTL, false) == true);
    CHECK(feed(KC_LCTL, false) == true);

    /* LCTL+B -> PGUP */
    reset_engine();
    kv_set_mode(KV_MODE_NORMAL);
    CHECK(feed(KC_LCTL, true) == true);
    CHECK(feed(KC_B, true) == false);
    CHECK(s_hits[KC_PGUP] == 1);
    CHECK(tap16_mods_of(KC_PGUP) == 0); /* G9: Ctrl stripped for the tap */
    CHECK(feed(KC_B, false) == false);
    CHECK(feed(KC_LCTL, false) == true);

    /* RCTL+B -> PGUP */
    reset_engine();
    kv_set_mode(KV_MODE_NORMAL);
    CHECK(feed(KC_RCTL, true) == true);
    CHECK(feed(KC_B, true) == false);
    CHECK(s_hits[KC_PGUP] == 1);
    CHECK(tap16_mods_of(KC_PGUP) == 0);
    CHECK(feed(KC_B, false) == false);
    CHECK(feed(KC_RCTL, false) == true);

    /* bare F -> no hit; passes through as a non-vim key */
    reset_engine();
    kv_set_mode(KV_MODE_NORMAL);
    CHECK(feed(KC_F, true) == true);
    CHECK(s_hits[KC_PGDN] == 0);
    CHECK(feed(KC_F, false) == true);

    /* bare B -> no hit; the engine eats it as the plain `b` motion */
    reset_engine();
    kv_set_mode(KV_MODE_NORMAL);
    CHECK(feed(KC_B, true) == false);
    kv_emit_flush_now();
    CHECK(s_hits[KC_PGUP] == 0);
    CHECK(s_hits[KC_LEFT] == 1); /* Ctrl+Left from the motion, not the shortcut */
    CHECK(feed(KC_B, false) == false);
}

static void test_shortcut_extra_mods(void) {
    /* LALT+BSPC: ALT is inside BSPC's NO_CAG mask -> must not hit */
    reset_engine();
    kv_set_mode(KV_MODE_NORMAL);
    CHECK(feed(KC_LALT, true) == true);
    CHECK(feed(KC_BSPC, true) == true); /* CAG passthrough */
    CHECK(s_hits[KC_LEFT] == 0);
    CHECK(feed(KC_BSPC, false) == true);
    CHECK(feed(KC_LALT, false) == true);

    /* LGUI+SLSH: GUI inside SLSH's mask -> must not hit */
    reset_engine();
    kv_set_mode(KV_MODE_NORMAL);
    CHECK(feed(KC_LGUI, true) == true);
    CHECK(feed(KC_SLSH, true) == true);
    CHECK(s_hits[KC_F] == 0);
    CHECK(feed(KC_SLSH, false) == true);
    CHECK(feed(KC_LGUI, false) == true);

    /* LALT+F: F's req = CTRL, ALT present -> must not hit */
    reset_engine();
    kv_set_mode(KV_MODE_NORMAL);
    CHECK(feed(KC_LALT, true) == true);
    CHECK(feed(KC_F, true) == true);
    CHECK(s_hits[KC_PGDN] == 0);
    CHECK(feed(KC_F, false) == true);
    CHECK(feed(KC_LALT, false) == true);

    /* LSFT+F: no Ctrl at all -> must not hit (F req = CTRL) */
    reset_engine();
    kv_set_mode(KV_MODE_NORMAL);
    CHECK(feed(KC_LSFT, true) == true);
    CHECK(feed(KC_F, true) == true);
    CHECK(s_hits[KC_PGDN] == 0);
    CHECK(feed(KC_F, false) == true);
    CHECK(feed(KC_LSFT, false) == true);

    /* LCTL+LSFT+F: SHIFT is outside F's mask, so held set is exactly CTRL
     * inside the mask -> HIT (source semantics: mask filters Shift out). */
    reset_engine();
    kv_set_mode(KV_MODE_NORMAL);
    CHECK(feed(KC_LCTL, true) == true);
    CHECK(feed(KC_LSFT, true) == true);
    CHECK(feed(KC_F, true) == false);
    CHECK(s_hits[KC_PGDN] == 1);
    CHECK(feed(KC_F, false) == false);
    CHECK(feed(KC_LSFT, false) == true);
    CHECK(feed(KC_LCTL, false) == true);

    /* Shift+BSPC: SHIFT is outside BSPC's mask, req = 0 -> HIT */
    reset_engine();
    kv_set_mode(KV_MODE_NORMAL);
    CHECK(feed(KC_LSFT, true) == true);
    CHECK(feed(KC_BSPC, true) == false);
    CHECK(s_hits[KC_LEFT] == 1);
    CHECK(feed(KC_BSPC, false) == false);
    CHECK(feed(KC_LSFT, false) == true);

    /* Shift+MINS: SHIFT is inside MINS' mask, req = 0 -> must NOT hit */
    reset_engine();
    kv_set_mode(KV_MODE_NORMAL);
    CHECK(feed(KC_LSFT, true) == true);
    CHECK(feed(KC_MINS, true) == true);
    CHECK(s_hits[KC_UP] == 0);
    CHECK(s_hits[KC_HOME] == 0);
    CHECK(feed(KC_MINS, false) == true);
    CHECK(feed(KC_LSFT, false) == true);

    /* LALT+EQL: req = SHIFT, ALT present -> must NOT hit */
    reset_engine();
    kv_set_mode(KV_MODE_NORMAL);
    CHECK(feed(KC_LALT, true) == true);
    CHECK(feed(KC_EQL, true) == true);
    CHECK(s_hits[KC_DOWN] == 0);
    CHECK(feed(KC_EQL, false) == true);
    CHECK(feed(KC_LALT, false) == true);
}

/* G6 — required modifier + one extra (unrequired) modifier must not misfire.
 * F's entry is {base=KC_F, req=MOD_MASK_CTRL, mask=VIM_NO_CAG_MASK}, so the
 * masked held set with LCTL+LALT down is CTRL|ALT while req is only CTRL:
 * `held & ~req` is non-zero, the non-empty-subset test fails, the table does
 * NOT consume F (feed == true, i.e. passed through), and no PGDN is emitted. */
static void test_shortcut_req_plus_extra_mods(void) {
    reset_engine();
    kv_set_mode(KV_MODE_NORMAL);
    CHECK(feed(KC_LCTL, true) == true);
    CHECK(feed(KC_LALT, true) == true);
    CHECK(feed(KC_F, true) == true);   /* required CTRL + extra ALT -> no hit */
    CHECK(s_hits[KC_PGDN] == 0);
    /* release every held modifier again; every edge stays paired */
    CHECK(feed(KC_F, false) == true);
    CHECK(feed(KC_LALT, false) == true);
    CHECK(feed(KC_LCTL, false) == true);
    CHECK(s_orphan == 0);

    /* contrast: the same required CTRL alone still hits (guards against the
     * extra ALT accidentally disabling the entry outright). */
    reset_engine();
    kv_set_mode(KV_MODE_NORMAL);
    CHECK(feed(KC_LCTL, true) == true);
    CHECK(feed(KC_F, true) == false);
    CHECK(s_hits[KC_PGDN] == 1);
    CHECK(feed(KC_F, false) == false);
    CHECK(feed(KC_LCTL, false) == true);
}

/* ======================================================================
 * C. gating: vim off / mode != NORMAL / shortcuts == NULL / release.
 * ====================================================================== */
static void test_shortcut_gating(void) {
    /* vim disabled */
    reset_engine();
    kv_set_mode(KV_MODE_NORMAL);
    kv_disable();
    CHECK(feed(KC_BSPC, true) == true);
    CHECK(s_hits[KC_LEFT] == 0);
    CHECK(feed(KC_BSPC, false) == true);

    /* mode INSERT */
    reset_engine(); /* INSERT */
    CHECK(feed(KC_BSPC, true) == true);
    CHECK(s_hits[KC_LEFT] == 0);

    /* mode VISUAL */
    reset_engine();
    kv_set_mode(KV_MODE_VISUAL);
    CHECK(feed(KC_BSPC, true) == true);
    CHECK(s_hits[KC_LEFT] == 0);
    CHECK(kv_get_mode() == KV_MODE_VISUAL);

    /* shortcuts == NULL */
    reset_engine();
    kv_set_mode(KV_MODE_NORMAL);
    vim_cfg_t cfg = g_cfg;
    cfg.shortcuts = NULL;
    CHECK(feed_cfg(KC_BSPC, true, &cfg) == true);
    CHECK(s_hits[KC_LEFT] == 0);
    CHECK(feed_cfg(KC_BSPC, false, &cfg) == true);

    /* release edge is never matched by the table (no press -> no table entry) */
    reset_engine();
    kv_set_mode(KV_MODE_NORMAL);
    CHECK(feed(KC_MINS, false) == true);
    CHECK(s_hits[KC_UP] == 0);
}

/* ======================================================================
 * D. kv_cancel() pre-step: a pending operator is dropped and only the
 *    shortcut action is emitted.
 * ====================================================================== */
static void test_shortcut_cancel_pending(void) {
    reset_engine();
    kv_set_mode(KV_MODE_NORMAL);
    CHECK(feed(KC_D, true) == false);   /* operator-pending */
    CHECK(kv_pending() == true);
    CHECK(feed(KC_BSPC, true) == false); /* shortcut fires after kv_cancel */
    CHECK(kv_pending() == false);
    CHECK(s_hits[KC_LEFT] == 1);        /* only the BSPC shortcut action */
    CHECK(s_hits[KC_X] == 0);           /* no Ctrl+X from a half-typed d */
    CHECK(feed(KC_BSPC, false) == false);
    CHECK(feed(KC_D, false) == false);  /* paired release of the consumed d */
    CHECK(s_orphan == 0);

    /* negative: with no pending command kv_cancel is a harmless no-op */
    reset_engine();
    kv_set_mode(KV_MODE_NORMAL);
    CHECK(kv_pending() == false);
    CHECK(feed(KC_BSPC, true) == false);
    CHECK(s_hits[KC_LEFT] == 1);
    CHECK(feed(KC_BSPC, false) == false);
}

/* ======================================================================
 * E. send_plain_tap(): physical modifiers are cleared for the tap and
 *    restored afterwards.
 * ====================================================================== */
static void test_send_plain_tap_restores_shift(void) {
    reset_engine();
    kv_set_mode(KV_MODE_NORMAL);
    register_code(KC_LSFT); /* physical Shift is held / host-registered */
    CHECK((get_mods() & MOD_BIT_LSHIFT) != 0);

    int before = s_tap16_n;
    CHECK(feed(KC_BSPC, true) == false);
    CHECK(s_hits[KC_LEFT] == 1);
    CHECK(s_tap16_n > before);
    CHECK(s_tap16[before] == KC_LEFT);       /* bare low byte, no cap bits */
    CHECK(s_tap16_mods[before] == 0);        /* cleared while the tap is in flight */
    CHECK((get_mods() & MOD_BIT_LSHIFT) != 0); /* restored afterwards */

    CHECK(feed(KC_BSPC, false) == false);
    unregister_code(KC_LSFT);
    CHECK(get_mods() == 0);
    CHECK(s_orphan == 0);
}

/* ======================================================================
 * F. vim_emit(): add/drop quadrants driven by real commands.
 * ====================================================================== */

/* F1: no physical modifiers, `dd` -> the command requests LSFT and LCTL; both
 * are registered around their taps and the report returns to 0. */
static void test_emit_add_quadrant_dd(void) {
    reset_engine();
    kv_set_mode(KV_MODE_NORMAL);
    CHECK(feed(KC_D, true) == false);
    CHECK(feed(KC_D, true) == false);
    kv_emit_flush_now();

    CHECK(s_hits[KC_HOME] == 2);
    CHECK(s_hits[KC_END] == 1);
    CHECK(s_hits[KC_X] == 1);
    CHECK(s_hits[KC_BSPC] == 1);

    CHECK(s_add_n == 2);
    CHECK(log_has(s_add, s_add_n, MOD_BIT_LSHIFT));
    CHECK(log_has(s_add, s_add_n, MOD_BIT_LCTRL));
    CHECK(s_del_n == 2);
    CHECK(log_has(s_del, s_del_n, MOD_BIT_LSHIFT));
    CHECK(log_has(s_del, s_del_n, MOD_BIT_LCTRL));

    /* in-flight tap modifier state == the command's requested mods */
    CHECK(click_mods_of(KC_END) == MOD_BIT_LSHIFT);
    CHECK(click_mods_of(KC_X) == MOD_BIT_LCTRL);
    CHECK(click_mods_of(KC_BSPC) == 0);
    CHECK(get_mods() == 0); /* add fully unwound */

    CHECK(feed(KC_D, false) == false);
    CHECK(s_orphan == 0);
}

/* F2: no physical modifiers, `w` -> register LCTL only. */
static void test_emit_add_quadrant_w(void) {
    reset_engine();
    kv_set_mode(KV_MODE_NORMAL);
    CHECK(feed(KC_W, true) == false);
    kv_emit_flush_now();

    CHECK(s_hits[KC_RGHT] == 1);
    CHECK(s_add_n == 1);
    CHECK(s_add[0] == MOD_BIT_LCTRL);
    CHECK(s_del_n == 1);
    CHECK(s_del[0] == MOD_BIT_LCTRL);
    CHECK(click_mods_of(KC_RGHT) == MOD_BIT_LCTRL);
    CHECK(get_mods() == 0);
    CHECK(feed(KC_W, false) == false);
}

/* F3: physical Shift + x (folds to X -> Backspace, a no-mod command) -> the
 * physical Shift must be DROPPED for the tap and restored afterwards. */
static void test_emit_drop_quadrant_shift_x(void) {
    reset_engine();
    kv_set_mode(KV_MODE_NORMAL);
    CHECK(feed(KC_LSFT, true) == true);

    int before = s_click_n;
    CHECK(feed(KC_X, true) == false); /* X = backspace command, no mods */
    kv_emit_flush_now();

    CHECK(s_hits[KC_BSPC] == 1);
    CHECK(click_mods_after(KC_BSPC, before) == 0); /* Shift dropped in-flight */
    CHECK(log_has(s_del, s_del_n, MOD_BIT_LSHIFT));
    CHECK(get_mods() == MOD_BIT_LSHIFT); /* restored */

    CHECK(feed(KC_X, false) == false);
    CHECK(feed(KC_LSFT, false) == true);
    CHECK(s_orphan == 0);
}

/* F4: physical Shift + `d d` (folds to D; the command itself carries LSFT for
 * the Shift+End selection) -> the Shift is NOT dropped, and the Ctrl part is
 * added/dropped normally. */
static void test_emit_keep_quadrant_shift_dd(void) {
    reset_engine();
    kv_set_mode(KV_MODE_NORMAL);
    CHECK(feed(KC_LSFT, true) == true);

    int before = s_click_n;
    CHECK(feed(KC_D, true) == false); /* D = delete-to-eol */
    CHECK(feed(KC_D, true) == false);
    kv_emit_flush_now();

    CHECK(s_hits[KC_END] == 2);
    CHECK(s_hits[KC_X] == 2);
    /* the Shift+End tap keeps the physical Shift (command wants it) */
    CHECK(click_mods_after(KC_END, before) == MOD_BIT_LSHIFT);
    /* the Ctrl+X tap gets LCTL and drops the unwanted Shift */
    CHECK(click_mods_after(KC_X, before) == MOD_BIT_LCTRL);
    CHECK(log_has(s_add, s_add_n, MOD_BIT_LCTRL));
    CHECK(log_has(s_del, s_del_n, MOD_BIT_LSHIFT));
    CHECK(get_mods() == MOD_BIT_LSHIFT); /* restored */

    CHECK(feed(KC_D, false) == false);
    CHECK(feed(KC_LSFT, false) == true);
    CHECK(get_mods() == 0);
    CHECK(s_orphan == 0);
}

/* F5: Shift folding — Shift+A / Shift+I produce bare End / Home; Shift+W
 * produces Ctrl+Right with no leaked Shift. */
static void test_emit_shift_fold(void) {
    /* Shift+A -> END, no Shift in the tap */
    reset_engine();
    kv_set_mode(KV_MODE_NORMAL);
    CHECK(feed(KC_LSFT, true) == true);
    int before = s_click_n;
    CHECK(feed(KC_A, true) == false);
    kv_emit_flush_now();
    CHECK(s_hits[KC_END] == 1);
    CHECK(click_mods_after(KC_END, before) == 0);
    CHECK(get_mods() == MOD_BIT_LSHIFT);
    CHECK(feed(KC_A, false) == false);
    CHECK(feed(KC_LSFT, false) == true);
    CHECK(kv_get_mode() == KV_MODE_INSERT);

    /* Shift+I -> HOME, no Shift in the tap */
    reset_engine();
    kv_set_mode(KV_MODE_NORMAL);
    CHECK(feed(KC_LSFT, true) == true);
    before = s_click_n;
    CHECK(feed(KC_I, true) == false);
    kv_emit_flush_now();
    CHECK(s_hits[KC_HOME] == 1);
    CHECK(click_mods_after(KC_HOME, before) == 0);
    CHECK(get_mods() == MOD_BIT_LSHIFT);
    CHECK(feed(KC_I, false) == false);
    CHECK(feed(KC_LSFT, false) == true);
    CHECK(kv_get_mode() == KV_MODE_INSERT);

    /* Shift+W -> Ctrl+Right, no leaked Shift */
    reset_engine();
    kv_set_mode(KV_MODE_NORMAL);
    CHECK(feed(KC_LSFT, true) == true);
    before = s_click_n;
    CHECK(feed(KC_W, true) == false);
    kv_emit_flush_now();
    CHECK(s_hits[KC_RGHT] == 1);
    int m = click_mods_after(KC_RGHT, before);
    CHECK(m == MOD_BIT_LCTRL);
    CHECK((m & MOD_BIT_LSHIFT) == 0);
    CHECK(get_mods() == MOD_BIT_LSHIFT);
    CHECK(feed(KC_W, false) == false);
    CHECK(feed(KC_LSFT, false) == true);

    /* Shift+O -> Home, Shift+Enter, Up: the fold must drop Shift for the bare
     * Home/Up taps yet keep it for the Shift+Enter tap (mixed add/drop in one
     * command), and restore the physical Shift afterwards. */
    reset_engine();
    kv_set_mode(KV_MODE_NORMAL);
    CHECK(feed(KC_LSFT, true) == true);
    before = s_click_n;
    CHECK(feed(KC_O, true) == false);
    kv_emit_flush_now();
    CHECK(s_hits[KC_HOME] == 1);
    CHECK(s_hits[KC_ENT] == 1);
    CHECK(s_hits[KC_UP] == 1);
    CHECK(click_mods_after(KC_HOME, before) == 0);
    CHECK(click_mods_after(KC_UP, before) == 0);
    CHECK(click_mods_after(KC_ENT, before) == MOD_BIT_LSHIFT);
    CHECK(get_mods() == MOD_BIT_LSHIFT);
    CHECK(feed(KC_O, false) == false);
    CHECK(feed(KC_LSFT, false) == true);
    CHECK(kv_get_mode() == KV_MODE_INSERT);
}

/* F6: a bare h registers the held arrow directly with no register_mods(). */
static void test_emit_held_motion_no_mods(void) {
    reset_engine();
    kv_set_mode(KV_MODE_NORMAL);
    CHECK(feed(KC_H, true) == false);
    kv_emit_flush_now();
    CHECK(reg_count(KC_LEFT) == 1);
    CHECK(s_add_n == 0);
    CHECK(s_del_n == 0);
    CHECK(click_mods_of(KC_LEFT) == 0);
    CHECK(feed(KC_H, false) == false);
    CHECK(reg_count(KC_LEFT) == 0);
    CHECK(s_orphan == 0);
}

/* F7: a second press of the same held motion while its arrow is already
 * register-held must hit the `!s_arrow_reg[ai]` FALSE branch in vim_emit()
 * and NOT register_code() the arrow a second time.  The single key-up clears
 * the single registration, and the physical releases stay paired (no orphan).
 *
 * The host auto-repeat relies on the arrow being held exactly once: a
 * duplicate register here would leave a stuck arrow after the one key-up
 * (s_arrow_reg is cleared on the first unregister, so a second registration
 * would never be unwound). */
static void test_emit_held_motion_repeat_guard(void) {
    reset_engine();
    kv_set_mode(KV_MODE_NORMAL);

    CHECK(feed(KC_H, true) == false);
    kv_emit_flush_now();
    CHECK(reg_count(KC_LEFT) == 1);
    CHECK(s_hits[KC_LEFT] == 1);

    /* second press, still held: the false branch must skip register_code() */
    CHECK(feed(KC_H, true) == false);
    kv_emit_flush_now();
    CHECK(reg_count(KC_LEFT) == 1); /* unchanged: not registered twice */
    CHECK(s_hits[KC_LEFT] == 1);    /* exactly one register_code in total */
    CHECK(s_add_n == 0);            /* no modifier churn on the guard path */
    CHECK(s_del_n == 0);

    /* the one release unwinds the one registration completely */
    CHECK(feed(KC_H, false) == false);
    CHECK(reg_count(KC_LEFT) == 0);
    CHECK(s_orphan == 0);
}

/* ======================================================================
 * G. pairing table — idempotent pair_add and full-table oldest-overwrite.
 * ====================================================================== */
static void test_pair_idempotent(void) {
    reset_engine(); /* INSERT */
    vim_glue_swallow(KC_A);
    vim_glue_swallow(KC_A);
    vim_glue_swallow(KC_A);
    /* one entry only: the first release consumes it, the second (with no
     * duplicate entry left) passes through. */
    CHECK(pipeline_cfg(KC_A, false, &g_cfg) == false);
    CHECK(pipeline_cfg(KC_A, false, &g_cfg) == true);

    /* a different key is unaffected */
    vim_glue_swallow(KC_B);
    CHECK(pipeline_cfg(KC_B, false, &g_cfg) == false);
    CHECK(pipeline_cfg(KC_B, false, &g_cfg) == true);

    /* negative path: an unswallowed release always passes */
    CHECK(pipeline_cfg(KC_C, false, &g_cfg) == true);
}

static void test_pair_overflow(void) {
    /* Fn swallows 17 distinct undeclared keys; pair_add keeps the newest 16. */
    reset_engine();
    fn_on();
    uint16_t keys[17];
    for (int i = 0; i < 17; i++) {
        keys[i] = (uint16_t)(KC_A + i); /* KC_A .. KC_Q (none declared) */
        CHECK(test_declared(keys[i]) == false);
        CHECK(feed(keys[i], true) == false);
    }
    fn_off();

    /* oldest (index 0) was evicted -> its release passes */
    CHECK(pipeline_cfg(keys[0], false, &g_cfg) == true);
    /* newest (index 16) is present -> its release is consumed */
    CHECK(pipeline_cfg(keys[16], false, &g_cfg) == false);
    /* the second-oldest is still present */
    CHECK(pipeline_cfg(keys[1], false, &g_cfg) == false);
}

/* ======================================================================
 * H. vim_glue_release_all() unregisters held motions on every axis, and is a
 *    harmless no-op when nothing is held.
 * ====================================================================== */
static void test_release_all_motion(void) {
    /* false path: nothing held -> no registrations are touched */
    reset_engine();
    kv_set_mode(KV_MODE_NORMAL);
    vim_glue_release_all();
    CHECK(reg_count(KC_LEFT) == 0);
    CHECK(reg_count(KC_DOWN) == 0);
    CHECK(reg_count(KC_UP) == 0);
    CHECK(reg_count(KC_RGHT) == 0);
    /* a second call is still safe (arrow_reg already false) */
    vim_glue_release_all();
    CHECK(reg_count(KC_LEFT) == 0);
    CHECK(reg_count(KC_DOWN) == 0);
    CHECK(reg_count(KC_UP) == 0);
    CHECK(reg_count(KC_RGHT) == 0);

    /* true path: every motion axis is register-held, then force-released */
    static const uint16_t motion[4] = {KC_H, KC_J, KC_K, KC_L};
    static const uint16_t arrow[4]  = {KC_LEFT, KC_DOWN, KC_UP, KC_RGHT};
    for (int i = 0; i < 4; i++) {
        reset_engine();
        kv_set_mode(KV_MODE_NORMAL);
        CHECK(feed(motion[i], true) == false);
        kv_emit_flush_now();
        CHECK(reg_count(arrow[i]) == 1);
        vim_glue_release_all();
        CHECK(reg_count(arrow[i]) == 0);
        /* the physical release is still paired (no orphan) */
        CHECK(feed(motion[i], false) == false);
        CHECK(s_orphan == 0);
    }
}

/* ======================================================================
 * I. packed_mods_to_hid(): white-box coverage of the right-side flag
 *    (bits 8..12 carrying the 0x10 side bit) and the left LALT/LGUI arms.
 *
 * The right-side packed values are unreachable from a real key press, so they
 * are injected at the emit boundary: kv_emit_tap() + kv_emit_flush_now() run
 * the vim_emit() callback registered by reset_engine() -> vim_glue_init().
 * This exercises the decoder directly, not the key pipeline (design §4.12 #5).
 * ====================================================================== */
static void test_emit_packed_mods_arms(void) {
    static const struct { kv_keycode_t tap; uint8_t hid; } kCases[] = {
        { KV_MOD_RCTL | KV_A, MOD_BIT_RCTRL },  /* right switch case 0x01 */
        { KV_MOD_RSFT | KV_A, MOD_BIT_RSHIFT }, /* right switch case 0x02 */
        { KV_MOD_RALT | KV_A, MOD_BIT_RALT },   /* right switch case 0x04 */
        { KV_MOD_RGUI | KV_A, MOD_BIT_RGUI },   /* right switch case 0x08 */
        { KV_MOD_LALT | KV_A, MOD_BIT_LALT },   /* left LALT arm */
        { KV_MOD_LGUI | KV_A, MOD_BIT_LGUI },   /* left LGUI arm */
    };
    for (unsigned i = 0; i < sizeof(kCases) / sizeof(kCases[0]); i++) {
        reset_engine(); /* kv_init clears the queue + registers vim_emit */
        int before = s_click_n;
        kv_emit_tap(kCases[i].tap);
        kv_emit_flush_now();

        CHECK(s_hits[KV_A] == 1); /* the basic key is a plain tap, once */
        CHECK(click_mods_after(KV_A, before) == kCases[i].hid);
        CHECK(s_add_n == 1);
        CHECK(s_add[0] == kCases[i].hid);
        CHECK(s_del_n == 1);
        CHECK(s_del[0] == kCases[i].hid);
        CHECK(get_mods() == 0); /* add fully unwound after the tap */
    }

    /* right-side flag with reserved low-nibble byte 0x03 -> switch `default:`:
     * no HID bit is registered, yet the basic key still taps cleanly. */
    reset_engine();
    int before = s_click_n;
    kv_emit_tap(KV_MOD_RCTL | KV_MOD_LSFT | KV_A); /* packed m = 0x13 */
    kv_emit_flush_now();
    CHECK(s_hits[KV_A] == 1);
    CHECK(click_mods_after(KV_A, before) == 0);
    CHECK(s_add_n == 0);
    CHECK(s_del_n == 0);
    CHECK(get_mods() == 0);
}

int main(void) {
    test_shadow_bits();
    test_shortcut_nomod();
    test_shortcut_shift_eql();
    test_shortcut_ctrl_fb();
    test_shortcut_extra_mods();
    test_shortcut_req_plus_extra_mods();
    test_shortcut_gating();
    test_shortcut_cancel_pending();
    test_send_plain_tap_restores_shift();
    test_emit_add_quadrant_dd();
    test_emit_add_quadrant_w();
    test_emit_drop_quadrant_shift_x();
    test_emit_keep_quadrant_shift_dd();
    test_emit_shift_fold();
    test_emit_held_motion_no_mods();
    test_emit_held_motion_repeat_guard();
    test_emit_packed_mods_arms();
    test_pair_idempotent();
    test_pair_overflow();
    test_release_all_motion();
    printf("glue-modifiers: pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
