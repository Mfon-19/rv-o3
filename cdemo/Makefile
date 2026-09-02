include toolchain.mk

.PHONY: all run disassemble sim clean

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

sim:
	$(MAKE) -C .. rvsim

run: demo.bin sim
	../rvsim demo.bin

# This target is useful for checking exactly which instructions Clang emitted.
disassemble: demo.elf
	$(OBJDUMP) -d -M no-aliases,numeric $<

clean:
	rm -f demo.o runtime.o start.o demo.elf demo.bin
