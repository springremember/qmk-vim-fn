/* test_adapter_regress2.c — second audit probe set. */
#include "qmk_stub.h"
#include "qmk-vim-fn/engine/include/kv.h"
#include "qmk-vim-fn/engine/src/emit.h"
#include "qmk-vim-fn/qmk/vim_glue.h"
#include "qmk-vim-fn/qmk/vim_keymap_common.h"

layer_state_t layer_state         = 0;
layer_state_t default_layer_state = 0;
static uint8_t s_mods;
uint8_t get_mods(void) { return s_mods; }
void    clear_mods(void) { s_mods = 0; }
void    set_mods(uint8_t m) { s_mods = m; }
void    register_mods(uint8_t m) { s_mods |= m; }
void    unregister_mods(uint8_t m) { s_mods &= (uint8_t)~m; }
#define REG_CAP 128
static uint16_t s_reg[REG_CAP]; static int s_reg_n;
void register_code(uint16_t kc) {
    if (IS_MODIFIER_KEYCODE(kc)) s_mods |= (uint8_t)(1u << (kc - KC_LCTL));
    if (s_reg_n < REG_CAP) s_reg[s_reg_n++] = kc;
}
void unregister_code(uint16_t kc) {
    if (IS_MODIFIER_KEYCODE(kc)) s_mods &= (uint8_t)~(1u << (kc - KC_LCTL));
    for (int i = 0; i < s_reg_n; i++) if (s_reg[i] == kc) { s_reg[i] = s_reg[--s_reg_n]; break; }
}
void tap_code(uint16_t kc) { register_code(kc); unregister_code(kc); }
void tap_code16(uint16_t kc) { tap_code((uint16_t)(kc & 0xFF)); }
static uint32_t g_now;
uint16_t timer_read(void) { return (uint16_t)g_now; }
uint16_t timer_elapsed(uint16_t since) { return (uint16_t)((uint16_t)g_now - since); }
uint32_t timer_read32(void) { return g_now; }
uint32_t timer_elapsed32(uint32_t since) { return g_now - since; }
static int g_pass, g_fail;
#define CHECK(c) do { if (c) g_pass++; else { g_fail++; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)
#define NOTE(...) do { printf(__VA_ARGS__); } while (0)
static const vim_cfg_t g_cfg = {
    .fn_layer = 4, .trigger_kc = TEST_TRIGGER_KC, .mod_win = KC_RALT, .mod_mac = KC_RGUI,
    .is_mac = NULL, .link_ok = NULL, .hold_ms = 200, .shift_esc_enable = true,
    .led_index = 0, .insert_flash_color = 0xFF8000,
    .hook_pre = NULL, .hook_post_myfn = NULL, .myfn_declared = NULL, .myfn = NULL,
    .vim_set_enabled = NULL, .shortcuts = vim_default_shortcuts,
};
static bool pipeline(uint16_t kc, bool pressed) {
    keyrecord_t r = {0}; r.event.pressed = pressed;
    const bool pass = vim_pipeline_process(kc, &r, &g_cfg);
    if (pass) { if (pressed) register_code(kc); else unregister_code(kc); }
    return pass;
}
static bool sim_held(uint16_t kc) {
    for (int i = 0; i < s_reg_n; i++) if (s_reg[i] == kc) return true;
    return false;
}
static void reset_engine(void) {
    g_now = 1000; s_mods = 0; s_reg_n = 0; layer_state = 0; default_layer_state = 0;
    vim_keymap_common_init();
}

/* P0-B: 12 non-F keys fill the Caps held table (so a synth LCTL is owned),
 * then physical LCTL overflows and is swallowed; its release clears
 * s_caps_ctrl_owned without unregistering the synth -> stuck LCTL. */
static void probe_caps_overflow_ctrl(void) {
    reset_engine();
    CHECK(pipeline(KC_CAPS, true) == false);
    const uint16_t many[12] = {KC_A, KC_B, KC_C, KC_D, KC_E, KC_F, KC_G,
                               KC_H, KC_I, KC_J, KC_K, KC_L};
    for (int i = 0; i < 12; i++) CHECK(pipeline(many[i], true) == false);
    CHECK(sim_held(KC_LCTL));
    NOTE("after 12 keys: s_mods=0x%02X\n", s_mods);
    CHECK(pipeline(KC_LCTL, true) == false);   /* 13th: overflow, swallowed */
    /* P0-2 修复后：该 release **透传给 QMK**（不再被配对表消费），
     * 由 QMK 的 del_mods 清掉共享位 —— 这正是修复的目的。 */
    CHECK(pipeline(KC_LCTL, false) == true);
    for (int i = 0; i < 12; i++) CHECK(pipeline(many[i], false) == false);
    NOTE("after all keys up: s_mods=0x%02X (expect 0x00)\n", s_mods);
    CHECK(s_mods == 0x00);
    CHECK(pipeline(KC_CAPS, false) == false);
    NOTE("after Caps exit: s_mods=0x%02X (expect 0x00)\n", s_mods);
    CHECK(s_mods == 0x00);                     /* stuck LCTL if it fails */
    reset_engine();
}

/* Control: the same 12-key sequence WITHOUT the overflow Ctrl must release
 * the synth LCTL cleanly (proves the probe is specific). */
static void probe_caps_overflow_control(void) {
    reset_engine();
    CHECK(pipeline(KC_CAPS, true) == false);
    const uint16_t many[12] = {KC_A, KC_B, KC_C, KC_D, KC_E, KC_F, KC_G,
                               KC_H, KC_I, KC_J, KC_K, KC_L};
    for (int i = 0; i < 12; i++) CHECK(pipeline(many[i], true) == false);
    for (int i = 0; i < 12; i++) CHECK(pipeline(many[i], false) == false);
    CHECK(s_mods == 0x00);
    CHECK(pipeline(KC_CAPS, false) == false);
    CHECK(s_mods == 0x00);
    reset_engine();
}

/* Non-overflow: physical LCTL pressed+released while synth LCTL owned. QMK's
 * own del_mods for the passed release clears the shared bit -> no stuck. */
static void probe_caps_phys_ctrl_shared_bit(void) {
    reset_engine();
    CHECK(pipeline(KC_CAPS, true) == false);
    CHECK(pipeline(KC_A, true) == false);      /* synth LCTL owned, A held */
    CHECK(sim_held(KC_LCTL));
    CHECK(pipeline(KC_LCTL, true) == true);    /* physical LCTL passes */
    CHECK(pipeline(KC_LCTL, false) == true);   /* release passes -> del_mods */
    NOTE("non-overflow phys LCTL: s_mods=0x%02X\n", s_mods);
    CHECK(pipeline(KC_A, false) == false);
    CHECK(pipeline(KC_CAPS, false) == false);
    CHECK(s_mods == 0x00);
    reset_engine();
}

int main(void) {
    probe_caps_overflow_ctrl();
    probe_caps_overflow_control();
    probe_caps_phys_ctrl_shared_bit();
    printf("adapter-regress2: pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
