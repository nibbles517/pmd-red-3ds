# pmd-red-3ds top-level build.
#
#   make game      compile pmd-red's C for ARM11 (needs `make upstream` first)
#   make upstream  build pret/pmd-red's own modern ROM, which extracts and
#                  assembles all game data from your baserom.gba
#   make harness   link the test build that runs in the CPU-emulator harness
#   make 3ds       link twinembers-style .3dsx (needs devkitPro + libctru)

ROOT     := $(abspath .)
UPSTREAM ?= $(ROOT)/build/upstream
BUILD    := $(ROOT)/build
GAME_OBJ := $(BUILD)/game
PREFIX   ?= arm-none-eabi-

include port/game.mk

.PHONY: all game upstream harness prelink 3ds clean
all: game

upstream:
	$(MAKE) -C $(UPSTREAM) modern

game: $(GAME_OBJS)

clean:
	rm -rf $(GAME_OBJ) $(BUILD)/harness $(BUILD)/3ds

# ---------------------------------------------------------------------------
# Platform core and the test-harness build

CORE_CFLAGS := $(ARCH_FLAGS) -O2 -g -Wall -Wextra -Wno-unused-parameter -ffunction-sections -fdata-sections
CORE_SRCS   := port/core/frame.c port/core/ppu.c port/core/dma.c port/core/save.c port/core/cgb.c
CORE_OBJ    := $(BUILD)/core

$(CORE_OBJ)/%.o: port/core/%.c port/core/port.h
	@mkdir -p $(dir $@)
	@$(GAME_CC) $(CORE_CFLAGS) -c $< -o $@
	@echo "  CC   $<"

# sound.c reads the m4a driver's structures, so it builds like game code.
$(CORE_OBJ)/sound.o: port/core/sound.c port/core/port.h
	@mkdir -p $(dir $@)
	@# No tail calls: it calls the Thumb sound driver (see port/game.mk).
	@$(GAME_CC) $(ARCH_FLAGS) $(GAME_ABI_FLAGS) -O2 -g -w -fno-optimize-sibling-calls $(GAME_CPPFLAGS) -c $< -o $@
	@echo "  CC   $<"

# The m4a mixer stays in its original ARM/Thumb assembly, which the 3DS CPU runs natively.
$(CORE_OBJ)/m4a_1.o: $(UPSTREAM)/src/m4a_1.s
	@mkdir -p $(dir $@)
	@$(GAME_CC) -E -x assembler-with-cpp -iquote $(UPSTREAM)/include -I $(UPSTREAM)/include -DPLATFORM_3DS -DMODERN=1 $< -o $(CORE_OBJ)/m4a_1.i.s
	@# 3dsxtool rejects any relocation at an address that is not 4-byte aligned,
	@# including Thumb `bl` calls. Align each one, padding with Thumb nops.
	@sed -E 's/^([[:space:]]*)bl([[:space:]])/\1.balign 4\n\1bl\2/' $(CORE_OBJ)/m4a_1.i.s > $(CORE_OBJ)/m4a_1.aligned.s
	@cd $(UPSTREAM) && $(GAME_AS) -march=armv6k -mfloat-abi=hard -I include --defsym MODERN=1 -o $@ $(CORE_OBJ)/m4a_1.aligned.s
	@echo "  AS   m4a_1.s"

# options.c reads the game's options, so it builds like game code too.
$(CORE_OBJ)/options.o: port/core/options.c port/core/port.h
	@mkdir -p $(dir $@)
	@$(GAME_CC) $(ARCH_FLAGS) $(GAME_ABI_FLAGS) -O2 -g -w $(GAME_CPPFLAGS) -c $< -o $@
	@echo "  CC   $<"

PORT_OBJS := $(CORE_SRCS:port/core/%.c=$(CORE_OBJ)/%.o) $(CORE_OBJ)/sound.o $(CORE_OBJ)/options.o $(CORE_OBJ)/m4a_1.o

HARNESS_OBJ := $(BUILD)/harness
HARNESS_ELF := $(BUILD)/pmd-red-harness.elf

$(HARNESS_OBJ)/%.o: port/harness/%.c port/core/port.h
	@mkdir -p $(dir $@)
	@$(GAME_CC) $(CORE_CFLAGS) -c $< -o $@
$(HARNESS_OBJ)/%.o: port/harness/%.S
	@mkdir -p $(dir $@)
	@$(GAME_CC) $(ARCH_FLAGS) -c $< -o $@

harness: $(HARNESS_ELF)

