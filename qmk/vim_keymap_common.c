// Copyright 2026 qmk-vim-fn
// SPDX-License-Identifier: GPL-2.0-or-later
//
// vim_keymap_common.c — shared keymap layer for the qmk-vim-fn engine.
// Design authority: qmk-vim-fn/vim/design.md §2.1, §4.9, §4.10, §4.12.
//
// vim_pipeline_process() is the single-source interception chain; every
// keyboard feeds process_record_user() into it.  The steps are explicitly
// named and ordered (design §4.12):
//
//   0  modifier shadow update (before anything may swallow a modifier)
//   1  Caps trigger (press = Caps mode immediately; bare tap = nothing; Fn+Caps = vim toggle)
//   2  Caps mode interception (caps/design.md: F-row / Ctrl+base, captures all)
//   3  cfg->hook_pre
//   4  myfn skeleton (layer exemption / undeclared swallow / dispatch)
//   5  cfg->hook_post_myfn
//   6  mouse-mode state machine
//   7  Shift+Esc (Insert only)
//   8  Esc toggle (Insert <-> Normal, with the escape grace window)
//   9  §2.1 shortcut table
//  10  vim_glue_engine (Esc falls straight through to the engine)

#include "qmk-vim-fn/qmk/vim_keymap_common.h"
#include "qmk-vim-fn/qmk/vim_glue.h"

// ==========================================================================
// Shared helpers
// ==========================================================================
uint16_t vim_timer_start(void) {
    uint16_t t = timer_read();
    return t ? t : 1; // guard against a zero reading disabling the timer
}

bool vim_timer_elapsed(uint16_t start, uint16_t ms) {
    return start != 0 && timer_elapsed(start) >= ms;
}

// 32-bit stamp/compare for windows that may go unchecked across the 16-bit wrap:
// QMK's timer_read() is (uint16_t)timer_read32(), so an *expired* window re-reads as
// "elapsed" again after 65536 ms (design §4.12: Esc grace window).
uint32_t vim_timer_start32(void) {
    uint32_t t = timer_read32();
    return t ? t : 1; // same zero-reading guard as the 16-bit helper
}

bool vim_timer_elapsed32(uint32_t start, uint32_t ms) {
    return start != 0 && timer_elapsed32(start) >= ms;
}

// Real QMK (quantum/keycodes.h) always provides these; the glue-test host stub
// may not, so keep the shared layer compilable against both.
#ifndef IS_QK_TO
#define IS_QK_TO(kc) false
#endif
#ifndef IS_QK_TOGGLE_LAYER
#define IS_QK_TOGGLE_LAYER(kc) false
#endif
#ifndef IS_QK_DEF_LAYER
#define IS_QK_DEF_LAYER(kc) false
#endif

bool vim_is_layer_key(uint16_t keycode) {
    return IS_QK_MOMENTARY(keycode) || IS_QK_LAYER_TAP(keycode) || IS_QK_LAYER_MOD(keycode) ||
           IS_QK_LAYER_TAP_TOGGLE(keycode) || IS_QK_ONE_SHOT_LAYER(keycode) ||
           IS_QK_TO(keycode) || IS_QK_TOGGLE_LAYER(keycode) || IS_QK_DEF_LAYER(keycode);
}

void send_plain_tap(uint16_t keycode) {
    uint8_t saved = get_mods();
    clear_mods();
    tap_code16(keycode);
    set_mods(saved);
}

// ==========================================================================
// Step 9 — §2.1 keyboard-layer shortcut table
// ==========================================================================
static void sc_left(void) { send_plain_tap(KC_LEFT); }
static void sc_right(void) { send_plain_tap(KC_RGHT); }
static void sc_up_home(void) {
    send_plain_tap(KC_UP);
    send_plain_tap(KC_HOME);
}
static void sc_down_home(void) {
    send_plain_tap(KC_DOWN);
    send_plain_tap(KC_HOME);
}
static void sc_find(void) { send_plain_tap(LCTL(KC_F)); }
static void sc_pgdn(void) { send_plain_tap(KC_PGDN); }
static void sc_pgup(void) { send_plain_tap(KC_PGUP); }

#define VIM_NO_CAG_MASK (MOD_MASK_CTRL | MOD_MASK_ALT | MOD_MASK_GUI)

const vim_shortcut_t vim_default_shortcuts[] = {
    {KC_BSPC, 0, VIM_NO_CAG_MASK, sc_left},
    {KC_SPC, 0, VIM_NO_CAG_MASK, sc_right},
    {KC_MINS, 0, VIM_NO_CAG_MASK | MOD_MASK_SHIFT, sc_up_home},
    {KC_EQL, MOD_MASK_SHIFT, VIM_NO_CAG_MASK | MOD_MASK_SHIFT, sc_down_home},
    {KC_SLSH, 0, VIM_NO_CAG_MASK | MOD_MASK_SHIFT, sc_find},
    {KC_F, MOD_MASK_CTRL, VIM_NO_CAG_MASK, sc_pgdn},
    {KC_B, MOD_MASK_CTRL, VIM_NO_CAG_MASK, sc_pgup},
    {0, 0, 0, NULL}, // terminator
};

