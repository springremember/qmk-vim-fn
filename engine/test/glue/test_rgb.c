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
#include "qmk-vim-fn/engine/src/emit.h"   /* kv_emit_flush_now（队列式 emit 的冲刷） */
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

static uint32_t g_now;
uint16_t timer_read(void) { return (uint16_t)g_now; }
uint16_t timer_elapsed(uint16_t since) { return (uint16_t)((uint16_t)g_now - since); }
uint32_t timer_read32(void) { return g_now; }
uint32_t timer_elapsed32(uint32_t since) { return g_now - since; }

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
    .insert_flash_color = 0xFF8000, /* 与两键盘一致；改 cfg 必须能改变 vim_insert_flash_color() 输出 */
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

/* Caps 模块（caps/testcase.md）：观察 host 实际注册了哪些键。 */
static bool sim_held(uint16_t kc) {
    for (int i = 0; i < s_reg_n; i++)
        if (s_reg[i] == kc) return true;
    return false;
}
static bool sim_ctrl_held(void) { return sim_held(KC_LCTL); }
static int sim_arrow_count(void) {
    int n = 0;
    for (int i = 0; i < s_reg_n; i++)
        if (s_reg[i] == KC_DOWN || s_reg[i] == KC_UP || s_reg[i] == KC_LEFT || s_reg[i] == KC_RGHT) n++;
    return n;
}

