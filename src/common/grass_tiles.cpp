// The grass tile cache. The renderer keeps 512 entries, each a vertex buffer of a tile's blades keyed by the tile's
// address, and skips tiles once a frame took 512. Further grass takes more, so the cache grows in chunks of our own
// entries and buffers, keyed by a hash of the blade words, least recent reused first. Far misses past the stock radius
// refill at most 256 a frame, nearest first; the rest wait a frame.
#include "common/grass_tiles.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <limits>
#include <unordered_map>
#include <vector>

#include "common/settings.h"
#include "kernel/memory.h"
#include "xdk/hle.h"

using namespace kernel;
using fcip::grass::Admission;
using fcip::grass::kStockEntries;

namespace {

// grass renderer
constexpr uint32_t kEntries = 0x17C;
constexpr uint32_t kEntrySize = 0x48;
constexpr uint32_t kBlades = 0x9190;
constexpr uint32_t kStamp = 0x91A8;  // frame counter
// tile params
constexpr uint32_t kParamCamera = 0x00;  // position at +4
constexpr uint32_t kParamRadius = 0x1C;  // grass distance, scaled
constexpr uint32_t kParamCentre = 0x28;
constexpr uint32_t kParamBlades = 0x3C;  // blade words, one per blade
constexpr uint32_t kParamKey = 0x48;     // tile address
// entry
constexpr uint32_t kKey = 0x00;
constexpr uint32_t kDesc = 0x04;  // what the draw keeps
constexpr uint32_t kTexture = 0x20;
constexpr uint32_t kBuffer = 0x40;
constexpr uint32_t kUsed = 0x44;
// vertex buffer header
constexpr uint32_t kFence = 0x04;
constexpr uint32_t kLockRange = 0x08;
constexpr uint32_t kData = 0x0C;   // address | 3
constexpr uint32_t kFetch = 0x10;  // size | flags
constexpr uint32_t kHeaderSize = 0x14;

constexpr uint32_t kChunk = 1024;
constexpr uint32_t kMaxEntries = 32768;
constexpr uint32_t kMaxBlades = 4096;
constexpr uint32_t kFarMisses = 256;
constexpr uint32_t kAuditsPerFrame = 32;
constexpr uint32_t kNone = 0xFFFFFFFFu;

struct Slot {
    uint32_t entry;
    uint64_t key = 0;
    uint32_t stamp = 0;
    bool live = false;  // key in the map
    bool used = false;  // stamp is valid
    uint32_t prev = kNone;
    uint32_t next = kNone;
};

// kChunk entries then kChunk headers in block, the headers slicing buffer
struct Chunk {
    uint32_t buffer;
    uint32_t block;
};

// slots run from head, most recent, to tail
struct Pool {
    uint32_t renderer = 0;
    uint32_t slice = 0;
    std::vector<Chunk> chunks;
    std::vector<Slot> slots;
    std::unordered_map<uint64_t, uint32_t> keys;
    uint32_t head = kNone;
    uint32_t tail = kNone;
};

Pool g_pool;

// the miss being refilled, checked once the copy is done
struct Refill {
    uint32_t slot = kNone;
    uint64_t key = 0;
};
Refill g_refill;

// the far tile the budget test hashed, for its lookup
struct Pending {
    uint32_t params = 0;
    uint64_t key = 0;
};
Pending g_pending;

// far misses this frame; limit: the kFarMisses nearest of those the last frame turned away
struct Far {
    uint32_t stamp = 0;
    uint32_t left = kFarMisses;
    float limit = std::numeric_limits<float>::max();
    std::vector<float> waiting;  // squared distances
};
Far g_far;

// hits checked against their buffer this frame
struct Audit {
    uint32_t stamp = 0;
    uint32_t taken = 0;
    uint32_t done = 0;
};
Audit g_audit;

// bumped by the hooks, which may run off the drawing thread
std::atomic<uint32_t> g_attach{0};
std::atomic<uint32_t> g_forget{0};
std::atomic<uint32_t> g_texture{0};
uint32_t g_seen_attach = 0;
uint32_t g_seen_forget = 0;
uint32_t g_seen_texture = 0;

// the squared camera distance of a tile past the stock radius, 0 within it or at 1x; the same centre as the tile
// loop's radius test
float FarDistance(uint32_t params) {
    uint32_t mult = fcip::Get().grass_distance;
    if (mult <= 1) return 0.0f;
    uint32_t camera = R32(params + kParamCamera);
    float d2 = 0.0f;
    for (uint32_t i = 0; i < 12; i += 4) {
        float v = GF(params + kParamCentre + i) - GF(camera + 4 + i);
        d2 += v * v;
    }
    float stock = GF(params + kParamRadius) / float(mult);
    return d2 > stock * stock ? d2 : 0.0f;
}

bool TakeFar(uint32_t stamp, float d2) {
    Far& f = g_far;
    if (stamp != f.stamp) {
        f.stamp = stamp;
        f.left = kFarMisses;
        f.limit = std::numeric_limits<float>::max();
        if (f.waiting.size() > kFarMisses) {
            std::nth_element(f.waiting.begin(), f.waiting.begin() + (kFarMisses - 1), f.waiting.end());
            f.limit = f.waiting[kFarMisses - 1];
        }
        f.waiting.clear();
    }
    if (f.left && d2 <= f.limit) {
        f.left--;
        return true;
    }
    f.waiting.push_back(d2);
    return false;
}

// the blade words the refill turns into the entry's buffer
uint64_t BladeKey(uint32_t renderer, uint32_t params) {
    uint32_t bytes = std::min(R32(renderer + kBlades), kMaxBlades) * 2;
    const uint8_t* p = static_cast<const uint8_t*>(HostPtr(R32(params + kParamBlades)));
    uint64_t h = 0x9E3779B97F4A7C15ull ^ bytes;
    uint32_t i = 0;
    for (; i + 8 <= bytes; i += 8) {
        uint64_t w;
        std::memcpy(&w, p + i, 8);
        h = (h ^ w) * 0xFF51AFD7ED558CCDull;
        h ^= h >> 32;
    }
    for (; i < bytes; i++) h = (h ^ p[i]) * 0x100000001B3ull;
    h ^= h >> 29;
    h *= 0xC4CEB9FE1A85EC53ull;
    return h ^ (h >> 32);
}

// whether the entry's buffer holds what the refill writes for these words
bool Holds(uint32_t renderer, uint32_t params, uint32_t entry) {
    uint32_t header = R32(entry + kBuffer);
    if (header < 0x10000) return false;
    uint32_t data = R32(header + kData) & ~3u;
    uint32_t words = R32(params + kParamBlades);
    uint32_t blades = std::min(R32(renderer + kBlades), kMaxBlades);
    for (uint32_t i = 0; i < blades; i++) {
        uint32_t v = R16(words + i * 2);
        uint32_t hi = (((v & 7) * 255) | 0x7F800) >> 3;
        uint32_t mid = (((v >> 3) & 0x1F) * 255) >> 5;
        uint32_t c = (((hi << 8) | mid) << 8) | (v >> 8);
        for (uint32_t k = 0; k < 16; k += 4)
            if (R32(data + i * 16 + k) != c) return false;
    }
    return true;
}

// a new frame starts the counts; whether to audit this tile's hit, 1 in 32 up to 32 a frame
bool ShouldAudit(uint32_t stamp) {
    Audit& a = g_audit;
    if (stamp != a.stamp) a = {stamp, 0, 0};
    a.taken++;
    return a.done < kAuditsPerFrame && (a.taken + stamp * 7) % 32 == 0;
}

uint32_t ChunkEntry(const Chunk& c, uint32_t i) { return c.block + i * kEntrySize; }
uint32_t ChunkHeader(const Chunk& c, uint32_t i) { return c.block + kChunk * kEntrySize + i * kHeaderSize; }

void Unlink(Pool& p, uint32_t i) {
    const Slot& s = p.slots[i];
    if (s.prev != kNone) p.slots[s.prev].next = s.next;
    else p.head = s.next;
    if (s.next != kNone) p.slots[s.next].prev = s.prev;
    else p.tail = s.prev;
}

void PushFront(Pool& p, uint32_t i) {
    Slot& s = p.slots[i];
    s.prev = kNone;
    s.next = p.head;
    if (p.head != kNone) p.slots[p.head].prev = i;
    else p.tail = i;
    p.head = i;
}

void PushBack(Pool& p, uint32_t entry) {
    uint32_t i = uint32_t(p.slots.size());
    p.slots.push_back({entry});
    p.slots[i].prev = p.tail;
    if (p.tail != kNone) p.slots[p.tail].next = i;
    else p.head = i;
    p.tail = i;
}

// entry 0's fields with its own buffer and no key
void Clone(uint32_t renderer, uint32_t entry, uint32_t header) {
    std::memcpy(HostPtr(entry), HostPtr(renderer + kEntries), kEntrySize);
    W32(entry + kKey, 0);
    W32(entry + kBuffer, header);
}

// the draws record their fence in our headers, so the release waits on the newest
void DropChunks(PPCContext& ctx) {
    for (const Chunk& c : g_pool.chunks) {
        uint32_t fence = 0;
        for (uint32_t i = 0; i < kChunk; i++) fence = std::max(fence, R32(ChunkHeader(c, i) + kFence));
        W32(c.buffer + kFence, fence);
        CallGuest(ctx, SYM_D3DResource_Release, {c.buffer});
        mem::FreeSystem(c.block);
    }
    g_pool.chunks.clear();
}

// rebuilds the slots over this renderer's entries; the chunks stay unless the blade count changed
void Attach(PPCContext& ctx, uint32_t renderer) {
    Pool& p = g_pool;
    uint32_t slice = R32(renderer + kBlades) * 16;
    if (slice != p.slice) DropChunks(ctx);
    p.renderer = renderer;
    p.slice = slice;
    p.keys.clear();
    p.slots.clear();
    p.head = kNone;
    p.tail = kNone;
    for (uint32_t i = 0; i < kStockEntries; i++) PushBack(p, renderer + kEntries + i * kEntrySize);
    for (const Chunk& c : p.chunks) {
        for (uint32_t i = 0; i < kChunk; i++) {
            Clone(renderer, ChunkEntry(c, i), ChunkHeader(c, i));
            PushBack(p, ChunkEntry(c, i));
        }
    }
    g_refill = {};
    g_far = {};
}

void Forget() {
    g_pool.keys.clear();
    for (Slot& s : g_pool.slots) s.live = false;
}

// the texture set writes only the stock entries
void CopyTexture(uint32_t renderer) {
    uint32_t texture = R32(renderer + kEntries + kTexture);
    for (const Chunk& c : g_pool.chunks)
        for (uint32_t i = 0; i < kChunk; i++) W32(ChunkEntry(c, i) + kTexture, texture);
}

// applies what the hooks asked for since the last tile
void Sync(PPCContext& ctx, uint32_t renderer) {
    uint32_t attach = g_attach;
    uint32_t forget = g_forget;
    uint32_t texture = g_texture;
    if (g_pool.renderer != renderer || attach != g_seen_attach) {
        g_seen_attach = attach;
        g_seen_forget = forget;
        g_seen_texture = texture;
        Attach(ctx, renderer);
        return;
    }
    if (forget != g_seen_forget) {
        g_seen_forget = forget;
        Forget();
    }
    if (texture != g_seen_texture) {
        g_seen_texture = texture;
        CopyTexture(renderer);
    }
}

// the chunk's headers slice one vertex buffer, set up as the library left the buffer's own header
void AddChunk(const Chunk& c) {
    Pool& p = g_pool;
    uint32_t common = R32(c.buffer);
    uint32_t range = R32(c.buffer + kLockRange);
    uint32_t data = R32(c.buffer + kData) & ~3u;
    uint32_t flags = R32(c.buffer + kFetch) & 0xFC000003u;
    for (uint32_t i = 0; i < kChunk; i++) {
        uint32_t header = ChunkHeader(c, i);
        W32(header, common);
        W32(header + kFence, 0);
        W32(header + kLockRange, range);
        W32(header + kData, (data + i * p.slice) | 3);
        W32(header + kFetch, flags | p.slice);
        Clone(p.renderer, ChunkEntry(c, i), header);
        PushBack(p, ChunkEntry(c, i));
    }
}

// adds kChunk entries at the tail
bool Grow(PPCContext& ctx) {
    Pool& p = g_pool;
    if (!p.slice || p.slots.size() + kChunk > kMaxEntries) return false;
    uint32_t buffer = uint32_t(CallGuest(ctx, SYM_D3DDevice_CreateVertexBuffer, {kChunk * p.slice, 0}));
    if (!buffer) return false;
    uint32_t block = mem::AllocSystem(kChunk * (kEntrySize + kHeaderSize));
    if (!block) {
        CallGuest(ctx, SYM_D3DResource_Release, {buffer});
        return false;
    }
    Chunk c{buffer, block};
    p.chunks.push_back(c);
    AddChunk(c);
    return true;
}

}  // namespace

