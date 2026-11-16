// The second screen (DS-style, 256x192), as in the DS version:
// - in dungeons, the team panel: each member's sprite (same animation as on
//   the main screen, always facing down-left), name, level, HP and HP bar,
//   held item and tactic;
// - elsewhere, the world map around the current place, with a yellow dot on
//   it and the place's name.
//
// Built from the game's own data and graphics (its font, the monsters' sprite
// files and the palettes in OBJ palette RAM). It only reads game state, and
// never calls anything that could change it (no RNG, no allocation).
// Compiled with pmd-red's include path.

#include <stdio.h>
#include "global.h"
#include "constants/direction.h"
#include "constants/dungeon.h"
#include "constants/event_flag.h"
#include "constants/monster.h"
#include "decompress_at.h"
#include "decompress_sir.h"
#include "code_8004AA0.h"
#include "def_filearchives.h"
#include "event_flag.h"
#include "friend_area.h"
#include "constants/friend_area.h"
#include "game_options.h"
#include "graphics_memory.h"
#include "ground_place.h"
#include "string_format.h"
#include "dungeon_info.h"
#include "dungeon_util.h"
#include "dungeon_vram.h"
#include "items.h"
#include "weather.h"
#include "pokemon.h"
#include "strings.h"
#include "text_1.h"
#include "text_2.h"
#include "text_3.h"
#include "structs/axdata.h"
#include "structs/str_dungeon.h"
#include "number_util.h"
#include "../core/port.h"
#include "port_game.h"

extern Dungeon *gDungeon;
extern const DungeonPos gDungeonCoordinates[64]; // world_map.c
extern struct FriendAreasMap *gFriendAreasMapPtr; // friend_areas_map.c (NULL when not showing)
s32 Port_GroundExitRequest(void);                 // ground_main.c
bool8 Port_GroundIsDemo(void);                    // ground_main.c
s32 Port_TitleBrightness(void);                   // main_loops.c
u16 Port_GroundBrightness(void);                  // palette_util.c
bool8 Port_FriendAreaMapPos(u8 areaId, DungeonPos *pos); // friend_areas_map.c

// The picture's size: 320x240 with Sub-Screen Settings > Size: Fit outside
// dungeons (the world map), else 256x192 (set by Port_RenderSubScreen).
static int sSubW = 256, sSubH = 192;
#define W sSubW
#define H sSubH
#define CANVAS_PIXELS (SUBSCREEN_W_MAX * SUBSCREEN_H_MAX)
#define FIT (W > 256) // the picture is 320x240 (Sub-Screen Settings > Size: Fit)
#define ROW_H (H / 4)  // Blue: a member's row, 48 lines (60 in Fit)
#define ROW_EXTRA (ROW_H - 48)

static u16 *sDirect;    // the caller's picture when it holds the canvas as it is (else NULL):
static bool8 sDirectDone; // ... a layout that drew into it too sets this (nothing to copy out)
static u16 *sOut;       // where drawing goes
static u16 *sOutFrame;  // the caller's buffer
static const u16 *const sPltt = (const u16 *)gPortPltt; // BG palettes, then OBJ

static inline u16 Rgb(int r, int g, int b)
{
    return (u16)((r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10));
}

// Drawing stays inside the clip box (normally the whole picture); with
// sTrackBox, the box around what is drawn grows (the team panel's sprites).
static int sClipX0, sClipY0, sClipX1 = 0x7FFF, sClipY1 = 0x7FFF;
static bool8 sTrackBox;
static int sBoxX0, sBoxY0, sBoxX1, sBoxY1;

static inline void Plot(int x, int y, u16 c)
{
    if ((unsigned)x < W && (unsigned)y < H) {
        if (sTrackBox) {
            if (x < sBoxX0) sBoxX0 = x;
            if (y < sBoxY0) sBoxY0 = y;
            if (x >= sBoxX1) sBoxX1 = x + 1;
            if (y >= sBoxY1) sBoxY1 = y + 1;
        }
        if (x >= sClipX0 && x < sClipX1 && y >= sClipY0 && y < sClipY1)
            sOut[y * W + x] = c;
    }
}

static void Fill(int x, int y, int w, int h, u16 c)
{
    for (int j = y; j < y + h; j++)
        for (int i = x; i < x + w; i++)
            Plot(i, j, c);
}

u16 Port_PaletteColor(u16 index); // palette_fade_util.c: as set, before fades
bool8 PortShiny_SlotColors(s32 slot, u16 *out); // port_shiny.c: a shiny's colours in an OBJ palette

// The colours of OBJ palette pal as the game set them (the main screen's
// fades and flashes do not reach the second screen).
static void ObjPalette(int pal, u16 *out)
{
    for (int i = 0; i < 16; i++)
        out[i] = Port_PaletteColor(256 + (pal & 15) * 16 + i) & 0x7FFF;
}

// For the performance meter (Port_SubScreenStats)
static struct {
    int all, rows, sprites, water, map, darken, copied; // copied: pixels copied out
} sStats;

static void Dirty(int x, int y, int w, int h);
static void Restore(const u16 *from, int x, int y, int w, int h);

static int Lerp(int a, int b, int t, int n) { return a + (b - a) * t / n; }

// Is (x, y) outside a w x h box with corners rounded by radius r?
static bool8 OutsideRounded(int x, int y, int w, int h, int r)
{
    int cx = x < r ? r - 1 : (x >= w - r ? w - r : x);
    int cy = y < r ? r - 1 : (y >= h - r ? h - r : y);
    int dx = (x - cx) * 2 + (x < r ? 1 : (x >= w - r ? -1 : 0));
    int dy = (y - cy) * 2 + (y < r ? 1 : (y >= h - r ? -1 : 0));
    if (cx == x || cy == y)
        return FALSE;
    return dx * dx + dy * dy > (2 * r - 1) * (2 * r - 1);
}

// --- Backgrounds ------------------------------------------------------------

static u16 Gray(int r, int g, int b, bool8 gray)
{
    if (gray) {
        int v = (r * 3 + g * 6 + b) / 10;
        v = v * 4 / 5; // the empty slot is a little darker
        return Rgb(v, v, v);
    }
    return Rgb(r, g, b);
}

// The brown box behind a sprite, with the cream floor it stands on.
static void DrawSpriteBox(int y0, bool8 gray)
{
    const int w = 47, h = ROW_H - 1;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            if (OutsideRounded(x, y, w, h, 4))
                continue;
            int r, g, b;
            if (y < 4) {
                r = Lerp(196, 142, y, 4); g = Lerp(150, 77, y, 4); b = Lerp(88, 16, y, 4);
            } else if (y < 27 + ROW_EXTRA) {
                r = 134; g = 63; b = 2;
            } else {
                // floor, lighter towards the middle of an ellipse
                int dx = x - 23, dy = y - 37 - ROW_EXTRA;
                int d = dx * dx * 100 / (22 * 22) + dy * dy * 100 / (9 * 9); // < 100 inside
                if (y < 29 + ROW_EXTRA) { r = 170; g = 115; b = 50; }
                else if (d < 100) {
                    r = Lerp(250, 222, d, 100); g = Lerp(230, 190, d, 100); b = Lerp(190, 132, d, 100);
                } else { r = 212; g = 172; b = 110; }
            }
            Plot(x, y0 + y, Gray(r, g, b, gray));
        }
    }
}

// The lavender panel with the member's details.
static void DrawInfoPanel(int y0)
{
    const int x0 = 49, w = W - x0, h = ROW_H - 1;
    for (int y = 0; y < h; y++) {
        int r = Lerp(184, 100, y, h), g = Lerp(183, 101, y, h), b = Lerp(224, 150, y, h);
        if (y == 0) { r = 200; g = 200; b = 236; }
        u16 c = Rgb(r, g, b);
        for (int x = 0; x < w; x++)
            if (!OutsideRounded(x, y, w, h, 4))
                Plot(x0 + x, y0 + y, c);
    }
}

// --- Text, with the game's font ------------------------------------------------

#define COLOR_TEXT   Rgb(248, 248, 248)
#define COLOR_NAME   Rgb(240, 240, 40)
#define COLOR_SHADOW Rgb(40, 40, 72)

static void DrawGlyph(int x, int y, u32 chr, u16 color)
{
    const unkChar *glyph = GetCharacter(chr);
    const u16 *rows = glyph->unk0;
    int height = gCharHeight[gCurrentCharmap];
    bool8 ownColors = glyph->unkA & 1; // symbols drawn in the font palette's colors

    for (int pass = 0; pass < 2; pass++) { // shadow first, then the glyph
        const u16 *row = rows;
        for (int j = 0; j < height; j++, row += 3) {
            u64 bits = row[0] | ((u32)row[1] << 16) | ((u64)row[2] << 32);
            for (int i = 0; i < 12; i++) {
                u32 n = (bits >> (i * 4)) & 0xF;
                if (n == 0)
                    continue;
                if (pass == 0) {
                    if (!ownColors) {
                        Plot(x + i + 1, y + j, COLOR_SHADOW);
                        Plot(x + i + 1, y + j + 1, COLOR_SHADOW);
                    }
                } else if (ownColors) {
                    Plot(x + i, y + j, sPltt[0xF0 + n] & 0x7FFF); // BG palette 15: the font's
                } else {
                    Plot(x + i, y + j, color);
                }
            }
        }
    }
}

static int GlyphWidth(u32 chr) { return GetCharacter(chr)->width; }

// Draws one line of game-encoded text; returns the x after it. Skips the
// game's formatting codes (#C colour changes and the like).
static int DrawText(int x, int y, const u8 *str, u16 color)
{
    while (TRUE) {
        u32 chr;
        const u8 *next = GetNextCharFromStr(str, &chr);
        if (chr == '\0' || chr == '\n' || chr == '\r')
            break;
        str = next;
        if (chr == '#') {
            switch (*str) {
                case 'C': case 'c': case '=': case '~': str += 2; break;
                case 'R': case 'r': case '+': case 'W': str += 1; break;
            }
            continue;
        }
        if (chr == '`') { x += 6; continue; }
        DrawGlyph(x, y, chr, color);
        x += GlyphWidth(chr);
    }
    return x;
}

static int TextWidth(const u8 *str)
{
    return GetStringLineWidth(str);
}

static void FormatInt(u8 *buf, int v)
{
    char tmp[12];
    int n = 0;
    if (v < 0) v = 0;
    do { tmp[n++] = '0' + v % 10; v /= 10; } while (v);
    while (n) *buf++ = tmp[--n];
    *buf = 0;
}

// --- HP bar ---------------------------------------------------------------------

static void DrawBar(int x, int y, int w, int hp, int maxHp, u16 fillColor);

static void DrawHpBar(int x, int y, int w, int hp, int maxHp)
{
    DrawBar(x, y, w, hp, maxHp, Rgb(72, 216, 72));
}

// The HP bar's style, with another colour for what is left (the belly's)
static void DrawBar(int x, int y, int w, int hp, int maxHp, u16 fillColor)
{
    int fill = maxHp > 0 ? (hp * w + maxHp - 1) / maxHp : 0;
    if (fill > w) fill = w;
    if (hp > 0 && fill == 0) fill = 1;
    Fill(x, y, w, 1, Rgb(16, 16, 16));
    Fill(x, y + 1, w, 1, Rgb(248, 248, 248));
    Fill(x, y + 2, fill, 4, fillColor);
    Fill(x + fill, y + 2, w - fill, 4, Rgb(232, 120, 96));
    Fill(x, y + 6, w, 1, Rgb(248, 248, 248));
    Fill(x, y + 7, w, 1, Rgb(16, 16, 16));
}

// --- Sprites -----------------------------------------------------------------------

static const u8 sObjSizes[3][4][2] = { // [shape][size] = width, height in tiles
    { {1, 1}, {2, 2}, {4, 4}, {8, 8} },
    { {2, 1}, {4, 1}, {4, 2}, {8, 4} },
    { {1, 2}, {1, 4}, {2, 4}, {4, 8} },
};

// Tile graphics of the current pose, as the game would load them into VRAM.
static u8 sTiles[0x4000];

static int AnimLength(const ax_anim *a)
{
    int n = 0;
    while (a[n].frames != 0 && n < 256)
        n++;
    return n;
}

