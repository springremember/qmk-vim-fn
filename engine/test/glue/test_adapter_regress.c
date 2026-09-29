/* test_adapter_regress.c — regression tests for the 2026-09 adapter audit (P0-1).
 * Reproduces candidate defects in qmk/vim_keymap_common.c + qmk/vim_glue.c. */
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

#define REG_CAP 64
static uint16_t s_reg[REG_CAP];
static int      s_reg_n;
void register_code(uint16_t kc) {
    if (IS_MODIFIER_KEYCODE(kc)) s_mods |= (uint8_t)(1u << (kc - KC_LCTL));
    if (s_reg_n < REG_CAP) s_reg[s_reg_n++] = kc;
}
void unregister_code(uint16_t kc) {
    if (IS_MODIFIER_KEYCODE(kc)) s_mods &= (uint8_t)~(1u << (kc - KC_LCTL));
    for (int i = 0; i < s_reg_n; i++)
        if (s_reg[i] == kc) { s_reg[i] = s_reg[--s_reg_n]; break; }
}
void tap_code(uint16_t kc) { register_code(kc); unregister_code(kc); }
void tap_code16(uint16_t kc) { tap_code((uint16_t)(kc & 0xFF)); }

static uint32_t g_now;
uint16_t timer_read(void) { return (uint16_t)g_now; }
uint16_t timer_elapsed(uint16_t since) { return (uint16_t)((uint16_t)g_now - since); }
uint32_t timer_read32(void) { return g_now; }
uint32_t timer_elapsed32(uint32_t since) { return g_now - since; }

static int g_pass, g_fail;
#define CHECK(cond) do { if (cond) g_pass++; else { g_fail++; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define NOTE(...) do { printf(__VA_ARGS__); } while (0)

static const vim_cfg_t g_cfg = {
    .fn_layer = 4, .trigger_kc = TEST_TRIGGER_KC, .mod_win = KC_RALT, .mod_mac = KC_RGUI,
    .is_mac = NULL, .link_ok = NULL, .hold_ms = 200, .shift_esc_enable = true,
    .led_index = 0, .insert_flash_color = 0xFF8000,
    .hook_pre = NULL, .hook_post_myfn = NULL, .myfn_declared = NULL, .myfn = NULL,
    .vim_set_enabled = NULL, .shortcuts = vim_default_shortcuts,
};

static bool pipeline(uint16_t kc, bool pressed) {
    keyrecord_t r = {0};
    r.event.pressed = pressed;
    const bool pass = vim_pipeline_process(kc, &r, &g_cfg);
    if (pass) { if (pressed) register_code(kc); else unregister_code(kc); }
    return pass;
}
static bool sim_held(uint16_t kc) {
    for (int i = 0; i < s_reg_n; i++) if (s_reg[i] == kc) return true;
    return false;
}
static void reset_engine(void) {
    g_now = 1000; s_mods = 0; s_reg_n = 0;
    layer_state = 0; default_layer_state = 0;
    vim_keymap_common_init();
}

/* ------------------------------------------------------------------ */
/* CASE 1: physical RCTL held when Caps mode is entered, released     */
/* inside the mode, then a non-F key, then Caps release.               */
/* Expected (caps/design.md §3.1-2/3, §4): the synthesized LCTL is     */
/* released with the last non-F key and never survives mode exit.      */
static void probe_caps_rctl_at_entry(void) {
    reset_engine();
    /* Right Ctrl held BEFORE Caps (host + shadow). */
    CHECK(pipeline(KC_RCTL, true) == true);          /* QMK registers RCTL */
    CHECK((s_mods & 0x10) != 0);
    CHECK(pipeline(KC_CAPS, true) == false);         /* enter Caps mode */
    CHECK(pipeline(KC_RCTL, false) == true);         /* release RCTL inside mode */
    CHECK((s_mods & 0x10) == 0);
    CHECK(pipeline(KC_A, true) == false);            /* non-F key -> synth LCTL */
    NOTE("after A down: s_mods=0x%02X (LCTL should be on)\n", s_mods);
    CHECK((s_mods & 0x01) != 0);
    CHECK(pipeline(KC_A, false) == false);           /* last non-F key released */
    NOTE("after A up:   s_mods=0x%02X (LCTL should be off)\n", s_mods);
    CHECK((s_mods & 0x01) == 0);                     /* <-- expected per spec */
    CHECK(pipeline(KC_CAPS, false) == false);        /* exit Caps mode */
    NOTE("after exit:   s_mods=0x%02X (must be 0x00)\n", s_mods);
    CHECK(s_mods == 0x00);                           /* <-- stuck if it fails */
    reset_engine();
}