// ==========================================================================
// Step 6 — mouse-mode state machine (design §4.9)
// ==========================================================================
static const vim_cfg_t *s_cfg;

static kv_mode_t s_mouse_entry_mode;
static uint16_t s_mouse_timer;  // trigger-key hold timer
static bool     s_mouse_held;   // trigger long-press -> modifier registered
static uint16_t s_mouse_mod_reg; // exact modifier keycode actually registered
static uint16_t s_lbtn_timer;   // Space hold timer
static bool     s_lbtn_held;    // Space long-press -> left button held

// Remember the keycode actually registered per movement key, so the release
// unregisters exactly that one even if Shift changes mid-hold (P0-2).
enum { MV_H = 0, MV_J, MV_K, MV_L, MV_COUNT };
static uint16_t s_move_reg[MV_COUNT];

static bool fn_layer_active(void) {
    return layer_state_cmp(layer_state | default_layer_state, s_cfg->fn_layer);
}

static uint16_t mouse_active_mod(void) {
    if (s_cfg->is_mac && s_cfg->is_mac()) return s_cfg->mod_mac;
    return s_cfg->mod_win;
}

static bool mouse_link_ok(void) { return s_cfg->link_ok ? s_cfg->link_ok() : true; }

// Single source of truth: MOUSE liveness is exactly the engine mode (A-P1-6).
static bool mouse_active(void) { return kv_get_mode() == KV_MODE_MOUSE; }

static void mouse_release_all(void) {
    for (int i = 0; i < MV_COUNT; i++) {
        if (s_move_reg[i]) {
            unregister_code(s_move_reg[i]);
            s_move_reg[i] = KC_NO;
        }
    }
    unregister_code(MS_BTN1); // harmless if it was only tapped
    unregister_code(MS_BTN2);
    s_lbtn_held  = false;
    s_lbtn_timer = 0;
}

static void mouse_enter(void) {
    if (!kv_vim_enabled()) return; // vim off: never enter MOUSE (design §4.9)
    s_mouse_entry_mode = kv_get_mode();
    vim_glue_release_all(); // held motion arrows must not stick (design §4.10)
    kv_set_mode(KV_MODE_MOUSE);
}

static void mouse_exit(void) {
    mouse_release_all();
    kv_set_mode(s_mouse_entry_mode); // back to the entry mode
    vim_glue_release_all();
}

// Returns true when the event is consumed by the mouse state machine.
static bool mouse_process(uint16_t keycode, keyrecord_t *record) {
    const bool pressed = record->event.pressed;

    // Trigger key: tap toggles mouse mode, hold = Win/Mac modifier.
    if (keycode == s_cfg->trigger_kc) {
        if (pressed) {
            s_mouse_timer = vim_timer_start();
            s_mouse_held  = false;
        } else {
            if (s_mouse_held) {
                // Unregister the exact keycode registered on long-press; is_mac()
                // may have flipped mid-hold (P2), so never recompute it here.
                if (s_mouse_mod_reg) unregister_code(s_mouse_mod_reg);
                s_mouse_mod_reg = KC_NO;
            } else if (s_mouse_timer) {
                if (mouse_active()) {
                    mouse_exit();
                } else if (mouse_link_ok()) {
                    mouse_enter();
                }
            }
            s_mouse_timer = 0;
            s_mouse_held  = false;
        }
        return true;
    }

    if (!mouse_active()) return false;

    // Modifier handling (design §4.9).  Shift stays in MOUSE so that Shift+J /
    // Shift+K can produce wheel-down / wheel-up; its press is paired (consumed)
    // and its release falls through so a Shift QMK registered before entering
    // MOUSE can still be unregistered.  Ctrl/Alt/GUI exit MOUSE on press and
    // are re-identified in the entry mode (the pipeline continues, so QMK
    // registers the modifier normally); their release then passes through.
    if (IS_MODIFIER_KEYCODE(keycode)) {
        if (pressed) {
            if (keycode == KC_LSFT || keycode == KC_RSFT) {
                vim_glue_swallow(keycode);
                return true;
            }
            mouse_exit(); // force-release mouse keys/pointer/wheel first
            return false; // re-identify the modifier in the entry mode
        }
        return false;
    }

    const bool shift = (vim_glue_mods() & MOD_MASK_SHIFT) != 0;

    if (pressed) {
        switch (keycode) {
            case KC_H:
                s_move_reg[MV_H] = MS_LEFT;
                register_code(MS_LEFT);
                vim_glue_swallow(keycode);
                return true;
            case KC_J:
                s_move_reg[MV_J] = shift ? MS_WHLD : MS_DOWN;
                register_code(s_move_reg[MV_J]);
                vim_glue_swallow(keycode);
                return true;
            case KC_K:
                s_move_reg[MV_K] = shift ? MS_WHLU : MS_UP;
                register_code(s_move_reg[MV_K]);
                vim_glue_swallow(keycode);
                return true;
            case KC_L:
                s_move_reg[MV_L] = MS_RGHT;
                register_code(MS_RGHT);
                vim_glue_swallow(keycode);
                return true;
            case KC_SPC:
                s_lbtn_timer = vim_timer_start();
                vim_glue_swallow(keycode);
                return true;
            case KC_ENT:
                register_code(MS_BTN2);
                vim_glue_swallow(keycode);
                return true;
            default:
                // Any other key leaves mouse mode; force-release every mouse
                // key and re-identify the key by continuing the pipeline.
                mouse_exit();
                return false;
        }
    }

    // Release: unregister the host mouse code, then do NOT consume; step 8's
    // shared pairing table consumes the paired release (design §4.10/§4.12).
    switch (keycode) {
        case KC_H:
            if (s_move_reg[MV_H]) {
                unregister_code(s_move_reg[MV_H]);
                s_move_reg[MV_H] = KC_NO;
            }
            return false;
        case KC_J:
            if (s_move_reg[MV_J]) {
                unregister_code(s_move_reg[MV_J]);
                s_move_reg[MV_J] = KC_NO;
            }
            return false;
        case KC_K:
            if (s_move_reg[MV_K]) {
                unregister_code(s_move_reg[MV_K]);
                s_move_reg[MV_K] = KC_NO;
            }
            return false;
        case KC_L:
            if (s_move_reg[MV_L]) {
                unregister_code(s_move_reg[MV_L]);
                s_move_reg[MV_L] = KC_NO;
            }
            return false;
        case KC_SPC:
            if (s_lbtn_held) {
                unregister_code(MS_BTN1);
                s_lbtn_held = false;
            } else if (s_lbtn_timer) {
                tap_code(MS_BTN1);
            }
            s_lbtn_timer = 0;
            return false;
        case KC_ENT:
            unregister_code(MS_BTN2);
            return false;
        default:
            return false;
    }
}

