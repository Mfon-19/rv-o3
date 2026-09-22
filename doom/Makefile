include ../cdemo/toolchain.mk
include sources.mk

DEPS := .deps
ENGINE := $(DEPS)/doomgeneric/doomgeneric
PICOLIBC := $(DEPS)/picolibc
BUILD := build
WAD ?= $(DEPS)/freedoom1.wad

CFLAGS += -isystem $(PICOLIBC)/include -I$(ENGINE) -I$(BUILD) \
          -D_DEFAULT_SOURCE -DDOOMGENERIC_RESX=320 -DDOOMGENERIC_RESY=200 \
          -ffunction-sections -fdata-sections -Wno-unused-variable \
          -Wno-unused-but-set-variable -Wno-implicit-int -Wno-format
LIBC := $(PICOLIBC)/lib/rv32im/ilp32/libc.a
LIBGCC := $(DEPS)/gcc/libgcc.a
OBJECTS := $(addprefix $(BUILD)/,$(addsuffix .o,$(filter-out w_file_stdc,$(DOOM_SOURCES)))) \
           $(BUILD)/platform.o $(BUILD)/runtime.o $(BUILD)/wad_io.o $(BUILD)/start.o $(BUILD)/gamewad.o

.PHONY: all setup run demo clean
all: $(BUILD)/doom.bin
setup:
	python3 setup.py
run:
	python3 run.py --play
demo:
	python3 run.py

$(BUILD):
	mkdir -p $@
$(BUILD)/options.h: | $(BUILD)
	printf '%s\n' '#define ASCII_COLS 80' '#define ASCII_ROWS 25' \
	  '#define MAX_FRAMES 0' '#define ANSI_OUTPUT 0' '#define PLAY_MODE 0' > $@
$(BUILD)/%.o: $(ENGINE)/%.c | $(BUILD)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@
$(BUILD)/platform.o: platform.c $(BUILD)/options.h ecall.h
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@
$(BUILD)/runtime.o: runtime.c ecall.h | $(BUILD)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@
$(BUILD)/wad_io.o: wad_io.c | $(BUILD)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@
$(BUILD)/start.o: start.S | $(BUILD)
	$(CC) $(ASFLAGS) -c $< -o $@

# A stable input name gives the embedded image stable linker symbols.
$(BUILD)/game.wad: $(WAD) | $(BUILD)
	cp $< $@
$(BUILD)/gamewad.o: $(BUILD)/game.wad
	cd $(BUILD) && $(OBJCOPY) -I binary -O elf32-littleriscv -B riscv \
	  --rename-section .data=.rodata.wad,alloc,load,readonly,data,contents game.wad gamewad.o
$(BUILD)/doom.elf: $(OBJECTS) linker.ld $(LIBC) $(LIBGCC)
	$(LD) -m elf32lriscv --no-relax --gc-sections --wrap=I_Quit -T linker.ld \
	  -Map $(BUILD)/doom.map $(OBJECTS) --start-group $(LIBC) $(LIBGCC) --end-group -o $@
$(BUILD)/doom.bin: $(BUILD)/doom.elf
	$(OBJCOPY) -O binary $< $@
clean:
	rm -rf $(BUILD)

-include $(wildcard $(BUILD)/*.d)
