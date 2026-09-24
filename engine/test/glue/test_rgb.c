/* test_rgb.c — host tests for the shared QMK keymap layer's RGB helpers
 * (vim_rgb_led_index() and vim_rgb_state_color() in vim_keymap_common.c).
 *
 * Design authority: vim/design.md §4.9 (mouse mode), §4.12 (RGB spec-level
 * colour + keyboard-supplied LED index) and vim/readme.md §10 (six-state
 * colour table).  Test-only; no product code.  The harness (host QMK API,
 * CHECK macro) mirrors test/glue/test_glue.c but deliberately records no taps.
 */
#include "qmk_stub.h"
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

/* The engine/glue only need these symbols to link; recording the codes keeps
 * the stub faithful without any per-test assertion (no taps asserted here). */
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

/* ---------------- test bookkeeping ---------------- */
static int g_pass, g_fail;
#define CHECK(cond)                                                        \
    do {                                                                   \
        if (cond) { g_pass++; }                                            \
        else { g_fail++; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } \
    } while (0)

/* ---------------- generic test cfg ---------------- */
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
    .myfn_declared    = NULL,
    .myfn             = NULL,
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
    layer_state = 0;
    default_layer_state = 0;
    vim_glue_init(); /* kv_init + enable + INSERT */
}

/* ---------------- RGB colour helpers ---------------- */
typedef struct { uint8_t r, g, b; } rgb_t;

#define CHECK_RGB(got, er, eg, eb) \
    CHECK((got).r == (er) && (got).g == (eg) && (got).b == (eb))

/* The keyboard layer always calls it with the mouse flag `m == KV_MODE_MOUSE`. */
static rgb_t color_from_engine(void) {
    rgb_t c = {0, 0, 0};
    vim_rgb_state_color(kv_vim_enabled(), kv_get_mode(), kv_pending(),
                        kv_get_mode() == KV_MODE_MOUSE, &c.r, &c.g, &c.b);
    return c;
}

/* Pure-function call for the combinations the reachable engine state cannot
 * produce (e.g. pending while in Visual, or mouse while vim is off). */
static rgb_t color_raw(bool enabled, kv_mode_t m, bool pending, bool mouse) {
    rgb_t c = {0, 0, 0};
    vim_rgb_state_color(enabled, m, pending, mouse, &c.r, &c.g, &c.b);
    return c;
}

/* Reach NORMAL from the real input path (Caps short press), leaving the
 * release paired. */
static void enter_normal(void) {
    CHECK(pipeline(KC_CAPS, true) == false);
    CHECK(pipeline(KC_CAPS, false) == false);
    CHECK(kv_get_mode() == KV_MODE_NORMAL);
}

/* ================= tests ================= */

/* Branch: s_cfg == NULL -> 0.  MUST run before any vim_pipeline_process()
 * call, because s_cfg is a file-static (never reset by vim_glue_init). */
static void test_rgb_led_index_null(void) {
    reset_engine();
    CHECK(vim_rgb_led_index() == 0);
    /* !s_cfg early-return branch of vim_keymap_common_task() */
    vim_keymap_common_task(1000);
    CHECK(vim_rgb_led_index() == 0);
}

/* Branch: s_cfg set by the pipeline -> cfg->led_index (two distinct values). */
static void test_rgb_led_index_after_pipeline(void) {
    reset_engine();
    (void)pipeline(KC_Z, true); /* s_cfg = &g_cfg, led_index == 0 */
    CHECK(vim_rgb_led_index() == 0);

    static vim_cfg_t cfg6;
    cfg6 = g_cfg;
    cfg6.led_index = 6;
    (void)pipeline_cfg(KC_Z, true, &cfg6); /* s_cfg = &cfg6 */
    CHECK(vim_rgb_led_index() == 6);
}

/* Branch: INSERT (and every mode that is not Visual/Normal) -> green. */
static void test_rgb_insert_green(void) {
    reset_engine();
    CHECK(kv_vim_enabled());
    CHECK(kv_get_mode() == KV_MODE_INSERT);
    CHECK(!kv_pending());
    CHECK_RGB(color_from_engine(), 0x00, 0xFF, 0x00);

    /* default branch: MOUSE with the mouse flag false, and an out-of-range mode */
    CHECK_RGB(color_raw(true, KV_MODE_MOUSE, false, false), 0x00, 0xFF, 0x00);
    CHECK_RGB(color_raw(true, (kv_mode_t)42, false, false), 0x00, 0xFF, 0x00);
}

/* Branch: NORMAL + pending == false -> blue. */
static void test_rgb_normal_blue(void) {
    reset_engine();
    enter_normal();
    CHECK(!kv_pending());
    CHECK_RGB(color_from_engine(), 0x00, 0x00, 0xFF);
}

