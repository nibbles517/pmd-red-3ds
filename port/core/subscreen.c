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

#include "global.h"
#include "constants/direction.h"
#include "constants/dungeon.h"
#include "constants/event_flag.h"
#include "decompress_at.h"
#include "decompress_sir.h"
#include "def_filearchives.h"
#include "event_flag.h"
#include "game_options.h"
#include "graphics_memory.h"
#include "ground_place.h"
#include "string_format.h"
#include "dungeon_util.h"
#include "items.h"
#include "pokemon.h"
#include "strings.h"
#include "text_1.h"
#include "text_2.h"
#include "text_3.h"
#include "structs/axdata.h"
#include "structs/str_dungeon.h"
#include "../core/port.h"

extern Dungeon *gDungeon;
extern const DungeonPos gDungeonCoordinates[64]; // world_map.c

#define W SUBSCREEN_W
#define H SUBSCREEN_H
#define ROW_H 48

static u16 *sOut;       // where drawing goes
static u16 *sOutFrame;  // the caller's buffer
static const u16 *const sPltt = (const u16 *)gPortPltt; // BG palettes, then OBJ

static inline u16 Rgb(int r, int g, int b)
{
    return (u16)((r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10));
}

static inline void Plot(int x, int y, u16 c)
{
    if ((unsigned)x < W && (unsigned)y < H)
        sOut[y * W + x] = c;
}