namespace fcip::grass {

// a frame touches no more entries than it takes tiles, so Take leaves one free
Admission Admit(PPCContext& ctx, uint32_t renderer, uint32_t tiles, uint32_t params) {
    Sync(ctx, renderer);
    g_pending = {};
    if (tiles >= g_pool.slots.size() && !Grow(ctx)) return Admission::Full;
    float d2 = FarDistance(params);
    if (d2 == 0.0f) return Admission::Take;
    uint64_t key = BladeKey(renderer, params);
    if (!g_pool.keys.count(key) && !TakeFar(R32(renderer + kStamp), d2)) return Admission::Wait;
    g_pending = {params, key};
    return Admission::Take;
}

bool Lookup(PPCContext& ctx, uint32_t renderer, uint32_t params, uint32_t& desc) {
    Pool& p = g_pool;
    Sync(ctx, renderer);
    Pending pending = g_pending;
    g_pending = {};
    uint64_t key = pending.params == params ? pending.key : BladeKey(renderer, params);
    uint32_t stamp = R32(renderer + kStamp);
    auto it = p.keys.find(key);
    // an audited hit whose buffer no longer holds its blades is stale
    if (it != p.keys.end() && ShouldAudit(stamp)) {
        g_audit.done++;
        if (!Holds(renderer, params, p.slots[it->second].entry)) {
            p.slots[it->second].live = false;
            p.keys.erase(it);
            it = p.keys.end();
        }
    }
    bool hit = it != p.keys.end();
    // only if Admit was bypassed
    if (!hit && p.slots[p.tail].used && p.slots[p.tail].stamp == stamp) Grow(ctx);
    uint32_t i = hit ? it->second : p.tail;
    Slot& s = p.slots[i];
    if (!hit) {
        if (s.live) p.keys.erase(s.key);
        s.key = key;
        s.live = true;
        p.keys.emplace(key, i);
        W32(s.entry + kKey, R32(params + kParamKey));
    }
    g_refill = hit ? Refill{} : Refill{i, key};
    if (i != p.head) {
        Unlink(p, i);
        PushFront(p, i);
    }
    s.stamp = stamp;
    s.used = true;
    W32(s.entry + kUsed, stamp);
    desc = s.entry + kDesc;
    return hit;
}

// changed since the lookup, the copy may hold neither version: the entry still draws this tile but no other tile
// finds it
void Refilled(uint32_t renderer, uint32_t params) {
    Pool& p = g_pool;
    Refill r = g_refill;
    g_refill = {};
    if (r.slot >= p.slots.size() || p.renderer != renderer) return;
    if (BladeKey(renderer, params) == r.key) return;
    Slot& s = p.slots[r.slot];
    if (s.live && s.key == r.key) {
        p.keys.erase(s.key);
        s.live = false;
    }
}

}  // namespace fcip::grass

PPC_EXTERN_FUNC(GUEST_ORIG(Grass_Init));
PPC_EXTERN_FUNC(GUEST_ORIG(Grass_Release));
PPC_EXTERN_FUNC(GUEST_ORIG(Grass_Reset));
PPC_EXTERN_FUNC(GUEST_ORIG(Grass_SetTexture));

// level load runs texture set, init, then the blade rebuild ending in reset

// init, vtable +0x8C, rebuilds every stock entry
PPC_FUNC(GUEST(Grass_Init)) {
    GUEST_ORIG(Grass_Init)(ctx, base);
    g_attach++;
}

// release, +0x94, empties the stock entries; the next tile attaches again
PPC_FUNC(GUEST(Grass_Release)) {
    GUEST_ORIG(Grass_Release)(ctx, base);
    g_attach++;
}

// cache reset, +0x9C: every tile refills
PPC_FUNC(GUEST(Grass_Reset)) {
    GUEST_ORIG(Grass_Reset)(ctx, base);
    g_forget++;
}

// texture set, +0xA0
PPC_FUNC(GUEST(Grass_SetTexture)) {
    GUEST_ORIG(Grass_SetTexture)(ctx, base);
    g_texture++;
}
