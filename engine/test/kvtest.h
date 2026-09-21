/* kvtest.h — tiny assertion helpers + emit recorder for host tests. */
#ifndef KVTEST_H
#define KVTEST_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../include/kv.h"

extern int  g_fail;
extern int  g_pass;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (cond) { g_pass++; }                                            \
        else {                                                             \
            g_fail++;                                                      \
            fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);\
        }                                                                  \
    } while (0)

/* emit recorder */
void        rec_start(void);          /* clear recorded output + feed state */
void        rec_note(const char *label);
kv_keycode_t rec_at(int i);
int         rec_count(void);
void        rec_print(void);

/* convenience */
void  feed(const char *keys);         /* not used; feed via kv_kbd */
void  flush_emit(void);

#endif /* KVTEST_H */
