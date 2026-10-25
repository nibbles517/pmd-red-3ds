/* pmd-red's initialized RAM (EWRAM_INIT / IWRAM_INIT). On the GBA these are
 * copied from ROM at boot; here the loader places them, and AgbMain's copy is
 * a no-op because the "ROM" source is the data itself. Included inside an
 * output section by each platform's linker script. */
. = ALIGN(4);
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