static void Fill(int x, int y, int w, int h, u16 c)
{
    for (int j = y; j < y + h; j++)
        for (int i = x; i < x + w; i++)
            Plot(i, j, c);
}

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
            } else if (y < 27) {
                r = 134; g = 63; b = 2;
            } else {
                // floor, lighter towards the middle of an ellipse
                int dx = x - 23, dy = y - 37;
                int d = dx * dx * 100 / (22 * 22) + dy * dy * 100 / (9 * 9); // < 100 inside
                if (y < 29) { r = 170; g = 115; b = 50; }
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

static void DrawHpBar(int x, int y, int w, int hp, int maxHp)
{
    int fill = maxHp > 0 ? (hp * w + maxHp - 1) / maxHp : 0;
    if (fill > w) fill = w;
    if (hp > 0 && fill == 0) fill = 1;
    Fill(x, y, w, 1, Rgb(16, 16, 16));
    Fill(x, y + 1, w, 1, Rgb(248, 248, 248));
    Fill(x, y + 2, fill, 4, Rgb(72, 216, 72));
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

// Draws a monster's current animation frame, facing down-left, with its
// anchor (the point the game positions it by) at (ax, ay).
static void DrawMonSprite(Entity *entity, int ax, int ay)
{
    axObject *obj = &entity->axObj;
    const axmain *m;
    const ax_anim *cur, *sw;
    int frame, n;

    if (obj->spriteFile == NULL || obj->axdata.activeAnimData == NULL)
        return;
    m = (const axmain *)obj->spriteFile->data;
    if (m == NULL || obj->unk43_animId2 >= m->animCount)
        return;
    cur = m->animations[obj->unk43_animId2][obj->unk45_orientation & DIRECTION_MASK];
    sw = m->animations[obj->unk43_animId2][DIRECTION_SOUTHWEST];
    if (cur == NULL || sw == NULL)
        return;

    // The main sprite is showing the frame before activeAnimData. Show the
    // same frame of the down-left animation.
    frame = (int)(obj->axdata.activeAnimData - cur) - 1;
    n = AnimLength(sw);
    if (n == 0)
        return;
    if (frame < 0 || frame >= AnimLength(cur))
        frame = 0;
    if (frame >= n)
        frame = n - 1;

    DrawPose(m->poses[sw[frame].poseId], m->spriteData, NULL, 0,
             &sPltt[0x100], obj->axdata.sub1.paletteNum,
             ax + sw[frame].offset.x, ay + sw[frame].offset.y);
}

// --- Team panel -----------------------------------------------------------------------

static const u8 sLevel[] = "Level ";
static const u8 sItem[] = "Item: ";
static const u8 sNone[] = "None";
static const u8 sLeader[] = "Leader";
static const u8 sSlash[] = "/";

static void DrawMember(int y0, Entity *entity)
{
    EntityInfo *info = GetEntInfo(entity);
    u8 buf[64];
    int x;

    DrawSpriteBox(y0, FALSE);
    DrawInfoPanel(y0);
    DrawMonSprite(entity, 23, y0 + 46);

    // Name (team members' own names; the game's StrncpyCustom stops at 0 too)
    if (info->teamIndex < MAX_TEAM_MEMBERS) {
        const u8 *name = gRecruitedPokemonRef->dungeonTeam[info->teamIndex].name;
        int i;
        for (i = 0; i < POKEMON_NAME_LENGTH && name[i]; i++)
            buf[i] = name[i];
        buf[i] = 0;
        DrawText(56, y0 + 5, buf, COLOR_NAME);
    }

    // Level
    x = DrawText(60, y0 + 17, sLevel, COLOR_TEXT);
    FormatInt(buf, info->level);
    DrawText(x, y0 + 17, buf, COLOR_TEXT);

    // HP: "hp/ max", each number right-aligned in three digits
    FormatInt(buf, info->HP);
    DrawText(138 - TextWidth(buf), y0 + 17, buf, COLOR_TEXT);
    DrawText(138, y0 + 17, sSlash, COLOR_TEXT);
    FormatInt(buf, info->maxHPStat);
    DrawText(160 - TextWidth(buf), y0 + 17, buf, COLOR_TEXT);
    DrawHpBar(163, y0 + 20, 52, info->HP, info->maxHPStat);

    // Held item
    x = DrawText(60, y0 + 29, sItem, COLOR_TEXT);
    if (ItemExists(&info->heldItem))
        DrawText(x, y0 + 29, gItemParametersData[info->heldItem.id].name, COLOR_TEXT);
    else
        DrawText(x, y0 + 29, sNone, COLOR_TEXT);

    // Tactic
    if (info->isTeamLeader)
        DrawText(148, y0 + 29, sLeader, COLOR_TEXT);
    else if (info->tactic < NUM_TACTICS)
        DrawText(148, y0 + 29, gTacticsNames[info->tactic], COLOR_TEXT);
}

static void DrawTeamPanel(void)
{
    Entity *members[MAX_TEAM_MEMBERS];
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

    Fill(0, 0, W, H, Rgb(74, 74, 116));
    for (i = 0; i < MAX_TEAM_MEMBERS; i++) {
        int y0 = i * ROW_H;
        if (i < count)
            DrawMember(y0, members[i]);
        else
            DrawSpriteBox(y0, TRUE);
    }
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

static const char *sMapFiles[2][3] = {
    { "wmapfont", "wmapmcc", "wmappal" }, // the friend areas map (the DS version's look)
    { "wmp2font", "wmp2mcc", "wmp2pal" }, // the map shown on the way to a dungeon
};

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
    return TRUE;
}

// The map's marker sprites ("wmapspr": location markers, arrows). Their
// colours are OBJ palette 0 as the map scenes set it, from "palet".
static const EfoFileData *sMapSprites;
static u16 sMapSpritePals[256];

static bool8 LoadMapSprites(void)
{
    const RGB_Struct *pal = FindFile(&gMonsterFileArchive, "palet");
    sMapSprites = FindFile(&gTitleMenuFileArchive, "wmapspr");
    if (sMapSprites == NULL || pal == NULL)
        return FALSE;
    for (int i = 0; i < 240; i++)
        sMapSpritePals[i] = Rgb(pal[i].r, pal[i].g, pal[i].b);
    return TRUE;
}

static void DrawMapSpriteFrame(int anim, int frame, int x, int y)
{
    const ax_anim *a = sMapSprites->animations[anim][0];
    DrawPose(sMapSprites->poses[a[frame].poseId], NULL, sMapSprites->tiles, sMapSprites->tileCount,
             sMapSpritePals, 0, x + a[frame].offset.x, y + a[frame].offset.y);
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

static void DrawMapArea(int cx, int cy)
{
    int left = cx - W / 2, top = cy - H / 2;
    if (left < 0) left = 0;
    if (top < 0) top = 0;
    if (left > MAP_W - W) left = MAP_W - W;
    if (top > MAP_H - H) top = MAP_H - H;
    sViewLeft = left;
    sViewTop = top;

    for (int y = 0; y < H; y++) {
        int my = top + y;
        for (int x = 0; x < W; x++) {
            int mx = left + x;
            int layer, c = 0, palBase = 0;
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
                    if (c == 0)
                        c = FontTilePixel(font, TILE_FILL, FALSE, FALSE, x, y);
                    if (c != 0)
                        Plot(px + tx * 8 + x, py + ty * 8 + y, sWindowPal[c] & 0x7FFF);
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

#define TEAM_BASE_X 0x163 // LOCATION_TEAM_BASE on the friend areas map
#define TEAM_BASE_Y 0xA1
#define MARKER_YELLOW_DOT 1 // wmapspr animation

static bool8 DrawWorldMap(void)
{
    // The map only changes with the place, so it is drawn once per place;
    // the marker goes on top every frame.
    static u16 sCache[W * H];
    static s32 sCachedPlace = -1;
    static int sLoadedSet = -1, sDotX, sDotY;
    static bool8 sSpritesLoaded;
    s32 place = GetScriptVarValue(NULL, GROUND_PLACE);
    s32 dungeon = PlaceDungeon(place);
    u8 name[128];

    if (dungeon < 0)
        return FALSE;
    bool8 newColors = LoadWindowPalette();
    if (place != sCachedPlace || newColors) {
        int set = dungeon == PLACE_TOWN ? 0 : 1;
        int x = dungeon == PLACE_TOWN ? TEAM_BASE_X : gDungeonCoordinates[dungeon].x;
        int y = dungeon == PLACE_TOWN ? TEAM_BASE_Y : gDungeonCoordinates[dungeon].y;
        if (set != sLoadedSet) {
            if (!LoadMapSet(set))
                return FALSE;
            sLoadedSet = set;
        }
        if (!sSpritesLoaded && !LoadMapSprites())
            return FALSE;
        sSpritesLoaded = TRUE;
        sOut = sCache;
        DrawMapArea(x, y);
        sDotX = x - sViewLeft;
        sDotY = y - sViewTop;
        FormatString(GetGroundPlaceName(place), name, name + sizeof(name) - 1, 0);
        DrawNameBox(name);
        sCachedPlace = place;
        sOut = sOutFrame;
    }
    memcpy(sOutFrame, sCache, sizeof(sCache));
    DrawMapSpriteFrame(MARKER_YELLOW_DOT, 0, sDotX, sDotY);
    return TRUE;
}

bool Port_RenderSubScreen(u16 *out)
{
    sOut = sOutFrame = out;
    if (gDungeon != NULL) {
        DrawTeamPanel();
        return TRUE;
    }
    return DrawWorldMap();
}