// Draws one pose (a list of OAM-style pieces) with its anchor at (ax, ay).
// Tile graphics come from `tiles` (already in VRAM order, as for effect and
// map sprite files) followed by the pieces' own graphics chunks (monsters).
// `pals` holds the 16 OBJ palettes; pieces use palNum unless they name their
// own (as AddAxSprite does).
static void DrawPose(const ax_pose *first, const ax_sprite *const *spriteData,
                     const u32 *tiles, int tileCount, const u16 *pals, int palNum, int ax, int ay)
{
    const ax_pose *piece, *last;
    int tilesUsed = 0;

    if (tiles != NULL) {
        tilesUsed = tileCount * 32;
        if (tilesUsed > (int)sizeof(sTiles))
            return;
        memcpy(sTiles, tiles, tilesUsed);
    }
    // First load the pieces' graphics in order, as the game does.
    for (piece = first; (u16)piece->sprite != 0xFFFF || *(const u16 *)&piece->unk2 != 0xFFFF; piece++) {
        const ax_sprite *s;
        if (piece->sprite < 0 || spriteData == NULL)
            continue;
        for (s = spriteData[piece->sprite]; s->byteCount != 0; s++) {
            if (tilesUsed + s->byteCount > (int)sizeof(sTiles))
                return;
            if (s->gfx != NULL)
                memcpy(sTiles + tilesUsed, s->gfx, s->byteCount);
            else
                memset(sTiles + tilesUsed, 0, s->byteCount);
            tilesUsed += s->byteCount;
        }
    }
    last = piece;

    // Then draw the pieces, last first so the first ends up on top (like OAM).
    for (piece = last - 1; piece >= first; piece--) {
        u16 a0 = piece->flags1, a1 = piece->flags2, a2 = piece->flags3;
        int shape = (a0 >> 14) & 3, size = (a1 >> 14) & 3;
        int tw, th, px, py, hflip, vflip, tile;
        if (shape == 3)
            continue;
        tw = sObjSizes[shape][size][0];
        th = sObjSizes[shape][size][1];
        px = ax + (a1 & 0x1FF) - 0x100;
        py = ay + (a0 & 0x3FF) - 0x200;
        hflip = (a1 >> 12) & 1;
        vflip = (a1 >> 13) & 1;
        tile = a2 & 0x3FF;
        const u16 *pal = &pals[(((a1 >> 10) & 1) ? (a2 >> 12) : palNum) * 16];
        for (int y = 0; y < th * 8; y++) {
            int sy = vflip ? th * 8 - 1 - y : y;
            for (int x = 0; x < tw * 8; x++) {
                int sx = hflip ? tw * 8 - 1 - x : x;
                int t = tile + (sy / 8) * tw + sx / 8; // 1D mapping
                int off = t * 32 + (sy & 7) * 4 + (sx & 7) / 2;
                int c;
                if (off >= tilesUsed)
                    continue;
                c = (sTiles[off] >> ((sx & 1) * 4)) & 0xF;
                if (c)
                    Plot(px + x, py + y, pal[c] & 0x7FFF);
            }
        }
    }
}

static const void *FindFile(const FileArchive *arc, const char *name);

// Monster palettes, as the game sets them in OBJ palette RAM from "palet"
// (GetPokemonOverworldPalette picks one per species). Read from the file, so
// the screen fades and colour effects of the main screen don't reach them.
static u16 sPalet[256];
static bool8 sPaletLoaded;

static bool8 LoadPalet(void)
{
    const RGB_Struct *pal;
    if (sPaletLoaded)
        return TRUE;
    pal = FindFile(&gMonsterFileArchive, "palet");
    if (pal == NULL)
        return FALSE;
    for (int i = 0; i < 240; i++)
        sPalet[i] = Rgb(pal[i].r, pal[i].g, pal[i].b);
    sPaletLoaded = TRUE;
    return TRUE;
}

// --- Team panel -----------------------------------------------------------------------
//
// What the panel shows is kept per row, refreshed from the dungeon while the
// team's sprites are loaded. When they are not (between floors, on the
// results screen after clearing or fainting), the rows stay as they were and
// the sprites keep animating on their own.

#define MAX_ROW_SYMBOLS 8

typedef struct {
    bool8 used;
    bool8 leader;
    u8 name[POKEMON_NAME_LENGTH + 1];
    u8 level;
    s16 hp, maxHp;
    s16 belly, maxBelly;
    const u8 *item;     // NULL: none
    s16 itemIcon;       // the item's sprite as on the floor (-1: none): its tiles
    u8 itemPal;         // ... and OBJ palette
    const u8 *tactic;
    const axmain *sprite;
    u16 pal[16];        // its palette's colours as the main screen has them (a shiny's own: port_shiny.c)
    bool8 palOk;
    u8 anim, palNum;
    s16 frame, frameLeft; // position in the down-left animation
    struct PortStatusSymbol symbols[2]; // status symbols over the sprite
    u8 symbolCount;
    struct PortStatusSymbol allSymbols[MAX_ROW_SYMBOLS]; // all, not cycling (Blue in Fit)
    u8 allCount;
} PanelRow;

static PanelRow sRows[MAX_TEAM_MEMBERS];

// The down-left animation of a row's current animation, and its length.
static const ax_anim *RowAnim(const PanelRow *row, int *length)
{
    const ax_anim *sw;
    if (row->sprite == NULL || row->anim >= row->sprite->animCount)
        return NULL;
    sw = row->sprite->animations[row->anim][DIRECTION_SOUTHWEST];
    if (sw == NULL || (*length = AnimLength(sw)) == 0)
        return NULL;
    return sw;
}

// Takes the sprite state from a monster on the main screen: the same frame
// of the same animation, facing down-left. Returns FALSE if its sprite is not
// loaded.
static bool8 FollowSprite(PanelRow *row, Entity *entity)
{
    axObject *obj = &entity->axObj;
    const axmain *m;
    const ax_anim *cur, *sw;
    int frame, n;

    // The sprite file must still be the dungeon's open file for the species:
    // when the dungeon closes it, the file cache slot may be reused.
    if (obj->spriteFile == NULL || obj->spriteFile->data == NULL || obj->axdata.activeAnimData == NULL)
        return FALSE;
    if (GetEntInfo(entity)->apparentID < 0 || GetEntInfo(entity)->apparentID >= MONSTER_MAX)
        return FALSE;
    if (obj->spriteFile != gDungeon->sprites[GetEntInfo(entity)->apparentID]
        && obj->spriteFile != gDungeon->sprites[MONSTER_DECOY])
        return FALSE;
    if (obj->spriteFile->file == NULL || strncmp(obj->spriteFile->file->name, "ax", 2) != 0)
        return FALSE;
    m = (const axmain *)obj->spriteFile->data;
    if (obj->unk43_animId2 >= m->animCount)
        return FALSE;
    cur = m->animations[obj->unk43_animId2][obj->unk45_orientation & DIRECTION_MASK];
    sw = m->animations[obj->unk43_animId2][DIRECTION_SOUTHWEST];
    if (cur == NULL || sw == NULL || (n = AnimLength(sw)) == 0)
        return FALSE;

    // The main sprite is showing the frame before activeAnimData.
    frame = (int)(obj->axdata.activeAnimData - cur) - 1;
    if (frame < 0 || frame >= AnimLength(cur))
        frame = 0;
    if (frame >= n)
        frame = n - 1;

    row->sprite = m;
    row->anim = obj->unk43_animId2;
    row->palNum = obj->axdata.sub1.paletteNum;
    // (a shiny's palette is one borrowed and loaded with its colours, not
    // "palet"'s of that number: those, kept for when it can't be followed)
    row->palOk = PortShiny_SlotColors(row->palNum, row->pal);
    row->frame = frame;
    row->frameLeft = obj->axdata.animFrames > 0 ? obj->axdata.animFrames : sw[frame].frames;
    return TRUE;
}

// Plays the row's animation by itself, one frame per call.
static void AdvanceSprite(PanelRow *row)
{
    int n;
    const ax_anim *sw = RowAnim(row, &n);
    if (sw == NULL)
        return;
    if (--row->frameLeft <= 0) {
        row->frame = (row->frame + 1) % n;
        row->frameLeft = sw[row->frame].frames;
    }
}

// A status symbol, placed like DrawStatusSprite places it on the main screen:
// from the pose's position set (the head, for the first symbol).
static void DrawStatusSymbol(const PanelRow *row, const struct PortStatusSymbol *sym,
                             const ax_anim *frame, int ax, int ay)
{
    DungeonPos at = { 0, 0 };
    const u16 *pal = &sPalet[sym->palNum * 16];

    if (row->sprite->positions != NULL) {
        at = row->sprite->positions[frame->poseId].set[sym->anchor];
        if (at.x == 99 && at.y == 99)
            return; // no such point in this pose: the game draws it off screen
    }
    ax += at.x + sym->dx; // (without the frame's offset, as the sprite: DrawRowSprite)
    ay += at.y + sym->dy;
    for (int y = 0; y < sym->height * 8; y++) {
        for (int x = 0; x < sym->width * 8; x++) {
            int t = (y / 8) * sym->width + x / 8;
            int c = (sym->gfx[t * 32 + (y & 7) * 4 + (x & 7) / 2] >> ((x & 1) * 4)) & 0xF;
            if (c)
                Plot(ax + x, ay + y, pal[c] & 0x7FFF);
        }
    }
}

static int BlueBarX(void);

// Blue in Fit: all the status symbols, side by side under the belly bar
// (from where the bars start), level with the held item's sprite.
static void DrawRowSymbols(const PanelRow *row, int y0)
{
    int x = BlueBarX(), mid = y0 + 1 + 35 + 8; // the item sprite's middle
    if (!LoadPalet())
        return;
    for (int i = 0; i < row->allCount; i++) {
        const struct PortStatusSymbol *sym = &row->allSymbols[i];
        const u16 *pal = &sPalet[sym->palNum * 16];
        int top = mid - sym->height * 4;
        for (int y = 0; y < sym->height * 8; y++) {
            for (int xx = 0; xx < sym->width * 8; xx++) {
                int t = (y / 8) * sym->width + xx / 8;
                int c = (sym->gfx[t * 32 + (y & 7) * 4 + (xx & 7) / 2] >> ((xx & 1) * 4)) & 0xF;
                if (c)
                    Plot(x + xx, top + y, pal[c] & 0x7FFF);
            }
        }
        x += sym->width * 8 + 2;
        if (x >= W)
            break;
    }
}

static void DrawRowSprite(const PanelRow *row, int ax, int ay)
{
    int n;
    const ax_anim *sw = RowAnim(row, &n);
    if (sw == NULL || !LoadPalet())
        return;
    if (row->frame >= n)
        return;
    // (the frame's pose only, not its offset: lunges, hops and recoils move
    // the sprite on the main screen, but here it stays on its floor)
    static u16 pals[256];
    memcpy(pals, sPalet, sizeof(pals));
    if (row->palOk)
        memcpy(&pals[(row->palNum & 15) * 16], row->pal, sizeof(row->pal));
    DrawPose(row->sprite->poses[sw[row->frame].poseId], row->sprite->spriteData, NULL, 0,
             pals, row->palNum, ax, ay);
    if (FIT)
        return; // the symbols go under the belly instead (DrawRowSymbols)
    for (int i = 0; i < row->symbolCount; i++)
        DrawStatusSymbol(row, &row->symbols[i], &sw[row->frame], ax, ay);
}

static void ReadMember(PanelRow *row, Entity *entity)
{
    EntityInfo *info = GetEntInfo(entity);
    int i;

    row->used = TRUE;
    row->leader = info->isTeamLeader;
    row->name[0] = 0;
    if (info->teamIndex < MAX_TEAM_MEMBERS) {
        // team members' own names (the game's StrncpyCustom stops at 0 too)
        const u8 *name = gRecruitedPokemonRef->dungeonTeam[info->teamIndex].name;
        for (i = 0; i < POKEMON_NAME_LENGTH && name[i]; i++)
            row->name[i] = name[i];
        row->name[i] = 0;
    }
    row->level = info->level;
    row->hp = info->HP;
    row->maxHp = info->maxHPStat;
    row->belly = FixedPointToInt(info->belly);
    row->maxBelly = FixedPointToInt(info->maxBelly);
    row->item = ItemExists(&info->heldItem) ? gItemParametersData[info->heldItem.id].name : NULL;
    row->itemIcon = row->item != NULL ? gItemParametersData[info->heldItem.id].icon : -1;
    row->itemPal = row->item != NULL ? gItemParametersData[info->heldItem.id].palette : 0;
    row->tactic = info->tactic < NUM_TACTICS ? gTacticsNames[info->tactic] : NULL;
    row->symbolCount = Port_GetStatusSymbols(info->dungeonSpriteId, row->symbols);
    row->allCount = Port_GetAllStatusSymbols(info->dungeonSpriteId, row->allSymbols, MAX_ROW_SYMBOLS);
}