$(HARNESS_ELF): $(GAME_OBJS) $(PORT_OBJS) $(GAME_DATA_OBJS) $(HARNESS_OBJ)/start.o $(HARNESS_OBJ)/backend.o port/harness/harness.ld port/game_init_sections.ld port/game_bss_sections.ld
	@# Link against newlib's ARM-mode hard-float build (armv5te+fp runs on the 3DS's ARMv6K).
	@$(GAME_CC) -marm -march=armv5te+fp -mfloat-abi=hard -nostartfiles -T port/harness/harness.ld -L port \
		-Wl,--no-warn-rwx-segments -Wl,-Map,$(BUILD)/pmd-red-harness.map -o $@ \
		$(HARNESS_OBJ)/start.o $(HARNESS_OBJ)/backend.o $(PORT_OBJS) $(GAME_OBJS) $(GAME_DATA_OBJS) -lc -lnosys -lgcc
	@echo "  LD   $@"

# ---------------------------------------------------------------------------
# 3DS build
#
# Two steps, because the game half needs your ROM's data and pret's tools while
# the 3DS half needs devkitPro:
#   make prelink   (where the ROM and pret's tools are) -> build/pmd-red-game.o
#                  game + platform core, partially linked; contains game data,
#                  so it is for personal use only and never committed
#   make 3ds       (in devkitPro's MSYS2) -> build/pmd-red-3ds.3dsx

PRELINK := $(BUILD)/pmd-red-game.o

# A plain target, not a file rule: `make 3ds` must use an existing
# pmd-red-game.o as is, never try to rebuild it.
prelink: $(GAME_OBJS) $(PORT_OBJS) $(GAME_DATA_OBJS)
	@$(PREFIX)ld -r -S -o $(PRELINK) $(PORT_OBJS) $(GAME_OBJS) $(GAME_DATA_OBJS)
	@echo "  LD   $(PRELINK) (partial)"

DEVKITPRO ?= /opt/devkitpro
DEVKITARM ?= $(DEVKITPRO)/devkitARM
CTRU      := $(DEVKITPRO)/libctru
TOOLS_3DS := $(DEVKITPRO)/tools/bin
CC_3DS    := $(DEVKITARM)/bin/arm-none-eabi-gcc
GAME_PRELINK ?= $(PRELINK)

APP_TITLE  := PMD Red Rescue Team
APP_DESC   := pmd-red-3ds port (personal build)
APP_AUTHOR := pret / pmd-red-3ds

3DS_OBJ := $(BUILD)/3ds
3DS_ELF := $(BUILD)/pmd-red-3ds.elf

$(3DS_OBJ)/main.o: port/3ds/main.c port/core/port.h
	@mkdir -p $(dir $@)
	@$(CC_3DS) -O2 -g -Wall -marm -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft -D__3DS__ -I$(CTRU)/include -c $< -o $@
	@echo "  CC   $<"

$(GAME_PRELINK):
	@echo "Missing $(GAME_PRELINK). Put pmd-red-game.o in the build folder (or run make prelink)."
	@false

# devkitARM's 3dsx.specs, with port/3ds/game_sections.ld listed before
# 3dsx.ld so its INSERT commands can find their anchors. The path is relative:
# devkitARM's linker is a native Windows program and cannot read /c/... paths.
3DS_SPECS := $(BUILD)/pmd-red-3ds.specs
$(3DS_SPECS): Makefile
	@mkdir -p $(dir $@)
	@printf '%s\n' '%include <sync-dmb.specs>' '' '*link:' \
		'+ -T port/3ds/game_sections.ld -T 3dsx.ld%s -d --emit-relocs --use-blx --gc-sections' '' \
		'*startfile:' '3dsx_crt0%O%s crti%O%s crtbegin%O%s' > $@

$(3DS_ELF): $(3DS_OBJ)/main.o $(GAME_PRELINK) port/3ds/game_sections.ld $(3DS_SPECS)
	@$(CC_3DS) -specs=$(3DS_SPECS) -g -marm -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft \
		-Wl,-Map,$(BUILD)/pmd-red-3ds.map -Wl,--no-warn-rwx-segments \
		-o $@ $(3DS_OBJ)/main.o $(GAME_PRELINK) -L$(CTRU)/lib -lctru -lm
	@echo "  LD   $@"

$(BUILD)/pmd-red-3ds.smdh: port/3ds/icon.png
	@$(TOOLS_3DS)/smdhtool --create "$(APP_TITLE)" "$(APP_DESC)" "$(APP_AUTHOR)" $< $@

3ds: $(BUILD)/pmd-red-3ds.3dsx
$(BUILD)/pmd-red-3ds.3dsx: $(3DS_ELF) $(BUILD)/pmd-red-3ds.smdh
	@$(TOOLS_3DS)/3dsxtool $< $@ --smdh=$(BUILD)/pmd-red-3ds.smdh
	@echo "  3DSX $@"
