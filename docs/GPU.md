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
- **What can't be read from a map that way** goes in as *line strips*: the
  line's pixels, copied into a strip texture. That is the wide view's HUD
  row, BG1's repeated side tiles (the dungeon darkness), the full-screen
  tiled move effects, lines of a background whose cache doesn't match (its
  settings changed mid-frame, mosaic), and lines whose colours are being
  looked up during a fade.
- **Sprites** are one rectangle each, of a sprite texture into which their
  pixels are coloured with their palettes (flips are the rectangle's).
- **Priority**: each rectangle has a key: within a priority (3 to 0),
  sprites (lower OAM index in front) are in front of BG0, BG0 of BG1, and
  so on. The list is drawn back to front over the backdrop colour, with
  transparent texels left out.
- **Windows** cut each layer's pieces of a line to where the windows show
  it (the game keeps windows 0 and 1 on all the time, mostly to show or
  hide the dungeon darkness, BG1; the light circle will be the same, a line
  at a time).
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
pixel (`--gpu-list 1` only draws from it). On the first runs (town, intro,
dungeon floors, menus, all three screen sizes) every frame but those with a
semi-transparent sprite was listed, and every listed frame matched.

On the 3DS the list is not used yet (`Backend_GpuListMode` returns 0).

## Next stages

2. **The 3DS side.** Each background cache as a 512x512 GPU texture (the
   GBA's 8x8 tiles match the GPU's own 8x8 texture blocks, so a changed tile
   is a small update: only the tiles decoded or recoloured since the last
   frame are sent); the strip and sprite textures each frame; the list's
   rectangles as quads, sorted back to front; alpha blending with the
   stencil buffer; the picture into the framebuffer as now. Then tuning on
   the hardware, with port.log showing how much CPU time the list takes.
3. **Fewer strips.** BG1's side tiles as repeated quads instead of pixels,
   and the HUD row from BG0's cache: in the dungeon those are most of the
   strip rows.
4. **Fades on the GPU.** The game fades by changing every palette; the
   caches' colours are then looked up line by line (and go into strips).
   With the un-faded colours in the textures and the fade amount known, the
   GPU would darken the picture instead.
5. **More effects**: brighten/darken, semi-transparent sprites, the object
   window, where they turn out to matter.
