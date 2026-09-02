# Benchmark suite. Each program builds twice from the same source:
#   NAME.bin:  cross-compiled for rvsim on the cdemo runtime
#   NAME.host: native build, the independent oracle (shares no code
#              with the simulator or its reference model)
# `make benchtest` at the repo root runs sim-vs-host-vs-refmodel.

include ../cdemo/toolchain.mk
HOSTCC ?= cc

BENCHES := $(patsubst %.c,%,$(wildcard *.c))

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
