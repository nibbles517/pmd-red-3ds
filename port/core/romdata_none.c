// A build with the ROM's data in it: no table of it to copy in (romdata.c).
// The shared build's romdata_table.s defines these for real.
#include <stdint.h>

const uint32_t gPortRomDataCount __attribute__((weak)) = 0;
const uint32_t gPortRomDataSum __attribute__((weak)) = 0;
const uint32_t gPortRomDataGroups __attribute__((weak)) = 0;
const uint32_t gPortRomData[1] __attribute__((weak)) = {0};