// ==========================================================================
// Step 7 — Shift+Esc (Insert only, design §4.10 / readme §4)
// ==========================================================================
static bool shift_esc_process(uint16_t keycode, keyrecord_t *record) {
    if (!kv_vim_enabled()) return false; // vim off: never hijack Shift+Esc
    if (!s_cfg->shift_esc_enable) return false;
    if (keycode != KC_ESC || !record->event.pressed) return false;
    if (kv_get_mode() != KV_MODE_INSERT) return false;

    uint8_t mods = vim_glue_mods();
    if (mods & (MOD_MASK_CTRL | MOD_MASK_ALT | MOD_MASK_GUI)) return false;

    if (mods & MOD_BIT_LSHIFT) {
        send_plain_tap(LSFT(KC_GRV)); // ~
        vim_glue_swallow(keycode);
        return true;
    }
    if (mods & MOD_BIT_RSHIFT) {
        send_plain_tap(KC_GRV); // `
        vim_glue_swallow(keycode);
        return true;
    }
    return false;
}

// ==========================================================================
// Step 8 — Esc toggle (Insert <-> Normal) + escape grace window
// ==========================================================================
//
// With vim on, Esc toggles typing <-> command:
//   Insert  -> swallow the Esc and drop into NORMAL (no host Esc);
//   Normal  -> emit the real Esc, return to INSERT, and open a short grace
//              window so a burst of Escapes (e.g. leaving a shell prompt)
//              stays a real Esc instead of re-entering NORMAL.
// The window is 3 s, is opened ONLY by this Normal -> Insert transition, and
// is reset by every in-window Esc.  Visual / pending-Normal / CAG Escapes are
// left to the engine and shortcut layers, unchanged.
#define VIM_ESC_GRACE_MS 3000
static uint32_t s_esc_grace; // 0 = no window; else vim_timer_start32() stamp (32-bit: no wrap)

static bool esc_process(uint16_t keycode, keyrecord_t *record) {
    if (keycode != KC_ESC || !record->event.pressed) return false;
    if (!kv_vim_enabled()) return false; // vim off: plain Esc

    uint8_t mods = vim_glue_mods();
    if (mods & (MOD_MASK_CTRL | MOD_MASK_ALT | MOD_MASK_GUI)) return false; // CAG
    kv_mode_t m = kv_get_mode();

    // Visual: the engine exits to NORMAL and emits nothing.
    if (m == KV_MODE_VISUAL || m == KV_MODE_VISUAL_LINE) return false;

    // NORMAL with a pending prefix/operator: the engine cancels it (no key).
    if (m == KV_MODE_NORMAL && kv_pending()) return false;

    if (m == KV_MODE_NORMAL) {
        // Normal idle: real Esc, back to typing, open the grace window.
        kv_set_mode(KV_MODE_INSERT);
        s_esc_grace = vim_timer_start32();
        return false; // pass -> host receives the real Esc
    }

    // INSERT.
    if (s_esc_grace && !vim_timer_elapsed32(s_esc_grace, VIM_ESC_GRACE_MS)) {
        s_esc_grace = vim_timer_start32(); // in-window Esc: real Esc, reset window
        return false;
    }
    // No window (entered Insert another way) or it expired: swallow, go NORMAL.
    s_esc_grace = 0;
    kv_cancel();
    kv_set_mode(KV_MODE_NORMAL);
    vim_glue_swallow(KC_ESC);
    return true;
}

