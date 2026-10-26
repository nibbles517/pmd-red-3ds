# Compiles pret/pmd-red's C sources for the 3DS ARM11 (ARMv6K, hard float).
#
# Included by the top-level Makefile. Expects:
#   UPSTREAM  path to the patched pmd-red tree (after `make modern` there, so
#             tools, generated headers and data objects exist)
#   GAME_OBJ  output directory for game objects
#   PREFIX    toolchain prefix (arm-none-eabi-)

GAME_CC      := $(PREFIX)gcc
GAME_AS      := $(PREFIX)as
# Deferred (=): only evaluated when compiling game code, so `make 3ds`
# works without this toolchain on the PATH.
GAME_CC1      = $(shell $(GAME_CC) --print-prog-name=cc1)
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
GAME_SONG_OBJS := $(patsubst $(UPSTREAM)/sound/songs/%.s,$(GAME_OBJ)/songs/%.o,$(wildcard $(UPSTREAM)/sound/songs/*.s))
GAME_UP_DATA   := $(patsubst $(UP_BUILD)/data/%.o,$(GAME_OBJ)/data/%.o,$(wildcard $(UP_BUILD)/data/*.o))
GAME_DATA_OBJS := $(GAME_UP_DATA) $(GAME_SONG_OBJS)

# Some data files switch sections without an .align, leaving them byte-aligned.
# On the GBA they happen to land on aligned addresses; in the port they can end
# up at odd ones, and the 3DS CPU faults on misaligned word and multi-word
# accesses (the GBA's silently ignores the low bits). Align them all.
$(GAME_OBJ)/data/%.o: $(UP_BUILD)/data/%.o
	@mkdir -p $(dir $@)
	@$(PREFIX)objcopy --set-section-alignment .rodata=4 --set-section-alignment .data=4 \
		--set-section-alignment .bss=4 $< $@

# Songs are reassembled with self-relative jump targets (see tools/port_song.py).
$(GAME_OBJ)/songs/%.o: $(UPSTREAM)/sound/songs/%.s $(ROOT)/tools/port_song.py
	@mkdir -p $(dir $@)
	@python3 $(ROOT)/tools/port_song.py $< $(GAME_OBJ)/songs/$*.port.s
	@$(GAME_CC) -E -x assembler-with-cpp -iquote $(UPSTREAM)/include -Wno-trigraphs -DMODERN=1 $(GAME_OBJ)/songs/$*.port.s -o $(GAME_OBJ)/songs/$*.i.s
	@cd $(UPSTREAM) && $(GAME_AS) -mcpu=arm7tdmi --defsym MODERN=1 -o $@ $(GAME_OBJ)/songs/$*.i.s

$(GAME_OBJ)/%.o: $(UPSTREAM)/src/%.c
	@mkdir -p $(dir $@)
	@$(GAME_CC) -E $(GAME_CPPFLAGS) -MMD -MP -MT $@ -MF $(GAME_OBJ)/$*.d $< -o $(GAME_OBJ)/$*.i
	@# preproc reads INCBIN files relative to the upstream tree, so run it there.
	@cd $(UPSTREAM) && $(GAME_PREPROC) $(GAME_OBJ)/$*.i $(GAME_CHARMAP) | $(GAME_CC1) -quiet $(GAME_CFLAGS) -o $(GAME_OBJ)/$*.s
	@$(GAME_AS) -march=armv6k -mfloat-abi=hard -o $@ $(GAME_OBJ)/$*.s
	@echo "  CC   $*.c"

# m4a.c calls into the Thumb sound driver (m4a_1.s). An ARM tail call (`b`)
# cannot switch to Thumb, so the linker would add stubs whose relocations the
# .3dsx loaders reject. Without sibling-call optimisation these become `bl`,
# which the linker turns into `blx`.
$(GAME_OBJ)/m4a.o: GAME_CFLAGS += -fno-optimize-sibling-calls

# Game data tables in C. Some code reads past the end of one array into the
# next one defined after it in the same file (effect animations keep each
# frame's header and its tilemap as two arrays), so these keep their variables
# in source order (GCC otherwise emits them reversed) and in one section, so
# the linker can neither reorder nor drop the "unused" second halves.
GAME_DATA_C_OBJS := $(filter $(GAME_OBJ)/data/%,$(GAME_OBJS))
$(GAME_DATA_C_OBJS): GAME_CFLAGS := $(filter-out -fdata-sections,$(GAME_CFLAGS)) -fno-toplevel-reorder

# Header dependencies, so edits to patched headers rebuild what uses them.
-include $(GAME_OBJS:.o=.d)
