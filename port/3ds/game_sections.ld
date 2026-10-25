/* Added to devkitARM's 3dsx linker script (passed with -T, uses INSERT).
 * Places pmd-red's GBA-style RAM sections; see port/game_*_sections.ld. */

gNumMusicPlayers = 8;
gMaxLines = 0;

SECTIONS
{
    .game_init : ALIGN(4)
    {
        INCLUDE game_init_sections.ld
    }
}
INSERT AFTER .data;

SECTIONS
{
    .game_bss (NOLOAD) : ALIGN(4)
    {
        INCLUDE game_bss_sections.ld
    }
}
INSERT AFTER .bss;