// ==========================================================================
// ==========================================================================
// Caps 模块（规格 caps/design.md；行为 caps/readme.md；用例 caps/testcase.md）
//   Caps tap  = 键盘既有短按语义（本层不改动，通常为 vim 开关）
//   Caps hold = Caps 模式：1..0 - = -> F1..F12（不带 Ctrl）；其余键 -> Ctrl+base
//               松开 Caps 退出并反注册本模式注册过的全部键与 Ctrl（防卡键）
// ==========================================================================
#define CAPS_FKEY_COUNT 12
// 模式内实际注册过的键（有界表；溢出时该键仍会发出，仅退出时不保证被强制释放）
// held 表容量：必须 **< glue 的配对表容量（PAIR_CAP=16）**，否则同时按住的键会把 Caps 的
// 配对记录挤出配对表，导致 release 变成孤立 key-up（design §4.10）。12 已远超正常同时按键数。
#define CAPS_HELD_MAX 12

static bool     s_caps_armed;               // 本按下不是 Fn+Caps（走 Caps 模式语义）
static bool     s_caps_touched;             // 本次按下期间是否已按过其它键（决定快速抬起是否撤销）
static bool     s_caps_was_pressed;         // Caps 是否确实按下过（release 守卫，design §3.1-6）
static bool     s_caps_mode;                // 模式是否激活
static uint16_t s_caps_held[CAPS_HELD_MAX]; // 本模式注册过的键
static uint8_t  s_caps_held_n;
static uint8_t  s_caps_ctrl_n;              // 非 F 键按下的计数（Ctrl 引用计数）
static bool     s_caps_phys_ctrl;           // 进入时物理 Ctrl 是否已按住
static uint8_t  s_caps_ctrl_owned;          // 本模式注册过哪些 Ctrl 键码（LCTL/RCTL 位掩码）
#define CAPS_OWN_LCTL 0x1
#define CAPS_OWN_RCTL 0x2
static uint8_t  s_caps_phys_ctrl_held;      // 模式内**物理**按住的 Ctrl 键码位（退出时不得反注册）

// 1..0 - = -> F1..F12（传入 QMK 基础键码）
static uint16_t caps_fkey_of(uint16_t keycode) {
    switch (keycode) {
        case KC_1:    return KC_F1;
        case KC_2:    return KC_F2;
        case KC_3:    return KC_F3;
        case KC_4:    return KC_F4;
        case KC_5:    return KC_F5;
        case KC_6:    return KC_F6;
        case KC_7:    return KC_F7;
        case KC_8:    return KC_F8;
        case KC_9:    return KC_F9;
        case KC_0:    return KC_F10;
        case KC_MINS: return KC_F11;
        case KC_EQL:  return KC_F12;
        default:      return KC_NO;
    }
}

static void caps_held_reset(void) { s_caps_held_n = 0; }

static void caps_held_add(uint16_t keycode) {
    if (s_caps_held_n < CAPS_HELD_MAX) s_caps_held[s_caps_held_n++] = keycode;
}

static bool caps_held_remove(uint16_t keycode) {
    for (uint8_t i = 0; i < s_caps_held_n; i++) {
        if (s_caps_held[i] == keycode) {
            s_caps_held[i] = s_caps_held[--s_caps_held_n];
            return true;
        }
    }
    return false;
}

static void caps_mode_exit(void); // 前向声明：重入清理需要先退出上一实例

// 进入模式（caps/design.md §3）：不碰 vim 状态，也不重置引擎 —— 模式内的键不进引擎。
static void caps_mode_enter(void) {
    // 重入清理（caps/design.md §3.1-1）：上一实例的 release 可能丢失（或一次按抬被上报两次、
    // 两个物理键都映射 KC_CAPS）。若模式已激活，必须先把上一实例登记过的键与 Ctrl 反注册，
    // 否则 held 表被重置后它们永远无法反注册 —— 宿主键永久卡住。
    if (s_caps_mode) caps_mode_exit();
    s_caps_mode      = true;
    // 物理影子里 Ctrl 是否按住（影子在第 0 步更新，不受合成位影响）。
    s_caps_phys_ctrl = (vim_glue_mods() & (MOD_BIT(KC_LCTL) | MOD_BIT(KC_RCTL))) != 0;
    s_caps_ctrl_n    = 0;
    caps_held_reset();
}

