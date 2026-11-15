# Building pmd-red-3ds

The build has two halves:

1. **Game half** (`make prelink`). Compiles pret/pmd-red's C for the 3DS CPU and
   partially links it with the platform core into `build/pmd-red-game.o`. This
   needs your own ROM and pret's tools, and the result contains game data from
   your ROM: keep it to yourself.
2. **3DS half** (`make 3ds`). Compiles the small 3DS backend and links it with
   `pmd-red-game.o` into `pmd-red-3ds.3dsx`. Needs only devkitPro.

## 3DS half on Windows (devkitPro)

1. Install devkitPro with the **3DS Development** component, then in the
   devkitPro MSYS2 window run:

   ```sh
   pacman -Syu
   pacman -S --needed 3ds-dev git make
   ```

   (`3ds-dev` includes citro3d, which puts the pictures on the screens.)

2. Get this repository (anywhere without spaces in the path):

   ```sh
   cd /c/
   git clone https://github.com/ethangarsia517/pmd-red-3ds
   cd pmd-red-3ds
   ```

3. Put `pmd-red-game.o` in the `build` folder (create it if needed), then:

   ```sh
   make 3ds DEVKITPRO=/opt/devkitpro
   ```

   The result is `build/pmd-red-3ds.3dsx`.

## Running it

The GPU puts the pictures on the screens. If they look wrong (upside down,
misplaced, garbled), put an empty `cpu-video.txt` in `SD:/3ds/pmd-red-3ds/`:
the CPU then copies them, as earlier builds did.

- **Azahar:** File → Load File → `pmd-red-3ds.3dsx`.
- **3DS:** copy it to `SD:/3ds/pmd-red-3ds/pmd-red-3ds.3dsx` and start it from the
  Homebrew Launcher. Sound needs `SD:/3ds/dspfirm.cdc` (most custom firmware
  setups already have it).

The save file is `SD:/3ds/pmd-red-3ds/pmd-red.sav`, a standard 128 KiB GBA save:
you can copy a save from a GBA emulator there, or take the port's save back to
an emulator. Back up saves before testing early builds. That is Save Data 1;
Game Options > Change Current Save Data on the title's main menu switches to
Save Data 2 or 3 (`pmd-red-2.sav`, `pmd-red-3.sav` in the same folder).

Game Options > Music Options on the title's main menu plays the DS version's
music: put your own dump of Pokémon Mystery Dungeon: Blue Rescue Team, named
`pmd-blue.nds`, in that same folder (`SD:/3ds/pmd-red-3ds/`).
Its Music Style (GBA | DS) then picks which music the game plays: with DS,
each GBA song plays as the DS track it is matched with.
The Music Player lists the ROM's sound effects after its music (in green);
an effect plays once. Each GBA sound effect plays as the DS one with the
same number (built in).
Game Options > DS Volume Balance sets each DS track's own volume against the
GBA song or sound effect it stands for: A plays the GBA one, Y the DS one,
L/R change the DS one's volume by 1%, ZL/ZR by 10% (heard at once), X switches
between the music and the sound effects (SELECT plays a DS song under the
effects; with L/R or ZL/ZR held, another), START or B saves. The volumes are
built in (port/core/music.c); `ds-volumes.txt` in the same folder keeps only
those changed from the built-in ones (each with the built-in one beside it),
so a player's file is their balance changes, ready to be merged.
Sound Effects (GBA | DS), under Music Style, picks which sound effects the
game plays: with DS (the default), each GBA sound effect plays as its DS
match, at the SFX Volume; it starts, fades and stops with the GBA
one (which plays on silently), and a DS effect longer than the GBA one plays
on to its own end. Both settings are GBA only (in red) with another game's ROM.
Music Volume and SFX Volume (Left/Right: 10% a press) set the DS music's and
the DS sound effects' volumes; the Music Player's volume buttons (Circle Pad
up/down or ZL/ZR) set the volume of what plays (or of the track the cursor is
on), and DS Volume Balance's (Circle Pad up/down, on its music) the DS
music's.
With Dual Core on, the DS music and sound effects are made on the second CPU
core while the game runs, a frame ahead; that is all the second core does (the
picture is drawn on the main core).

The matches are built in (port/core/music.c, sDefaultMap). Music Matching, the
development aid they were made with, is hidden; a build with
`-DPORT_DEV_MUSIC_MATCHING=1` shows it again in Game Options. It writes
`music-map.txt` to the same folder, and while that file is there the game uses
its matches instead of the built-in ones.

Controls are the GBA's (A, B, L, R, START, SELECT, D-pad or Circle Pad).
**Y** switches the picture between 1:1 and 1.5x. Use **HOME** to quit, after
saving in the game.

For a log, create an empty file `SD:/3ds/pmd-red-3ds/debug.txt`; the port then
writes `port.log` next to it. Every 5 seconds it has a line with the time each part
of a frame took, what the second screen drew, and how busy the second core
was (`core2`: all its work, and of that the DS music's, in ms per frame).
With GPU Drawing its `gpu:` part says whether it is on, how many of the
frames drawn came from the drawing list (`list=listed/drawn`), how long
writing its textures took (`up`), and why the other frames were drawn by the
CPU (`window=`, `effect=`, `obj-semi=`, ...: docs/GPU.md).
A frame that takes over 20 ms gets its own `slow frame` line (up to 20 every
5 seconds): where its time went, its texture writing (`up`: backgrounds,
strips and sprites, and how much of it was flushing the CPU's cache), and whether it
was a GPU frame in a fade (`list fade`, with the fade), a palette change the
GPU couldn't take as a fade (`list palette-change`) or a CPU frame
(`cpu-picture`).

Others > Graphics Options > **GPU Drawing** (On at first) has the GPU draw the
main screen from the drawing list (docs/GPU.md); Off draws it on the CPU as
before. The GPU reads its textures' rows bottom row first, so they are
written that way; an empty `gpu-noflip.txt` in `SD:/3ds/pmd-red-3ds/` writes
them top row first (the first stage 2 build did, and every piece came out
upside down or missing). Its textures are flushed from the CPU's cache by the
kernel, on the main core; an empty `gpu-gspflush.txt` there flushes them
through the GSP service instead, as before (which, with Dual Core on, waited
for the DS music's worker on the system core).

## Game half (Linux or WSL)

```sh
tools/bootstrap.sh                      # fetch pret/pmd-red at the pinned commit, apply patches
cp /path/to/your-rom.gba build/upstream/baserom.gba
make upstream                           # pret's own build: extracts game data from the ROM
make prelink                            # -> build/pmd-red-game.o
```

Needs an `arm-none-eabi` GCC, host GCC, libpng and Python 3, as pret/pmd-red
does (see its INSTALL.md). For the test harness, also Unicorn: see
`tools/harness.py`.
