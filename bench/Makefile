# RV32IM benchmarks, built on the cdemo runtime.

include ../cdemo/toolchain.mk

BENCHES := $(patsubst %.c,%,$(wildcard *.c))

all: $(BENCHES:%=%.bin)

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

clean:
	rm -f *.o *.elf *.bin