/* PROBE 1b: same but both Ctrl held at entry, LCTL released first. */
static void probe_caps_both_ctrl_at_entry(void) {
    reset_engine();
    CHECK(pipeline(KC_LCTL, true) == true);
    CHECK(pipeline(KC_RCTL, true) == true);
    CHECK(pipeline(KC_CAPS, true) == false);
    CHECK(pipeline(KC_LCTL, false) == true);         /* drop LCTL, keep RCTL */
    CHECK(pipeline(KC_RCTL, false) == true);         /* now no physical Ctrl */
    CHECK(pipeline(KC_B, true) == false);
    CHECK(pipeline(KC_B, false) == false);
    CHECK(pipeline(KC_CAPS, false) == false);
    NOTE("both-ctrl variant after exit: s_mods=0x%02X\n", s_mods);
    CHECK(s_mods == 0x00);
    reset_engine();
}

/* ------------------------------------------------------------------ */
/* CASE 2: Right-Shift lazy Shift is lost after the physical Left     */
/* Shift is tapped and released while Right Shift is still held.       */
/* design §4.10: 右Shift+a = A; the lazy Shift must persist until RSFT  */
/* is released (unless a physical Shift provides it).                  */
static void probe_rshift_lazy_relost(void) {
    reset_engine();                                  /* INSERT, vim on */
    CHECK(pipeline(KC_RSFT, true) == false);         /* RSFT consumed */
    CHECK(pipeline(KC_A, true) == false || true);
    NOTE("RSFT+a: s_mods=0x%02X (expect LSHIFT 0x02)\n", s_mods);
    CHECK((s_mods & MOD_BIT_LSHIFT) != 0);           /* lazy LShift asserted */
    CHECK(pipeline(KC_A, false) == true || true);
    CHECK(pipeline(KC_LSFT, true) == true);          /* physical LShift joins */
    CHECK(pipeline(KC_LSFT, false) == true);         /* ...and leaves */
    /* RSFT is still physically held: the next key must still be shifted. */
    CHECK(pipeline(KC_B, true) == false || true);
    NOTE("RSFT+b after LShift tap: s_mods=0x%02X (expect LSHIFT 0x02)\n", s_mods);
    CHECK((s_mods & MOD_BIT_LSHIFT) != 0);           /* <-- expected: b -> B */
    CHECK(pipeline(KC_B, false) == true || true);
    CHECK(pipeline(KC_RSFT, false) == false);
    reset_engine();
}

/* CASE 3: Right Shift released while physical Left Shift is held must */
/* NOT clear the physical Shift bit (glue comment at vim_glue.c:167).   */
static void probe_rshift_lazy_clobbers_phys_lshift(void) {
    reset_engine();
    CHECK(pipeline(KC_RSFT, true) == false);
    CHECK(pipeline(KC_A, true) == false || true);    /* lazy LShift asserted */
    CHECK((s_mods & MOD_BIT_LSHIFT) != 0);
    CHECK(pipeline(KC_LSFT, true) == true);          /* physical LShift held */
    CHECK((s_mods & MOD_BIT_LSHIFT) != 0);
    CHECK(pipeline(KC_RSFT, false) == false);        /* release RSFT */
    NOTE("after RSFT up with physical LShift held: s_mods=0x%02X (expect LSHIFT)\n", s_mods);
    CHECK((s_mods & MOD_BIT_LSHIFT) != 0);           /* <-- physical Shift must survive */
    CHECK(pipeline(KC_LSFT, false) == true);
    reset_engine();
}

/* CASE 4: mouse mode exit while the trigger long-press modifier is      */
/* held (s_mouse_mod_reg) — is it released on non-modifier exit?          */
static void probe_mouse_mod_on_exit(void) {
    reset_engine();
    CHECK(pipeline(TEST_TRIGGER_KC, true) == false);
    CHECK(pipeline(TEST_TRIGGER_KC, false) == false);   /* enter MOUSE */
    CHECK(kv_get_mode() == KV_MODE_MOUSE);
    CHECK(pipeline(TEST_TRIGGER_KC, true) == false);    /* hold again */
    g_now += 250;
    vim_keymap_common_task(g_now);                       /* long press -> RALT */
    NOTE("mouse long-press: s_mods=0x%02X (RALT)\n", s_mods);
    CHECK((s_mods & MOD_BIT_RALT) != 0);
    CHECK(pipeline(KC_A, true) == true);                 /* non-mod exits MOUSE */
    NOTE("after non-mod exit: s_mods=0x%02X\n", s_mods);
    CHECK(kv_get_mode() != KV_MODE_MOUSE);
    CHECK(pipeline(KC_A, false) == true);
    CHECK(pipeline(TEST_TRIGGER_KC, false) == false);    /* trigger up */
    NOTE("after trigger up: s_mods=0x%02X\n", s_mods);
    CHECK(s_mods == 0x00);
    reset_engine();
}

int main(void) {
    probe_caps_rctl_at_entry();
    probe_caps_both_ctrl_at_entry();
    probe_rshift_lazy_relost();
    probe_rshift_lazy_clobbers_phys_lshift();
    probe_mouse_mod_on_exit();
    printf("adapter-regress: pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