// 退出模式：反注册本模式注册过的全部键（含仍按住的）与 Ctrl（caps/design.md §4.3）
static void caps_mode_exit(void) {
    if (!s_caps_mode) return;
    s_caps_mode = false;
    for (uint8_t i = 0; i < s_caps_held_n; i++) {
        const uint16_t k = s_caps_held[i];
        // 物理 Ctrl 仍按住的位不能反注册（第 3 轮对抗审核 C：否则退出会清掉物理位，
        // 真机 Ctrl 失效到重按）。它随后由 QMK 自己的 release 处理。
        if ((k == KC_LCTL || k == KC_RCTL) && s_caps_phys_ctrl_held) continue;
        unregister_code(k);
    }
    // 只反注册本模式**确实注册过**的 Ctrl，且按实际键码逐位处理（LCTL/RCTL 在真机是**不同 bit**）：
    // 从未注册过（进入时物理 Ctrl 已按住）就绝不能反注册，否则会卸掉物理按住（§4 不变量 2）。
    if (s_caps_ctrl_owned & CAPS_OWN_LCTL) unregister_code(KC_LCTL);
    if (s_caps_ctrl_owned & CAPS_OWN_RCTL) unregister_code(KC_RCTL);
    s_caps_ctrl_owned = 0;
    s_caps_phys_ctrl_held = 0;
    s_caps_ctrl_n     = 0;
    caps_held_reset();
    s_caps_phys_ctrl  = false;
}

// 模式内按键翻译（caps/design.md §4）：拦截在 myfn/鼠标/Esc/快捷键/引擎之前。
static bool caps_mode_process(uint16_t keycode, keyrecord_t *record) {
    if (!s_caps_mode) return false;

    // 层键豁免且放行（caps/design.md §3.1-4）：层键（MO/LT/LM/TT/OSL/TO/...）的低字节不是键位语义，
    // 绝不能按 `& 0xFF` 翻译成宿主键；不消费 press，使 Fn 层仍能正常激活（与 myfn 层键豁免一致）。
    if (vim_is_layer_key(keycode)) return false;
    // 非基础键码（自定义键 0x7E00+/QK_KB_*/厂商键码）的低字节不是键位语义：
    // 同层键一样豁免并放行，否则 Caps 模式内按它会在宿主发出无关的 Ctrl+低字节（第 3 轮 P3-V）。
    if ((keycode & 0xFF00) != 0) return false;

    const bool     pressed = record->event.pressed;
    // 按下期间夹了键 -> 抬起时不再撤销（caps/design.md §3）
    if (pressed) s_caps_touched = true;
    const uint16_t base    = (uint16_t)(keycode & 0xFF); // QMK 基础键码
    const uint16_t fkey    = caps_fkey_of(base);

    if (pressed) {
        // 溢出（caps/design.md §3.1-5）：held 表满时**既不注册、也不让键继续走后续流水线**
        // （否则会被快捷键表/引擎劫持 —— 第 3 轮对抗审核 K/O2：第 13 键按 Space 会发裸 →
        // 按 d 会执行 dd 删行）。做法：两沿都吞掉（press 记入配对表、release 由配对表消费），
        // 宿主什么也收不到，且绝不会出现"注册了但没记表"的卡键。
        if (s_caps_held_n >= CAPS_HELD_MAX) {
            if (pressed) vim_glue_swallow(keycode);
            return true;
        }
        if (fkey != KC_NO) {
            register_code(fkey); // F 区：不带 Ctrl（引用计数不变）
            caps_held_add(fkey);
        } else {
            // 物理 Ctrl 的按键（LCTL/RCTL）：由"真实按住的修饰键"承担，不参与合成引用计数，
            // 否则一次点按会让 ctrl_n 永久 +1（第 3 轮对抗审核 A/Y：F 区带 Ctrl、Ctrl 卡到退出）。
            if (base == KC_LCTL || base == KC_RCTL) {
                s_caps_phys_ctrl = true; // 物理 Ctrl 现在确实按住
                s_caps_phys_ctrl_held |= (base == KC_LCTL) ? CAPS_OWN_LCTL : CAPS_OWN_RCTL;
                register_code(base);     // 对称反注册在 release 分支按实际键码处理
                caps_held_add(base);
            } else {
                // 需要合成 Ctrl 的条件：物理位当前不可用（未按住或已松开）且本模式尚未注册过。
                if (!s_caps_phys_ctrl_held && !s_caps_ctrl_owned) {
                    register_code(KC_LCTL);
                    s_caps_ctrl_owned = CAPS_OWN_LCTL;
                }
                s_caps_ctrl_n++;
                register_code(base);
                caps_held_add(base);
            }
            // 含修饰键：Shift -> Ctrl+Shift（对称反注册，不会卸掉物理按住）
            register_code(base);
            caps_held_add(base);
        }
        vim_glue_swallow(keycode); // 消费 press，release 由配对表无条件消费
        return true;
    }

    // 物理 Ctrl 在模式内按/松（caps/design.md §3.1-3）：只更新 baseline，绝不改变引用计数，
    // 也绝不反注册本模式的 Ctrl 位（真机 Ctrl 是位图，误反注册会把合成位一起清掉）。
    if (base == KC_LCTL || base == KC_RCTL) {
        s_caps_phys_ctrl = false; // 物理位不可见 -> 下一个非 F 键按条件补注册（P0-2）
        s_caps_phys_ctrl_held &= (base == KC_LCTL) ? (uint8_t)~CAPS_OWN_LCTL : (uint8_t)~CAPS_OWN_RCTL;
        // 只反注册**本模式注册过**的 Ctrl 键码（第 3 轮 P0-S：RCTL 与 LCTL 是不同 bit，
        // 只清 LCTL 会永久卡住 RCTL）；物理按键若从未由本模式注册，则从 held 移除即可、不反注册。
        if (caps_held_remove(base)) unregister_code(base);
        return false; // 交配对表消费
    }

    // release 过滤（caps/design.md §3.1-2）：只有本实例注册过的键才反注册/改引用计数；
    // 否则（press 早于本次进入、或属于上一实例）只交配对表消费。
    const uint16_t sent = (fkey != KC_NO) ? fkey : base;
    if (!caps_held_remove(sent)) return false;
    unregister_code(sent);
    if (fkey == KC_NO && s_caps_ctrl_n) {
        s_caps_ctrl_n--;
        if (s_caps_ctrl_n == 0 && (s_caps_ctrl_owned & CAPS_OWN_LCTL)) {
            unregister_code(KC_LCTL);
            s_caps_ctrl_owned &= (uint8_t)~CAPS_OWN_LCTL;
        }
    }
    return false; // release 交配对表消费
}