/* Branch: NORMAL + pending == true -> yellow (operator 'd' and count '3'). */
static void test_rgb_normal_pending_yellow(void) {
    reset_engine();
    enter_normal();
    CHECK(pipeline(KC_D, true) == false); /* operator -> pending */
    CHECK(kv_pending());
    CHECK(kv_get_mode() == KV_MODE_NORMAL);
    CHECK_RGB(color_from_engine(), 0xFF, 0xFF, 0x00);
    (void)pipeline(KC_D, false);

    reset_engine();
    enter_normal();
    CHECK(pipeline(KC_3, true) == false); /* count -> pending */
    CHECK(kv_pending());
    CHECK(kv_get_mode() == KV_MODE_NORMAL);
    CHECK_RGB(color_from_engine(), 0xFF, 0xFF, 0x00);
    (void)pipeline(KC_3, false);
}

/* Branches: VISUAL and VISUAL_LINE -> purple. */
static void test_rgb_visual_purple(void) {
    reset_engine();
    enter_normal();
    CHECK(pipeline(KC_V, true) == false); /* 'v' -> Visual */
    CHECK(kv_get_mode() == KV_MODE_VISUAL);
    CHECK(!kv_pending());
    CHECK_RGB(color_from_engine(), 0x80, 0x00, 0x80);
    (void)pipeline(KC_V, false);

    reset_engine();
    enter_normal();
    CHECK(pipeline(KC_LSFT, true) == true); /* shadow Shift */
    CHECK(pipeline(KC_V, true) == false);   /* Shift+'v' -> Visual-Line */
    CHECK(kv_get_mode() == KV_MODE_VISUAL_LINE);
    CHECK(!kv_pending());
    CHECK_RGB(color_from_engine(), 0x80, 0x00, 0x80);
    (void)pipeline(KC_V, false);
    (void)pipeline(KC_LSFT, false);
}

/* Branch: pending never overrides Visual / Visual-Line.  The engine never
 * leaves a pending flag set while in Visual (feed_visual swallows illegal
 * keys without entering a pending state), so this precedence is only
 * reachable through the RGB API itself. */
static void test_rgb_visual_pending_stays_purple(void) {
    CHECK_RGB(color_raw(true, KV_MODE_VISUAL, true, false), 0x80, 0x00, 0x80);
    CHECK_RGB(color_raw(true, KV_MODE_VISUAL_LINE, true, false), 0x80, 0x00, 0x80);
}

/* Branches: MOUSE -> cyan, and cyan wins over the "vim off" red.  The second
 * half is reached for real by disabling vim while still in MOUSE (kv_disable()
 * leaves s_mode untouched, design §4.7). */
static void test_rgb_mouse_cyan(void) {
    reset_engine();
    CHECK(pipeline(TEST_TRIGGER_KC, true) == false);
    CHECK(pipeline(TEST_TRIGGER_KC, false) == false);
    CHECK(kv_get_mode() == KV_MODE_MOUSE);
    CHECK_RGB(color_from_engine(), 0x00, 0xFF, 0xFF);

    kv_disable(); /* vim off, but still in MOUSE */
    CHECK(!kv_vim_enabled());
    CHECK(kv_get_mode() == KV_MODE_MOUSE);
    CHECK_RGB(color_from_engine(), 0x00, 0xFF, 0xFF);
}

/* Branch: vim off (mouse flag false) -> red, beats mode/pending. */
static void test_rgb_off_red(void) {
    reset_engine();
    kv_disable();
    CHECK(!kv_vim_enabled());
    CHECK(kv_get_mode() == KV_MODE_INSERT);
    CHECK(!kv_pending());
    CHECK_RGB(color_from_engine(), 0xFF, 0x00, 0x00);

    CHECK_RGB(color_raw(false, KV_MODE_NORMAL, true, false), 0xFF, 0x00, 0x00);
    CHECK_RGB(color_raw(false, KV_MODE_VISUAL, false, false), 0xFF, 0x00, 0x00);
    CHECK_RGB(color_raw(false, KV_MODE_VISUAL_LINE, true, false), 0xFF, 0x00, 0x00);
}

/* Branch: the mouse flag is tested before enabled/mode, so cyan always wins. */
static void test_rgb_mouse_precedence(void) {
    CHECK_RGB(color_raw(false, KV_MODE_INSERT, false, true), 0x00, 0xFF, 0xFF);
    CHECK_RGB(color_raw(false, KV_MODE_NORMAL, true, true), 0x00, 0xFF, 0xFF);
    CHECK_RGB(color_raw(true, KV_MODE_VISUAL, true, true), 0x00, 0xFF, 0xFF);
}

int main(void) {
    /* The s_cfg==NULL case must be observed before the first pipeline call. */
    test_rgb_led_index_null();
    test_rgb_led_index_after_pipeline();
    test_rgb_insert_green();
    test_rgb_normal_blue();
    test_rgb_normal_pending_yellow();
    test_rgb_visual_purple();
    test_rgb_visual_pending_stays_purple();
    test_rgb_mouse_cyan();
    test_rgb_off_red();
    test_rgb_mouse_precedence();
    printf("rgb: pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
