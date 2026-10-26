/* Added to devkitARM's 3dsx.ld (through build/pmd-red-3ds.specs, which lists
 * this script before 3dsx.ld so the INSERT commands can find their anchors).
 * Places pmd-red's GBA-style RAM sections inside the data segment. Kept
 * self-contained (no INCLUDE) because the 3DS link does not control -L order.
 * Same contents as port/game_init_sections.ld and port/game_bss_sections.ld. */

gNumMusicPlayers = 8;
gMaxLines = 0;

SECTIONS
{
    /* Initialized RAM (EWRAM_INIT / IWRAM_INIT). AgbMain's copy from "ROM"
     * is a no-op: the source is the data itself. */
    .game_init : ALIGN(4)
    {
        ewram_init_start = .;
        *(ewram_init*)
        . = ALIGN(4);
        ewram_init_end = .;
        EWRAM_INIT_ROM_START = ewram_init_start;
        iwram_init_start = .;
        *(iwram_init*)
        . = ALIGN(4);
        iwram_init_end = .;
        IWRAM_INIT_ROM_START = iwram_init_start;
    }
}
INSERT AFTER .data;

SECTIONS
{
    /* Zero-initialized RAM, contiguous and in GBA order; AgbMain clears it.
     * Placed before .bss so it stays inside the loadable data segment. */
    .game_bss (NOLOAD) : ALIGN(4)
    {
        ewram_start = .;
        *sound_data.o(.bss)
        *(ewram_data*)
        *(ewram_lib*)
        . = ALIGN(4);
        ewram_end = .;
        iwram_start = .;
        *(iwram_data*)
        . = ALIGN(4);
        iwram_end = .;
    }
}
INSERT AFTER .dtors;
