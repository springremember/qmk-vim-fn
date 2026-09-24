/* test_pending_clear_probe.c — host falsification probe for the strict-clear
 * contract at the *glue* level, i.e. through vim_pipeline_process().
 *
 * Test-only; separate TU.  Contract (design §4.1 decision 1, §4.2 "严格清空",
 * testcase §9):
 *   "遇到非期望键，立即清空 pending，再重新识别——是 vim 键码则当作新命令首键；
 *    否则原样透传宿主"
 *   "操作符+非期望(非 vim)：d F5 → 清空 d，原样发 F5"
 *   "g 前缀不吞键：g F5 → 发 F5"
 *
 * The Visual passthrough fix (741f4d9) adds an early `return true` for non-vim
 * keys inside vim_glue_engine(), which may bypass the engine entirely and thus
 * skip the pending clear the engine would have performed.  This probe checks
 * whether that side effect is lost.
 */
#include "qmk_stub.h"
#include "qmk-vim-fn/engine/include/kv.h"
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

static uint16_t g_now;
uint16_t timer_read(void) { return g_now; }
uint16_t timer_elapsed(uint16_t since) { return (uint16_t)(g_now - since); }

static int g_pass, g_fail;
#define CHECK(cond)                                                        \
    do {                                                                   \
        if (cond) { g_pass++; }                                            \
        else { g_fail++; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } \
    } while (0)

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
    vim_glue_init();
    kv_set_mode(KV_MODE_NORMAL);
}

/* Feed a pending prefix (optionally shift-folded) then a non-vim key.  Both
 * the non-vim pass-through and the strict-clear of pending are asserted. */
static void probe(const char *what, bool shift, uint16_t k1, uint16_t k2) {
    reset_engine();

    if (shift) {
        CHECK(pipeline(KC_LSFT, true) == true);   /* physical shift, passes */
    }
    if (pipeline(k1, true) != false) {
        g_fail++;
        printf("FAIL %s:%d  %s: prefix key 0x%04X was not consumed\n", __FILE__, __LINE__, what, k1);
        return;
    }
    if (kv_pending() != true) {
        g_fail++;
        printf("FAIL %s:%d  %s: prefix 0x%04X did not create pending\n", __FILE__, __LINE__, what, k1);
        return;
    }

    bool pass = pipeline(k2, true);
    if (!pass) {
        g_fail++;
        printf("FAIL %s:%d  %s: non-vim 0x%04X was swallowed (expected pass-through)\n",
               __FILE__, __LINE__, what, k2);
    } else {
        g_pass++;
    }
    if (kv_pending()) {
        g_fail++;
        printf("FAIL %s:%d  %s: pending NOT cleared by non-vim 0x%04X "
               "(kv_pending=true; a following motion would mis-combine)\n",
               __FILE__, __LINE__, what, k2);
    } else {
        g_pass++;
    }
    CHECK(pipeline(k2, false) == true); /* paired pass-through release */

    if (shift) {
        CHECK(pipeline(k1, false) == false);
        CHECK(pipeline(KC_LSFT, false) == true);
    } else {
        CHECK(pipeline(k1, false) == false);
    }
}

int main(void) {
    probe("d + F5",  false, KC_D,  KC_F5);  /* testcase §9: clear d, emit F5 */
    probe("3 + F5",  false, KC_3,  KC_F5);  /* count strict-clear */
    probe("g + F5",  false, KC_G,  KC_F5);  /* testcase §9: g prefix not swallowing */
    probe("Z + F5",  true,  KC_Z,  KC_F5);  /* shift-folded Z prefix strict-clear */
    probe("> + F5",  true,  KC_DOT, KC_F5); /* shift-folded indent prefix strict-clear */

    printf("pending-clear-probe: pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
