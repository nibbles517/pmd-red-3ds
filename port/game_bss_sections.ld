/* pmd-red's zero-initialized RAM, kept contiguous and in the same order as the
 * GBA build (its AgbMain clears [ewram_start, ewram_end) and
 * [iwram_start, iwram_end) at boot). Included inside .bss by each platform's
 * linker script. */
. = ALIGN(4);
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