// Updates the rows from the dungeon, unless the team's sprites are not loaded.
static void UpdateRows(void)
{
    Entity *members[MAX_TEAM_MEMBERS];
    PanelRow rows[MAX_TEAM_MEMBERS];
    int count = 0, i;

    // Leader first, then the others in team order.
    for (i = 0; i < MAX_TEAM_MEMBERS; i++) {
        Entity *e = gDungeon->teamPokemon[i];
        if (EntityIsValid(e) && GetEntInfo(e)->isTeamLeader)
            members[count++] = e;
    }
    for (i = 0; i < MAX_TEAM_MEMBERS; i++) {
        Entity *e = gDungeon->teamPokemon[i];
        if (EntityIsValid(e) && !GetEntInfo(e)->isTeamLeader)
            members[count++] = e;
    }

    memset(rows, 0, sizeof(rows));
    for (i = 0; i < count; i++) {
        ReadMember(&rows[i], members[i]);
        if (!FollowSprite(&rows[i], members[i]))
            count = -1;
    }
    if (count > 0) {
        memcpy(sRows, rows, sizeof(sRows));
    } else {
        for (i = 0; i < MAX_TEAM_MEMBERS; i++)
            AdvanceSprite(&sRows[i]);
    }
}

static const u8 sLevel[] = "Level ";
static const u8 sItem[] = "Item: ";
static const u8 sNone[] = "None";
static const u8 sLeader[] = "Leader";
static const u8 sSlash[] = "/";
static const u8 sHp[] = "HP: ";
static const u8 sLv[] = "Lv. ";
static const u8 sWeather[] = "Weather: ";
static const u8 sMoney[] = "Money: ";
static const u8 sBelly[] = "Belly: ";

// Draws a number right-aligned at xRight with every digit in a cell as wide
// as '0', so that the columns line up.
static void DrawNumberRight(int xRight, int y, int v)
{
    u8 buf[12], one[2] = { 0, 0 };
    int cell = GlyphWidth('0'), n, i;
    FormatInt(buf, v);
    n = strlen((const char *)buf);
    for (i = 0; i < n; i++) {
        int x = xRight - (n - i) * cell;
        one[0] = buf[i];
        DrawText(x + (cell - GlyphWidth(buf[i])) / 2, y, one, COLOR_TEXT);
    }
}

// An item's sprite as dungeon_items.c puts it on the floor: 16x16, from OBJ
// tile 0x1A0 + icon * 4 (where the dungeon loads them), in OBJ palette pal.
static void DrawItemSprite(int x0, int y0, int icon, int pal)
{
    const u8 *tiles = &gPortVram[0x10000 + (0x1A0 + icon * 4) * 32];
    u16 colors[16];
    ObjPalette(pal, colors);
    for (int y = 0; y < 16; y++) {
        for (int x = 0; x < 16; x++) {
            int t = (y >> 3) * 2 + (x >> 3); // 1D: top-left, top-right, bottom-left, bottom-right
            int c = (tiles[t * 32 + (y & 7) * 4 + (x & 7) / 2] >> ((x & 1) * 4)) & 0xF;
            if (c)
                Plot(x0 + x, y0 + y, colors[c] & 0x7FFF);
        }
    }
}

// Blue in Fit: where the "HP:"/"Belly:" labels start, where their first
// numbers end (the "/"), and where the bars start
static int BlueLabelX(void) { return 136 - 3 * GlyphWidth('0') + 12; } // (the bars up to 100 pixels)
static int BlueHpRight(void)
{
    int labelW = TextWidth(sBelly) > TextWidth(sHp) ? TextWidth(sBelly) : TextWidth(sHp);
    return BlueLabelX() + labelW + 3 * GlyphWidth('0');
}
static int BlueBarX(void) { return BlueHpRight() + TextWidth(sSlash) + 3 * GlyphWidth('0') + 3; }

static void DrawRowText(int y0, const PanelRow *row)
{
    u8 buf[16];
    int x, barMax = FIT ? 140 : 80; // (Fit: as far as the wider panel allows)

    // Fit: four lines (name, level, item, tactics) in the middle of the
    // taller row; else three, the tactics beside the item
    y0 += FIT ? 1 : 0;

    DrawText(56, y0 + 5, row->name, COLOR_NAME);

    x = DrawText(60, y0 + 17, sLevel, COLOR_TEXT);
    FormatInt(buf, row->level);
    DrawText(x, y0 + 17, buf, COLOR_TEXT);

    // HP: "hp/ max", each number right-aligned in three digit cells, then
    // the bar, one pixel per max HP as on the DS (which stops at 60; here
    // at 80).
    // Fit: "HP: hp/max" on the name's line and "Belly: belly/max" on the
    // level's, numbers and bars lined up, the bars ending 17 pixels from
    // the screen's edge.
    if (FIT) {
        int cell = GlyphWidth('0'), labelX = BlueLabelX();
        int hpRight = BlueHpRight(), maxRight = hpRight + TextWidth(sSlash) + 3 * cell;
        int barX = BlueBarX();
        barMax = W - 17 - barX;
        DrawText(labelX, y0 + 5, sHp, COLOR_TEXT);
        DrawNumberRight(hpRight, y0 + 5, row->hp);
        DrawText(hpRight, y0 + 5, sSlash, COLOR_TEXT);
        DrawNumberRight(maxRight, y0 + 5, row->maxHp);
        DrawHpBar(barX, y0 + 8, row->maxHp < 1 ? 1 : row->maxHp > barMax ? barMax : row->maxHp, row->hp, row->maxHp);
        DrawText(labelX, y0 + 17, sBelly, COLOR_TEXT);
        DrawNumberRight(hpRight, y0 + 17, row->belly);
        DrawText(hpRight, y0 + 17, sSlash, COLOR_TEXT);
        DrawNumberRight(maxRight, y0 + 17, row->maxBelly);
        // (always to 17 pixels from the edge: the belly scaled to the bar)
        DrawBar(barX, y0 + 20, barMax, row->belly, row->maxBelly < 1 ? 1 : row->maxBelly, Rgb(248, 208, 48)); // yellow
        // the held item's sprite, as on the floor, under the belly's "/",
        // in the middle between its line and the panel's bottom
        if (row->itemIcon >= 0)
            DrawItemSprite(hpRight + TextWidth(sSlash) / 2 - 8, y0 + 35, row->itemIcon, row->itemPal);
    }
    else {
        DrawNumberRight(136, y0 + 17, row->hp);
        DrawText(136, y0 + 17, sSlash, COLOR_TEXT);
        DrawNumberRight(160, y0 + 17, row->maxHp);
        DrawHpBar(163, y0 + 20, row->maxHp < 1 ? 1 : row->maxHp > barMax ? barMax : row->maxHp, row->hp, row->maxHp);
    }

    x = DrawText(60, y0 + 29, sItem, COLOR_TEXT);
    DrawText(x, y0 + 29, row->item != NULL ? row->item : sNone, COLOR_TEXT);

    {
        int tx = FIT ? 60 : 148, ty = y0 + (FIT ? 41 : 29);
        if (row->leader)
            DrawText(tx, ty, sLeader, COLOR_TEXT);
        else if (row->tactic != NULL)
            DrawText(tx, ty, row->tactic, COLOR_TEXT);
    }
}

static bool8 sInDungeon;

// --- Redrawing only what changed -----------------------------------------------
//
// Each layout describes what it shows in a key; when the key is the same as
// last frame's, the picture is too, and nothing is drawn.

#define KEY_MAX 4096
typedef struct { u8 data[KEY_MAX]; int size; } DrawKey;

static void KeyAdd(DrawKey *k, const void *p, int n)
{
    if (k->size + n <= KEY_MAX)
        memcpy(&k->data[k->size], p, n);
    k->size += n;
}

// Whether `k` differs from `last` (then `last` becomes `k`).
static bool8 KeyChanged(DrawKey *last, const DrawKey *k)
{
    if (k->size == last->size && memcmp(k->data, last->data, k->size < KEY_MAX ? k->size : KEY_MAX) == 0)
        return FALSE;
    memcpy(last->data, k->data, k->size < KEY_MAX ? k->size : KEY_MAX);
    last->size = k->size;
    return TRUE;
}

static int sLayoutDrawn = -1; // the layout the picture holds (-1: none)

static void StartDungeonLayout(int layout, DrawKey *k)
{
    if (!sInDungeon) { // a new dungeon: forget the last one's team
        memset(sRows, 0, sizeof(sRows));
        sInDungeon = TRUE;
    }
    UpdateRows();
    k->size = 0;
    KeyAdd(k, &layout, sizeof(layout));
    KeyAdd(k, &sLayoutDrawn, sizeof(sLayoutDrawn));
}

// A row's text (all the panels show of it, but for the sprite).
static void AddRowText(DrawKey *k, int i)
{
    {
        const PanelRow *row = &sRows[i];
        KeyAdd(k, &row->used, sizeof(row->used));
        KeyAdd(k, &row->leader, sizeof(row->leader));
        KeyAdd(k, row->name, sizeof(row->name));
        KeyAdd(k, &row->level, sizeof(row->level));
        KeyAdd(k, &row->hp, sizeof(row->hp));
        KeyAdd(k, &row->maxHp, sizeof(row->maxHp));
        KeyAdd(k, &row->belly, sizeof(row->belly));
        KeyAdd(k, &row->maxBelly, sizeof(row->maxBelly));
        KeyAdd(k, &row->item, sizeof(row->item));
        KeyAdd(k, &row->itemIcon, sizeof(row->itemIcon));
        if (row->itemIcon >= 0) { // (its colours: drawn again when they change)
            u16 pal[16];
            ObjPalette(row->itemPal, pal);
            KeyAdd(k, pal, sizeof(pal));
        }
        KeyAdd(k, &row->tactic, sizeof(row->tactic));
    }
}

// The rows' text
static void AddRowsText(DrawKey *k)
{
    for (int i = 0; i < MAX_TEAM_MEMBERS; i++)
        AddRowText(k, i);
}

// Blue: each row's boxes and text are drawn into sTeamBg when they change
// (the boxes themselves made once per size), and the sprites over them when
// a frame changes. Only the rows that changed are drawn again: a sprite can
// reach over the next row, so the area it covered (before and now) is put
// back from sTeamBg and every sprite there drawn again, clipped to it.
static void DrawRowText(int y0, const PanelRow *row);

