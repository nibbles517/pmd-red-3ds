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

.PHONY: all game upstream harness 3ds clean
all: game

upstream:
	$(MAKE) -C $(UPSTREAM) modern

game: $(GAME_OBJS)

clean:
	rm -rf $(GAME_OBJ) $(BUILD)/harness $(BUILD)/3ds

# ---------------------------------------------------------------------------
# Platform core and the test-harness build

CORE_CFLAGS := $(ARCH_FLAGS) -O2 -g -Wall -Wextra -Wno-unused-parameter -ffunction-sections -fdata-sections
CORE_SRCS   := port/core/frame.c port/core/ppu.c port/core/dma.c port/core/save.c
CORE_OBJ    := $(BUILD)/core

$(CORE_OBJ)/%.o: port/core/%.c port/core/port.h
	@mkdir -p $(dir $@)
	@$(GAME_CC) $(CORE_CFLAGS) -c $< -o $@
	@echo "  CC   $<"

# sound.c reads the m4a driver's structures, so it builds like game code.
$(CORE_OBJ)/sound.o: port/core/sound.c port/core/port.h
	@mkdir -p $(dir $@)
	@$(GAME_CC) $(ARCH_FLAGS) -O2 -g $(GAME_CPPFLAGS) -c $< -o $@
	@echo "  CC   $<"

# The m4a mixer stays in its original ARM/Thumb assembly, which the 3DS CPU runs natively.
$(CORE_OBJ)/m4a_1.o: $(UPSTREAM)/src/m4a_1.s
	@mkdir -p $(dir $@)
	@$(GAME_CC) -E -x assembler-with-cpp -iquote $(UPSTREAM)/include -I $(UPSTREAM)/include -DPLATFORM_3DS -DMODERN=1 $< -o $(CORE_OBJ)/m4a_1.i.s
	@cd $(UPSTREAM) && $(GAME_AS) -march=armv6k -mfloat-abi=hard -I include --defsym MODERN=1 -o $@ $(CORE_OBJ)/m4a_1.i.s
	@echo "  AS   m4a_1.s"

PORT_OBJS := $(CORE_SRCS:port/core/%.c=$(CORE_OBJ)/%.o) $(CORE_OBJ)/sound.o $(CORE_OBJ)/m4a_1.o

HARNESS_OBJ := $(BUILD)/harness
HARNESS_ELF := $(BUILD)/pmd-red-harness.elf

$(HARNESS_OBJ)/%.o: port/harness/%.c port/core/port.h
	@mkdir -p $(dir $@)
	@$(GAME_CC) $(CORE_CFLAGS) -c $< -o $@
$(HARNESS_OBJ)/%.o: port/harness/%.S
	@mkdir -p $(dir $@)
	@$(GAME_CC) $(ARCH_FLAGS) -c $< -o $@

harness: $(HARNESS_ELF)

$(HARNESS_ELF): $(GAME_OBJS) $(PORT_OBJS) $(HARNESS_OBJ)/start.o $(HARNESS_OBJ)/backend.o port/harness/harness.ld port/game_init_sections.ld port/game_bss_sections.ld
	@# Link against newlib's ARM-mode hard-float build (armv5te+fp runs on the 3DS's ARMv6K).
	@$(GAME_CC) -marm -march=armv5te+fp -mfloat-abi=hard -nostartfiles -T port/harness/harness.ld -L port \
		-Wl,--no-warn-rwx-segments -Wl,-Map,$(BUILD)/pmd-red-harness.map -o $@ \
		$(HARNESS_OBJ)/start.o $(HARNESS_OBJ)/backend.o $(PORT_OBJS) $(GAME_OBJS) $(GAME_DATA_OBJS) -lc -lnosys -lgcc
	@echo "  LD   $@"
