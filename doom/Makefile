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
# CMAP256=1 keeps Doom's 8-bit palette indices and lets the host expand them;
# CMAP256=0 converts every pixel to XRGB on the guest (the original path).
CMAP256 ?= 1
ifeq ($(CMAP256),1)
CFLAGS += -DCMAP256
endif
LIBC := $(PICOLIBC)/lib/rv32im/ilp32/libc.a
LIBGCC := $(DEPS)/gcc/libgcc.a
OBJECTS := $(addprefix $(BUILD)/,$(addsuffix .o,$(filter-out w_file_stdc,$(DOOM_SOURCES)))) \
           $(BUILD)/platform.o $(BUILD)/runtime.o $(BUILD)/memops.o $(BUILD)/fixed.o $(BUILD)/wad_io.o $(BUILD)/start.o $(BUILD)/gamewad.o

.PHONY: all setup run demo clean viewer testviewer
all: $(BUILD)/doom.bin
setup:
	python3 setup.py
run:
	python3 run.py --play
demo:
	python3 run.py

# The native viewer is separate: the simulator and guest never link SDL.
HOST_CXX ?= c++
PKG_CONFIG ?= pkg-config
viewer: $(BUILD)/doom-viewer
testviewer: viewer
	python3 ../tests/display_viewer.py
$(BUILD)/doom-viewer: viewer.cpp ../sim/display_protocol.h | $(BUILD)
	@$(PKG_CONFIG) --exists sdl3 || { echo 'SDL3 missing: on macOS run brew install sdl3 pkg-config' >&2; exit 1; }
	$(HOST_CXX) -std=c++20 -O2 -Wall -Wextra $$($(PKG_CONFIG) --cflags sdl3) $< -o $@ $$($(PKG_CONFIG) --libs sdl3)

$(BUILD):
	mkdir -p $@
$(BUILD)/options.h: | $(BUILD)
	printf '%s\n' '#define MAX_FRAMES 0' '#define PLAY_MODE 0' > $@
$(BUILD)/%.o: $(ENGINE)/%.c Makefile | $(BUILD)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@
$(BUILD)/platform.o: platform.c $(BUILD)/options.h ecall.h Makefile
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@
$(BUILD)/runtime.o: runtime.c ecall.h | $(BUILD)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@
$(BUILD)/memops.o: memops.c Makefile | $(BUILD)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@
$(BUILD)/fixed.o: fixed.c Makefile | $(BUILD)
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
	$(LD) -m elf32lriscv --no-relax --gc-sections --wrap=I_Quit --wrap=FixedDiv -T linker.ld \
	  -Map $(BUILD)/doom.map $(OBJECTS) --start-group $(LIBC) $(LIBGCC) --end-group -o $@
$(BUILD)/doom.bin: $(BUILD)/doom.elf
	$(OBJCOPY) -O binary $< $@
clean:
	rm -rf $(BUILD)

-include $(wildcard $(BUILD)/*.d)
