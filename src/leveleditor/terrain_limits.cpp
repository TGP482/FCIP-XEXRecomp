// ige.xex: the terrain tool's per sector buffers follow the level's sector count. The map editor's templates have
// 4x4 sectors, so it sizes its height undo snapshot, its paint layer table at 0x8295B010 and its paint undo copies for
// 16 and indexes them by sector number; a campaign level has 16x16. Each count of 16 becomes the level's, the layer
// table moves to one sized for it ([[hook]] in config/leveleditor.toml), and the solo editor leaves a level with its
// undo stack, whose records were sized for that level. A campaign level's sectors keep their own colour maps and
// surface types: a terrain edit rebuilds a sector from the editor's three map textures by height (820D5A08, 820D5EF8),
// which a campaign level has none of, so only its painted texels change their surface type. Paint lays any of the
// level's own surface types (LevelData SurfaceTypes) in place of the map editor's sand, grass and cliff.
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "common/widgets.h"
#include "editor/tools.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"
#include "leveleditor/campaign_levels.h"
#include "leveleditor/guest.h"
#include "leveleditor/mode.h"
#include "xdk/hle.h"

using namespace kernel;

namespace {

constexpr uint32_t kLayerBytes = 0x441;  // 33x33 texture numbers a sector, 0xFF unpainted
constexpr uint32_t kLayerSide = 33;
constexpr uint8_t kUnpainted = 0xFF;
// a terrain sector: its number and its render data
constexpr uint32_t kSectorNumber = 0x78;  // int16
constexpr uint32_t kSectorRender = 0x7C;
// the paint undo record: vtable, then its copy of every layer
constexpr uint32_t kRecordCopies = 0x04;
// the guest allocator's tags as the paint undo frees (82111AD4)
constexpr uint32_t kTerrainHeap = 0xB03;
constexpr uint32_t kCopyFreeLine = 0xF5;
constexpr uint32_t kTableFreeLine = 0xF8;
constexpr uint32_t kDeletingDestructor = 0x00;
// the terrain tool: the surface type Paint lays, and the widget its caption is on
constexpr uint32_t kPaintTexture = 0x12C;
constexpr uint32_t kTextureWidget = 0x82067C70;  // "B_xBoxBtn_Power"

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

bool CampaignTerrain() { return fcip::editor::g_solo && !fcip::editor::CampaignLevel().empty(); }

// 820D5EF8 for a campaign level: each painted texel's number becomes its surface type, the rest keep theirs
void PaintSurfaceTypes(PPCContext& ctx, uint32_t sector) {
    int16_t number = int16_t(R16(sector + kSectorNumber));
    uint32_t render = R32(sector + kSectorRender);
    uint32_t layer = R32(PaintLayers() + uint32_t(number) * 4);
    for (uint32_t row = 0; row < kLayerSide; row++) {
        for (uint32_t column = 0; column < kLayerSide; column++) {
            uint8_t type = R8(layer + row * kLayerSide + column);
            if (type == kUnpainted) continue;
            uint32_t cell = uint32_t(CallGuest(ctx, SYM_Terrain_Cell, {render, row, column}));
            CallGuest(ctx, SYM_Terrain_SetSurfaceType, {cell, type});
        }
    }
}

}  // namespace

// the paint layers in sector order, as textureinfo.ige holds them
std::vector<uint8_t> fcip::editor::PaintLayerBytes() {
    uint32_t table = PaintLayers();
    uint32_t count = SectorCount();
    std::vector<uint8_t> bytes(size_t(count) * kLayerBytes, kUnpainted);
    for (uint32_t i = 0; table && i < count; i++) {
        uint32_t layer = R32(table + i * 4);
        if (layer) std::memcpy(&bytes[size_t(i) * kLayerBytes], HostPtr(layer), kLayerBytes);
    }
    return bytes;
}

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

// the number Paint lays, r25 from the map editor's table of three (820D4CB0); a campaign level's is r4, its surface type
void PaintSurfaceNumber(PPCContext& ctx, uint8_t*) {
    if (CampaignTerrain()) ctx.r[25] = ctx.r[4];
}

// the pad's next texture stops at the last (820D28FC compares r11 with 2); a campaign level's last surface type
void PaintTextureLast(PPCContext& ctx, uint8_t*) {
    if (!CampaignTerrain()) return;
    int64_t number = int32_t(ctx.r[11]);
    int64_t last = int64_t(fcip::editor::PaintTextures().size()) - 1;
    ctx.cr[6] = {number < last, number > last, number == last, ctx.cr[6].so};
}

// the terrain's height cap, f13 where the guest loads 225: set height target (820D2A9C, 820D2AF4), raise (820D3ADC)
void TerrainCeiling(PPCContext& ctx, uint8_t*) { ctx.f[13] = fcip::editor::MaxSetHeight(); }

PPC_EXTERN_FUNC(__imp__sub_82112FD8);
PPC_EXTERN_FUNC(__imp__sub_820D5EF8);
PPC_EXTERN_FUNC(__imp__sub_820D5A08);
PPC_EXTERN_FUNC(__imp__sub_820D18D0);

// the texture's caption (r3 the terrain tool), from the editor's string table; a campaign level's own name, as the
// table words them
PPC_FUNC(sub_820D18D0) {
    if (!CampaignTerrain()) return __imp__sub_820D18D0(ctx, base);
    uint32_t tool = uint32_t(ctx.r[3]);
    std::vector<std::string> textures = fcip::editor::PaintTextures();
    uint32_t number = R32(tool + kPaintTexture);
    std::string name = number < textures.size() ? textures[number] : std::string();
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return char(std::toupper(c)); });
    char caption[96];
    std::snprintf(caption, sizeof(caption), "{DPUD} %s (%u/%zu)", name.c_str(), number + 1, textures.size());
    if (uint32_t text = fcip::ui::View(caption)) CallGuest(ctx, SYM_Page_SetText, {tool, kTextureWidget, text});
}

// a sector's surface types from its paint layer (r3 the sector), unpainted texels by height
PPC_FUNC(sub_820D5EF8) {
    if (!CampaignTerrain()) return __imp__sub_820D5EF8(ctx, base);
    PaintSurfaceTypes(ctx, uint32_t(ctx.r[3]));
}

// a sector's colour map from the editor's three map textures (r3 the sector); a campaign level keeps its own
PPC_FUNC(sub_820D5A08) {
    if (!CampaignTerrain()) __imp__sub_820D5A08(ctx, base);
}

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