static void set_vim_enabled(bool enabled) {
    s_esc_grace = 0; // an enable/disable transition invalidates the window
    if (s_cfg->vim_set_enabled) {
        s_cfg->vim_set_enabled(enabled);
    } else if (enabled) {
        kv_enable(); // always restarts in INSERT (design §4.7)
    } else {
        kv_disable();
    }
    // Mode/enable transition: no held arrow may stick (both paths).
    vim_glue_release_all();
}

// Caps 触发（caps/design.md §3）：
//   按下即进入 Caps 模式（不等 hold_ms）；按下期间未按其它键就抬起则撤销；
//   Fn 先按住 + Caps = 单击开关 vim；裸 Caps 单击无任何效果。
// 注意：本段先于 caps_mode_process 分发，使 Caps 的 release 仍能退出模式。
static bool caps_process(uint16_t keycode, keyrecord_t *record) {
    if (keycode != KC_CAPS) return false;

    if (record->event.pressed) {
        s_caps_touched    = false;
        s_caps_was_pressed = true;
        // Fn 在按下这一刻已激活 -> Fn+Caps（单击语义）；否则立即进入 Caps 模式。
        s_caps_armed = !fn_layer_active();
        // 重入清理（caps/design.md §3.1-1）：模式已激活时再次按下 Caps，必须先反注册
        // 上一实例登记过的键与 Ctrl。进入模式本身（caps_mode_enter）也会做一次清理，
        // 故"模式未激活"的情形无需在此重复处理。
        if (s_caps_mode) caps_mode_exit();
        if (s_caps_armed) caps_mode_enter();
        vim_glue_swallow(KC_CAPS); // Caps 始终被消费，永不作 Caps Lock
        return true;
    }

    // release 守卫（caps/design.md §3.1-6）：只有"确实按下过"的 release 才做开关 vim 的动作；
    // 孤立 release 不做。但**模式的退出/撤销必须照常执行**（否则模式会残留）。
    const bool had_press = s_caps_was_pressed;
    s_caps_was_pressed = false;

    if (s_caps_armed) {
        // 未夹键就抬起 = 撤销本次进入；夹过键 = 正常退出。两条路径都反注册本实例的键，
        // 差别只在于"未夹键时本来就没有已发出的键"（caps/design.md §3）。
        caps_mode_exit();
        s_caps_armed = false;
    } else if (had_press) {
        set_vim_enabled(!kv_vim_enabled()); // Fn+Caps 单击 = 开关 vim
    }
    return false; // press 交给配对表消费 release
}

// ==========================================================================
// Step 4 — myfn skeleton (fn readme §3)
// ==========================================================================
// Returns true when the key is consumed here.
//
// Declared keys are passed back to QMK unchanged (design §4.12 "已声明放行/
// 分发"): cfg->myfn() still runs on both edges for any per-key keyboard action,
// but ownership stays with QMK, so later pipeline steps and any keyboard's
// process_record_kb tail (e.g. other vendor keys) still see the
// key.  Undeclared keys (incl. modifiers) are swallowed on press
// (fn readme rule 3).
static bool myfn_process(uint16_t keycode, keyrecord_t *record) {
    if (vim_is_layer_key(keycode)) return false; // exempt: always pass to QMK

    if (!fn_layer_active()) return false;

    bool declared = s_cfg->myfn_declared && s_cfg->myfn_declared(keycode);

    if (!declared) {
        // Undeclared keys (incl. modifiers) are swallowed on press to keep
        // Fn+<mod>+<key> from leaking.  The press is registered in the shared
        // pairing table; the release is left to the pipeline so QMK can
        // unregister a modifier it registered before Fn went down (a swallowed
        // press is never registered, so its paired release is harmless).  This
        // fixes "Shift down -> Fn -> Shift up" leaving Shift stuck.
        if (record->event.pressed) {
            // 被 myfn 吞掉的键也是"非 vim 键"，必须作废可视模式已累积的输入（design §4.10）。
            if (kv_pending() || kv_visual_count_pending()) kv_visual_cancel();
            vim_glue_swallow(keycode);
            return true;
        }
        return false;
    }

    // Declared key: the keyboard decides per edge.  Returning true consumes
    // it (paired release swallowed via the shared table); false passes it to
    // QMK (e.g. F-keys, or vendor keys handled in the process_record_kb tail).
    bool consume = s_cfg->myfn ? s_cfg->myfn(keycode, record->event.pressed) : false;
    if (consume) {
        if (record->event.pressed) {
            vim_glue_swallow(keycode);
            return true;
        }
        return false; // release consumed by step 8's pairing table
    }
    return false; // pass to QMK
}

