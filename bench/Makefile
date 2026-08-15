# Benchmark suite. Each program builds twice from the same source:
#   NAME.bin:  cross-compiled for rvsim on the cdemo runtime
#   NAME.host: native build, the independent oracle (shares no code
#              with the simulator or its reference model)
# `make benchtest` at the repo root runs sim-vs-host-vs-refmodel.

CC := clang
LD := riscv64-linux-gnu-ld
OBJCOPY := riscv64-linux-gnu-objcopy
HOSTCC ?= cc

ARCH_FLAGS := --target=riscv32-none-elf -march=rv32im -mabi=ilp32
CFLAGS := $(ARCH_FLAGS) -std=c11 -O2 -Wall -Wextra \
	-ffreestanding -fno-builtin -fno-stack-protector \
	-fno-pic -fno-pie -mno-relax -msmall-data-limit=0
ASFLAGS := $(ARCH_FLAGS) -mno-relax
LDFLAGS := -m elf32lriscv --no-relax -T ../cdemo/linker.ld

BENCHES := ptrchase mlpbench matmul mm64 qsortb rle branchy

all: $(BENCHES:%=%.bin) $(BENCHES:%=%.host)

start.o: ../cdemo/start.S
	$(CC) $(ASFLAGS) -c $< -o $@

runtime.o: ../cdemo/runtime.c
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.c bench.h
	$(CC) $(CFLAGS) -c $< -o $@

%.elf: %.o start.o runtime.o ../cdemo/linker.ld
	$(LD) $(LDFLAGS) start.o $*.o runtime.o -o $@

%.bin: %.elf
	$(OBJCOPY) -O binary $< $@

%.host: %.c bench.h
	$(HOSTCC) -DHOST -O2 -Wall -o $@ $<

clean:
	rm -f *.o *.elf *.bin *.host
