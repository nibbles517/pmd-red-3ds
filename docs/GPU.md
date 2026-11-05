# Drawing on the GPU

The main screen is drawn by a scanline renderer (`port/core/ppu.c`) on the
main CPU core. On an Old 3DS that takes most of a frame in busy scenes, while
the 3DS's GPU (the PICA200) sits nearly idle. The plan is to let the GPU draw
the frames that make up nearly all of play, and keep the renderer for the
rest, frame by frame: a frame the GPU path can't express is drawn by the
renderer exactly as before, so switching between the two is invisible.

## The drawing list (stage 1: done)

`port/core/gpulist.h`. With `Backend_GpuListMode()` on, frame.c describes each
frame instead of drawing it, line by line as the frame's events go by
(`Ppu_ListLine`), keeping a copy of the registers each line had:

- **Backgrounds** are rectangles of their maps as the background cache keeps
  them decoded (`bgcache.h`: each layer a 512x512 picture). A line reading
  the same columns of the next map row as the line before continues the
  same rectangle, so a background is usually one to four rectangles (more
  where it wraps, or where its scroll changes down the screen).
- **BG1's side tiles** in the wide view (the dungeon darkness reaching the
  screen's edges: its fill tile repeated) are rectangles of that tile in
  BG1's cache, one per 8-pixel column of the pattern, each the height of
  the screen.
- **What can't be read from a map that way** goes in as *line strips*: the
  line's pixels, copied into a strip texture. That is the wide view's HUD
  row, the full-screen tiled move effects, lines of a background whose
  cache doesn't match (its settings changed mid-frame, mosaic), and lines
  whose colours are being looked up during a fade.
- **Sprites** are one rectangle each, of a sprite texture into which their
  pixels are coloured with their palettes (flips are the rectangle's).
- **Priority**: each rectangle has a key: within a priority (3 to 0),
  sprites (lower OAM index in front) are in front of BG0, BG0 of BG1, and
  so on. The list is drawn back to front over the backdrop colour, with
  transparent texels left out.
- **Windows** cut each layer's pieces of a line to where the windows show
  it (the game keeps windows 0 and 1 on all the time, mostly to show or
  hide the dungeon darkness, BG1; the light circle is the same, a line at a
  time). A line whose window settings are the last line's reuses its cuts.
- **Alpha blending** (on in nearly every frame: 50/50 with BG2 in towns, the
  darkness over the floor in dungeons): every pixel keeps a flag, which will
  be the GPU's stencil: whether what is there is a second target. A first
  target layer's rectangle (where the windows allow the effect) blends with
  what is under it where the flag is set, else covers it; each rectangle
  sets the flag to whether it is a second target. As the GBA blends a
  pixel's top layer with the one right under it, that is exact as long as no
  layer is both a first and a second target, which the game doesn't do.

Frames the list doesn't take (the renderer draws them): forced blank, modes
1-5 (rotated backgrounds), the object window, brighten/darken effects,
blending settings that change down the screen, semi-transparent, rotated or
mosaic sprites, and sprites the windows hide in part of the screen. The
harness logs every 300 frames how many frames were listed and why the others
weren't (`gpu-list:` lines).

`GpuList_Draw` draws a list in software. The test harness's `--gpu-list 2`
draws every frame from its list and checks it against the renderer, pixel for
pixel (`--gpu-list 1` only draws from it).

Results so far (harness runs through the title, intro, town, four dungeon
saves, menus and attacks, in the GBA, DS and Fit sizes; over 40,000 frames):
every frame was listed except a few with a semi-transparent sprite (about 1
in 1,000), and every listed frame matched the renderer's, pixel for pixel.
A dungeon frame (Fit) is about 190 rectangles and 45 strip rows. Describing
it takes the main core about a tenth of the instructions drawing it does
(dungeon, Fit: about 0.35 million against 4.5 million, plus 0.16 million to
check the caches either way); what is left is the GPU's work, and sending
it the changed tiles and the strips.

## The 3DS side (stage 2: built, being tried on hardware)

`port/3ds/main.c`, "GPU Drawing". With Graphics Options > GPU Drawing On (and
the GPU path in use, not `cpu-video.txt`), `Backend_GpuListMode` returns 3:
frame.c hands each listed frame to `Backend_PresentList` instead of drawing
it, and the GPU draws it in `GpuDrawFrame`:

- **Textures** in linear memory, RGBA5551 (alpha 0: transparent), written by
  the CPU straight in the GPU's layout (8x8 tiles, each in Z order): one
  512x512 per background cache, coloured from the cache's colour numbers
  and the list's base palette; the strip texture (pixels) and the sprite
  texture (colour numbers, coloured from the base palette), only where the
  frame's rectangles read. A background tile is written when the frame shows
  it and it changed since: decoded again (`gpuDirty`, set by
  `BgCache_Decode`), or its palette bank's base colours changed. Written
  after `C3D_FrameBegin` (the GPU is done with the last frame).
- **Fades** (gpulist.c `GpuList_Palette`): the game's palette buffer keeps,
  for every entry, the colour it was made from and the fade that made it
  (`gPortPalBase`, `gPortPalFade`: copied to palette memory with the colours;
  the town's palette fades say theirs, the dungeon's brightness is its own).
  When every entry the frame shows (the palette banks its background tiles
  and sprites use) is the same fade of its own colour, the base palette is
  those colours and the GPU does the fade (each texel * mul + add): the
  textures don't change during a fade, and nothing becomes strips. Otherwise
  the base palette is the palettes as they are, and what changed is written
  again (only where shown).
- **Drawing** straight onto the screen at the picture's place and scale (the
  same as the CPU picture's: Size and Scale settings): the target cleared
  black, a backdrop rectangle, then the rectangles back to front, cut to the
  list's area, transparent texels left out by the alpha test. Three texture
  stages: the fade's multiply, its added colour (not for strips: their
  pixels are already coloured), and the screen change fade's grey.
- **Blending**: the screens' render targets have a stencil buffer, cleared to
  whether the backdrop is a second target. A rectangle that isn't a first
  target replaces the stencil with whether it is a second target; one that
  is draws twice: where the stencil is 0 as it is, then where it is 1 with
  GPU blending (constant colour eva/16, constant alpha evb/16), clearing it.
- **Rows**: texture coordinate v = y / 512 is image row y, which the GPU
  reads from memory row 511 - y, so the rows are written bottom row first
  (the first build on hardware wrote them top first: the pieces read the
  wrong end of their textures, mostly empty, and came out missing or
  upside down). `gpu-noflip.txt` writes them top first again.

port.log's `gpu:` part: on/off, frames from the list out of those drawn,
the time writing textures takes, and why the others weren't listed.

## Next stages

2. **Tuning on the hardware** (stage 2 is built): what port.log shows the
   list, its textures and the GPU taking; the strips and sprites written in
   the GPU's format by the core instead of converted.
3. **Fewer strips.** The HUD row from BG0's cache, and the other strips
   that remain (a few rows a frame, now that fading lines aren't strips).
4. **More effects**: brighten/darken, semi-transparent sprites, the object
   window, where they turn out to matter.