// ==========================================================================
// Step 9 — shortcut dispatch
// ==========================================================================
static bool shortcuts_process(uint16_t keycode, keyrecord_t *record) {
    if (!record->event.pressed) return false;
    if (!kv_vim_enabled() || kv_get_mode() != KV_MODE_NORMAL) return false;
    if (!s_cfg->shortcuts) return false;

    uint8_t mods = vim_glue_mods(); // physical shadow, not get_mods() (design §4.10)
    for (const vim_shortcut_t *s = s_cfg->shortcuts; s->base; s++) {
        if (keycode != s->base) continue;
        // `mods_req` names a *side-agnostic* mask (e.g. MOD_MASK_CTRL = LCTL|RCTL),
        // so compare on the masked, physically-held set: no requirement means no
        // masked modifier may be down, otherwise the held set must be a non-empty
        // subset of the requirement (either Ctrl / either Shift qualifies).
        uint8_t held = (uint8_t)(mods & s->mods_mask);
        bool    hit  = (s->mods_req == 0)
                           ? (held == 0)
                           : ((held & s->mods_req) != 0 && (held & (uint8_t)~s->mods_req) == 0);
        if (hit) {
            kv_cancel(); // drop any half-typed command first
            s->action();
            vim_glue_swallow(keycode); // consume the matching release too
            return true;
        }
    }
    return false;
}

// ==========================================================================
// Public pipeline / task / RGB
// ==========================================================================
// Keyboard hooks may consume a press; the shared layer then owns its release
// (design §4.10) and pairs it automatically, so a hook never needs to track
// key-up itself.  A hook's claim to consume a *release* is deliberately
// ignored: releases are always governed by the shared pairing table, so a
// stale release predicate (e.g. a keyboard hook checking get_mods()) can never
// strand a host key.
static bool hook_process(uint16_t keycode, keyrecord_t *record, bool (*hook)(uint16_t, keyrecord_t *)) {
    if (!hook) return false;
    if (!hook(keycode, record)) return false;
    if (record->event.pressed) {
        // 被键盘 hook（hook_pre/hook_post_myfn）消费的键也是"非 vim 键"，同样要作废可视输入
        // （design §4.10 第 4 条路径：QK61 的 CAD / Fn+Esc，NUT65 的 boot combo）。
        if (kv_pending() || kv_visual_count_pending()) kv_visual_cancel();
        vim_glue_swallow(keycode);
        return true;
    }
    return false; // release: fall through to the shared pairing table
}

static bool vim_dispatch(uint16_t keycode, keyrecord_t *record, const vim_cfg_t *cfg) {
    // 0 — physical modifier shadow (must precede every swallow).
    vim_glue_mod_update(keycode, record->event.pressed);

    // 1 — Caps tap/hold.  Runs before the Caps-mode interception so the Caps
    // release still exits the mode (caps/design.md §5).
    if (caps_process(keycode, record)) return false;

    // 2 — Caps 模式拦截（caps/design.md §5）：模式内接管一切按键，先于 myfn/鼠标/
    // Esc/快捷键/引擎，因此模式内 Esc = Ctrl+Esc，不会触发 vim 的 Esc 切换。
    if (caps_mode_process(keycode, record)) return false;

    // 3 — keyboard pre-hook (high-priority keyboard combos).
    if (hook_process(keycode, record, cfg->hook_pre)) return false;

    // 4 — myfn skeleton.
    if (myfn_process(keycode, record)) return false;

    // 5 — keyboard post-myfn hook (per-key keyboard actions).
    if (hook_process(keycode, record, cfg->hook_post_myfn)) return false;

    // 6 — mouse mode.
    if (mouse_process(keycode, record)) return false;

    // 7 — Shift+Esc.
    if (shift_esc_process(keycode, record)) return false;

    // 8 — Esc toggle (opens/resets the escape grace window on Normal->Insert).
    if (esc_process(keycode, record)) return false;

    // 9 — §2.1 shortcuts.
    if (shortcuts_process(keycode, record)) return false;

    // 10 — engine (Esc falls straight through here; no keyboard Esc branch).
    return vim_glue_engine(keycode, record);
}

bool vim_pipeline_process(uint16_t keycode, keyrecord_t *record, const vim_cfg_t *cfg) {
    s_cfg = cfg;

    // The escape grace window only exists while typing; any key observed outside
    // INSERT invalidates it (esc_process re-opens it on Normal->Insert).
    if (kv_get_mode() != KV_MODE_INSERT) s_esc_grace = 0;

    return vim_dispatch(keycode, record, cfg);
}

