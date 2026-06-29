/*
 * hello_c.c — HeteroKern "hello world" in C
 */

#include "hk_libc.h"

static inline __attribute__((always_inline))
void hk_main(volatile void *mb, void *input, void *output, int width, int height)
{
    hk_write(mb, 1, "hello from GCN C!\n", 18);
    (void)input; (void)output; (void)width; (void)height;
    hk_exit(mb, 0);
}

HK_ENTRY
