// Grass past the stock radius drawn as blocks of tiles. A tile is one draw of its 128 blades, so a grass distance of
// N draws N squared times the tiles. Past the stock radius a block of 2x2 tiles, then 4x4 past twice it and so on up
// to a sector's width, is one draw of its tiles' blades, so each doubling of the distance adds about as many draws as
// the stock radius holds. Blocks past 4x4 keep an even share of every tile's blades, wider, 2048 at most. A block
// keeps every blade's place and height, its heights in 8 bits over the block's range. Nearer tiles stay the
// console's.
#include "game/grass_blocks.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numeric>
#include <unordered_map>
#include <vector>

#include "common/grass_tiles.h"
#include "common/settings.h"
#include "core/log.h"
#include "kernel/memory.h"
#include "xdk/hle.h"

using namespace kernel;

namespace {

// renderer
constexpr uint32_t kEntries = 0x17C;    // stock entry 0
constexpr uint32_t kTemplate = 0x917C;  // stream 0 vertex buffer, 4 vertices a blade
constexpr uint32_t kBlades = 0x9190;
constexpr uint32_t kStamp = 0x91A8;  // frame counter
// sector detail data
constexpr uint32_t kSectorTiles = 0x94;
constexpr uint32_t kSectorX = 0xA8;
constexpr uint32_t kSectorY = 0xAC;
constexpr uint32_t kTileSize = 0xB0;
constexpr uint32_t kSectorTileCount = 0xBC;
constexpr uint32_t kTileStride = 0x114;
// tile: its place in the sector's grid, z range, grass flag, a blade word per blade
constexpr uint32_t kTileX = 0x00;
constexpr uint32_t kTileY = 0x04;
constexpr uint32_t kTileZMin = 0x08;
constexpr uint32_t kTileZMax = 0x0C;
constexpr uint32_t kTileGrass = 0x10;
constexpr uint32_t kTileBlades = 0x14;
constexpr uint32_t kTileHeader = 0x1C;  // hashed: grid place, z range, flag and the first 4 blades
// tile params
constexpr uint32_t kParamCamera = 0x00;  // position at +4, view direction at +0x1C
constexpr uint32_t kParamRadius = 0x1C;  // grass distance, scaled
constexpr uint32_t kParamOrigin = 0x20;
constexpr uint32_t kParamCentre = 0x28;
constexpr uint32_t kParamZMin = 0x34;
constexpr uint32_t kParamZMax = 0x38;
constexpr uint32_t kParamTile = 0x48;
constexpr uint32_t kParamSize = 0x54;
// entry, the draw reads it from +4
constexpr uint32_t kEntrySize = 0x48;
constexpr uint32_t kKey = 0x00;
constexpr uint32_t kVertices = 0x0C;
constexpr uint32_t kIndexCount = 0x10;
constexpr uint32_t kStream0 = 0x18;
constexpr uint32_t kIndexBuffer = 0x1C;
constexpr uint32_t kTexture = 0x20;
constexpr uint32_t kStream1 = 0x40;
// vertex buffer
constexpr uint32_t kBufferData = 0x0C;  // CPU address | 3
// template vertex: POSITION0 the corner off the blade's place, float3, then POSITION1 the place in the tile, float2
constexpr uint32_t kVertexSize = 0x20;
constexpr uint32_t kCorner = 0x00;
constexpr uint32_t kPlace = 0x0C;
constexpr uint32_t kStream1Size = 4;  // COLOR2 per vertex: height in the low byte

constexpr uint32_t kMaxLevel = 6;
constexpr uint32_t kMaxBlockBlades = 2048;
constexpr uint32_t kBudgetBytes = 48u << 20;
constexpr uint32_t kMaxGridSide = 256;
constexpr uint32_t kNoTile = 0;
constexpr uint32_t kNever = 0xFFFFFFFFu;

void WF(uint32_t addr, float value) {
    uint32_t raw;
    std::memcpy(&raw, &value, 4);
    W32(addr, raw);
}

struct Slot {
    uint32_t entry = 0;
    uint32_t buffer = 0;  // stream 1
    uint64_t key = 0;
    uint32_t stamp = 0;
    bool live = false;
};

// a block size: side x side tiles, keep blades of each, their corners width times as wide
struct Level {
    uint32_t side = 0;
    uint32_t keep = 0;
    float width = 1.0f;
    float tile_size = 0.0f;
    uint32_t stream0 = 0;
    uint32_t indices[2] = {};
    uint32_t sorted = kNever;  // stamp of the last sort
    uint32_t now = 0;          // indices in use this frame
    std::vector<uint16_t> blades;  // kept blades of a tile, template order
    std::vector<float> places;     // x, y of each block blade
    std::vector<uint32_t> order;
    std::vector<Slot> slots;
    std::unordered_map<uint64_t, uint32_t> keys;