static bool8 DrawTeamPanel(void)
{
    static u16 sTeamBg[CANVAS_PIXELS];
    static u16 sRowUsed[SUBSCREEN_W_MAX * (SUBSCREEN_H_MAX / 4)], sRowEmpty[SUBSCREEN_W_MAX * (SUBSCREEN_H_MAX / 4)];
    static int sRowsW = -1;
    static DrawKey sLastAll, sLastText[MAX_TEAM_MEMBERS], sLastSprite[MAX_TEAM_MEMBERS], k;
    static int sSprBox[MAX_TEAM_MEMBERS][4]; // what each row's sprite covered (x0, y0, x1, y1)
    u16 *canvas = sOut;
    bool8 all = FALSE, drawn = FALSE;
    int area[4] = { W, H, 0, 0 }; // drawn again this time

    StartDungeonLayout(PORT_SUB_LAYOUT_BLUE, &k);
    KeyAdd(&k, &W, sizeof(W));
    if (KeyChanged(&sLastAll, &k)) { // all of it again
        all = TRUE;
        sStats.all++;
        for (int i = 0; i < MAX_TEAM_MEMBERS; i++) {
            sLastText[i].size = -1;
            sLastSprite[i].size = -1;
            sSprBox[i][0] = sSprBox[i][1] = sSprBox[i][2] = sSprBox[i][3] = 0;
        }
    }
    if (sRowsW != W) { // the empty row, and the boxes of a used one
        sRowsW = W;
        sOut = sRowEmpty;
        Fill(0, 0, W, ROW_H, Rgb(74, 74, 116));
        DrawSpriteBox(0, TRUE);
        memcpy(sRowUsed, sRowEmpty, W * ROW_H * sizeof(u16));
        sOut = sRowUsed;
        DrawSpriteBox(0, FALSE);
        DrawInfoPanel(0);
        sOut = canvas;
    }

    for (int i = 0; i < MAX_TEAM_MEMBERS; i++) {
        const PanelRow *row = &sRows[i];
        int y0 = i * ROW_H;
        k.size = 0;
        AddRowText(&k, i);
        if (KeyChanged(&sLastText[i], &k)) {
            sStats.rows++;
            memcpy(&sTeamBg[y0 * W], row->used ? sRowUsed : sRowEmpty, W * ROW_H * sizeof(u16));
            if (row->used) {
                sOut = sTeamBg;
                DrawRowText(y0, row);
                sOut = canvas;
            }
            sLastSprite[i].size = -1; // (and the sprite over it)
            if (area[0] > 0) area[0] = 0;
            if (area[1] > y0) area[1] = y0;
            area[2] = W;
            if (area[3] < y0 + ROW_H) area[3] = y0 + ROW_H;
        }
        k.size = 0;
        KeyAdd(&k, &row->used, sizeof(row->used));
        KeyAdd(&k, &row->sprite, sizeof(row->sprite));
        KeyAdd(&k, &row->anim, sizeof(row->anim));
        KeyAdd(&k, &row->palNum, sizeof(row->palNum));
        KeyAdd(&k, &row->frame, sizeof(row->frame));
        if (FIT) {
            KeyAdd(&k, &row->allCount, sizeof(row->allCount));
            KeyAdd(&k, row->allSymbols, sizeof(row->allSymbols[0]) * row->allCount);
        } else {
            KeyAdd(&k, &row->symbolCount, sizeof(row->symbolCount));
            KeyAdd(&k, row->symbols, sizeof(row->symbols[0]) * row->symbolCount);
        }
        if (KeyChanged(&sLastSprite[i], &k)) {
            int *b = sSprBox[i];
            sStats.sprites++;
            if (b[0] < b[2]) { // where it was
                if (area[0] > b[0]) area[0] = b[0];
                if (area[1] > b[1]) area[1] = b[1];
                if (area[2] < b[2]) area[2] = b[2];
                if (area[3] < b[3]) area[3] = b[3];
            }
            // where it is now (drawn once to find out, off the picture)
            sTrackBox = TRUE;
            sBoxX0 = W; sBoxY0 = H; sBoxX1 = 0; sBoxY1 = 0;
            sClipX0 = sClipY0 = sClipX1 = sClipY1 = 0;
            if (row->used) {
                DrawRowSprite(row, 23, y0 + 33 + ROW_EXTRA); // feet on the middle of the floor (8 up)
                if (FIT)
                    DrawRowSymbols(row, y0);
            }
            sTrackBox = FALSE;
            // (the clip back at once: the next rows' text is drawn in this loop;
            // left at none, a partner's text drawn after the leader's sprite
            // changed was never drawn, until it changed again)
            sClipX0 = 0; sClipY0 = 0; sClipX1 = 0x7FFF; sClipY1 = 0x7FFF;
            if (sBoxX0 < sBoxX1) {
                b[0] = sBoxX0; b[1] = sBoxY0; b[2] = sBoxX1; b[3] = sBoxY1;
                if (area[0] > b[0]) area[0] = b[0];
                if (area[1] > b[1]) area[1] = b[1];
                if (area[2] < b[2]) area[2] = b[2];
                if (area[3] < b[3]) area[3] = b[3];
            } else {
                b[0] = b[1] = b[2] = b[3] = 0;
            }
        }
    }
    if (all) {
        area[0] = 0; area[1] = 0; area[2] = W; area[3] = H;
    }
    sClipX0 = 0; sClipY0 = 0; sClipX1 = 0x7FFF; sClipY1 = 0x7FFF;
    if (area[0] >= area[2] || area[1] >= area[3])
        return FALSE;

    // That area again: the boxes and text, then the sprites over it, in order
    Restore(sTeamBg, area[0], area[1], area[2] - area[0], area[3] - area[1]);
    sClipX0 = area[0]; sClipY0 = area[1]; sClipX1 = area[2]; sClipY1 = area[3];
    for (int i = 0; i < MAX_TEAM_MEMBERS; i++) {
        const int *b = sSprBox[i];
        if (!sRows[i].used || b[0] >= b[2] || b[2] <= area[0] || b[0] >= area[2] || b[3] <= area[1] || b[1] >= area[3])
            continue;
        DrawRowSprite(&sRows[i], 23, i * ROW_H + 33 + ROW_EXTRA);
        if (FIT)
            DrawRowSymbols(&sRows[i], i * ROW_H);
        drawn = TRUE;
    }
    sClipX0 = 0; sClipY0 = 0; sClipX1 = 0x7FFF; sClipY1 = 0x7FFF;
    Dirty(area[0], area[1], area[2] - area[0], area[3] - area[1]);
    (void)drawn;
    return TRUE;
}


// --- World map ----------------------------------------------------------------------

// The world map's graphics, as the game's world map scene loads them.
#define MAP_TILES 64 // the map is 64x64 tiles
static u8 sMapGfx[0x10000]; // charblocks 2-3, where the game decompresses it
// [0]: the map itself (BG3); [1]: details drawn over it (BG2). The game
// decompresses both in one go, into two adjacent arrays.
static u16 sMapTilemap[2][MAP_TILES][MAP_TILES];
static u16 sMapPal[256];

// Looks a file up in an archive without opening it (OpenFile would take a
// slot in the game's file cache).
static const void *FindFile(const FileArchive *arc, const char *name)
{
    s32 lo = 0, hi = arc->count - 1;
    while (lo < hi) {
        s32 mid = (lo + hi) / 2, r = strcmp(arc->entries[mid].name, name);
        if (r == 0) { lo = mid; break; }
        if (r < 0) lo = mid + 1; else hi = mid;
    }
    if (strcmp(arc->entries[lo].name, name) != 0)
        return NULL;
    {
        const SiroArchive *siro = arc->entries[lo].data;
        if (siro->magic[0] == 'S' && siro->magic[1] == 'I' && siro->magic[2] == 'R' && (siro->magic[3] == 'O' || siro->magic[3] == '0'))
            return siro->data;
        return siro;
    }
}

static const char *sMapFiles[2][4] = {
    { "wmapfont", "wmapmcc", "wmappal", "wmapcani" }, // the friend areas map (the DS version's look)
    { "wmp2font", "wmp2mcc", "wmp2pal", "wmp2cani" }, // the map shown on the way to a dungeon
};

// The water: BG colours 0xB0-0xBF cycle, from the map's "cani" file, as the
// friend areas map scene animates them (friend_areas_map_util.c,
// UpdateAnimatedColors).
#define MAP_ANIM_FIRST 0xB0
#define MAP_ANIM_COLORS 16
static struct {
    const RGB_Struct *start, *end, *cur;
    s32 duration, timer;
} sMapAnim[MAP_ANIM_COLORS];
static uint64_t sMapAnimFrame;
static bool8 sMapBlend; // the friend areas map (BLDALPHA 16/16 in its scene)

static void LoadMapAnimation(const void *file)
{
    const AnimatedColorData *const *data = file;
    for (int i = 0; i < MAP_ANIM_COLORS; i++) {
        const AnimatedColorData *d = data != NULL ? data[i] : NULL;
        sMapAnim[i].start = NULL;
        if (d == NULL || d->colorCount == 0 || d->duration <= 0)
            continue;
        sMapAnim[i].start = d->colors;
        sMapAnim[i].end = &d->colors[d->colorCount];
        sMapAnim[i].cur = d->colors;
        sMapAnim[i].duration = d->duration;
        sMapAnim[i].timer = 1; // the first colour at once
    }
    sMapAnimFrame = Port_FrameCount();
}

// Advances the cycle by the frames since the last call; returns whether a
// colour changed.
static bool8 AnimateMapColors(void)
{
    uint64_t now = Port_FrameCount();
    s32 frames = (s32)(now - sMapAnimFrame);
    bool8 changed = FALSE;

    if (frames <= 0)
        return FALSE;
    if (frames > 600)
        frames = 600;
    sMapAnimFrame = now;
    for (int i = 0; i < MAP_ANIM_COLORS; i++) {
        s32 n = frames;
        if (sMapAnim[i].start == NULL)
            continue;
        while (n > 0) {
            if (sMapAnim[i].timer > n) {
                sMapAnim[i].timer -= n;
                break;
            }
            n -= sMapAnim[i].timer;
            sMapAnim[i].timer = sMapAnim[i].duration;
            if (sMapAnim[i].cur >= sMapAnim[i].end)
                sMapAnim[i].cur = sMapAnim[i].start;
            {
                const RGB_Struct *c = sMapAnim[i].cur++;
                u16 rgb = Rgb(c->r, c->g, c->b);
                if (sMapPal[MAP_ANIM_FIRST + i] != rgb) {
                    sMapPal[MAP_ANIM_FIRST + i] = rgb;
                    changed = TRUE;
                }
            }
        }
    }
    return changed;
}

static bool8 LoadMapSet(int set)
{
    File f;
    OpenedFile of;
    const RGB_Struct *pal;
    const void *gfx = FindFile(&gTitleMenuFileArchive, sMapFiles[set][0]);
    const void *map = FindFile(&gTitleMenuFileArchive, sMapFiles[set][1]);
    int i;

    pal = FindFile(&gTitleMenuFileArchive, sMapFiles[set][2]);
    if (gfx == NULL || map == NULL || pal == NULL)
        return FALSE;
    of.file = &f;
    of.data = gfx;
    DecompressATGlobalFile((u32 *)sMapGfx, 0, &of);
    of.data = map;
    DecompressATFile(sMapTilemap, sizeof(sMapTilemap), &of);
    for (i = 0; i < 224; i++)
        sMapPal[i] = Rgb(pal[i].r, pal[i].g, pal[i].b);
    LoadMapAnimation(FindFile(&gTitleMenuFileArchive, sMapFiles[set][3]));
    sMapBlend = set == 0; // (friend_areas_map.c: gUnknown_2026E4E = 0x1010)
    AnimateMapColors(); // (the first colours)
    return TRUE;
}

// The map's marker sprites ("wmapspr": location markers, arrows). Their
// colours are OBJ palette 0 as the map scenes set it, from "palet".
static const EfoFileData *sMapSprites;

static bool8 LoadMapSprites(void)
{
    sMapSprites = FindFile(&gTitleMenuFileArchive, "wmapspr");
    return sMapSprites != NULL && LoadPalet();
}

static void DrawMapSpriteFrame(int anim, int frame, int x, int y)
{
    const ax_anim *a = sMapSprites->animations[anim][0];
    DrawPose(sMapSprites->poses[a[frame].poseId], NULL, sMapSprites->tiles, sMapSprites->tileCount,
             sPalet, 0, x + a[frame].offset.x, y + a[frame].offset.y);
}

// Development aid (test harness): every frame of every marker animation, one
// animation per row.
void Port_DebugRenderMapSprites(u16 *out)
{
    u16 *saved = sOut;
    sOut = out;
    LoadMapSprites();
    for (int i = 0; i < W * H; i++)
        out[i] = Rgb(0, 96, 160);
    for (int anim = 0; anim < sMapSprites->animCount && anim < 12 + 1; anim++) {
        const ax_anim *a = sMapSprites->animations[anim][0];
        int n = AnimLength(a);
        for (int f = 0; f < n && f < 10; f++)
            DrawMapSpriteFrame(anim, f, 12 + f * 24, 8 + anim * 14);
    }
    sOut = saved;
}

// Development aid (test harness): the whole 512x512 map of a set.
void Port_DebugRenderFullMap(int set, u16 *out)
{
    LoadMapSet(set);
    for (int my = 0; my < 512; my++)
        for (int mx = 0; mx < 512; mx++) {
            int layer, c = 0, palBase = 0;
            for (layer = 1; layer >= 0; layer--) {
                u16 e = sMapTilemap[layer][my >> 3][mx >> 3];
                int tx = mx & 7, ty = my & 7;
                if (e & 0x400) tx = 7 - tx;
                if (e & 0x800) ty = 7 - ty;
                c = (sMapGfx[(e & 0x3FF) * 32 + ty * 4 + tx / 2] >> ((tx & 1) * 4)) & 0xF;
                palBase = (e >> 12) * 16;
                if (c != 0)
                    break;
            }
            out[my * 512 + mx] = c ? sMapPal[(palBase + c) & 0xFF] : sMapPal[0];
        }
}

