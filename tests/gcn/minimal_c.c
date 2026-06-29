/* Minimal test: just writes state=3 directly */
#include "hk_libc.h"

void hk_main(void *input, void *output, int width, int height) {
    /* Just exit immediately */
    hk_exit(0);
}
