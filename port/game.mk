# Compiles pret/pmd-red's C sources for the 3DS ARM11 (ARMv6K, hard float).
#
# Included by the top-level Makefile. Expects:
#   UPSTREAM  path to the patched pmd-red tree (after `make modern` there, so
#             tools, generated headers and data objects exist)
#   GAME_OBJ  output directory for game objects
#   PREFIX    toolchain prefix (arm-none-eabi-)

GAME_CC      := $(PREFIX)gcc
GAME_AS      := $(PREFIX)as
GAME_CC1     := $(shell $(GAME_CC) --print-prog-name=cc1 2>/dev/null)
GAME_PREPROC := $(UPSTREAM)/tools/preproc/preproc
GAME_CHARMAP := $(UPSTREAM)/charmap.txt

ARCH_FLAGS   := -marm -march=armv6k+fp -mtune=mpcore -mfloat-abi=hard -mtp=soft

# MODERN=1 selects pmd-red's C fallbacks for functions that only match as asm.
GAME_CPPFLAGS := -iquote $(UPSTREAM)/include -iquote $(ROOT)/port/decomp -Wno-trigraphs -DMODERN=1 -DPLATFORM_3DS
# pmd-red's structs, data tables and save format assume the GBA's old APCS rule
# that every struct's size is a multiple of 4 bytes. Reproduce it for game code.
GAME_ABI_FLAGS := -mstructure-size-boundary=32
# -fno-strict-aliasing/-fwrapv: decompiled code relies on GBA-era compiler leniency.
GAME_CFLAGS   := $(ARCH_FLAGS) $(GAME_ABI_FLAGS) -O2 -g -fno-strict-aliasing -fwrapv -fno-common \
                 -ffunction-sections -fdata-sections -w

# Every game C file, as upstream builds them. libisagbprn is a debug printer for
# GBA emulators and touches cartridge addresses directly; the port has its own log.
GAME_C_ALL   := $(wildcard $(UPSTREAM)/src/*.c $(UPSTREAM)/src/*/*.c $(UPSTREAM)/src/*/*/*.c)
GAME_C_SRCS  := $(filter-out %.inc.c $(UPSTREAM)/src/libisagbprn.c,$(GAME_C_ALL))
GAME_OBJS    := $(patsubst $(UPSTREAM)/src/%.c,$(GAME_OBJ)/%.o,$(GAME_C_SRCS))

# Data and music are target-independent: reuse the objects upstream's modern
# build assembled from your ROM's extracted assets.
UP_BUILD     := $(UPSTREAM)/build/pmd_red_modern
GAME_DATA_OBJS := $(wildcard $(UP_BUILD)/data/*.o) $(wildcard $(UP_BUILD)/sound/songs/*.o)

$(GAME_OBJ)/%.o: $(UPSTREAM)/src/%.c
	@mkdir -p $(dir $@)
	@$(GAME_CC) -E $(GAME_CPPFLAGS) $< -o $(GAME_OBJ)/$*.i
	@# preproc reads INCBIN files relative to the upstream tree, so run it there.
	@cd $(UPSTREAM) && $(GAME_PREPROC) $(GAME_OBJ)/$*.i $(GAME_CHARMAP) | $(GAME_CC1) -quiet $(GAME_CFLAGS) -o $(GAME_OBJ)/$*.s
	@$(GAME_AS) -march=armv6k -mfloat-abi=hard -o $@ $(GAME_OBJ)/$*.s
	@echo "  CC   $*.c"
