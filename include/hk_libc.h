/*
 * hk_libc.h — HeteroKern GCN C Library (single-file inline)
 *
 * CONSTRAINTS:
 *   1. buffer_store/load don't work under VMID 0 → no function calls
 *      (everything is static inline, compiled at -O2)
 *   2. GPU cannot write to IB memory → no writable global variables
 *      (mailbox pointer is passed as local variable through inline chain)
 *
 * Usage:
 *
 *   // my_program.c
 *   #include "hk_libc.h"
 *
 *   static inline __attribute__((always_inline))
 *   void hk_main(volatile void *mb, void *input, void *output, int w, int h) {
 *       hk_write(mb, 1, "hello\n", 6);
 *       hk_exit(mb, 0);
 *   }
 *
 *   HK_ENTRY
 *
 * Build:
 *   clang --target=amdgcn-amd-amdhsa -mcpu=gfx902 -O2 \
 *       -ffreestanding -nostdlib -I include -c my_program.c -o my.o
 *   ld.lld -T hk.ld my.o -o my.hsaco
 *   llvm-objcopy -O binary -j .text my.hsaco my.co
 */
#ifndef HK_LIBC_H
#define HK_LIBC_H

/* Mailbox field offsets */
#define HK_MB_STATE      0x00u
#define HK_MB_SYSCALL_NR 0x08u
#define HK_MB_ARG0       0x10u
#define HK_MB_ARG1       0x18u
#define HK_MB_ARG2       0x20u
#define HK_MB_RETVAL     0x38u
#define HK_MB_INPUT_ADDR 0xC0u
#define HK_MB_OUTPUT_ADDR 0xC8u
#define HK_MB_WIDTH      0xD0u
#define HK_MB_HEIGHT     0xD4u

/* ---- Syscall helpers (all take mailbox pointer as first arg) ---- */

static inline __attribute__((always_inline))
int hk_syscall_wait(volatile void *mbp)
{
    volatile unsigned char *mb = (volatile unsigned char *)mbp;
    volatile unsigned int *state = (volatile unsigned int *)(mb + HK_MB_STATE);
    for (;;) {
        asm volatile("buffer_wbinvl1_vol" ::: "memory");
        if (*state == 0)
            break;
    }
    volatile int *retval = (volatile int *)(mb + HK_MB_RETVAL);
    return *retval;
}

static inline __attribute__((always_inline))
int hk_syscall3(volatile void *mbp, unsigned int nr,
                unsigned long a0, unsigned long a1, unsigned long a2)
{
    volatile unsigned char *mb = (volatile unsigned char *)mbp;
    asm volatile("s_waitcnt vmcnt(0)" ::: "memory");
    *(volatile unsigned int *)(mb + HK_MB_SYSCALL_NR) = nr;
    *(volatile unsigned long *)(mb + HK_MB_ARG0) = a0;
    *(volatile unsigned long *)(mb + HK_MB_ARG1) = a1;
    *(volatile unsigned long *)(mb + HK_MB_ARG2) = a2;
    asm volatile("s_waitcnt vmcnt(0)" ::: "memory");
    *(volatile unsigned int *)(mb + HK_MB_STATE) = 1;
    asm volatile("s_waitcnt vmcnt(0)" ::: "memory");
    asm volatile("s_sendmsg sendmsg(MSG_INTERRUPT)" ::: "memory");
    return hk_syscall_wait(mbp);
}

/* ---- Public API ---- */

static inline __attribute__((always_inline))
int hk_read(volatile void *mb, int fd, void *buf, int count)
{
    return hk_syscall3(mb, 0, (unsigned long)fd,
                       (unsigned long)buf, (unsigned long)count);
}

static inline __attribute__((always_inline))
int hk_write(volatile void *mb, int fd, const void *buf, int count)
{
    return hk_syscall3(mb, 1, (unsigned long)fd,
                       (unsigned long)buf, (unsigned long)count);
}

static inline __attribute__((always_inline))
void hk_exit(volatile void *mbp, int code)
{
    volatile unsigned char *mb = (volatile unsigned char *)mbp;
    *(volatile unsigned int *)(mb + HK_MB_STATE) = 3;
    asm volatile("s_waitcnt vmcnt(0)" ::: "memory");
    asm volatile("s_endpgm");
    __builtin_unreachable();
}

/* ---- User entry point (forward declaration) ----
 * The first parameter is the mailbox pointer (volatile void *).
 * Everything inlines at -O2 — no function calls, no buffer ops. */
static inline __attribute__((always_inline))
void hk_main(volatile void *mb, void *input, void *output, int width, int height);

/* ---- Entry point macro — place at END of user .c file ----
 *
 * Reads mailbox from s[4:5] (USER_DATA_4/5), calls hk_main.
 * No globals, no buffer instructions, no function calls. */

#define HK_ENTRY \
__attribute__((noreturn, section(".text.entry"))) \
void __hk_entry(void) \
{ \
    unsigned int mb_lo, mb_hi; \
    asm volatile("v_mov_b32_e32 %0, s4\n" \
                 "v_mov_b32_e32 %1, s5\n" \
                 : "=v"(mb_lo), "=v"(mb_hi)); \
    volatile unsigned char *mb = (volatile unsigned char *) \
        (((unsigned long long)mb_hi << 32) | mb_lo); \
    void *input  = (void *)*(volatile unsigned long long *)(mb + HK_MB_INPUT_ADDR); \
    void *output = (void *)*(volatile unsigned long long *)(mb + HK_MB_OUTPUT_ADDR); \
    int width   = *(volatile unsigned int *)(mb + HK_MB_WIDTH); \
    int height  = *(volatile unsigned int *)(mb + HK_MB_HEIGHT); \
    hk_main((volatile void *)mb, input, output, width, height); \
    *(volatile unsigned int *)(mb + HK_MB_STATE) = 3; \
    asm volatile("s_waitcnt vmcnt(0)"); \
    asm volatile("s_endpgm"); \
    __builtin_unreachable(); \
}

#endif /* HK_LIBC_H */