// Where each place outside dungeons is on the world map: the dungeon it is
// part of, or the town (shown on the friend areas map, at the team base).
#define PLACE_TOWN 1000
static s32 PlaceDungeon(s32 place)
{
    switch (place) {
        case GROUND_PLACE_SQUARE: case GROUND_PLACE_SQUARE_2: case GROUND_PLACE_POKEMON_SQUARE:
        case GROUND_PLACE_WHISCASH_POND: case GROUND_PLACE_PELIPPER_POST_OFFICE:
        case GROUND_PLACE_PELIPPER_POST_OFFICE_INSIDE: case GROUND_PLACE_MAKUHITA_DOJO:
        case GROUND_PLACE_MAKUHITA_DOJO_INSIDE: case GROUND_PLACE_LUMINOUS_CAVE:
        case GROUND_PLACE_FRIEND_AREAS: case GROUND_PLACE_TEAM_BASE: case GROUND_PLACE_TEAM_BASE_INSIDE:
            return PLACE_TOWN;
        case GROUND_PLACE_TINY_WOODS: return DUNGEON_TINY_WOODS;
        case GROUND_PLACE_THUNDERWAVE_CAVE: return DUNGEON_THUNDERWAVE_CAVE;
        case GROUND_PLACE_MT_STEEL: return DUNGEON_MT_STEEL;
        case GROUND_PLACE_SINISTER_WOODS: return DUNGEON_SINISTER_WOODS;
        case GROUND_PLACE_SILENT_CHASM: return DUNGEON_SILENT_CHASM;
        case GROUND_PLACE_MT_THUNDER: return DUNGEON_MT_THUNDER;
        case GROUND_PLACE_MT_THUNDER_MID: return DUNGEON_MT_THUNDER_PEAK;
        case GROUND_PLACE_GREAT_CANYON: return DUNGEON_GREAT_CANYON;
        case GROUND_PLACE_HILL_OF_THE_ANCIENTS: return DUNGEON_GREAT_CANYON;
        case GROUND_PLACE_LAPIS_CAVE: case GROUND_PLACE_LAPIS_CAVE_EXIT: return DUNGEON_LAPIS_CAVE;
        case GROUND_PLACE_MT_BLAZE: return DUNGEON_MT_BLAZE;
        case GROUND_PLACE_MT_BLAZE_MID: return DUNGEON_MT_BLAZE_PEAK;
        case GROUND_PLACE_FROSTY_FOREST: return DUNGEON_FROSTY_FOREST;
        case GROUND_PLACE_FROSTY_CLEARING: return DUNGEON_FROSTY_GROTTO;
        case GROUND_PLACE_MT_FREEZE: return DUNGEON_MT_FREEZE;
        case GROUND_PLACE_MT_FREEZE_MID: return DUNGEON_MT_FREEZE_PEAK;
        case GROUND_PLACE_MAGMA_CAVERN: return DUNGEON_MAGMA_CAVERN;
        case GROUND_PLACE_MAGMA_CAVERN_MID: return DUNGEON_MAGMA_CAVERN_PIT;
        case GROUND_PLACE_SKY_TOWER: return DUNGEON_SKY_TOWER;
        case GROUND_PLACE_SKY_TOWER_MID: return DUNGEON_SKY_TOWER_SUMMIT;
        case GROUND_PLACE_WESTERN_CAVE: return DUNGEON_WESTERN_CAVE;
    }
    return -1; // not a place on the map (title screen, cutscene backgrounds...)
}

// Both maps fill the top-left 480x320 of their 512x512 tilemaps.
#define MAP_W 480
#define MAP_H 320

static int sViewLeft, sViewTop;

// The colour number of each pixel DrawMapArea drew, for the water's
// colours (0: not one of them)
static u8 sMapIdx[CANVAS_PIXELS];
static u16 sMapUnder[CANVAS_PIXELS]; // ... and the colour under it (BG3)

// GBA colour effects, BLDALPHA 16/16: BG2 added to BG3
static inline u16 AddColors(u16 a, u16 b)
{
    int r = (a & 31) + (b & 31), g = ((a >> 5) & 31) + ((b >> 5) & 31), bl = ((a >> 10) & 31) + ((b >> 10) & 31);
    return (u16)((r > 31 ? 31 : r) | ((g > 31 ? 31 : g) << 5) | ((bl > 31 ? 31 : bl) << 10));
}

static inline u16 MapPixel(int i)
{
    return sMapBlend ? AddColors(sMapPal[sMapIdx[i]], sMapUnder[i]) : sMapPal[sMapIdx[i]];
}

// The part of the map around (cx, cy) the screen shows (DrawMapRows draws it)
static void SetMapView(int cx, int cy)
{
    int left = cx - W / 2, top = cy - H / 2;
    if (left < 0) left = 0;
    if (top < 0) top = 0;
    if (left > MAP_W - W) left = MAP_W - W;
    if (top > MAP_H - H) top = MAP_H - H;
    sViewLeft = left;
    sViewTop = top;
}

static void DrawMapRows(int y0, int y1)
{
    int left = sViewLeft, top = sViewTop;
    for (int y = y0; y < y1; y++) {
        int my = top + y;
        for (int x = 0; x < W; x++) {
            int mx = left + x;
            int layer, c = 0, palBase = 0;
            if (sMapBlend) { // the friend areas map: its details (BG2) add to the map (BG3)
                int idx[2];
                for (layer = 0; layer < 2; layer++) {
                    u16 e = sMapTilemap[layer][my >> 3][mx >> 3];
                    int tx = mx & 7, ty = my & 7;
                    if (e & 0x400) tx = 7 - tx;
                    if (e & 0x800) ty = 7 - ty;
                    c = (sMapGfx[(e & 0x3FF) * 32 + ty * 4 + tx / 2] >> ((tx & 1) * 4)) & 0xF;
                    idx[layer] = c ? ((e >> 12) * 16 + c) & 0xFF : 0;
                }
                u16 under = sMapPal[idx[0]]; // (0: the backdrop)
                sMapUnder[y * W + x] = under;
                sMapIdx[y * W + x] = idx[1];
                sOut[y * W + x] = idx[1] ? AddColors(sMapPal[idx[1]], under) : under;
                continue;
            }
            for (layer = 1; layer >= 0; layer--) { // the top layer, unless transparent there
                u16 e = sMapTilemap[layer][my >> 3][mx >> 3];
                int tx = mx & 7, ty = my & 7;
                if (e & 0x400) tx = 7 - tx;
                if (e & 0x800) ty = 7 - ty;
                c = (sMapGfx[(e & 0x3FF) * 32 + ty * 4 + tx / 2] >> ((tx & 1) * 4)) & 0xF;
                palBase = (e >> 12) * 16;
                if (c != 0)
                    break;
            }
            sOut[y * W + x] = c ? sMapPal[(palBase + c) & 0xFF] : sMapPal[0]; // else the backdrop
            sMapIdx[y * W + x] = c ? (palBase + c) & 0xFF : 0;
        }
    }
}

// --- Game windows ----------------------------------------------------------------
//
// Drawn from the same pieces as the game's own (text_1.c, WINDOW_TYPE_NORMAL):
// border tiles on BG0 over the fill tile on BG1, from the "font" graphics the
// game loads at tile 0x278, in BG palette 15 as it is outside dungeons
// (sub_8099690: gFontPalette, whose colours follow the window colour option
// and the player's gender).

#define FONT_FIRST_TILE 0x278
#define TILE_CORNER 0x2D8
#define TILE_EDGE_H 0x2D9
#define TILE_EDGE_V 0x2DA
#define TILE_FILL   0x2DB

static u16 sWindowPal[16];

// Returns whether the colours changed since the last call.
static bool8 LoadWindowPalette(void)
{
    const RGB_Struct *p = &gFontPalette[gGameOptionsRef->playerGender != MALE ? 0x50 : 0x10];
    bool8 changed = FALSE;
    for (int i = 0; i < 16; i++) {
        u16 c = Rgb(p[i].r, p[i].g, p[i].b);
        changed |= c != sWindowPal[i];
        sWindowPal[i] = c;
    }
    return changed;
}

// The colour index of a pixel of a font tile (0 = transparent).
static int FontTilePixel(const u8 *font, int tile, bool8 hflip, bool8 vflip, int x, int y)
{
    if (hflip) x = 7 - x;
    if (vflip) y = 7 - y;
    return (font[(tile - FONT_FIRST_TILE) * 32 + y * 4 + x / 2] >> ((x & 1) * 4)) & 0xF;
}

// A window at tile-sized cells: (x, y) is the top-left of its border, and
// w x h the size of its inside in tiles.
static void DrawGameWindow(int px, int py, int w, int h)
{
    const u8 *font = FindFile(&gSystemFileArchive, "font");
    if (font == NULL)
        return;
    font += 4; // tile count, then the tiles

    for (int ty = 0; ty < h + 2; ty++) {
        for (int tx = 0; tx < w + 2; tx++) {
            bool8 top = ty == 0, bottom = ty == h + 1, left = tx == 0, right = tx == w + 1;
            int tile = 0;
            bool8 hf = FALSE, vf = FALSE;
            if ((top || bottom) && (left || right)) { tile = TILE_CORNER; hf = right; vf = bottom; }
            else if (top || bottom) { tile = TILE_EDGE_H; vf = bottom; }
            else if (left || right) { tile = TILE_EDGE_V; hf = right; }
            for (int y = 0; y < 8; y++) {
                for (int x = 0; x < 8; x++) {
                    int c = tile ? FontTilePixel(font, tile, hf, vf, x, y) : 0;
                    bool8 fill = c == 0;
                    if (fill)
                        c = FontTilePixel(font, TILE_FILL, FALSE, FALSE, x, y);
                    if (c == 0)
                        continue;
                    int sx = px + tx * 8 + x, sy = py + ty * 8 + y;
                    Plot(sx, sy, sWindowPal[c] & 0x7FFF);
                }
            }
        }
    }
}

// The place's name in a window, centred, as the DS version shows it.
static void DrawNameBox(const u8 *name)
{
    const int w = 20, h = 2; // inside, in tiles
    const int px = (W - (w + 2) * 8) / 2, py = H - (h + 2) * 8 - 8;
    DrawGameWindow(px, py, w, h);
    DrawText(px + 8 + (w * 8 - TextWidth(name)) / 2, py + 8 + 2, name, COLOR_TEXT);
}

// --- Map and team (Layout Options > Sub-Screen: M+T) -------------------------------

// The dungeon map, as the main screen shows it but always opaque (worked out
// by dungeon_map.c into gPortSubMap), over a grid, and below it a window with
// each member's name, level and HP, as the DS version's "M+T" screen.

extern u8 gPortSubMap[DUNGEON_MAX_SIZE_Y][DUNGEON_MAX_SIZE_X];
const u32 *Port_MinimapGfx(s32 type, s32 quadrant);
const RGB_Struct *Port_MinimapPalette(void);
bool8 Port_MinimapLeaderDot(DungeonPos *pos);

#define SUBMAP_FIRST_ROW 2 // the rows the game's map shows (DrawMinimapTile)
#define SUBMAP_ROWS (DUNGEON_MAX_SIZE_Y - 1 - SUBMAP_FIRST_ROW)
#define SUBMAP_X ((W - DUNGEON_MAX_SIZE_X * 4) / 2)
#define SUBMAP_Y (FIT ? 16 : 6) // (Fit: the middle of the space above the window)
#define MAP_GFX_PLAYER 8

static int sDotFrames;

static void DrawGrid(void)
{
    u16 back = Rgb(40, 32, 80), line = Rgb(56, 48, 112);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            sOut[y * W + x] = ((x & 7) == 0 || (y & 7) == 0) ? line : back;
}

static void MinimapPalette(u16 *pal)
{
    const RGB_Struct *p = Port_MinimapPalette();
    for (int i = 0; i < 16; i++) {
        const RGB_Struct *c = i == 14 ? &gFontPalette[1] : &p[i];
        pal[i] = Rgb(c->r, c->g, c->b);
    }
}

// Draws the pixels of one 8x8 map tile (two by two dungeon tiles).
static void DrawMapBlock(const u32 *tile, int px, int py, const u16 *pal)
{
    for (int y = 0; y < 8; y++) {
        u32 row = tile[y];
        for (int x = 0; x < 8; x++) {
            int c = (row >> (x * 4)) & 0xF;
            if (c)
                Plot(px + x, py + y, pal[c]);
        }
    }
}

// The map tile (two by two dungeon tiles) at block (bx, by), as
// DrawMinimapTile builds it: each dungeon tile sets one quarter of it,
// through a mask and its pixels.
static void BuildMapBlock(int bx, int by, u32 *tile)
{
    memset(tile, 0, 8 * sizeof(u32));
    for (int q = 0; q < 4; q++) {
        int x = bx * 2 + (q & 1), yy = by * 2 + (q >> 1), y = yy + SUBMAP_FIRST_ROW;
        const u32 *src;
        if (yy >= SUBMAP_ROWS)
            continue;
        src = Port_MinimapGfx(gPortSubMap[y][x], q);
        if (src == NULL)
            continue;
        for (int i = 0; i < 8; i++)
            tile[i] = (tile[i] & src[i * 2]) | src[i * 2 + 1];
    }
}

