#ifndef HETEROKERN_MAILBOX_H
#define HETEROKERN_MAILBOX_H

#include <stdint.h>

#define MAILBOX_STATE_IDLE           0
#define MAILBOX_STATE_SYSCALL_PENDING 1
#define MAILBOX_STATE_TRAP_PENDING   2

#define MAILBOX_SYSCALL_NARGS        6

#define MAILBOX_FAULT_TYPE_READ      0
#define MAILBOX_FAULT_TYPE_WRITE     1
#define MAILBOX_FAULT_TYPE_EXEC      2

struct mailbox {
	uint32_t state;
	uint32_t syscall_nr;
	uint64_t args[MAILBOX_SYSCALL_NARGS];
	uint64_t retval;
	uint64_t error_code;
	uint64_t fault_addr;
	uint32_t fault_type;
	uint8_t  reserved[36];
} __attribute__((aligned(128)));

_Static_assert(sizeof(struct mailbox) == 128,
	       "mailbox must be 128 bytes / cache-line aligned");

#endif
