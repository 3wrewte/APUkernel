# HeteroKern — Top-level Build System
#
# All build artifacts go under build/:
#   build/gcn/       — GCN .o and .hsaco
#   build/kernel/    — kernel module .ko
#
# Source trees are never polluted by compilation.

ROCM_PATH  ?= /usr/lib64/rocm/llvm
CLANG      ?= $(ROCM_PATH)/bin/clang
LLD        ?= $(ROCM_PATH)/bin/ld.lld
OBJDUMP    ?= $(ROCM_PATH)/bin/llvm-objdump
OBJCOPY    ?= $(ROCM_PATH)/bin/llvm-objcopy

GCN_CFLAGS  = --target=amdgcn-amd-amdhsa -mcpu=gfx902
GCN_CFLAGS += --rocm-path=$(ROCM_PATH)
GCN_CFLAGS += -ffreestanding -nostdlib -nostdinc

LDFLAGS_GCN  = -shared

KERNEL_SRC  ?= kernel-src
KDIR        ?= $(KERNEL_SRC)

BLD_GCN    = build/gcn
BLD_KERNEL = build/kernel

# ── GCN sources ──────────────────────────────────────────────

GCN_ASM_SRCS  = $(wildcard gcn/*.S)
GCN_ASM_OBJS  = $(GCN_ASM_SRCS:gcn/%.S=$(BLD_GCN)/%.o)
GCN_HSACOS    = $(GCN_ASM_OBJS:.o=.hsaco)
GCN_COS       = $(GCN_HSACOS:.hsaco=.co)

GCN_C_SRCS    = $(wildcard gcn/*.c)
GCN_C_OBJS    = $(GCN_C_SRCS:gcn/%.c=$(BLD_GCN)/%.o)

# ── Phonies ───────────────────────────────────────────────────

.PHONY: all clean gcn kernel dump dump-all check-tools

all: gcn

# ── GCN targets ──────────────────────────────────────────────

gcn: $(GCN_HSACOS) $(GCN_COS)

$(BLD_GCN)/%.o: gcn/%.S | $(BLD_GCN)
	$(CLANG) $(GCN_CFLAGS) -c -o $@ $<

$(BLD_GCN)/%.o: gcn/%.c | $(BLD_GCN)
	$(CLANG) $(GCN_CFLAGS) -c -o $@ $<

$(BLD_GCN)/%.hsaco: $(BLD_GCN)/%.o | $(BLD_GCN)
	$(LLD) $(LDFLAGS_GCN) $< -o $@

$(BLD_GCN)/%.co: $(BLD_GCN)/%.hsaco | $(BLD_GCN)
	$(OBJCOPY) -O binary -j .text $< $@

$(BLD_GCN)/%.ll: gcn/%.c | $(BLD_GCN)
	$(CLANG) $(GCN_CFLAGS) -S -emit-llvm -o $@ $<

$(BLD_GCN)/%.s: gcn/%.c | $(BLD_GCN)
	$(CLANG) $(GCN_CFLAGS) -S -o $@ $<

$(BLD_GCN):
	mkdir -p $@

# ── Kernel module ────────────────────────────────────────────

kernel:
	$(MAKE) -C $(KDIR) M=$(PWD)/kernel modules
	mkdir -p $(BLD_KERNEL)
	cp kernel/*.ko $(BLD_KERNEL)/ 2>/dev/null || true

# ── Disassembly ──────────────────────────────────────────────

dump: $(GCN_HSACOS)
	$(OBJDUMP) -d $(word 1,$(GCN_HSACOS))

dump-all: $(GCN_HSACOS)
	@for f in $(GCN_HSACOS); do \
		echo "=== $$f ==="; \
		$(OBJDUMP) -d $$f; \
	done

# ── Clean ────────────────────────────────────────────────────

clean:
	rm -rf $(BLD_GCN) $(BLD_KERNEL)
	rm -f  kernel/*.o kernel/*.mod kernel/*.mod.c kernel/*.ko
	rm -f  kernel/Module.symvers kernel/modules.order
	rm -f  tests/*.o tests/tests_*

distclean: clean
	rm -rf build

# ── Toolchain check ──────────────────────────────────────────

check-tools:
	@echo "Checking ROCm toolchain..."
	@$(CLANG) --version | head -1
	@$(CLANG) $(GCN_CFLAGS) -x c -c /dev/null -o /dev/null 2>&1 \
		&& echo "GCN compile: OK" || echo "GCN compile: FAIL"
	@$(LLD) --version | head -1
	@echo "Kernel source: $(KDIR)"
	@test -f $(KDIR)/Makefile \
		&& echo "Kernel build:  OK" || echo "Kernel build:  FAIL (missing source)"