#define SUBMAP_BW (DUNGEON_MAX_SIZE_X / 2)
#define SUBMAP_BH ((SUBMAP_ROWS + 1) / 2)

// The parts of the picture that changed this frame (Port_RenderSubScreen
// copies only those when it can).
static int sDirtyX0, sDirtyY0, sDirtyX1, sDirtyY1;

static void Dirty(int x, int y, int w, int h)
{
    if (sDirtyX0 >= sDirtyX1) {
        sDirtyX0 = x; sDirtyY0 = y; sDirtyX1 = x + w; sDirtyY1 = y + h;
        return;
    }
    if (x < sDirtyX0) sDirtyX0 = x;
    if (y < sDirtyY0) sDirtyY0 = y;
    if (x + w > sDirtyX1) sDirtyX1 = x + w;
    if (y + h > sDirtyY1) sDirtyY1 = y + h;
}

static void DirtyAll(void) { Dirty(0, 0, W, H); }

// Copies a w x h area from `from` (a W x H picture) to sOut.
static void Restore(const u16 *from, int x, int y, int w, int h)
{
    for (int j = y; j < y + h; j++)
        memcpy(&sOut[j * W + x], &from[j * W + x], w * sizeof(u16));
}

#define TEAM_WIN_X 16
#define TEAM_WIN_W (FIT ? 34 : 26) // inside, in tiles
#define TEAM_WIN_H (FIT ? 8 : 6) // (Fit: a fifth line, the weather and money)
#define TEAM_WIN_Y (H - (TEAM_WIN_H + 2) * 8 - (FIT ? 8 : 0)) // (Fit: 8 lines above the bottom)

// M+T, in layers: the grid and the empty window (sMtBase) change only with
// the window settings; the map, the team's lines and the leader's blinking
// dot are each drawn over it when they change, and only they are copied out.
static bool8 DrawMapAndTeam(void)
{
    static u16 sMtBase[CANVAS_PIXELS];
    static DrawKey sLastBase, sLastMap, sLastText, k;
    static bool8 sDotShown;
    static DungeonPos sDotAt;
    u16 *canvas = sOut;
    u16 pal[16];
    u32 tile[8];
    u8 buf[12];
    int x, y;
    DungeonPos dot;
    bool8 dotOn, mapDrawn = FALSE;

    if (Port_MinimapPalette() == NULL)
        return FALSE;

    // The grid and the window
    StartDungeonLayout(PORT_SUB_LAYOUT_MT, &k);
    LoadWindowPalette();
    KeyAdd(&k, sWindowPal, sizeof(sWindowPal));
    if (KeyChanged(&sLastBase, &k)) {
        sOut = sMtBase;
        DrawGrid();
        DrawGameWindow(TEAM_WIN_X, TEAM_WIN_Y, TEAM_WIN_W, TEAM_WIN_H);
        sOut = canvas;
        memcpy(canvas, sMtBase, W * H * sizeof(u16));
        DirtyAll();
        sLastMap.size = sLastText.size = -1; // all drawn again
        sDotShown = FALSE;
    }

    // The map
    MinimapPalette(pal);
    k.size = 0;
    KeyAdd(&k, gPortSubMap, sizeof(gPortSubMap));
    KeyAdd(&k, pal, sizeof(pal));
    if (KeyChanged(&sLastMap, &k)) {
        Restore(sMtBase, SUBMAP_X, SUBMAP_Y, SUBMAP_BW * 8, SUBMAP_BH * 8);
        for (int by = 0; by < SUBMAP_BH; by++) {
            for (int bx = 0; bx < SUBMAP_BW; bx++) {
                BuildMapBlock(bx, by, tile);
                DrawMapBlock(tile, SUBMAP_X + bx * 8, SUBMAP_Y + by * 8, pal);
            }
        }
        Dirty(SUBMAP_X, SUBMAP_Y, SUBMAP_BW * 8, SUBMAP_BH * 8);
        sDotShown = FALSE; // the dot went with it
        mapDrawn = TRUE;
    }

    // The leader's dot, blinking as on the main screen (FlashLeaderIcon)
    dotOn = (++sDotFrames & 8) == 0 && Port_MinimapLeaderDot(&dot)
            && dot.y >= SUBMAP_FIRST_ROW && dot.y < SUBMAP_FIRST_ROW + SUBMAP_ROWS;
    if (sDotShown && (!dotOn || dot.x != sDotAt.x || dot.y != sDotAt.y)) {
        // off, or moved: its block as the map has it
        int bx = sDotAt.x / 2, by = (sDotAt.y - SUBMAP_FIRST_ROW) / 2;
        Restore(sMtBase, SUBMAP_X + bx * 8, SUBMAP_Y + by * 8, 8, 8);
        BuildMapBlock(bx, by, tile);
        DrawMapBlock(tile, SUBMAP_X + bx * 8, SUBMAP_Y + by * 8, pal);
        Dirty(SUBMAP_X + bx * 8, SUBMAP_Y + by * 8, 8, 8);
        sDotShown = FALSE;
    }
    if (dotOn && !sDotShown) {
        int yy = dot.y - SUBMAP_FIRST_ROW, q = (dot.x & 1) + (yy & 1) * 2;
        const u32 *src = Port_MinimapGfx(MAP_GFX_PLAYER, q);
        if (src != NULL) {
            for (int i = 0; i < 8; i++)
                tile[i] = src[i * 2 + 1];
            DrawMapBlock(tile, SUBMAP_X + (dot.x & ~1) * 4, SUBMAP_Y + (yy & ~1) * 4, pal);
            Dirty(SUBMAP_X + (dot.x & ~1) * 4, SUBMAP_Y + (yy & ~1) * 4, 8, 8);
            sDotShown = TRUE;
            sDotAt = dot;
        }
    }

    // The team's lines (Fit: and the weather and money)
    k.size = 0;
    AddRowsText(&k);
    u8 weather = 0;
    s32 money = 0;
    if (FIT) {
        weather = GetApparentWeather(NULL);
        money = gTeamInventoryRef->teamMoney;
        KeyAdd(&k, &weather, sizeof(weather));
        KeyAdd(&k, &money, sizeof(money));
    }
    if (KeyChanged(&sLastText, &k)) {
        int cell = GlyphWidth('0');
        Restore(sMtBase, TEAM_WIN_X + 8, TEAM_WIN_Y + 8, TEAM_WIN_W * 8, TEAM_WIN_H * 8);
        y = TEAM_WIN_Y + 8 + (FIT ? 2 : 0);
        for (int i = 0; i < MAX_TEAM_MEMBERS; i++, y += 12) {
            const PanelRow *row = &sRows[i];
            if (!row->used)
                continue;
            DrawText(TEAM_WIN_X + 12, y, row->name, COLOR_NAME);
            if (FIT) {
                // "Lv. 100", "hp/max", and the HP bar (one pixel per max HP)
                // up to as far from the window's right edge as the name is
                // from its left
                int hpRight = TEAM_WIN_X + 132 + 3 * cell; // (the bar up to 100 pixels)
                int maxRight = hpRight + TextWidth(sSlash) + 3 * cell;
                int barX = maxRight + 3, barMax = TEAM_WIN_X + (TEAM_WIN_W + 2) * 8 - 12 - barX;
                x = DrawText(TEAM_WIN_X + 80, y, sLv, COLOR_TEXT);
                FormatInt(buf, row->level);
                DrawText(x, y, buf, COLOR_TEXT);
                DrawNumberRight(hpRight, y, row->hp);
                DrawText(hpRight, y, sSlash, COLOR_TEXT);
                DrawNumberRight(maxRight, y, row->maxHp);
                // (centred on the digits: their rows 1-8)
                DrawHpBar(barX, y + 1, row->maxHp < 1 ? 1 : row->maxHp > barMax ? barMax : row->maxHp, row->hp, row->maxHp);
            } else {
                // Positions as on the DS: names at 28, "Level" at 96, and HP
                // as "hp/max" in three-digit cells ending at 197.
                int maxRight = TEAM_WIN_X + 181, slashX = maxRight - 3 * cell - TextWidth(sSlash);
                x = DrawText(TEAM_WIN_X + 80, y, sLevel, COLOR_TEXT);
                FormatInt(buf, row->level);
                DrawText(x, y, buf, COLOR_TEXT);
                DrawNumberRight(slashX, y, row->hp);
                DrawText(slashX, y, sSlash, COLOR_TEXT);
                DrawNumberRight(maxRight, y, row->maxHp);
            }
        }
        if (FIT) {
            // "Weather: Clear    Money: 210 (poké)", centred on the last line
            u8 weatherText[64], moneyText[32], name[32];
            int n = 0, wW, wM, gap = 32, x0;
            GetWeatherName(name, weather);
            for (const u8 *s = sWeather; *s; s++) weatherText[n++] = *s;
            for (int i = 0; name[i] && n < 62; i++) weatherText[n++] = name[i];
            weatherText[n] = 0;
            FormatInt(buf, money);
            n = 0;
            for (const u8 *s = sMoney; *s; s++) moneyText[n++] = *s;
            for (int i = 0; buf[i]; i++) moneyText[n++] = buf[i];
            moneyText[n++] = ' ';
            moneyText[n++] = 0x83; moneyText[n++] = 0xBF; moneyText[n++] = 0x83; moneyText[n++] = 0xC4; // {POKE}
            moneyText[n] = 0;
            wW = TextWidth(weatherText);
            wM = TextWidth(moneyText);
            y = TEAM_WIN_Y + 8 + 2 + 4 * 12;
            x0 = TEAM_WIN_X + 8 + (TEAM_WIN_W * 8 - (wW + gap + wM)) / 2;
            DrawText(x0, y, weatherText, COLOR_TEXT);
            DrawText(x0 + wW + gap, y, moneyText, COLOR_TEXT);
        }
        Dirty(TEAM_WIN_X + 8, TEAM_WIN_Y + 8, TEAM_WIN_W * 8, TEAM_WIN_H * 8);
    }
    (void)mapDrawn;
    return sDirtyX0 < sDirtyX1;
}

// --- Message log (Layout Options > Sub-Screen: Log) ---------------------------------

// The dungeon's message log (Others > Message Log), always shown, as the DS
// version's "Log" screen: the latest lines over the M+T grid, in a window,
// with the game's colours and a line over the first line of each message.

#define LOG_WIN_X 16
#define LOG_WIN_Y 16
#define LOG_WIN_W (FIT ? 34 : 26) // inside, in tiles
#define LOG_WIN_H (FIT ? 24 : 18)
#define LOG_LINES (FIT ? 14 : 11)
#define LOG_LINES_MAX 14
#define LOG_LINE_H 13

// The game's text colour codes (InterpretColorChar, text_3.c).
static int ColorIndex(u8 c)
{
    if (c < 16)
        return c;
    if (c >= '0' && c <= '?')
        return c - '0';
    switch (c) {
        case 'H': case 'I': return 4;
        case 'E': case 'S': case 'T': return 15;
        case 'G': case 'M': return 5;
        case 'C': case 'D': case 'N': return 6;
        case 'W': return 2;
    }
    return 7;
}

static u16 TextColor(int index)
{
    switch (index) {
        case 2: return Rgb(248, 96, 64);    // red
        case 4: return Rgb(80, 232, 80);    // green
        case 5: return Rgb(80, 216, 248);   // cyan
        case 6: return COLOR_NAME;          // yellow
        case 3: return Rgb(16, 16, 16);     // black
        case 1: return Rgb(96, 136, 248);   // blue
        default: return COLOR_TEXT;
    }
}

// One line of the log, with its colour codes, as DrawStringInternal does.
static void DrawLogText(int x0, int y, const u8 *str)
{
    int x = x0, color = 7, saved = 7, saved2 = 7;
    while (TRUE) {
        u32 chr;
        const u8 *next;
        if (str[0] == '#') {
            switch (str[1]) {
                case 'C': saved = color; color = ColorIndex(str[2]); str += 3; continue;
                case 'c': saved2 = color; color = ColorIndex(str[2]); str += 3; continue;
                case 'R': color = saved; str += 2; continue;
                case 'r': color = saved2; str += 2; continue;
                case '=': x = x0 + str[2]; str += 3; if (*str == '.') str++; continue;
                case '_': saved = color; str += 3; if (*str == '.') str++; continue;
                case '[':
                    str += 2;
                    while (*str != '\0' && *str++ != ']') ;
                    continue;
                case '~': str += 3; continue;
                case 'W': case 'P': case 'p': str += 2; continue;
            }
        }
        next = GetNextCharFromStr(str, &chr);
        if (chr == '\0' || chr == '\n' || chr == '\r')
            break;
        str = next;
        if (chr == '`') { x += 6; continue; }
        DrawGlyph(x, y, chr, TextColor(color));
        x += GlyphWidth(chr);
    }
}

