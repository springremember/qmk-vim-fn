#include "kvtest.h"
#include "../src/emit.h"

int g_fail = 0;
int g_pass = 0;

#define REC_CAP 512
static kv_keycode_t s_out[REC_CAP];
static int          s_n;
static const char  *s_label = "";

static void rec_cb(kv_keycode_t kc) {
    if (s_n < REC_CAP) s_out[s_n++] = kc;
}

void rec_start(void) {
    s_n = 0;
    s_label = "";
    kv_emit_clear();
    kv_set_emit(rec_cb);
}

void rec_note(const char *label) { s_label = label; }

kv_keycode_t rec_at(int i) { return (i >= 0 && i < s_n) ? s_out[i] : 0xFFFF; }
int          rec_count(void) { return s_n; }

void flush_emit(void) {
    kv_emit_flush_now();
}

void rec_print(void) {
    fprintf(stderr, "emit[%s] n=%d:", s_label, s_n);
    for (int i = 0; i < s_n; i++) fprintf(stderr, " %04X", s_out[i]);
    fprintf(stderr, "\n");
}
