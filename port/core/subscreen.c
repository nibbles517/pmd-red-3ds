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

// Draws a monster's current animation frame, facing down-left, with its
// anchor (the point the game positions it by) at (ax, ay).
static void DrawMonSprite(Entity *entity, int ax, int ay)
{
    axObject *obj = &entity->axObj;
    const axmain *m;
    const ax_anim *cur, *sw;
    const ax_pose *piece;
    int frame, n, tilesUsed = 0;
    const u16 *pal;

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

    pal = &sPltt[0x100 + obj->axdata.sub1.paletteNum * 16];
    ax += sw[frame].offset.x;
    ay += sw[frame].offset.y;

    // First load the pieces' graphics in order, as the game does.
    for (piece = m->poses[sw[frame].poseId]; (u16)piece->sprite != 0xFFFF || *(const u16 *)&piece->unk2 != 0xFFFF; piece++) {
        const ax_sprite *s;
        if (piece->sprite < 0)
            continue;
        for (s = m->spriteData[piece->sprite]; s->byteCount != 0; s++) {
            if (tilesUsed + s->byteCount > (int)sizeof(sTiles))
                return;
            if (s->gfx != NULL)
                memcpy(sTiles + tilesUsed, s->gfx, s->byteCount);
            else
                memset(sTiles + tilesUsed, 0, s->byteCount);
            tilesUsed += s->byteCount;
        }
    }

    // Then draw the pieces, last first so the first ends up on top (like OAM).
    {
        const ax_pose *first = m->poses[sw[frame].poseId], *last = first;
        while ((u16)last->sprite != 0xFFFF || *(const u16 *)&last->unk2 != 0xFFFF)
            last++;
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
static bool8 sMapLoaded, sMapMissing;

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

static bool8 LoadMap(void)
{
    File f;
    OpenedFile of;
    const RGB_Struct *pal;
    const void *gfx = FindFile(&gTitleMenuFileArchive, "wmp2font");
    const void *map = FindFile(&gTitleMenuFileArchive, "wmp2mcc");
    int i;

    pal = FindFile(&gTitleMenuFileArchive, "wmp2pal");
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

// Where each place outside dungeons is on the world map: the dungeon it is
// part of, or the town.
static s32 PlaceDungeon(s32 place)
{
    switch (place) {
        case GROUND_PLACE_SQUARE: case GROUND_PLACE_SQUARE_2: case GROUND_PLACE_POKEMON_SQUARE:
        case GROUND_PLACE_WHISCASH_POND: case GROUND_PLACE_PELIPPER_POST_OFFICE:
        case GROUND_PLACE_PELIPPER_POST_OFFICE_INSIDE: case GROUND_PLACE_MAKUHITA_DOJO:
        case GROUND_PLACE_MAKUHITA_DOJO_INSIDE: case GROUND_PLACE_LUMINOUS_CAVE:
        case GROUND_PLACE_FRIEND_AREAS: case GROUND_PLACE_TEAM_BASE: case GROUND_PLACE_TEAM_BASE_INSIDE:
            return DUNGEON_OUT_ON_RESCUE; // the town: where the world map starts from it
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

static void DrawMapArea(int cx, int cy)
{
    const int mapPx = MAP_TILES * 8;
    int left = cx - W / 2, top = cy - H / 2;
    if (left < 0) left = 0;
    if (top < 0) top = 0;
    if (left > mapPx - W) left = mapPx - W;
    if (top > mapPx - H) top = mapPx - H;

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

    // The yellow dot.
    {
        int dx = cx - left, dy = cy - top;
        for (int j = -3; j <= 3; j++)
            for (int i = -3; i <= 3; i++) {
                int d = i * i + j * j;
                if (d <= 6)
                    Plot(dx + i, dy + j, Rgb(248, 232, 40));
                else if (d <= 12)
                    Plot(dx + i, dy + j, Rgb(56, 40, 0));
            }
    }
}

// A window like the game's, with the text centred.
static void DrawNameBox(const u8 *name)
{
    const int x0 = 62, y0 = 150, w = 164, h = 30;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            u16 c;
            if (OutsideRounded(x, y, w, h, 4))
                continue;
            if (x == 0 || y == 0 || x == w - 1 || y == h - 1)
                c = Rgb(40, 48, 120);
            else if (x == 1 || y == 1 || x == w - 2 || y == h - 2)
                c = Rgb(168, 184, 248);
            else
                c = Rgb(Lerp(64, 32, y, h), Lerp(88, 56, y, h), Lerp(216, 168, y, h));
            Plot(x0 + x, y0 + y, c);
        }
    }
    DrawText(x0 + (w - TextWidth(name)) / 2, y0 + 9, name, COLOR_TEXT);
}

static bool8 DrawWorldMap(void)
{
    // The picture only changes with the place, so it is drawn once per place.
    static u16 sCache[W * H];
    static s32 sCachedPlace = -1;
    s32 place = GetScriptVarValue(NULL, GROUND_PLACE);
    s32 dungeon = PlaceDungeon(place);
    u8 name[128];

    if (dungeon < 0)
        return FALSE;
    if (place != sCachedPlace) {
        if (!sMapLoaded && !sMapMissing) {
            sMapLoaded = LoadMap();
            sMapMissing = !sMapLoaded;
        }
        if (!sMapLoaded)
            return FALSE;
        sOut = sCache;
        DrawMapArea(gDungeonCoordinates[dungeon].x, gDungeonCoordinates[dungeon].y);
        FormatString(GetGroundPlaceName(place), name, name + sizeof(name) - 1, 0);
        DrawNameBox(name);
        sCachedPlace = place;
    }
    memcpy(sOutFrame, sCache, sizeof(sCache));
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
