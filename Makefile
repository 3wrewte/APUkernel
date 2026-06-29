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

.PHONY: all clean gcn kernel dump dump-all check-tools sdk

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

# ── SDK package ──────────────────────────────────────────────
#
# Packages everything a developer needs to write and run GCN C
# programs into build/sdk/:
#
#   build/sdk/
#   ├── include/hk_libc.h   — C library (read/write/exit, all inline)
#   ├── lib/hk.ld            — linker script (merges sections)
#   ├── bin/hk-compile       — one-command compiler wrapper
#   ├── examples/hello.c     — "hello world" sample
#   ├── host/compute_runner.c — host-side runner (compile on target)
#   └── README.md            — quick-start guide

SDK_DIR = build/sdk

sdk:
	rm -rf $(SDK_DIR)
	mkdir -p $(SDK_DIR)/include $(SDK_DIR)/lib $(SDK_DIR)/bin \
	         $(SDK_DIR)/examples $(SDK_DIR)/host
	cp include/hk_libc.h $(SDK_DIR)/include/
	cp gcn/hk.ld         $(SDK_DIR)/lib/
	cp tests/gcn/hello_c.c  $(SDK_DIR)/examples/hello.c
	cp tests/compute_runner.c $(SDK_DIR)/host/
	@# --- Generate hk-compile ---
	@echo '#!/bin/bash' > $(SDK_DIR)/bin/hk-compile
	@echo '# hk-compile — compile a C file to a HeteroKern GCN .co binary' >> $(SDK_DIR)/bin/hk-compile
	@echo '# Usage: hk-compile program.c [-o output.co]' >> $(SDK_DIR)/bin/hk-compile
	@echo '# Env:  CLANG (default: $$$(CLANG))' >> $(SDK_DIR)/bin/hk-compile
	@echo '#       LLD   (default: $$$(LLD))' >> $(SDK_DIR)/bin/hk-compile
	@echo '#       OBJCOPY (default: $$$(OBJCOPY))' >> $(SDK_DIR)/bin/hk-compile
	@echo '' >> $(SDK_DIR)/bin/hk-compile
	@echo 'set -e' >> $(SDK_DIR)/bin/hk-compile
	@echo 'CLANG="$${CLANG:-$(CLANG)}"' >> $(SDK_DIR)/bin/hk-compile
	@echo 'LLD="$${LLD:-$(LLD)}"' >> $(SDK_DIR)/bin/hk-compile
	@echo 'OBJCOPY="$${OBJCOPY:-$(OBJCOPY)}"' >> $(SDK_DIR)/bin/hk-compile
	@echo 'SDK_DIR="$$(cd "$$(dirname "$$0")/.." && pwd)"' >> $(SDK_DIR)/bin/hk-compile
	@echo 'CFLAGS="--target=amdgcn-amd-amdhsa -mcpu=gfx902 -O2 -ffreestanding -nostdlib -nostdinc -I $${SDK_DIR}/include"' >> $(SDK_DIR)/bin/hk-compile
	@echo 'SRC=$$1; shift' >> $(SDK_DIR)/bin/hk-compile
	@echo 'OUT=""' >> $(SDK_DIR)/bin/hk-compile
	@echo 'while [ $$# -gt 0 ]; do case $$1 in -o) shift; OUT=$$1;; esac; shift; done' >> $(SDK_DIR)/bin/hk-compile
	@echo '[ -n "$$OUT" ] || OUT="$${SRC%.c}.co"' >> $(SDK_DIR)/bin/hk-compile
	@echo 'TMP=$$(mktemp -d)' >> $(SDK_DIR)/bin/hk-compile
	@echo 'trap "rm -rf $$TMP" EXIT' >> $(SDK_DIR)/bin/hk-compile
	@echo '$$CLANG $$CFLAGS -c "$$SRC" -o "$$TMP/a.o"' >> $(SDK_DIR)/bin/hk-compile
	@echo '$$LLD -T "$${SDK_DIR}/lib/hk.ld" "$$TMP/a.o" -o "$$TMP/a.hsaco"' >> $(SDK_DIR)/bin/hk-compile
	@echo '$$OBJCOPY -O binary -j .text "$$TMP/a.hsaco" "$$OUT"' >> $(SDK_DIR)/bin/hk-compile
	@echo 'echo "Built $$OUT ($$(wc -c < $$OUT) bytes)"' >> $(SDK_DIR)/bin/hk-compile
	@chmod +x $(SDK_DIR)/bin/hk-compile
	@echo "SDK packaged in $(SDK_DIR)/"

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
