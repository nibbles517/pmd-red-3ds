# pmd-red-3ds

A native Nintendo 3DS port of [pret/pmd-red](https://github.com/pret/pmd-red), the
decompilation of *Pokémon Mystery Dungeon: Red Rescue Team*.

The game's C code is compiled for the 3DS's ARM11 processor instead of running in a
GBA emulator. The approach follows [Twin Embers](https://github.com/alex61194/twin-embers),
a 3DS port of pret's FireRed decompilation.

**Status:** early. In the test harness the game boots through the logos and
title screen to the main menu, plays music, and reads existing saves. The 3DS
backend is written but not yet tested on hardware. See
[docs/BUILDING.md](docs/BUILDING.md).

## Goals

1. Boot and be playable from start to finish, with the original 240 × 160 game on the
   top screen.
2. Later, make use of the second screen.
3. Later, a shareable setup: a separate builder that makes a data file from each
   player's own ROM, so no game data is ever distributed.

## Requirements

- Your own *Pokémon Mystery Dungeon: Red Rescue Team* (US) ROM, SHA-1
  `9f4cfc5b5f4859d17169a485462e977c7aac2b89`. No ROM or game data is included in this
  repository, and none ever will be.
- [devkitPro](https://devkitpro.org/wiki/Getting_Started) with the 3DS Development
  component.

## Credits

- [pret](https://github.com/pret/pmd-red) for the pmd-red decompilation.
- alex61194 ([Twin Embers](https://github.com/alex61194/twin-embers)) and ZallaxDev
  ([pokeemerald-3Ds-dualscreen](https://github.com/ZallaxDev/pokeemerald-3Ds-dualscreen))
  for the 3DS porting approach. Any code adapted from those projects keeps its
  original license and attribution.
