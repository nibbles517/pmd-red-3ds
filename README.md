# pmd-red-3ds

A native Nintendo 3DS port of [pret/pmd-red](https://github.com/pret/pmd-red), the
decompilation of *Pokémon Mystery Dungeon: Red Rescue Team*.

The game's C code is compiled for the 3DS's ARM11 processor instead of running in a
GBA emulator. The approach follows [Twin Embers](https://github.com/alex61194/twin-embers),
a 3DS port of pret's FireRed decompilation.

**No game data is included.** The `.3dsx` has none of the ROM's data in it: when it
starts, it reads it from your own copy of the game.

## Playing

You need:

- A 3DS with custom firmware and the Homebrew Launcher (or the Azahar emulator).
- Your own ROM of *Pokémon Mystery Dungeon: Red Rescue Team* (**USA**), SHA-1
  `9f4cfc5b5f4859d17169a485462e977c7aac2b89`, named **`pmdred.gba`**.
- For sound on a 3DS, `SD:/3ds/dspfirm.cdc` (most custom firmware setups already
  have it).

1. Download `pmd-red-3ds.3dsx`.
2. Copy it to `SD:/3ds/pmd-red-3ds/` (or anywhere in `SD:/3ds/`).
3. Put `pmdred.gba` in the same folder as the `.3dsx`, or in `SD:/3ds/pmd-red-3ds/`.
4. Start it from the Homebrew Launcher.

If the ROM is missing, or isn't the USA release, the port says so and stops. Other
releases (Europe), hacks and trimmed ROMs won't work.

Saves are `SD:/3ds/pmd-red-3ds/pmd-red-1.sav` (and `-2`, `-3` for Save Data 2 and 3),
standard 128 KiB GBA saves: you can bring a save over from a GBA emulator, or take
it back.

### What the port adds

- Uses the second screen.
- Wider views for the 3DS's screens, scaling options, and the GPU drawing the game.
- Three save files (Game Options > Change Current Save Data).
- Music Options: the DS version's music and sound effects from your own dump of
  *Blue Rescue Team*, picked from the SD card; Extra Music plays songs from your own
  *Explorers of Sky* on post-game dungeon floors.
  With Extra Music on, its songs are read from that ROM when the game starts,
  so starting takes a few seconds longer.
- Cheats in dungeons (Others > Cheats).

More detail: [docs/BUILDING.md](docs/BUILDING.md).

## Building

GitHub Actions builds the shareable `.3dsx` with no ROM at all
(`.github/workflows/build.yml`). To build it yourself, or a personal build with
your ROM's data in it, see [docs/BUILDING.md](docs/BUILDING.md).

## Credits

- [pret](https://github.com/pret/pmd-red) for the pmd-red decompilation.
- alex61194 ([Twin Embers](https://github.com/alex61194/twin-embers)) and ZallaxDev
  ([pokeemerald-3Ds-dualscreen](https://github.com/ZallaxDev/pokeemerald-3Ds-dualscreen))
  for the 3DS porting approach. Any code adapted from those projects keeps its
  original license and attribution.
- The NDS Music Player's core (`port/ndsmusic`), used for the DS music.

*Pokémon Mystery Dungeon* is © Nintendo, Creatures Inc., GAME FREAK inc. and
Spike Chunsoft. This project is not affiliated with or endorsed by them.