static bool8 DrawMessageLog(void)
{
    static DrawKey sLast, k;
    const struct MessageLogString *lines[LOG_LINES_MAX];
    int count = 0, id = gDungeon->unk16;

    StartDungeonLayout(PORT_SUB_LAYOUT_LOG, &k);
    LoadWindowPalette();
    KeyAdd(&k, sWindowPal, sizeof(sWindowPal));
    KeyAdd(&k, &gDungeon->unk16, sizeof(gDungeon->unk16));
    KeyAdd(&k, gDungeon->messageLogStrings, sizeof(gDungeon->messageLogStrings));
    if (!KeyChanged(&sLast, &k))
        return FALSE;

    // the latest lines, newest last
    for (int i = 0; i < MESSAGE_LOG_STRINGS_COUNT && count < LOG_LINES; i++) {
        if (--id < 0)
            id += MESSAGE_LOG_STRINGS_COUNT;
        if (gDungeon->messageLogStrings[id].unk0)
            lines[count++] = &gDungeon->messageLogStrings[id];
    }

    DrawGrid();
    LoadWindowPalette();
    DrawGameWindow(LOG_WIN_X, LOG_WIN_Y, LOG_WIN_W, LOG_WIN_H);
    for (int i = 0; i < count; i++) {
        const struct MessageLogString *line = lines[count - 1 - i];
        int y = LOG_WIN_Y + 11 + i * LOG_LINE_H;
        if (line->unk1) { // a new message: the game's double underline
            // a single line (white over its shadow), clear of the text
            // above and below
            Fill(LOG_WIN_X + 8, y - 1, LOG_WIN_W * 8 + 1, 1, COLOR_TEXT);
            Fill(LOG_WIN_X + 8, y, LOG_WIN_W * 8 + 1, 1, Rgb(0, 0, 0));
        }
        DrawLogText(LOG_WIN_X + 16, y, line->str);
    }
    return TRUE;
}

#define TEAM_BASE_X 0x163 // LOCATION_TEAM_BASE on the friend areas map
#define TEAM_BASE_Y 0xA1
#define MARKER_YELLOW_DOT 1 // wmapspr animation

// Set by main_loops.c when the title menu comes up: the dungeon of the
// quicksave the adventure goes on from, or -1.
s32 gPortTitleQuickSave = -1;
extern bool8 gPortTitleMenu;

// On the title menu after a quicksave: that dungeon, on the dungeon map.
static s32 TitleQuickSaveDungeon(void)
{
    if (!gPortTitleMenu || gDungeon != NULL || gPortTitleQuickSave < 0 || gPortTitleQuickSave >= 64)
        return -1;
    return gPortTitleQuickSave;
}

static bool8 sWorldMapChanged;

// The world map made for a new place while the second screen was black
// (not shown yet: out still holds the old one); the screen coming back from
// black (a map still being made is finished at once: not shown half made,
// nor the old place's)
static bool8 sMapNewPicture, sMapFinishNow, sMapBuilding;

static bool8 DrawWorldMap(void)
{
    // The map only changes with the place, so it is drawn once per place;
    // the marker goes on top every frame.
    static u16 sCache[CANVAS_PIXELS];
    static s32 sCachedPlace = -1, sCachedArea = -1, sCachedW = -1;
    static int sLoadedSet = -1, sDotX, sDotY;
    static bool8 sSpritesLoaded;
    static u32 sWater[CANVAS_PIXELS]; // the water's pixels (their colours cycle)
    static int sWaterCount, sWaterBox[4], sDotBox[4];
    s32 place = GetScriptVarValue(NULL, GROUND_PLACE);
    s32 dungeon = PlaceDungeon(place);
    s32 area = FRIEND_AREA_NONE;
    s32 quickSave = TitleQuickSaveDungeon();

    if (quickSave >= 0) {
        dungeon = quickSave;
        place = -2 - quickSave; // its own cache key
    }
    DungeonPos areaPos;
    u8 name[128];

    if (dungeon < 0) {
        sMapBuilding = FALSE;
        return FALSE;
    }
    // Inside a friend area: that area, where the friend areas map shows it.
    if (quickSave < 0 && place == GROUND_PLACE_FRIEND_AREAS) {
        area = MapIdToFriendAreaId(GetScriptVarValue(NULL, GROUND_ENTER));
        if (area == FRIEND_AREA_NONE || area >= FRIEND_AREA_COUNT || !Port_FriendAreaMapPos(area, &areaPos))
            area = FRIEND_AREA_NONE;
    }
    bool8 newColors = LoadWindowPalette();
    bool8 wasShown = sLayoutDrawn == -2 && sCachedPlace != -1; // (out holds a map: kept while a new one is made)
    sWorldMapChanged = sLayoutDrawn != -2;
    sLayoutDrawn = -2;
    // A new place: its map made a part a frame (MAP_STEP_ROWS rows, then the
    // name box, then the water's pixels found), the old one shown meanwhile:
    // all at once it took ~48 ms on an Old 3DS, on the frame a fade out
    // ends (the music stuttered with it). All at once when nothing is shown.
    enum { MAP_STEP_ROWS = 24, SCAN_STEP_ROWS = 60 };
    static int sStep = -1; // the next row to make (H: the name box; H + 1 ...: finding the water), -1: none
    static s32 sStepPlace, sStepArea, sStepW;
    static u8 sStepName[128];
    if (sStep >= 0 && (place != sStepPlace || area != sStepArea || W != sStepW || newColors))
        sStep = -1; // (somewhere else again: started over)
    if (sStep < 0 && (place != sCachedPlace || area != sCachedArea || W != sCachedW || newColors)) {
        int set = dungeon == PLACE_TOWN ? 0 : 1;
        int x = dungeon == PLACE_TOWN ? TEAM_BASE_X : gDungeonCoordinates[dungeon].x;
        int y = dungeon == PLACE_TOWN ? TEAM_BASE_Y : gDungeonCoordinates[dungeon].y;
        if (area != FRIEND_AREA_NONE) {
            x = areaPos.x;
            y = areaPos.y;
        }
        if (set != sLoadedSet) {
            if (!LoadMapSet(set))
                return FALSE;
            sLoadedSet = set;
        }
        if (!sSpritesLoaded && !LoadMapSprites())
            return FALSE;
        sSpritesLoaded = TRUE;
        SetMapView(x, y);
        sDotX = x - sViewLeft;
        sDotY = y - sViewTop;
        if (area != FRIEND_AREA_NONE)
            WriteFriendAreaName(name, area, FALSE);
        else if (quickSave >= 0)
            FormatString(GetDungeonName1(quickSave), name, name + sizeof(name) - 1, 0);
        else
            FormatString(GetGroundPlaceName(place), name, name + sizeof(name) - 1, 0);
        memcpy(sStepName, name, sizeof(sStepName));
        sStepPlace = place;
        sStepArea = area;
        sStepW = W;
        sStep = 0;
        sWaterCount = 0;
        sWaterBox[0] = W; sWaterBox[1] = H; sWaterBox[2] = 0; sWaterBox[3] = 0;
    }
    if (sStep >= 0) {
        int all = !wasShown || sMapFinishNow, scanEnd = H + 1 + (H + SCAN_STEP_ROWS - 1) / SCAN_STEP_ROWS;
        sOut = sCache;
        do {
            if (sStep < H) { // the map's rows
                int y1 = all || sStep + MAP_STEP_ROWS > H ? H : sStep + MAP_STEP_ROWS;
                DrawMapRows(sStep, y1);
                sStep = y1;
            }
            else if (sStep == H) { // the name box
                DrawNameBox(sStepName);
                sStep++;
            }
            else { // the water's pixels that the name box didn't cover
                int y0 = (sStep - H - 1) * SCAN_STEP_ROWS, y1 = y0 + SCAN_STEP_ROWS > H ? H : y0 + SCAN_STEP_ROWS;
                for (int i = y0 * W; i < y1 * W; i++) {
                    if (sMapIdx[i] < MAP_ANIM_FIRST || sMapIdx[i] >= MAP_ANIM_FIRST + MAP_ANIM_COLORS
                        || sCache[i] != MapPixel(i)) {
                        sMapIdx[i] = 0;
                        continue;
                    }
                    int x = i % W, y = i / W;
                    sWater[sWaterCount++] = i;
                    if (x < sWaterBox[0]) sWaterBox[0] = x;
                    if (y < sWaterBox[1]) sWaterBox[1] = y;
                    if (x >= sWaterBox[2]) sWaterBox[2] = x + 1;
                    if (y >= sWaterBox[3]) sWaterBox[3] = y + 1;
                }
                sStep++;
            }
        } while (all && sStep < scanEnd);
        sOut = sOutFrame;
        sMapBuilding = sStep < scanEnd;
        if (sStep < scanEnd)
            return TRUE; // (out keeps the old place's map: nothing new yet)
        sStep = -1;
        sCachedPlace = sStepPlace;
        sCachedArea = sStepArea;
        sCachedW = sStepW;
        sWorldMapChanged = TRUE;
        sMapNewPicture = TRUE; // (until out has it)
    }
    if (sWorldMapChanged) { // all of it (the marker does not move)
        sStats.map++;
        AnimateMapColors();
        for (int n = 0; n < sWaterCount; n++)
            sCache[sWater[n]] = MapPixel(sWater[n]);
        memcpy(sOutFrame, sCache, W * H * sizeof(u16));
        sTrackBox = TRUE;
        sBoxX0 = W; sBoxY0 = H; sBoxX1 = 0; sBoxY1 = 0;
        DrawMapSpriteFrame(MARKER_YELLOW_DOT, 0, sDotX, sDotY);
        sTrackBox = FALSE;
        sDotBox[0] = sBoxX0; sDotBox[1] = sBoxY0; sDotBox[2] = sBoxX1; sDotBox[3] = sBoxY1;
        DirtyAll();
    }
    else if (AnimateMapColors() && sWaterCount > 0) { // only the water's pixels, and the marker over them
        sStats.water++;
        for (int n = 0; n < sWaterCount; n++) {
            u32 i = sWater[n];
            sOutFrame[i] = sCache[i] = MapPixel(i);
        }
        DrawMapSpriteFrame(MARKER_YELLOW_DOT, 0, sDotX, sDotY);
        if (sDirect != NULL) { // the shown picture too: nothing to copy out
            for (int n = 0; n < sWaterCount; n++)
                sDirect[sWater[n]] = sCache[sWater[n]];
            sOut = sDirect;
            DrawMapSpriteFrame(MARKER_YELLOW_DOT, 0, sDotX, sDotY);
            sOut = sOutFrame;
            sDirectDone = TRUE;
        }
        Dirty(sWaterBox[0], sWaterBox[1], sWaterBox[2] - sWaterBox[0], sWaterBox[3] - sWaterBox[1]);
        if (sDotBox[0] < sDotBox[2])
            Dirty(sDotBox[0], sDotBox[1], sDotBox[2] - sDotBox[0], sDotBox[3] - sDotBox[1]);
        sWorldMapChanged = TRUE;
    }
    return TRUE;
}

// --- Transitions -----------------------------------------------------------------
//
// The second screen goes black with the main screen around some changes of
// scene, and comes back with the new one:
// - the friend areas map, and visiting a friend area: fades out and in with
//   the town, black while the map is up;
// - entering a dungeon: fades out with the town, black on the world map and
//   the dungeon's name banner, fades in with the first floor;
// - the end of a dungeon (cleared, escaped, fainted, quicksaved): fades out
//   with its last floor, black through the results, fades in with the town;
// - closing the dungeon results window: cuts to black with the main screen,
//   fades in with the town;
// - in the intro's scenes and on the title menu, it fades in and out with
//   the main screen.

enum {
    TR_NONE,
    TR_TOWN_OUT,   // the town is fading out (friend areas)
    TR_FRIEND_MAP, // the friend areas map is up
    TR_DUNGEON,    // on the way into a dungeon
    TR_CUT,        // the results window was closed; still in the dungeon
    TR_OVER,       // the dungeon is over: its last floor fading out
    TR_TOWN_IN,    // following the town's fade in
};
static int sTransition;

void Port_SubScreenCutToBlack(void)
{
    sTransition = TR_CUT;
}

static bool8 sOverBlack; // (TR_OVER: the floor went)
void Port_SubScreenDungeonOver(void) // (run_dungeon.c)
{
    sTransition = TR_OVER;
    sOverBlack = FALSE;
}