    uint32_t Count() const { return side * side * keep; }
    uint32_t IndexCount() const { return Count() * 6 - 2; }
};

// a block one of its tiles is in view for, with that tile's params
struct Pending {
    uint32_t level;
    uint32_t bx;
    uint32_t by;
    uint8_t params[kParamSize];
};

// the sector whose tile loop runs
struct Sector {
    uint32_t object = 0;
    uint32_t renderer = 0;
    uint32_t side = 0;
    std::vector<uint32_t> grid;  // tile address by grid place, kNoTile without grass
    std::vector<Pending> pending;
};

Level g_levels[kMaxLevel + 1];
Sector g_sector;
uint32_t g_bytes = 0;
uint32_t g_generation = kNever;
uint32_t g_resets = 0;
uint32_t g_params = 0;  // guest copy of a block's params while the renderer adds it
uint32_t g_desc = 0;
std::vector<uint32_t> g_tiles;

uint32_t Device() { return R32(SYM_Device_pointer); }

void Release(PPCContext& ctx, uint32_t& resource) {
    if (resource) CallGuest(ctx, SYM_D3DResource_Release, {resource});
    resource = 0;
}

void DropLevel(PPCContext& ctx, Level& lv) {
    for (Slot& s : lv.slots) {
        Release(ctx, s.buffer);
        mem::FreeSystem(s.entry);
        g_bytes -= lv.Count() * 4 * kStream1Size;
    }
    lv.slots.clear();
    lv.keys.clear();
    Release(ctx, lv.stream0);
    Release(ctx, lv.indices[0]);
    Release(ctx, lv.indices[1]);
    lv.tile_size = 0.0f;
    lv.sorted = kNever;
}

// a level load drops every block, a cache reset every block's contents
void Sync(PPCContext& ctx) {
    uint32_t generation = fcip::grass::Generation();
    uint32_t resets = fcip::grass::Resets();
    if (generation != g_generation) {
        g_generation = generation;
        for (Level& lv : g_levels) DropLevel(ctx, lv);
    }
    if (resets != g_resets) {
        g_resets = resets;
        for (Level& lv : g_levels) {
            lv.keys.clear();
            for (Slot& s : lv.slots) s.live = false;
        }
    }
}

// the same blades of every tile for every run: a fixed shuffle's first keep
void PickBlades(Level& lv, uint32_t blades) {
    std::vector<uint16_t> all(blades);
    std::iota(all.begin(), all.end(), uint16_t(0));
    uint32_t seed = 0x9E3779B9u;
    for (uint32_t i = blades; i > 1; i--) {
        seed = seed * 1664525u + 1013904223u;
        std::swap(all[i - 1], all[(seed >> 8) % i]);
    }
    lv.blades.assign(all.begin(), all.begin() + lv.keep);
    std::sort(lv.blades.begin(), lv.blades.end());
}

// the stock template's kept blades once per tile of the block, each moved to its tile
void FillTemplate(PPCContext& ctx, uint32_t renderer, Level& lv) {
    uint32_t src = R32(R32(renderer + kTemplate) + kBufferData) & ~3u;
    uint32_t bytes = lv.Count() * 4 * kVertexSize;
    uint32_t dst = uint32_t(CallGuest(ctx, SYM_D3DVertexBuffer_Lock, {lv.stream0, 0, bytes, 0}));
    lv.places.clear();
    for (uint32_t t = 0; t < lv.side * lv.side; t++) {
        float dx = float(t % lv.side) * lv.tile_size;
        float dy = float(t / lv.side) * lv.tile_size;
        for (uint16_t b : lv.blades) {
            uint32_t from = src + b * 4 * kVertexSize;
            lv.places.push_back(GF(from + kPlace) + dx);
            lv.places.push_back(GF(from + kPlace + 4) + dy);
            for (uint32_t v = 0; v < 4; v++, from += kVertexSize, dst += kVertexSize) {
                std::memcpy(HostPtr(dst), HostPtr(from), kVertexSize);
                WF(dst + kCorner, GF(from + kCorner) * lv.width);
                WF(dst + kCorner + 4, GF(from + kCorner + 4) * lv.width);
                WF(dst + kPlace, GF(from + kPlace) + dx);
                WF(dst + kPlace + 4, GF(from + kPlace + 4) + dy);
            }
        }
    }
    CallGuest(ctx, SYM_D3DVertexBuffer_Unlock, {lv.stream0});
}

// the level's template and index buffers for this tile size; false when the renderer has none yet
bool Ready(PPCContext& ctx, uint32_t renderer, uint32_t level, float tile_size) {
    Level& lv = g_levels[level];
    if (lv.stream0 && lv.tile_size == tile_size) return true;
    uint32_t blades = R32(renderer + kBlades);
    if (!R32(renderer + kTemplate) || !blades || blades > 0x10000) return false;
    DropLevel(ctx, lv);
    lv.side = 1u << level;
    lv.keep = std::clamp(kMaxBlockBlades / (lv.side * lv.side), 1u, blades);
    lv.width = std::sqrt(float(blades) / float(lv.keep));
    lv.tile_size = tile_size;
    PickBlades(lv, blades);
    lv.stream0 = uint32_t(CallGuest(ctx, SYM_D3DDevice_CreateVertexBuffer, {lv.Count() * 4 * kVertexSize, 0, 1}));
    for (uint32_t& ib : lv.indices) {
        ib = uint32_t(CallGuest(ctx, SYM_D3DDevice_CreateIndexBuffer, {lv.IndexCount() * 2, 0, 0, 1}));
    }
    if (!lv.stream0 || !lv.indices[0] || !lv.indices[1]) {
        DropLevel(ctx, lv);
        return false;
    }
    FillTemplate(ctx, renderer, lv);
    LOGR("grass blocks: %ux%u tiles of %.2f, %u of %u blades, %.2fx wide", lv.side, lv.side, tile_size, lv.keep,
         blades, lv.width);
    return true;
}

// once a frame the block's blades sorted far to near along the view, as the renderer sorts a tile's
void SortBlades(PPCContext& ctx, uint32_t renderer, Level& lv, uint32_t camera) {
    uint32_t stamp = R32(renderer + kStamp);
    if (lv.sorted == stamp) return;
    lv.sorted = stamp;
    lv.now ^= 1;
    float dx = GF(camera + 0x1C);
    float dy = GF(camera + 0x20);
    uint32_t count = lv.Count();
    lv.order.resize(count);
    std::iota(lv.order.begin(), lv.order.end(), 0u);
    auto depth = [&](uint32_t i) { return lv.places[i * 2] * dx + lv.places[i * 2 + 1] * dy; };
    std::sort(lv.order.begin(), lv.order.end(), [&](uint32_t a, uint32_t b) { return depth(a) > depth(b); });
    uint32_t ib = lv.indices[lv.now];
    CallGuest(ctx, SYM_D3DDevice_SetIndices, {Device(), 0});
    uint32_t p = uint32_t(CallGuest(ctx, SYM_D3DIndexBuffer_Lock, {ib, 0, lv.IndexCount() * 2, 0}));
    // a strip of quads joined by repeating the last index and the next first
    for (uint32_t k = 0; k < count; k++) {
        uint16_t first = uint16_t(lv.order[k] * 4);
        if (k) {
            W16(p, R16(p - 2));
            W16(p + 2, first);
            p += 4;
        }
        W16(p, first);
        W16(p + 2, first + 1);
        W16(p + 4, first + 3);
        W16(p + 6, first + 2);
        p += 8;
    }
    CallGuest(ctx, SYM_D3DIndexBuffer_Unlock, {ib});
}

// a block's tiles and z range; the key hashes their headers
struct Block {
    float zmin = 0.0f;
    float zmax = 0.0f;
    uint32_t first = kNoTile;
    uint64_t key = 0;
};

Block Gather(const Sector& s, const Level& lv, const Pending& p) {
    Block b;
    b.zmin = 1e30f;
    b.zmax = -1e30f;
    b.key = 0x9E3779B97F4A7C15ull ^ (uint64_t(p.level) << 56) ^ (uint64_t(R32(s.object + kSectorX)) << 24) ^
            R32(s.object + kSectorY);
    g_tiles.assign(lv.side * lv.side, kNoTile);
    for (uint32_t t = 0; t < lv.side * lv.side; t++) {
        uint32_t x = p.bx * lv.side + t % lv.side;
        uint32_t y = p.by * lv.side + t / lv.side;
        if (x >= s.side || y >= s.side) continue;
        uint32_t tile = s.grid[y * s.side + x];
        if (tile == kNoTile) continue;
        g_tiles[t] = tile;
        if (b.first == kNoTile) b.first = tile;
        b.zmin = std::min(b.zmin, GF(tile + kTileZMin));
        b.zmax = std::max(b.zmax, GF(tile + kTileZMax));
        const uint8_t* h = static_cast<const uint8_t*>(HostPtr(tile));
        for (uint32_t i = 0; i < kTileHeader; i += 4) {
            uint32_t w;
            std::memcpy(&w, h + i, 4);
            b.key = (b.key ^ w ^ t) * 0xFF51AFD7ED558CCDull;
            b.key ^= b.key >> 32;
        }
    }
    return b;
}

// heights over the block's range: codes 0 and 1 stay no blade, as the vertex shader drops those below 2/255
struct Heights {
    float base;
    float step;
};

Heights HeightsOf(const Block& b) {
    float step = std::max(b.zmax - b.zmin, 1.0f / 64.0f) / 253.0f;
    return {b.zmin - 2.0f * step, step};
}

// the tiles' blades as the renderer's refill writes them, heights in the block's range
void FillBlock(PPCContext& ctx, const Level& lv, const Block& b, uint32_t buffer) {
    Heights hs = HeightsOf(b);
    uint32_t bytes = lv.Count() * 4 * kStream1Size;
    CallGuest(ctx, SYM_D3DDevice_SetStreamSource, {Device(), 1, 0, 0, 0});
    uint32_t p = uint32_t(CallGuest(ctx, SYM_D3DVertexBuffer_Lock, {buffer, 0, bytes, 0}));
    for (uint32_t tile : g_tiles) {
        float z0 = tile ? GF(tile + kTileZMin) : 0.0f;
        float z1 = tile ? GF(tile + kTileZMax) : 0.0f;
        for (uint16_t blade : lv.blades) {
            uint32_t c = 0;
            uint32_t v = tile ? R16(tile + kTileBlades + blade * 2) : 0;
            uint32_t h = v >> 8;
            if (h >= 2) {
                float z = z0 + (z1 - z0) * float(h) / 255.0f;
                uint32_t code = uint32_t(std::clamp(std::lround((z - hs.base) / hs.step), 2l, 255l));
                uint32_t a = ((v & 7) * 255) >> 3;
                uint32_t mid = (((v >> 3) & 0x1F) * 255) >> 5;
                c = 0xFF000000u | a << 16 | mid << 8 | code;
            }
            for (uint32_t k = 0; k < 4; k++, p += 4) W32(p, c);
        }
    }
    CallGuest(ctx, SYM_D3DVertexBuffer_Unlock, {buffer});
}

// entry 0's fields with the level's streams and counts
uint32_t MakeEntry(uint32_t renderer, const Level& lv, uint32_t buffer) {
    uint32_t entry = mem::AllocSystem(kEntrySize);
    if (!entry) return 0;
    std::memcpy(HostPtr(entry), HostPtr(renderer + kEntries), kEntrySize);
    W32(entry + kKey, 0);
    W32(entry + kVertices, lv.Count() * 4);
    W32(entry + kIndexCount, lv.IndexCount());
    W32(entry + kStream0, lv.stream0);
    W32(entry + kStream1, buffer);
    return entry;
}

// a new slot within the budget, else the least recent one not drawn this frame; null when every one is
Slot* TakeSlot(PPCContext& ctx, uint32_t renderer, Level& lv, uint32_t stamp) {
    uint32_t bytes = lv.Count() * 4 * kStream1Size;
    if (g_bytes + bytes <= kBudgetBytes) {
        uint32_t buffer = uint32_t(CallGuest(ctx, SYM_D3DDevice_CreateVertexBuffer, {bytes, 0, 1}));
        uint32_t entry = buffer ? MakeEntry(renderer, lv, buffer) : 0;
        if (entry) {
            g_bytes += bytes;
            lv.slots.push_back({entry, buffer});
            return &lv.slots.back();
        }
        Release(ctx, buffer);
    }
    Slot* oldest = nullptr;
    for (Slot& s : lv.slots) {
        if (s.stamp != stamp && (!oldest || stamp - s.stamp > stamp - oldest->stamp)) oldest = &s;
    }
    if (oldest && oldest->live) lv.keys.erase(oldest->key);
    return oldest;
}

// the slot holding this block's blades, filled on a miss
Slot* FindSlot(PPCContext& ctx, uint32_t renderer, Level& lv, const Block& b) {
    uint32_t stamp = R32(renderer + kStamp);
    auto it = lv.keys.find(b.key);
    Slot* s = it != lv.keys.end() ? &lv.slots[it->second] : nullptr;
    if (!s) {
        s = TakeSlot(ctx, renderer, lv, stamp);
        if (!s) return nullptr;
        FillBlock(ctx, lv, b, s->buffer);
        s->key = b.key;
        s->live = true;
        lv.keys[b.key] = uint32_t(s - lv.slots.data());
    }
    s->stamp = stamp;
    return s;
}

// the renderer adds the block as a tile, its params moved to the block, then draws it with the level's indices
void Emit(PPCContext& ctx, const Pending& p) {
    const Sector& s = g_sector;
    Level& lv = g_levels[p.level];
    float tile_size = GF(s.object + kTileSize);
    if (!Ready(ctx, s.renderer, p.level, tile_size)) return;
    Block b = Gather(s, lv, p);
    if (b.first == kNoTile) return;
    Slot* slot = FindSlot(ctx, s.renderer, lv, b);
    if (!slot) return;
    std::memcpy(HostPtr(g_params), p.params, kParamSize);
    SortBlades(ctx, s.renderer, lv, R32(g_params + kParamCamera));
    Heights hs = HeightsOf(b);
    float extent = float(lv.side) * tile_size;
    float ox = GF(s.object + kSectorX) + float(p.bx) * extent;
    float oy = GF(s.object + kSectorY) + float(p.by) * extent;
    WF(g_params + kParamOrigin, ox);
    WF(g_params + kParamOrigin + 4, oy);
    WF(g_params + kParamCentre, ox + extent * 0.5f);
    WF(g_params + kParamCentre + 4, oy + extent * 0.5f);
    WF(g_params + kParamCentre + 8, (b.zmin + b.zmax) * 0.5f);
    WF(g_params + kParamZMin, hs.base);
    WF(g_params + kParamZMax, hs.base + 255.0f * hs.step);
    W32(g_params + kParamTile, b.first);
    W32(slot->entry + kTexture, R32(s.renderer + kEntries + kTexture));
    g_desc = slot->entry + 4;
    CallGuest(ctx, SYM_Grass_AddTile, {s.renderer, g_params});
    g_desc = 0;
    // the add set the renderer's own indices
    W32(slot->entry + kIndexBuffer, lv.indices[lv.now]);
}

// the sector's grid of tiles with grass, made at its first far tile
void BuildGrid(Sector& s) {
    uint32_t tiles = R32(s.object + kSectorTiles);
    uint32_t count = R32(s.object + kSectorTileCount);
    s.side = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t t = tiles + i * kTileStride;
        if (!R32(t + kTileGrass)) continue;
        s.side = std::max({s.side, R32(t + kTileX) + 1, R32(t + kTileY) + 1});
    }
    s.side = std::min(s.side, kMaxGridSide);
    s.grid.assign(s.side * s.side, kNoTile);
    for (uint32_t i = 0; i < count; i++) {
        uint32_t t = tiles + i * kTileStride;
        uint32_t x = R32(t + kTileX);
        uint32_t y = R32(t + kTileY);
        if (R32(t + kTileGrass) && x < s.side && y < s.side) s.grid[y * s.side + x] = t;
    }
}