static void reset_engine(void) {
    g_now = 1000;
    s_mods = 0;
    s_reg_n = 0;
    layer_state = 0;
    default_layer_state = 0;
    vim_keymap_common_init(); /* shared statics + kv_init/enable/INSERT */
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

/* Reach NORMAL from the real input path: Esc in INSERT with no grace window is
 * swallowed and drops to NORMAL, leaving the release paired. */
static void enter_normal(void) {
    CHECK(pipeline(KC_ESC, true) == false);
    CHECK(pipeline(KC_ESC, false) == false);
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

/* Branches: VISUAL -> purple; VISUAL_LINE -> rose #FF0080（行选独立颜色，design §4.12）。 */
static void test_rgb_visual_purple(void) {
    reset_engine();
    enter_normal();
    CHECK(pipeline(KC_V, true) == false); /* 'v' -> Visual */
    CHECK(kv_get_mode() == KV_MODE_VISUAL);
    CHECK(!kv_pending());
    CHECK_RGB(color_from_engine(), 0x80, 0x00, 0x80); /* 紫 */
    (void)pipeline(KC_V, false);

    reset_engine();
    enter_normal();
    CHECK(pipeline(KC_LSFT, true) == true); /* shadow Shift */
    CHECK(pipeline(KC_V, true) == false);   /* Shift+'v' -> Visual-Line */
    CHECK(kv_get_mode() == KV_MODE_VISUAL_LINE);
    CHECK(!kv_pending());
    CHECK_RGB(color_from_engine(), 0xFF, 0x00, 0x80); /* 洋红 rose */
    (void)pipeline(KC_V, false);
    (void)pipeline(KC_LSFT, false);
}

/* Branch: pending never overrides Visual / Visual-Line.  The engine never
 * leaves a pending flag set while in Visual (feed_visual swallows illegal
 * keys without entering a pending state), so this precedence is only
 * reachable through the RGB API itself. */
static void test_rgb_visual_pending_stays_purple(void) {
    CHECK_RGB(color_raw(true, KV_MODE_VISUAL, true, false), 0x80, 0x00, 0x80);      /* 紫 */
    CHECK_RGB(color_raw(true, KV_MODE_VISUAL_LINE, true, false), 0xFF, 0x00, 0x80); /* 洋红 rose，仍不被 pending 覆盖 */
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
    CHECK_RGB(color_raw(true, KV_MODE_VISUAL_LINE, true, true), 0x00, 0xFF, 0xFF); /* 紫红也让位给青 */
}

/* vim_insert_flash() — 规格见 design.md §4.12 / readme.md §10：
 * 真 ⟺ vim 开 + 模式 INSERT + Esc 宽限窗口（3000ms）未过期；
 * 该窗口只由「Normal 空闲 Esc → INSERT」开启、窗口内 Esc 重置、离开 INSERT 即失效。
 *
 * 各分支用 kv_set_mode() 直接摆位，避免与 Esc 状态机自身的路径互相纠缠
 * （Esc 切换语义已有 enter_normal()/test_esc* 覆盖）。 */
static void test_insert_flash(void) {
    /* 开机：INSERT、vim 开、无窗口 -> 假 */
    reset_engine();
    CHECK(kv_get_mode() == KV_MODE_INSERT);
    CHECK(!vim_insert_flash());

    /* 窗口开启：Normal 空闲 Esc -> 真 Esc + INSERT，判据为真 */
    kv_set_mode(KV_MODE_NORMAL);
    CHECK(!vim_insert_flash());            /* NORMAL 下窗口无意义 -> 假 */
    CHECK(pipeline(KC_ESC, true) == true); /* 真 Esc 透传 */
    CHECK(kv_get_mode() == KV_MODE_INSERT);
    CHECK(vim_insert_flash());
    (void)pipeline(KC_ESC, false);
    CHECK(vim_insert_flash());

    g_now += 2999;
    vim_keymap_common_task(g_now); /* 例行任务不得清掉窗口 */
    CHECK(vim_insert_flash());

    g_now += 1; /* 恰好 3000ms：过期 */
    CHECK(!vim_insert_flash());

    /* 窗口内 Esc 重置计时（橙色续期） */
    g_now += 10000;
    kv_set_mode(KV_MODE_NORMAL);
    CHECK(pipeline(KC_ESC, true) == true); /* t0 */
    (void)pipeline(KC_ESC, false);
    g_now += 2999;
    CHECK(pipeline(KC_ESC, true) == true); /* 窗口内：真 Esc，重置窗口 */
    (void)pipeline(KC_ESC, false);
    CHECK(vim_insert_flash());
    g_now += 2999;
    CHECK(vim_insert_flash()); /* t0+5998 仍在（窗口已重置） */
    g_now += 1;
    CHECK(!vim_insert_flash());

    /* 其它 Insert 入口不开窗口：引擎 'i' */
    kv_set_mode(KV_MODE_NORMAL);
    CHECK(pipeline(KC_I, true) == false); /* 'i' -> INSERT */
    CHECK(kv_get_mode() == KV_MODE_INSERT);
    CHECK(!vim_insert_flash());
    (void)pipeline(KC_I, false);

    /* 离开 INSERT 立即失效：窗口仍开着但模式切到 VISUAL / NORMAL */
    g_now += 10000;
    kv_set_mode(KV_MODE_NORMAL);
    CHECK(pipeline(KC_ESC, true) == true); /* 开窗 */
    CHECK(vim_insert_flash());
    (void)pipeline(KC_ESC, false);
    kv_set_mode(KV_MODE_NORMAL);
    CHECK(!vim_insert_flash());            /* NORMAL 不亮 */
    CHECK(pipeline(KC_V, true) == false); /* 'v' -> VISUAL */
    CHECK(kv_get_mode() == KV_MODE_VISUAL);
    CHECK(!vim_insert_flash());            /* VISUAL 不亮 */
    (void)pipeline(KC_V, false);

    /* vim 关闭：即使窗口戳还在也为假 */
    g_now += 10000;
    kv_set_mode(KV_MODE_NORMAL);
    CHECK(pipeline(KC_ESC, true) == true);
    CHECK(vim_insert_flash());
    kv_disable();
    CHECK(!kv_vim_enabled());
    CHECK(!vim_insert_flash());

    /* 收尾：复位引擎；重启/首次进入 Insert 不得带出提示色 */
    reset_engine();
    CHECK(vim_insert_flash() == false);
}

/* 审计 P1 回归：Esc 宽限窗口不得因 16 位计时回绕「复活」。
 * QMK 的 timer_read() 是 (uint16_t)timer_read32()，65536ms 后 elapsed 回绕为 0；
 * 窗口戳必须走 32 位，否则持续在 Insert 打字时每 65.5s 会假命中 3s。 */
static void test_insert_flash_wraparound(void) {
    reset_engine();               /* INSERT、无窗口 */
    g_now = 1000;                 /* 明确基准，避免上一用例的时间残留 */
    kv_set_mode(KV_MODE_NORMAL);
    CHECK(pipeline(KC_ESC, true) == true); /* t0=1000 开窗 */
    CHECK(vim_insert_flash());
    (void)pipeline(KC_ESC, false);

    g_now = 1000 + 65000;         /* 16 位窗口在 65536 处回绕之前应已过期 */
    CHECK(!vim_insert_flash());

    g_now = 1000 + 65536;         /* 恰好回绕：修复前这里会重新为真 */
    CHECK(!vim_insert_flash());
    g_now = 1000 + 68535;         /* 回绕后 2999ms：仍必须为假 */
    CHECK(!vim_insert_flash());

    /* 回绕后（窗口已过期）Insert 下 Esc 必须仍吞键进 Normal，不得因回绕变成真实 Esc */
    CHECK(kv_get_mode() == KV_MODE_INSERT);
    CHECK(pipeline(KC_ESC, true) == false);
    CHECK(kv_get_mode() == KV_MODE_NORMAL);
    (void)pipeline(KC_ESC, false);

    /* 复位并把桩的时间基准交回默认值（后续用例从 reset_engine 的 1000 起算） */
    reset_engine();
    CHECK(!vim_insert_flash());
}

/* vim_insert_flash_color()：判据与 cfg->insert_flash_color 的联合裁决（design §4.12）。
 * 色值 0 = 不覆盖；非零时拆出 0xRRGGBB 分量。 */
static void test_insert_flash_color(void) {
    uint8_t r = 0xAA, g = 0xBB, b = 0xCC;
    static vim_cfg_t cfg_none;  /* insert_flash_color == 0 */
    static vim_cfg_t cfg_odd;   /* 另一个非零色值 */

    reset_engine();
    /* 无窗口：即使色值非零也不覆盖 */
    r = 0xAA; g = 0xBB; b = 0xCC;
    CHECK(!vim_insert_flash_color(&r, &g, &b));
    CHECK(r == 0xAA && g == 0xBB && b == 0xCC); /* 未命中不得改动输出 */

    /* 开窗：命中判据，拆出 cfg 里的 0xFF8000 */
    kv_set_mode(KV_MODE_NORMAL);
    CHECK(pipeline(KC_ESC, true) == true);
    (void)pipeline(KC_ESC, false);
    r = 0; g = 0; b = 0;
    CHECK(vim_insert_flash_color(&r, &g, &b));
    CHECK(r == 0xFF && g == 0x80 && b == 0x00);

    /* 色值 0 = 不覆盖：判据仍真，但 helper 返回 false 且不动输出 */
    cfg_none          = g_cfg;
    cfg_none.insert_flash_color = 0;
    (void)pipeline_cfg(KC_Z, true, &cfg_none); /* 让 s_cfg 指向 cfg_none */
    r = 0x11; g = 0x22; b = 0x33;
    CHECK(vim_insert_flash() == true);         /* 窗口判据本身不受色值影响 */
    CHECK(!vim_insert_flash_color(&r, &g, &b));
    CHECK(r == 0x11 && g == 0x22 && b == 0x33);

    /* 换个非零色值：输出必须跟着 cfg 走（证明读的是 cfg，不是局部宏） */
    cfg_odd          = g_cfg;
    cfg_odd.insert_flash_color = 0x123456;
    (void)pipeline_cfg(KC_Z, true, &cfg_odd);
    r = 0; g = 0; b = 0;
    CHECK(vim_insert_flash_color(&r, &g, &b));
    CHECK(r == 0x12 && g == 0x34 && b == 0x56);

    /* 离开 Insert / vim 关：helper 与判据一起变假 */
    kv_set_mode(KV_MODE_NORMAL);
    r = 0x11; g = 0x22; b = 0x33;
    CHECK(!vim_insert_flash_color(&r, &g, &b));
    CHECK(r == 0x11 && g == 0x22 && b == 0x33);
    kv_set_mode(KV_MODE_INSERT);
    kv_disable();
    r = 0x11; g = 0x22; b = 0x33;
    CHECK(!vim_insert_flash_color(&r, &g, &b));
    CHECK(r == 0x11 && g == 0x22 && b == 0x33);

    /* 复位并把 s_cfg 交回默认实例 */
    reset_engine();
    (void)pipeline(KC_Z, true);
}

/* ======================================================================
 * Caps 长按模块（规格：caps/design.md、caps/readme.md；用例：caps/testcase.md）
 * ====================================================================== */
static void caps_enter(void) {
    CHECK(pipeline(KC_CAPS, true) == false);   /* press 被吞（配对表） */
    /* 按下即进入（不等 hold_ms，也不经过 task）—— caps/design.md §3 */
}
static void caps_exit(void) {
    CHECK(pipeline(KC_CAPS, false) == false);  /* release 由配对表消费 */
    vim_keymap_common_task(g_now);
}

/* Caps 触发语义（caps/readme.md §2）：
 *  - 裸 Caps 单击 = 无任何效果（不开关 vim、不产生键）
 *  - Fn 先按住 + Caps 单击 = 切换 vim 开/关
 *  - 按下即进入模式（无需等 200ms）；按下期间未按其它键就抬起则撤销 */
static void test_caps_trigger(void) {
    /* 裸 Caps 单击：什么都不做（vim 状态不变、无键注册） */
    reset_engine();
    bool was_on = kv_vim_enabled();
    CHECK(pipeline(KC_CAPS, true) == false);
    CHECK(pipeline(KC_CAPS, false) == false);
    CHECK(kv_vim_enabled() == was_on);
    CHECK(!sim_ctrl_held());
    CHECK(kv_get_mode() == KV_MODE_INSERT);

    /* Fn(层已激活) + Caps 单击：切换 vim */
    reset_engine();
    layer_state |= (1UL << g_cfg.fn_layer);
    bool before = kv_vim_enabled();
    CHECK(pipeline(KC_CAPS, true) == false);
    CHECK(pipeline(KC_CAPS, false) == false);
    CHECK(kv_vim_enabled() != before);               /* 单击 = 开关 vim */
    CHECK(!sim_ctrl_held());
    layer_state &= ~(1UL << g_cfg.fn_layer);

    /* 按下即进入：Caps 按下后立刻按 1 -> F1（不需要等 200ms，也不经过 task） */
    reset_engine();
    CHECK(pipeline(KC_CAPS, true) == false);
    CHECK(pipeline(KC_1, true) == false);            /* 模式已激活 */
    CHECK(sim_held(KC_F1) && !sim_ctrl_held());
    CHECK(pipeline(KC_1, false) == false);
    caps_exit();
    CHECK(kv_get_mode() == KV_MODE_INSERT);

    /* vim 关闭时也立即进入（Fn+Caps 用于重新开启） */
    reset_engine();
    kv_disable();
    CHECK(pipeline(KC_CAPS, true) == false);
    CHECK(pipeline(KC_C, true) == false);
    CHECK(sim_held(KC_LCTL) && sim_held(KC_C));
    CHECK(pipeline(KC_C, false) == false);
    caps_exit();
    CHECK(!sim_ctrl_held());
    reset_engine();
}

static void test_caps_mode(void) {
    /* §1 进入：长按进入，vim 开关与模式不变 */
    reset_engine();
    bool     was_on   = kv_vim_enabled();
    kv_mode_t was_mode = kv_get_mode();
    caps_enter();
    CHECK(kv_vim_enabled() == was_on);
    CHECK(kv_get_mode() == was_mode);

    /* §2 F 区：1..0 - = -> F1..F12，且不带 Ctrl（Ctrl 根本没按住） */
    const uint16_t row[12] = {KC_1, KC_2, KC_3, KC_4, KC_5, KC_6,
                              KC_7, KC_8, KC_9, KC_0, KC_MINS, KC_EQL};
    const uint16_t fkey[12] = {KC_F1, KC_F2, KC_F3, KC_F4, KC_F5, KC_F6,
                               KC_F7, KC_F8, KC_F9, KC_F10, KC_F11, KC_F12};
    for (int i = 0; i < 12; i++) {
        CHECK(!sim_ctrl_held());                 /* §4.1 F 区不按 Ctrl */
        CHECK(pipeline(row[i], true) == false);  /* 模式内被本层接管 */
        CHECK(sim_held(fkey[i]));
        CHECK(!sim_ctrl_held());
        CHECK(pipeline(row[i], false) == false);
        CHECK(!sim_held(fkey[i]));
    }

    /* §2/§3 其余键 = Ctrl+base；引用计数：首个非 F 键按住、最后一个松开 */
    CHECK(pipeline(KC_C, true) == false);
    CHECK(sim_held(KC_LCTL) && sim_held(KC_C));
    CHECK(pipeline(KC_V, true) == false);        /* 重叠：Ctrl 保持按住 */
    CHECK(sim_held(KC_LCTL) && sim_held(KC_V));
    CHECK(pipeline(KC_C, false) == false);
    CHECK(sim_held(KC_LCTL));                    /* 还有非 F 键按住 */
    CHECK(pipeline(KC_V, false) == false);
    CHECK(!sim_ctrl_held());                     /* 最后一个松开 -> Ctrl 释放 */

    /* §2 功能键同样 Ctrl+；修饰键 -> Ctrl+修饰 */
    CHECK(pipeline(KC_ENT, true) == false);
    CHECK(sim_held(KC_LCTL) && sim_held(KC_ENT));
    CHECK(pipeline(KC_ENT, false) == false);
    CHECK(pipeline(KC_LSFT, true) == false);
    CHECK(sim_held(KC_LCTL) && sim_held(KC_LSFT));
    CHECK(pipeline(KC_LSFT, false) == false);
    /* 桩把 register/unregister 当多重集，修饰键的"位图"语义由 /tmp 的位图桩另行覆盖；
     * 这里只断言引用计数收尾：最后一个非 F 键松开后合成 Ctrl 必须释放 */
    CHECK(!sim_held(KC_LCTL));

    /* §4 模式内 Esc = Ctrl+Esc，且不触发 vim 的 Esc 切换（模式不变） */
    CHECK(kv_get_mode() == was_mode);
    CHECK(pipeline(KC_ESC, true) == false);
    CHECK(sim_held(KC_LCTL) && sim_held(KC_ESC));
    CHECK(kv_get_mode() == was_mode);
    CHECK(pipeline(KC_ESC, false) == false);
    /* 模式内 Esc 是非 F 键：按下时应带 Ctrl（上面已断言）。此处不再重复喂同一 release
     * （那在现实中不可能出现，旧代码用恒真断言掩盖了它）。 */

    /* §1 退出防卡键：按住某键时直接松开 Caps */
    CHECK(pipeline(KC_A, true) == false);
    CHECK(pipeline(KC_W, true) == false);
    CHECK(sim_held(KC_A) && sim_held(KC_W) && sim_ctrl_held());
    caps_exit();
    CHECK(!sim_held(KC_A) && !sim_held(KC_W) && !sim_ctrl_held());
    CHECK(kv_vim_enabled() == was_on);
    CHECK(kv_get_mode() == was_mode);

    /* §2 裸 Caps 单击 = 无效果（不再开关 vim；也不注册 Ctrl） */
    reset_engine();
    bool before = kv_vim_enabled();
    g_now = 9000;
    CHECK(pipeline(KC_CAPS, true) == false);
    g_now += 100;                                /* 短按 */
    CHECK(pipeline(KC_CAPS, false) == false);
    CHECK(!sim_ctrl_held());
    CHECK(kv_vim_enabled() == before);           /* vim 状态不变 */
    /* Fn+Caps 才是开关 */
    layer_state |= (1UL << g_cfg.fn_layer);
    CHECK(pipeline(KC_CAPS, true) == false);
    CHECK(pipeline(KC_CAPS, false) == false);
    CHECK(kv_vim_enabled() != before);
    layer_state &= ~(1UL << g_cfg.fn_layer);

    /* §4 vim 关闭时同样可用 */
    reset_engine();
    kv_disable();
    CHECK(!kv_vim_enabled());
    caps_enter();
    CHECK(pipeline(KC_X, true) == false);
    CHECK(sim_held(KC_LCTL) && sim_held(KC_X));
    CHECK(pipeline(KC_X, false) == false);
    caps_exit();
    CHECK(!sim_ctrl_held());

    reset_engine();
}

/* design §4.10 + §4.9：可视模式已累积的计数必须被透传的非 vim 键作废。
 * 复现：v 3 F5 j 曾把 3 泄漏给 j（3 次推进），期望 1 次。 */
/* design §4.10：可视输入必须在**所有**截断路径上作废（CAG 分支 / myfn 吞键 / g 前缀）。 */
static void test_visual_cancel_all_paths(void) {
    /* CAG 分支：v 3 后按 Ctrl+C（带修饰的透传），计数必须作废 */
    reset_engine();
    enter_normal();
    (void)pipeline(KC_V, true); (void)pipeline(KC_V, false);   /* Visual */
    CHECK(pipeline(KC_3, true) == false);
    CHECK(kv_visual_count_pending() == true);
    CHECK(pipeline(KC_LCTL, true) == true);                    /* 物理 Ctrl 透传 */
    CHECK(pipeline(KC_C, true) == true);                       /* CAG 分支：透传 */
    CHECK(kv_visual_count_pending() == false);
    (void)pipeline(KC_C, false); (void)pipeline(KC_LCTL, false);
    CHECK(pipeline(KC_J, true) == false);                      /* 只推进 1 次 */
    CHECK(kv_get_mode() == KV_MODE_VISUAL);
    (void)pipeline(KC_J, false);

    /* g 前缀：v g 后按非 g 的透传键 -> 前缀必须作废，不能残留成伪 gg */
    reset_engine();
    enter_normal();
    (void)pipeline(KC_V, true); (void)pipeline(KC_V, false);
    CHECK(pipeline(KC_G, true) == false);                      /* g 前缀 */
    CHECK(kv_visual_count_pending() == true);                  /* 计数查询覆盖前缀 */
    CHECK(pipeline(KC_F5, true) == true);                      /* 透传 -> 作废前缀 */
    CHECK(kv_visual_count_pending() == false);
    CHECK(pipeline(KC_G, true) == false);                      /* 新的 g 前缀（不是 gg） */
    CHECK(kv_visual_count_pending() == true);
    (void)pipeline(KC_G, false);
    (void)pipeline(KC_F5, false);
    reset_engine();
}

static void test_visual_count_passthrough(void) {
    reset_engine();
    enter_normal();
    CHECK(pipeline(KC_V, true) == false);   /* 进 Visual */
    CHECK(kv_get_mode() == KV_MODE_VISUAL);
    (void)pipeline(KC_V, false);
    CHECK(pipeline(KC_3, true) == false);   /* 计数 3 */
    CHECK(kv_visual_count_pending() == true);
    CHECK(pipeline(KC_F5, true) == true);   /* F5 是非 vim 键：透传 */
    CHECK(kv_visual_count_pending() == false); /* 并且计数被作废（design §4.10） */
    CHECK(pipeline(KC_F5, false) == true);
    CHECK(pipeline(KC_J, true) == false);   /* 只推进 1 次（计数没泄漏） */
    kv_emit_flush_now();                    /* 队列式 emit：冲刷后再看宿主注册 */
    CHECK(sim_arrow_count() == 0);          /* 点按已配对，不留按住的方向键 */
    CHECK(kv_get_mode() == KV_MODE_VISUAL);
    (void)pipeline(KC_J, false);
    reset_engine();
}

/* Caps 模块的卡键/发错键修复（caps/design.md §3.1；审计发现）：
 * 重入清理、过期 release 过滤、物理 Ctrl 中途松开、层键豁免、溢出吞吐一致、孤立 release 守卫。 */
static void test_caps_cleanup(void) {
    /* §3.1-1 重入清理：模式已激活时再按 Caps，必须先把上一实例的键/Ctrl 反注册。
     * 复现原缺陷：Caps↓ C↓ Caps↓ Caps↑ → 宿主永久卡住 Ctrl+C。 */
    reset_engine();
    CHECK(pipeline(KC_CAPS, true) == false);   /* 进模式 */
    CHECK(pipeline(KC_C, true) == false);      /* Ctrl+C */
    CHECK(sim_held(KC_LCTL) && sim_held(KC_C));
    CHECK(pipeline(KC_CAPS, true) == false);   /* 第二次按下（release 丢失场景） */
    CHECK(!sim_held(KC_LCTL) && !sim_held(KC_C)); /* 上一实例必须先被清掉 */
    CHECK(pipeline(KC_CAPS, false) == false);  /* 抬起 */
    CHECK(!sim_held(KC_LCTL) && !sim_held(KC_C));
    /* 之后仍可正常使用，且退出不留残留 */
    CHECK(pipeline(KC_CAPS, true) == false);
    CHECK(pipeline(KC_1, true) == false);
    CHECK(sim_held(KC_F1));
    CHECK(pipeline(KC_1, false) == false);
    CHECK(pipeline(KC_CAPS, false) == false);
    CHECK(!sim_held(KC_F1) && !sim_held(KC_LCTL));

    /* §3.1-2 过期 release 过滤：模式前按住的键在模式内抬起，不得改 Ctrl 计数 */
    reset_engine();
    CHECK(pipeline(KC_A, true) == true);       /* 普通打字（透传） */
    CHECK(pipeline(KC_CAPS, true) == false);   /* 进模式 */
    CHECK(pipeline(KC_B, true) == false);      /* Ctrl+B */
    CHECK(sim_held(KC_LCTL) && sim_held(KC_B));
    /* 模式前那个键是普通透传键：其 release 也应透传（引擎不吞它） */
    CHECK(pipeline(KC_A, false) == true);
    CHECK(sim_held(KC_LCTL));                  /* Ctrl 必须仍按住（B 还按着） */
    CHECK(pipeline(KC_B, false) == false);
    CHECK(!sim_held(KC_LCTL));
    CHECK(pipeline(KC_CAPS, false) == false);

    /* §3.1-3b 物理 Ctrl 松开时**已有非 F 键按住**（ctrl_n>0）：后续键仍须带 Ctrl
     *（第 2 轮审核 P0-2：原实现只在 ctrl_n==0 时补注册） */
    reset_engine();
    pipeline(KC_LCTL, true);                   /* 物理 Ctrl 按住 */
    CHECK(pipeline(KC_CAPS, true) == false);   /* 进模式（phys_ctrl=true） */
    CHECK(pipeline(KC_A, true) == false);      /* A 按住（物理 Ctrl 可见，无需合成） */
    pipeline(KC_LCTL, false);                  /* 物理 Ctrl 松开 */
    CHECK(pipeline(KC_B, true) == false);      /* B：必须补注册 Ctrl */
    CHECK(sim_held(KC_LCTL));
    CHECK(pipeline(KC_B, false) == false);
    CHECK(pipeline(KC_A, false) == false);
    CHECK(!sim_held(KC_LCTL));                 /* 最后一个非 F 键松开 -> 反注册 */
    CHECK(pipeline(KC_CAPS, false) == false);

    /* §3.1-3 物理 Ctrl 中途松开：后续非 F 键必须仍带 Ctrl */
    reset_engine();
    pipeline(KC_LCTL, true);                   /* 物理 Ctrl 按住（透传） */
    CHECK(pipeline(KC_CAPS, true) == false);   /* 进模式：phys_ctrl=true */
    pipeline(KC_LCTL, false);                  /* 模式内松开物理 Ctrl */
    CHECK(pipeline(KC_C, true) == false);
    CHECK(sim_held(KC_LCTL) && sim_held(KC_C)); /* 必须重新自注册 Ctrl */
    CHECK(pipeline(KC_C, false) == false);
    CHECK(!sim_held(KC_LCTL));
    CHECK(pipeline(KC_CAPS, false) == false);

    /* §3.1-4 层键豁免且放行：模式内层键不注册任何宿主键、且不消费 */
    reset_engine();
    CHECK(pipeline(KC_CAPS, true) == false);
    uint16_t layer_kc = (uint16_t)MO(4);
    CHECK(pipeline(layer_kc, true) == true);   /* 放行给 QMK（Fn 层可激活） */
    CHECK(!sim_held(layer_kc) && !sim_held(KC_LCTL) && !sim_held((uint16_t)(layer_kc & 0xFF)));
    CHECK(pipeline(layer_kc, false) == true);
    CHECK(!sim_held(KC_LCTL));
    /* Caps 的 release：press 被吞时由配对表消费（false），层键放行时透传（true）——
     * 两种都自洽；此处只断言"不残留键、不残留模式" */
    (void)pipeline(KC_CAPS, false);
    CHECK(!sim_held(KC_LCTL));
    CHECK(pipeline(KC_1, true) == true);   /* 模式已退出：1 是普通键 */
    (void)pipeline(KC_1, false);

    /* §3.1-5 溢出吞吐一致：表满后新键既不注册也不消费；退出后无残留 */
    reset_engine();
    CHECK(pipeline(KC_CAPS, true) == false);
    /* 压满 held 表（CAPS_HELD_MAX=12）再超出：必须"既不注册也不消费"，且退出后无残留 */
    const uint16_t many[13] = {KC_A, KC_B, KC_C, KC_D, KC_E, KC_F, KC_G,
                              KC_H, KC_I, KC_J, KC_K, KC_L, KC_O};
    int consumed = 0, passed = 0;
    for (int i = 0; i < 13; i++) {
        if (pipeline(many[i], true) == false) consumed++; else passed++;
    }
    /* 表满后：超出的键**被本模式吞掉**（既不注册也不让后续流水线看到它 —— 第 3 轮 K/O2：
     * 否则会被快捷键表/引擎劫持）。故 consumed 应为全部 13 个；退出后无残留。
     * 精确容量边界由 /tmp 的位图桩与 clean-state 覆盖。 */
    CHECK(consumed == 13);
    CHECK(passed == 0);
    for (int i = 0; i < 13; i++) (void)pipeline(many[i], false);
    CHECK(pipeline(KC_CAPS, false) == false);
    for (int i = 0; i < 13; i++) CHECK(!sim_held(many[i]));
    CHECK(!sim_held(KC_LCTL));

    /* §3.1-6 孤立 release 守卫：没有 press 的 Caps 抬起不开关 vim */
    reset_engine();
    bool was = kv_vim_enabled();
    (void)pipeline(KC_CAPS, false);            /* 孤立 release：吞掉即可，不得改 vim 状态 */
    CHECK(kv_vim_enabled() == was);
    reset_engine();
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
    test_insert_flash();
    test_insert_flash_wraparound();
    test_insert_flash_color();
    test_visual_cancel_all_paths();
    test_visual_count_passthrough();
    test_caps_mode();
    test_caps_trigger();
    test_caps_cleanup();
    printf("rgb: pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
