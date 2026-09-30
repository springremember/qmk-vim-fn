/* probe.c — matrix probe: feed vim keycodes, print the full
 * HOST-VISIBLE stream (engine emissions + passthrough keys, in order).
 *
 * Built and driven by engine/test/host/matrix.py (`make matrix-test`).
 *
 * usage: probe <NORMAL|VISUAL|VLINE|INSERT> <hex>...
 *   (mode arg optional; default NORMAL; the sequence may itself contain the
 *    v/V keys that change mode, which is the faithful usage)
 *
 * HOST <hex...>   the keycodes the host editor receives, in order
 * EMIT <hex...>   only the engine-emitted ones (diagnostic)
 * META mode=<n> pend=<0/1> vcp=<0/1> maxpend=<n> nkeys=<n>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "kv.h"
#include "emit.h"

static kv_keycode_t em[65536];
static int emn;
static void cb(kv_keycode_t kc) { if (emn < 65536) em[emn++] = kc; }

static kv_keycode_t host[65536];
static int hostn;

static void push_host(kv_keycode_t kc) { if (hostn < 65536) host[hostn++] = kc; }

int main(int argc, char **argv) {
    kv_init();
    kv_enable();
    kv_set_emit(cb);
    kv_set_mode(KV_MODE_NORMAL);
    int i = 1;
    bool keymap = false;
    if (argc > 1 && !strcmp(argv[1], "KM")) { keymap = true; i = 2; }
    if (i < argc && (!strcmp(argv[i], "NORMAL") || !strcmp(argv[i], "VISUAL") ||
                     !strcmp(argv[i], "VLINE") || !strcmp(argv[i], "INSERT"))) {
        if (!strcmp(argv[i], "NORMAL")) kv_set_mode(KV_MODE_NORMAL);
        else if (!strcmp(argv[i], "VISUAL")) kv_set_mode(KV_MODE_VISUAL);
        else if (!strcmp(argv[i], "VLINE")) kv_set_mode(KV_MODE_VISUAL_LINE);
        else kv_set_mode(KV_MODE_INSERT);
        i++;
    }
    int maxpend = 0, emitn = 0;
    for (; i < argc; i++) {
        kv_keycode_t kc = (kv_keycode_t)strtoul(argv[i], NULL, 16);
        /* keymap layer (§4.12 step 8): Insert + Esc is SWALLOWED and switches to
         * Normal; the host never sees that Esc. */
        if (keymap && kv_get_mode() == KV_MODE_INSERT && KV_BASIC(kc) == KV_ESC) {
            kv_set_mode(KV_MODE_NORMAL);
            continue;
        }
        kv_result_t r = kv_kbd(kc);
        if (r == KV_PASSTHROUGH) push_host(kc);
        int p = kv_emit_pending();
        if (p > maxpend) maxpend = p;
        emn = 0;
        kv_emit_flush_now();
        for (int k = 0; k < emn; k++) { push_host(em[k]); emitn++; }
    }
    printf("HOST");
    for (int k = 0; k < hostn; k++) printf(" %04X", host[k]);
    printf("\nMETA mode=%d pend=%d vcp=%d maxpend=%d nkeys=%d\n",
           (int)kv_get_mode(), (int)kv_pending(), (int)kv_visual_count_pending(),
           maxpend, emitn);
    return 0;
}
