/* qmk_stub.h — minimal QMK host stub for testing the shared keymap layer
 * (qmk/vim_glue.c + qmk/vim_keymap_common.c) without a keyboard build.
 *
 * Test-only.  Not part of the engine or the firmware.  The stub mirrors the
 * QMK API surface the shared layer touches; keycode values come from the
 * engine's kv_kc.h so the engine classifies them identically to firmware. */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

#include "qmk-vim-fn/engine/include/kv_kc.h"

#define KC_NO 0x0000

/* ---- keyrecord ---- */
typedef struct { uint8_t row, col; } keypos_t;
typedef struct { bool pressed; keypos_t key; } keyevent_t;
typedef struct { keyevent_t event; } keyrecord_t;

/* ---- basic keycodes (values == QMK / kv_kc.h) ---- */
#define KC_A KV_A
#define KC_B KV_B
#define KC_C KV_C
#define KC_D KV_D
#define KC_E KV_E
#define KC_F KV_F
#define KC_G KV_G
#define KC_H KV_H
#define KC_I KV_I
#define KC_J KV_J
#define KC_K KV_K
#define KC_L KV_L
#define KC_O KV_O
#define KC_P KV_P
#define KC_S KV_S
#define KC_T KV_T
#define KC_V KV_V
#define KC_W KV_W
#define KC_X KV_X
#define KC_Y KV_Y
#define KC_Z KV_Z

#define KC_1 KV_1
#define KC_2 KV_2
#define KC_3 KV_3
#define KC_4 KV_4
#define KC_6 KV_6
#define KC_0 KV_0

#define KC_ENT KV_ENT
#define KC_ESC KV_ESC
#define KC_BSPC KV_BSPC
#define KC_TAB KV_TAB
#define KC_SPC KV_SPC
#define KC_MINS KV_MINS
#define KC_EQL KV_EQL
#define KC_SCLN KV_SCLN
#define KC_QUOT KV_QUOT
#define KC_GRV KV_GRV
#define KC_COMM KV_COMM
#define KC_DOT KV_DOT
#define KC_SLSH KV_SLSH
#define KC_CAPS KV_CAPS

#define KC_HOME KV_HOME
#define KC_PGUP KV_PGUP
#define KC_DEL KV_DEL
#define KC_END KV_END
#define KC_PGDN KV_PGDN
#define KC_RGHT KV_RGHT
#define KC_RIGHT KV_RGHT
#define KC_LEFT KV_LEFT
#define KC_DOWN KV_DOWN
#define KC_UP KV_UP

#define KC_LCTL KV_LCTL
#define KC_LSFT KV_LSFT
#define KC_LALT KV_LALT
#define KC_LGUI KV_LGUI
#define KC_RCTL KV_RCTL
#define KC_RSFT KV_RSFT
#define KC_RALT KV_RALT
#define KC_RGUI KV_RGUI

/* F-keys / volume: distinct values outside the layer ranges */
#define KC_F1 0x3A
#define KC_F2 0x3B
#define KC_F3 0x3C
#define KC_F4 0x3D
#define KC_F5 0x3E
#define KC_F6 0x3F
#define KC_F7 0x40
#define KC_F8 0x41
#define KC_F9 0x42
#define KC_F10 0x43
#define KC_F11 0x44
#define KC_F12 0x45
#define KC_VOLD 0x80
#define KC_VOLU 0x81

/* mouse keycodes (distinct) */
#define MS_LEFT 0xF0
#define MS_RGHT 0xF1
#define MS_DOWN 0xF2
#define MS_UP 0xF3
#define MS_BTN1 0xF4
#define MS_BTN2 0xF5
#define MS_WHLU 0xF6
#define MS_WHLD 0xF7

/* custom keyboard keycode used as the mouse trigger */
#define QK_KB_22 0x7E16

/* ---- modifier bits ---- */
#define MOD_BIT_LCTRL 0x01
#define MOD_BIT_LSHIFT 0x02
#define MOD_BIT_LALT 0x04
#define MOD_BIT_LGUI 0x08
#define MOD_BIT_RCTRL 0x10
#define MOD_BIT_RSHIFT 0x20
#define MOD_BIT_RALT 0x40
#define MOD_BIT_RGUI 0x80
#define MOD_MASK_CTRL (MOD_BIT_LCTRL | MOD_BIT_RCTRL)
#define MOD_MASK_SHIFT (MOD_BIT_LSHIFT | MOD_BIT_RSHIFT)
#define MOD_MASK_ALT (MOD_BIT_LALT | MOD_BIT_RALT)
#define MOD_MASK_GUI (MOD_BIT_LGUI | MOD_BIT_RGUI)

#define IS_MODIFIER_KEYCODE(code) ((code) >= KC_LCTL && (code) <= KC_RGUI)
#define MOD_BIT(code) (uint8_t)(1u << ((code) - KC_LCTL))

/* ---- layer keycodes (stub ranges) ---- */
#define QK_MOMENTARY 0x5200
#define QK_LAYER_TAP 0x4000
#define QK_LAYER_MOD 0x5000
#define QK_LAYER_TAP_TOGGLE 0x5800
#define QK_ONE_SHOT_LAYER 0x5280
#define MO(layer) (uint16_t)(QK_MOMENTARY | (layer))
#define LT(layer, kc) (uint16_t)(QK_LAYER_TAP | ((layer) << 8) | ((kc) & 0xFF))
#define LM(layer, mod) (uint16_t)(QK_LAYER_MOD | ((layer) << 8) | ((mod) & 0xFF))
#define TT(layer) (uint16_t)(QK_LAYER_TAP_TOGGLE | (layer))
#define OSL(layer) (uint16_t)(QK_ONE_SHOT_LAYER | (layer))

#define IS_QK_MOMENTARY(kc) (((kc) & 0xFF00) == QK_MOMENTARY)
#define IS_QK_LAYER_TAP(kc) (((kc) & 0xF000) == QK_LAYER_TAP)
#define IS_QK_LAYER_MOD(kc) (((kc) & 0xF000) == QK_LAYER_MOD)
#define IS_QK_LAYER_TAP_TOGGLE(kc) (((kc) & 0xFF00) == QK_LAYER_TAP_TOGGLE)
#define IS_QK_ONE_SHOT_LAYER(kc) (((kc) & 0xFF00) == QK_ONE_SHOT_LAYER)

/* mod-wrap constructors */
#define LSFT(kc) (uint16_t)((kc) | MOD_BIT_LSHIFT << 8)
#define LCTL(kc) (uint16_t)((kc) | MOD_BIT_LCTRL << 8)

/* ---- layer state ---- */
typedef uint32_t layer_state_t;
extern layer_state_t layer_state;
extern layer_state_t default_layer_state;
#define layer_state_cmp(state, layer) (((state) & (1UL << (layer))) != 0)

/* ---- host API ---- */
uint8_t get_mods(void);
void    clear_mods(void);
void    set_mods(uint8_t mods);
void    register_mods(uint8_t mods);
void    unregister_mods(uint8_t mods);
void    register_code(uint16_t keycode);
void    unregister_code(uint16_t keycode);
void    tap_code(uint16_t keycode);
void    tap_code16(uint16_t keycode);
uint16_t timer_read(void);
uint16_t timer_elapsed(uint16_t since);
