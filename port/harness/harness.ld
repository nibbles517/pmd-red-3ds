/* Memory layout for the test-harness build. Loaded at 0x00100000, where the
 * 3DS also loads homebrew, so addresses look like they will on hardware. */
ENTRY(_start)

/* Constants pmd-red's GBA linker script defines as absolute symbols. */
gNumMusicPlayers = 8;
gMaxLines = 0;

SECTIONS
{
    . = 0x00100000;
    .text : { KEEP(*(.text.start)) *(.text .text.*) *(.glue_7 .glue_7t) }
    .rodata : ALIGN(4) { *(.rodata .rodata.*) }
    .ARM.exidx : { *(.ARM.exidx*) }

    /* Data starts on a fresh page so the harness can map it non-executable,
     * as the 3DS does (catches code that runs from RAM). */
    . = ALIGN(0x1000);
    .data : ALIGN(4) {
        *(.data .data.*)
        INCLUDE game_init_sections.ld
    }

    .bss (NOLOAD) : ALIGN(4) {
        __bss_start = .;
        INCLUDE game_bss_sections.ld
        *(.bss .bss.*) *(COMMON)
        . = ALIGN(4);
        __bss_end = .;
    }

    . = ALIGN(16);
    __heap_start = .;
    . += 0x1800000; /* 24 MiB (the DS music too) */
    __heap_end = .;
    . += 0x40000;
    __stack_top = .;

    /DISCARD/ : { *(.ARM.attributes) *(.comment) *(.note*) }
}
