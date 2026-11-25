CC := clang
ifeq ($(shell uname -s),Darwin)
LLVM_PREFIX ?= $(shell brew --prefix llvm 2>/dev/null)
ifneq ($(LLVM_PREFIX),)
CC := $(LLVM_PREFIX)/bin/clang
endif
endif
RISCV_PREFIX ?= $(shell if command -v riscv64-linux-gnu-ld >/dev/null 2>&1; then echo riscv64-linux-gnu-; else echo riscv64-elf-; fi)
LD := $(RISCV_PREFIX)ld
OBJCOPY := $(RISCV_PREFIX)objcopy
OBJDUMP := $(RISCV_PREFIX)objdump

ARCH_FLAGS := --target=riscv32-none-elf -march=rv32im -mabi=ilp32

CFLAGS := $(ARCH_FLAGS) -std=c11 -O2 -Wall -Wextra \
	-ffreestanding -fno-builtin -fno-stack-protector \
	-fno-pic -fno-pie -mno-relax -msmall-data-limit=0

ASFLAGS := $(ARCH_FLAGS) -mno-relax

CDEMO_DIR := $(dir $(lastword $(MAKEFILE_LIST)))
LDFLAGS := -m elf32lriscv --no-relax -T $(CDEMO_DIR)linker.ld