// the biggest block of this tile that fits the sector and whose centre lies past the stock radius times its
// side over 2; 0 for the tile itself
uint32_t LevelOf(const Sector& s, uint32_t params, uint32_t x, uint32_t y, uint32_t mult) {
    uint32_t camera = R32(params + kParamCamera);
    float cx = GF(camera + 4);
    float cy = GF(camera + 8);
    float stock = GF(params + kParamRadius) / float(mult);
    float tile_size = GF(s.object + kTileSize);
    for (uint32_t level = kMaxLevel; level >= 1; level--) {
        uint32_t side = 1u << level;
        if (side > s.side) continue;
        float extent = float(side) * tile_size;
        float bx = GF(s.object + kSectorX) + (float(x / side) + 0.5f) * extent - cx;
        float by = GF(s.object + kSectorY) + (float(y / side) + 0.5f) * extent - cy;
        float reach = stock * float(side / 2);
        if (bx * bx + by * by >= reach * reach) return level;
    }
    return 0;
}

// a far tile in view marks its block instead of being added
bool Divert(uint32_t renderer, uint32_t params) {
    Sector& s = g_sector;
    uint32_t mult = fcip::Get().grass_distance;
    if (!s.object || !g_params || g_desc || mult <= 1) return false;
    if (s.grid.empty()) BuildGrid(s);
    uint32_t tile = R32(params + kParamTile);
    uint32_t x = R32(tile + kTileX);
    uint32_t y = R32(tile + kTileY);
    if (x >= s.side || y >= s.side) return false;
    uint32_t level = LevelOf(s, params, x, y, mult);
    if (!level) return false;
    s.renderer = renderer;
    uint32_t bx = x >> level;
    uint32_t by = y >> level;
    // tiles run in rows, so their block is most often the last one marked
    auto same = [&](const Pending& p) { return p.level == level && p.bx == bx && p.by == by; };
    if (std::find_if(s.pending.rbegin(), s.pending.rend(), same) != s.pending.rend()) return true;
    Pending p{level, bx, by, {}};
    std::memcpy(p.params, HostPtr(params), kParamSize);
    s.pending.push_back(p);
    return true;
}

}  // namespace

namespace fcip::grass_blocks {

bool Owns(uint32_t params) { return g_desc && params == g_params; }
uint32_t Desc() { return g_desc; }

}  // namespace fcip::grass_blocks

PPC_EXTERN_FUNC(GUEST_ORIG(Grass_DrawSector));
PPC_EXTERN_FUNC(GUEST_ORIG(Grass_AddTile));

// a sector's grass tile loop, r3 its detail data; its far tiles' blocks follow the loop
PPC_FUNC(GUEST(Grass_DrawSector)) {
    if (!g_params) g_params = mem::AllocSystem(kParamSize);
    Sync(ctx);
    g_sector.object = uint32_t(ctx.r[3]);
    GUEST_ORIG(Grass_DrawSector)(ctx, base);
    for (const Pending& p : g_sector.pending) Emit(ctx, p);
    g_sector.object = 0;
    g_sector.pending.clear();
    g_sector.grid.clear();
}

// renderer vtable +0x98, a tile in view: r3 the renderer, r4 the tile params
PPC_FUNC(GUEST(Grass_AddTile)) {
    if (Divert(uint32_t(ctx.r[3]), uint32_t(ctx.r[4]))) return;
    GUEST_ORIG(Grass_AddTile)(ctx, base);
}