void vim_keymap_common_init(void) {
    // Reset the shared-layer static state.  vim_glue_init() resets the engine +
    // glue; the rest is owned here (mouse FSM, Caps tap/hold, escape grace).
    s_cfg             = NULL;
    s_mouse_entry_mode = KV_MODE_INSERT;
    s_mouse_timer      = 0;
    s_mouse_held       = false;
    s_mouse_mod_reg    = KC_NO;
    s_lbtn_timer       = 0;
    s_lbtn_held        = false;
    for (int i = 0; i < MV_COUNT; i++) s_move_reg[i] = KC_NO;
    s_caps_armed       = false;
    s_caps_touched     = false;
    s_caps_was_pressed = false;
    s_caps_mode        = false;
    s_caps_held_n      = 0;
    s_caps_ctrl_n      = 0;
    s_caps_phys_ctrl   = false;
    s_caps_ctrl_owned  = 0;
    s_caps_phys_ctrl_held = 0;
    s_esc_grace        = 0;
    vim_glue_init();
}

void vim_keymap_common_task(uint32_t now_ms) {
    vim_glue_task(now_ms);

    if (!s_cfg) return;

    // Caps 模式现在在按下瞬间进入（caps/design.md §3），此处不再需要 hold_ms 判定。

    // Trigger-key long press: register the Win/Mac modifier and remember the
    // exact code so the release unregisters the same one even if is_mac() flips.
    if (s_mouse_timer && !s_mouse_held && vim_timer_elapsed(s_mouse_timer, s_cfg->hold_ms)) {
        s_mouse_held    = true;
        s_mouse_mod_reg = mouse_active_mod();
        register_code(s_mouse_mod_reg);
    }

    // Space long press inside mouse mode: hold the left button (drag).
    if (mouse_active() && s_lbtn_timer && !s_lbtn_held && vim_timer_elapsed(s_lbtn_timer, s_cfg->hold_ms)) {
        s_lbtn_held = true;
        register_code(MS_BTN1);
    }
}

uint16_t vim_rgb_led_index(void) { return s_cfg ? s_cfg->led_index : 0; }

void vim_rgb_state_color(bool enabled, kv_mode_t m, bool pending, bool mouse, uint8_t *r, uint8_t *g, uint8_t *b) {    // MOUSE cyan wins over the "vim off" red (design §4.9).
    if (mouse) {
        *r = 0x00; *g = 0xFF; *b = 0xFF; // cyan: mouse mode
        return;
    }
    if (!enabled) {
        *r = 0xFF; *g = 0x00; *b = 0x00; // red: vim off
        return;
    }
    switch (m) {
        case KV_MODE_VISUAL:
            // pending never overrides Visual
            *r = 0x80; *g = 0x00; *b = 0x80; // purple
            break;
        case KV_MODE_VISUAL_LINE:
            // 行选独立配色（design §4.12：七色），与字符选的紫区分
            *r = 0xFF; *g = 0x00; *b = 0x80; // rose（洋红）
            break;
        case KV_MODE_NORMAL:
            if (pending) {
                *r = 0xFF; *g = 0xFF; *b = 0x00; // yellow: pending
            } else {
                *r = 0x00; *g = 0x00; *b = 0xFF; // blue
            }
            break;
        case KV_MODE_INSERT:
        default:
            *r = 0x00; *g = 0xFF; *b = 0x00; // green
            break;
    }
}

bool vim_insert_flash(void) {
    // Design §4.12: true iff vim on + mode INSERT + the Esc grace window is still
    // open.  That window is opened by exactly one event — esc_process()'s
    // "idle-Normal Esc -> INSERT" — is restarted by an in-window Esc, and is
    // dropped by vim_pipeline_process() as soon as the mode leaves INSERT.  So it
    // is precisely "this INSERT came from an idle-Normal Esc, less than 3 s ago".
    if (!kv_vim_enabled()) return false;             // vim off: mode colour is red
    if (kv_get_mode() != KV_MODE_INSERT) return false;
    return s_esc_grace != 0 && !vim_timer_elapsed32(s_esc_grace, VIM_ESC_GRACE_MS);
}

bool vim_insert_flash_color(uint8_t *r, uint8_t *g, uint8_t *b) {
    // design §4.12: the shared layer owns the whole contract — predicate AND the
    // cfg->insert_flash_color decision (0 = do not override).  The keyboard only
    // supplies the colour value and decides which LEDs to repaint.
    if (!vim_insert_flash()) return false;
    if (!s_cfg || s_cfg->insert_flash_color == 0) return false;
    if (r) *r = (uint8_t)((s_cfg->insert_flash_color >> 16) & 0xFF);
    if (g) *g = (uint8_t)((s_cfg->insert_flash_color >> 8) & 0xFF);
    if (b) *b = (uint8_t)(s_cfg->insert_flash_color & 0xFF);
    return true;
}
