/* kv_kc.h — keycode types mirroring QMK's 16-bit layout (QMK-agnostic).
 *
 * Basic keycodes occupy the low byte (HID usage id); modifier bits occupy
 * bits 8..12, exactly like QMK's QK_* / LCTL() / LSFT() encoding.  This lets
 * the glue layer pass keycodes straight through to QMK without translation.
 */
#ifndef KV_KC_H
#define KV_KC_H

#include <stdint.h>
#include <stdbool.h>

typedef uint16_t kv_keycode_t;

#define KV_NO 0x0000

/* --- basic HID usage ids (low byte) --- */
#define KV_A 0x04
#define KV_B 0x05
#define KV_C 0x06
#define KV_D 0x07
#define KV_E 0x08
#define KV_F 0x09
#define KV_G 0x0A
#define KV_H 0x0B
#define KV_I 0x0C
#define KV_J 0x0D
#define KV_K 0x0E
#define KV_L 0x0F
#define KV_M 0x10
#define KV_N 0x11
#define KV_O 0x12
#define KV_P 0x13
#define KV_Q 0x14
#define KV_R 0x15
#define KV_S 0x16
#define KV_T 0x17
#define KV_U 0x18
#define KV_V 0x19
#define KV_W 0x1A
#define KV_X 0x1B
#define KV_Y 0x1C
#define KV_Z 0x1D

#define KV_1 0x1E
#define KV_2 0x1F
#define KV_3 0x20
#define KV_4 0x21
#define KV_5 0x22
#define KV_6 0x23
#define KV_7 0x24
#define KV_8 0x25
#define KV_9 0x26
#define KV_0 0x27

#define KV_ENT 0x28
#define KV_ESC 0x29
#define KV_BSPC 0x2A
#define KV_TAB 0x2B
#define KV_SPC 0x2C
#define KV_MINS 0x2D
#define KV_EQL 0x2E
#define KV_LBRC 0x2F
#define KV_RBRC 0x30
#define KV_BSLS 0x31
#define KV_NUHS 0x32
#define KV_SCLN 0x33
#define KV_QUOT 0x34
#define KV_GRV 0x35
#define KV_COMM 0x36
#define KV_DOT 0x37
#define KV_SLSH 0x38
#define KV_CAPS 0x39

#define KV_INS 0x49
#define KV_HOME 0x4A
#define KV_PGUP 0x4B
#define KV_DEL 0x4C
#define KV_END 0x4D
#define KV_PGDN 0x4E
#define KV_RGHT 0x4F
#define KV_LEFT 0x50
#define KV_DOWN 0x51
#define KV_UP 0x52

/* --- modifier keycodes --- */
#define KV_LCTL 0xE0
#define KV_LSFT 0xE1
#define KV_LALT 0xE2
#define KV_LGUI 0xE3
#define KV_RCTL 0xE4
#define KV_RSFT 0xE5
#define KV_RALT 0xE6
#define KV_RGUI 0xE7

/* --- modifier bits (bits 8..12), QMK-compatible --- */
#define KV_MOD_LCTL 0x0100
#define KV_MOD_LSFT 0x0200
#define KV_MOD_LALT 0x0400
#define KV_MOD_LGUI 0x0800
#define KV_MOD_RCTL 0x1100
#define KV_MOD_RSFT 0x1200
#define KV_MOD_RALT 0x1400
#define KV_MOD_RGUI 0x1800
#define KV_MOD_MASK 0x1F00

#define KV_BASIC(kc) ((kc) & 0x00FF)
#define KV_MODS(kc) ((kc) & KV_MOD_MASK)

/* --- mod-wrapped keycode constructors --- */
#define KV_LCTL_KC(kc) ((kv_keycode_t)((kc) | KV_MOD_LCTL))
#define KV_LSFT_KC(kc) ((kv_keycode_t)((kc) | KV_MOD_LSFT))
#define KV_CS(kc) ((kv_keycode_t)((kc) | KV_MOD_LCTL | KV_MOD_LSFT))

/* --- shifted command keycodes (what the keymap feeds the engine) --- */
#define KV_C_G KV_LSFT_KC(KV_G) /* 'G' */
#define KV_C_S KV_LSFT_KC(KV_S) /* 'S' */
#define KV_C_X KV_LSFT_KC(KV_X) /* 'X' */
#define KV_C_C KV_LSFT_KC(KV_C) /* 'C' */
#define KV_C_D KV_LSFT_KC(KV_D) /* 'D' */
#define KV_C_Y KV_LSFT_KC(KV_Y) /* 'Y' */
#define KV_C_P KV_LSFT_KC(KV_P) /* 'P' */
#define KV_C_J KV_LSFT_KC(KV_J) /* 'J' */
#define KV_C_W KV_LSFT_KC(KV_W) /* 'W' */
#define KV_C_B KV_LSFT_KC(KV_B) /* 'B' */
#define KV_C_E KV_LSFT_KC(KV_E) /* 'E' */
#define KV_C_V KV_LSFT_KC(KV_V) /* 'V' */
#define KV_C_I KV_LSFT_KC(KV_I) /* 'I' */
#define KV_C_A KV_LSFT_KC(KV_A) /* 'A' */
#define KV_C_O KV_LSFT_KC(KV_O) /* 'O' */
#define KV_C_Z KV_LSFT_KC(KV_Z) /* 'Z' */
#define KV_C_LT KV_LSFT_KC(KV_COMM) /* '<' */
#define KV_C_GT KV_LSFT_KC(KV_DOT)  /* '>' */
#define KV_C_DLR KV_LSFT_KC(KV_4)   /* '$' */
#define KV_C_CARET KV_LSFT_KC(KV_6) /* '^' */

#endif /* KV_KC_H */
