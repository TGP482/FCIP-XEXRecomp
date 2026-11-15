// ige.xex: the terrain tool's per sector buffers follow the level's sector count. The map editor's templates have
// 4x4 sectors, so it sizes its height undo snapshot, its paint layer table at 0x8295B010 and its paint undo copies for
// 16 and indexes them by sector number; a campaign level has 16x16. Each count of 16 becomes the level's, the layer
// table moves to one sized for it ([[hook]] in config/leveleditor.toml), and the solo editor leaves a level with its
// undo stack, whose records were sized for that level.
#include <cstdint>
#include <cstring>

#include "editor/tools.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"
#include "leveleditor/guest.h"
#include "leveleditor/mode.h"
#include "xdk/hle.h"

using namespace kernel;

namespace {

constexpr uint32_t kLayerBytes = 0x441;  // 33x33 texture numbers a sector, 0xFF unpainted
// the paint undo record: vtable, then its copy of every layer
constexpr uint32_t kRecordCopies = 0x04;
// the guest allocator's tags as the paint undo frees (82111AD4)
constexpr uint32_t kTerrainHeap = 0xB03;
constexpr uint32_t kCopyFreeLine = 0xF5;
constexpr uint32_t kTableFreeLine = 0xF8;
constexpr uint32_t kDeletingDestructor = 0x00;

uint32_t g_layers = 0;  // the paint layer table
uint32_t g_capacity = 0;

uint32_t SectorCount() {
    uint32_t side = R32(SYM_Terrain_SectorsPerSide);
    return side * side;
}

// a table is set up (820D44C8) before any other use, so it grows only then
uint32_t PaintLayers() {
    uint32_t count = SectorCount();
    if (count > g_capacity) {
        g_layers = mem::AllocSystem(count * 4);
        g_capacity = g_layers ? count : 0;
        if (g_layers) std::memset(HostPtr(g_layers), 0, count * 4);
    }
    return g_layers;
}

uint32_t LayersEnd(uint64_t table) { return uint32_t(table) + SectorCount() * 4; }

}  // namespace

// the per sector tables' bytes, r6 of the allocation: height undo snapshot (820D2DB0), paint undo copies (820D22AC)
void SectorTableBytes(PPCContext& ctx, uint8_t*) { ctx.r[6] = SectorCount() * 4; }

// the loops over per sector buffers, r31 their count: height undo snapshot made (820D2DC8) and freed (82110E58,
// 82110ED4), paint undo copies freed (821119F0)
void SectorBufferCount(PPCContext& ctx, uint8_t*) { ctx.r[31] = SectorCount(); }

// the paint layer table where the guest forms 0x8295B010, by the register it lands in
void PaintLayersR8(PPCContext& ctx, uint8_t*) { ctx.r[8] = PaintLayers(); }
void PaintLayersR24(PPCContext& ctx, uint8_t*) { ctx.r[24] = PaintLayers(); }
void PaintLayersR26(PPCContext& ctx, uint8_t*) { ctx.r[26] = PaintLayers(); }
void PaintLayersR29(PPCContext& ctx, uint8_t*) { ctx.r[29] = PaintLayers(); }
void PaintLayersR30(PPCContext& ctx, uint8_t*) { ctx.r[30] = PaintLayers(); }

// the ends of the loops over the table, base + 0x40 before, by the register the end lands in
void PaintLayersEndR3(PPCContext& ctx, uint8_t*) { ctx.r[3] = LayersEnd(ctx.r[30]); }
void PaintLayersEndR4(PPCContext& ctx, uint8_t*) { ctx.r[4] = LayersEnd(ctx.r[29]); }
void PaintLayersEndR8(PPCContext& ctx, uint8_t*) { ctx.r[8] = LayersEnd(ctx.r[30]); }
void PaintLayersEndR10(PPCContext& ctx, uint8_t*) { ctx.r[10] = LayersEnd(ctx.r[30]); }
void PaintLayersEndR11(PPCContext& ctx, uint8_t*) { ctx.r[11] = LayersEnd(ctx.r[30]); }

// the terrain's height cap, f13 where the guest loads 225: set height target (820D2A9C, 820D2AF4), raise (820D3ADC)
void TerrainCeiling(PPCContext& ctx, uint8_t*) { ctx.f[13] = fcip::editor::MaxSetHeight(); }

PPC_EXTERN_FUNC(__imp__sub_82112FD8);

// the paint undo (r3 the record) as 82111A48 does it, for the level's count of layers: copied back, every sector's
// textures rebuilt, the copies freed; the guest's loop copies four layers a turn, which fits only 16
PPC_FUNC(sub_82111A48) {
    uint32_t record = uint32_t(ctx.r[3]);
    uint32_t copies = R32(record + kRecordCopies);
    uint32_t table = PaintLayers();
    uint32_t count = SectorCount();
    for (uint32_t i = 0; i < count; i++) {
        std::memcpy(HostPtr(R32(table + i * 4)), HostPtr(R32(copies + i * 4)), kLayerBytes);
    }
    CallGuest(ctx, SYM_TerrainPaint_RebuildAll, {});
    for (uint32_t i = 0; i < count; i++) {
        CallGuest(ctx, SYM_Memory_Free, {kTerrainHeap, kCopyFreeLine, R32(copies + i * 4)});
    }
    CallGuest(ctx, SYM_Memory_Free, {kTerrainHeap, kTableFreeLine, copies});
    W32(record + kRecordCopies, 0);
}

// the editor leaves a level (r3 the manager): the solo editor's undo records go with it, sized for its sectors
PPC_FUNC(sub_82112FD8) {
    if (fcip::editor::g_solo) {
        while (uint32_t record = uint32_t(CallGuest(ctx, SYM_Undo_Pop, {}))) {
            CallGuest(ctx, fcip::editor::Method(record, kDeletingDestructor), {record, 1});
        }
    }
    __imp__sub_82112FD8(ctx, base);
}
