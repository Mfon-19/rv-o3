# LLVM Clang can emit RV32 code even when running on an x86 host. GNU's
# RISC-V binutils then assign final addresses and extract the raw binary.
CC := clang
LD := riscv64-linux-gnu-ld
OBJCOPY := riscv64-linux-gnu-objcopy
OBJDUMP := riscv64-linux-gnu-objdump

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
LDFLAGS := -m elf32lriscv --no-relax -T linker.ld

.PHONY: all run disassemble clean

all: demo.bin

demo.o: demo.c
	$(CC) $(CFLAGS) -c $< -o $@

runtime.o: runtime.c
	$(CC) $(CFLAGS) -c $< -o $@

start.o: start.S
	$(CC) $(ASFLAGS) -c $< -o $@

demo.elf: start.o demo.o runtime.o linker.ld
	$(LD) $(LDFLAGS) start.o demo.o runtime.o -o $@

# rvsim does not parse ELF metadata, so objcopy extracts the loadable bytes.
demo.bin: demo.elf
	$(OBJCOPY) -O binary $< $@

../rvsim: ../rv-five-stage.cpp ../Makefile
	$(MAKE) -C .. rvsim

run: demo.bin ../rvsim
	../rvsim demo.bin

# This target is useful for checking exactly which instructions Clang emitted.
disassemble: demo.elf
	$(OBJDUMP) -d -M no-aliases,numeric $<

clean:
	rm -f demo.o runtime.o start.o demo.elf demo.bin
