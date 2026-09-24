# The bare-metal RV32IM toolchain shared by the C demo and the benchmarks.
# LLVM Clang can emit RV32 code even when running on an x86 host; GNU's
# RISC-V binutils then assign final addresses and extract the raw binary.
CC := clang
# Apple's system Clang omits the RISC-V backend. Homebrew LLVM includes it.
ifeq ($(shell uname -s),Darwin)
LLVM_PREFIX ?= $(shell brew --prefix llvm 2>/dev/null)
ifneq ($(LLVM_PREFIX),)
CC := $(LLVM_PREFIX)/bin/clang
endif
endif
# Linux distro packages use linux-gnu; Homebrew uses elf. Command-line
# RISCV_PREFIX/LD/OBJCOPY/OBJDUMP overrides remain available.
RISCV_PREFIX ?= $(shell if command -v riscv64-linux-gnu-ld >/dev/null 2>&1; then echo riscv64-linux-gnu-; else echo riscv64-elf-; fi)
LD := $(RISCV_PREFIX)ld
OBJCOPY := $(RISCV_PREFIX)objcopy
OBJDUMP := $(RISCV_PREFIX)objdump

# --target selects a bare-metal 32-bit RISC-V target rather than this host.
# -march/-mabi constrain generated code to the simulator's RV32IM + ILP32 ABI.
ARCH_FLAGS := --target=riscv32-none-elf -march=rv32im -mabi=ilp32

# -ffreestanding says that no hosted C environment exists.
# -fno-builtin prevents Clang from replacing code with unavailable libc calls.
# -fno-stack-protector avoids references to an unavailable stack-check runtime.
# -fno-pic/-fno-pie produce a fixed-address image suitable for the flat loader.
# -mno-relax and -msmall-data-limit=0 avoid code that depends on an initialized
# global pointer; start.S only needs to initialize the stack pointer.
CFLAGS := $(ARCH_FLAGS) -std=c11 -O2 -Wall -Wextra \
	-ffreestanding -fno-builtin -fno-stack-protector \
	-fno-pic -fno-pie -mno-relax -msmall-data-limit=0

# Assembly needs the same ISA/ABI and must also avoid linker relaxation.
ASFLAGS := $(ARCH_FLAGS) -mno-relax

# The ELF linker resolves labels and lays sections out from address zero.
# --no-relax keeps the final instructions within the simulator's known subset.
CDEMO_DIR := $(dir $(lastword $(MAKEFILE_LIST)))
LDFLAGS := -m elf32lriscv --no-relax -T $(CDEMO_DIR)linker.ld