static int GroundLevel(void) { return Port_GroundBrightness(); } // 0-0x100

static s32 PlaceDungeon(s32 place);
static s32 TitleQuickSaveDungeon(void);
static bool8 PlaceOnMap(void) { return TitleQuickSaveDungeon() >= 0 || PlaceDungeon(GetScriptVarValue(NULL, GROUND_PLACE)) >= 0; }

// Where the world map shows the player: the ground place, and in a friend
// area that area (as DrawWorldMap tells them apart)
static s32 PlaceKey(void)
{
    s32 place = GetScriptVarValue(NULL, GROUND_PLACE), area = FRIEND_AREA_NONE;
    if (place == GROUND_PLACE_FRIEND_AREAS)
        area = MapIdToFriendAreaId(GetScriptVarValue(NULL, GROUND_ENTER));
    return place * 1024 + area;
}
// A change of place: where the player was when it began (the second screen
// stays black coming back until the place is another and its map is made,
// or the ground has faded all the way in); the map being made (DrawWorldMap)
static s32 sTransKey;
static bool8 sTransKeyValid;

// Set by the game (main_loops.c) for the port: the ground is running; the
// title menu is up.
bool8 gPortGroundRunning, gPortTitleMenu;

// The brightness (0-0x100) to show the second screen at, or -1 for normal.
static int TransitionBrightness(void)
{
    s32 request = gDungeon == NULL ? Port_GroundExitRequest() : 0;
    bool8 friendMap = gFriendAreasMapPtr != NULL;

    // Back on the title menu (quicksaved, say): whatever was going on is
    // over, and the ground's last exit request (into the dungeon) is old.
    if (gPortTitleMenu && gDungeon == NULL) {
        sTransition = TR_NONE;
        return PlaceOnMap() ? Port_TitleBrightness() * 0x100 / 32 : -1; // the title menu's fades
    }

    switch (sTransition) {
        case TR_NONE:
            if (friendMap || request == 1) {
                sTransKey = PlaceKey();
                sTransKeyValid = TRUE;
            }
            if (friendMap)
                sTransition = TR_FRIEND_MAP;
            else if (request == 1)
                sTransition = TR_TOWN_OUT;
            else if (request == 2)
                sTransition = TR_DUNGEON;
            else if (gDungeon == NULL && gPortTitleMenu)
                return PlaceOnMap() ? Port_TitleBrightness() * 0x100 / 32 : -1; // the title menu's fades
            else if (gDungeon == NULL && gPortGroundRunning && Port_GroundIsDemo())
                return PlaceOnMap() ? GroundLevel() : -1; // intro scenes: follow their fades
            else if (gDungeon == NULL && !gPortGroundRunning && PlaceOnMap())
                return 0; // between modes (the messages after quicksaving): black
            else
                return -1;
            break;
        case TR_TOWN_OUT:
            if (friendMap)
                sTransition = TR_FRIEND_MAP;
            else if (request == 2)
                sTransition = TR_DUNGEON;
            else if (request == 0)
                sTransition = TR_TOWN_IN; // the new place is loading
            break;
        case TR_FRIEND_MAP:
            if (!friendMap)
                sTransition = TR_TOWN_IN;
            break;
        case TR_DUNGEON:
            if (gDungeon != NULL) {
                // Black through the name banner; then the first floor fades
                // in (gUnknown_203B40C: the floor is ready).
                if (!gUnknown_203B40C)
                    return 0;
                if (gDungeonBrightness >= 31) {
                    sTransition = TR_NONE;
                    return -1;
                }
                return gDungeonBrightness * 0x100 / 31;
            }
            if (request == 2)
                return GroundLevel(); // the town fading out
            if (GroundLevel() > 0) // no dungeon after all: the town is back
                sTransition = TR_TOWN_IN;
            else
                return 0; // the world map
            break;
        case TR_CUT:
            if (gDungeon != NULL)
                return 0;
            sTransition = TR_TOWN_IN;
            break;
        case TR_OVER:
            if (gDungeon != NULL) {
                // Following the floor's fade (gUnknown_203B40C: the floor
                // is up), then black. (A floor up again: another floor
                // after all, faded in as on the way into a dungeon.)
                if (!gUnknown_203B40C) {
                    sOverBlack = TRUE;
                    return 0;
                }
                if (sOverBlack) {
                    sTransition = TR_DUNGEON;
                    return gDungeonBrightness * 0x100 / 31;
                }
                return gDungeonBrightness < 31 ? gDungeonBrightness * 0x100 / 31 : 0x100;
            }
            sTransition = TR_TOWN_IN;
            break;
    }
    if (sTransition == TR_FRIEND_MAP)
        return 0;
    if (sTransition == TR_TOWN_IN) {
        if (gPortTitleMenu) { // back to the title menu (after fainting, say)
            sTransition = TR_NONE;
            return PlaceOnMap() ? Port_TitleBrightness() * 0x100 / 32 : -1;
        }
        if (gDungeon != NULL) { // straight into a dungeon
            sTransition = TR_NONE;
            return -1;
        }
        if (GroundLevel() >= 0x100 && Port_GroundExitRequest() == 0) {
            sTransition = TR_NONE;
            sTransKeyValid = FALSE;
            return -1;
        }
        // Black until the new place's map is ready (not the old one shown
        // fading in, then the new one popping in)
        if (sTransKeyValid && GroundLevel() < 0x100 && gDungeon == NULL && (PlaceKey() == sTransKey || sMapBuilding))
            return 0;
        sTransKeyValid = FALSE;
    }
    return GroundLevel();
}

static void Darken(u16 *out, const u16 *in, int brightness)
{
    sStats.darken++;
    // Tables: red and green together (the low 10 bits), and blue
    static u16 sRg[1024], sB[32];
    static int sBuilt = -1;
    if (sBuilt != brightness) {
        sBuilt = brightness;
        for (int v = 0; v < 32; v++)
            sB[v] = (u16)((v * brightness >> 8) << 10);
        for (int c = 0; c < 1024; c++)
            sRg[c] = (u16)(((c & 31) * brightness >> 8) | ((((c >> 5) & 31) * brightness >> 8) << 5));
    }
    for (int i = 0; i < W * H; i++) {
        u16 c = in[i];
        out[i] = sRg[c & 0x3FF] | sB[(c >> 10) & 31];
    }
}

// Sub-Screen Settings > Enabled?: the screen fades out over this many frames
// when turned off (then shows the status text), and back in when turned on.
#define ENABLE_FADE_FRAMES 16
static int sEnableLevel = -1; // 0 (off) .. ENABLE_FADE_FRAMES (on)

static int EnableBrightness(void)
{
    int target = gPortConfig.subScreenDisabled ? 0 : ENABLE_FADE_FRAMES;
    if (sEnableLevel < 0)
        sEnableLevel = target; // the setting as loaded: no fade at boot
    else if (sEnableLevel < target)
        sEnableLevel++;
    else if (sEnableLevel > target)
        sEnableLevel--;
    return sEnableLevel * 0x100 / ENABLE_FADE_FRAMES;
}

// The picture is drawn into sCanvas, and copied out (darkened during
// transitions) only when it or the brightness changed.
static u16 sCanvas[CANVAS_PIXELS];
static bool8 sFadeByBackend;
static int sShownBrightness = 0x100;

void Port_SetSubScreenFadeByBackend(bool on) { sFadeByBackend = on; }
int Port_SubScreenBrightness(void) { return sFadeByBackend ? sShownBrightness : 0x100; }

static int sOutBrightness = -2; // what `out` holds: -2 unknown, else the brightness it was made at
static bool8 sChanged;

void Port_SubScreenSize(int *w, int *h)
{
    *w = sSubW;
    *h = sSubH;
}

bool Port_SubScreenChanged(void)
{
    return sChanged;
}

// The area of `out` that changed in the last call (when Port_SubScreenChanged).
void Port_SubScreenChangedArea(int *x0, int *y0, int *x1, int *y1)
{
    *x0 = sDirtyX0; *y0 = sDirtyY0; *x1 = sDirtyX1; *y1 = sDirtyY1;
}

bool Port_RenderSubScreen(u16 *out)
{
    int brightness, enabled;
    bool8 drawn;

    sChanged = FALSE;
    sDirect = NULL;
    sDirectDone = FALSE;
    sDirtyX0 = sDirtyX1 = 0;
    sDirtyY0 = sDirtyY1 = 0;
    sOut = sOutFrame = sCanvas;
    {
        bool8 fit = gPortConfig.layout[1][0] == PORT_SIZE_FIT;
        int w = fit ? 320 : 256, h = fit ? 240 : 192;
        if (w != sSubW) { // a new size: everything again
            sSubW = w;
            sSubH = h;
            sOutBrightness = -2;
            sLayoutDrawn = -1;
        }
    }
    if (gDungeon == NULL)
        sInDungeon = FALSE;
    brightness = TransitionBrightness();
    enabled = EnableBrightness();
    if (enabled == 0) {
        sOutBrightness = -2;
        sLayoutDrawn = -1;
        return FALSE;
    }
    if (enabled < 0x100)
        brightness = (brightness < 0 ? 0x100 : brightness) * enabled >> 8;
    if (brightness < 0 || brightness > 0x100)
        brightness = 0x100;
    // While black, a new place's world map is made (a part a frame); coming
    // back, what's left of it at once
    static bool8 sWasBlack;
    if (brightness == 0) {
        if (gDungeon == NULL) {
            sMapFinishNow = FALSE;
            u16 *saved = sOut;
            DrawWorldMap();
            sOut = saved;
        }
        sWasBlack = TRUE;
    }
    else {
        sMapFinishNow = sWasBlack;
        sWasBlack = FALSE;
    }
    if (sFadeByBackend) { // dimmed by the backend: drawn at full brightness (not at all while black)
        sShownBrightness = brightness;
        if (brightness == 0 && sOutBrightness == 0x100)
            return TRUE; // (out keeps its picture, shown black)
        if (brightness > 0)
            brightness = 0x100;
    }
    if (brightness == 0) {
        if (sOutBrightness != 0) {
            memset(out, 0, W * H * sizeof(u16));
            sOutBrightness = 0;
            sChanged = TRUE;
            DirtyAll();
        }
        return TRUE;
    }
    if (brightness == 0x100 && sOutBrightness == 0x100)
        sDirect = out;
    if (gDungeon != NULL) {
        if (gPortConfig.layoutSubScreen == PORT_SUB_LAYOUT_MT)
            drawn = DrawMapAndTeam();
        else if (gPortConfig.layoutSubScreen == PORT_SUB_LAYOUT_LOG)
            drawn = DrawMessageLog();
        else
            drawn = DrawTeamPanel();
        if (drawn && gPortConfig.layoutSubScreen == PORT_SUB_LAYOUT_LOG)
            DirtyAll(); // (M+T and Blue mark what they drew)
        sLayoutDrawn = gPortConfig.layoutSubScreen;
    } else {
        if (!DrawWorldMap()) {
            sOutBrightness = -2;
            sLayoutDrawn = -1;
            return FALSE;
        }
        drawn = sWorldMapChanged; // (it marks what it drew)
        if (sMapNewPicture) { // (made while black: all of it to out)
            drawn = TRUE;
            DirtyAll();
        }
        sMapNewPicture = FALSE;
    }
    if (!drawn && brightness == sOutBrightness)
        return TRUE; // out still holds this picture
    if (brightness == 0x100 && sOutBrightness == 0x100) {
        // only what changed (unless it was drawn there already)
        if (!sDirectDone)
            sStats.copied += (sDirtyY1 - sDirtyY0) * (sDirtyX1 - sDirtyX0);
        for (int y = sDirtyY0; y < sDirtyY1 && !sDirectDone; y++)
            memcpy(&out[y * W + sDirtyX0], &sCanvas[y * W + sDirtyX0], (sDirtyX1 - sDirtyX0) * sizeof(u16));
    } else {
        DirtyAll();
        sStats.copied += W * H;
        if (brightness == 0x100)
            memcpy(out, sCanvas, W * H * sizeof(u16));
        else
            Darken(out, sCanvas, brightness);
    }
    sOutBrightness = brightness;
    sChanged = TRUE;
    return TRUE;
}

const char *Port_SubScreenStats(void)
{
    static char buf[96];
    snprintf(buf, sizeof(buf), "all=%d rows=%d spr=%d map=%d water=%d dark=%d copyK=%d",
             sStats.all, sStats.rows, sStats.sprites, sStats.map, sStats.water, sStats.darken, sStats.copied / 1000);
    memset(&sStats, 0, sizeof(sStats));
    return buf;
}
