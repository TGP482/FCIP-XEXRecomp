// The volumetric fog's terrain shade. The terrain draws into no shadow map, so the fog reads its heights instead.
// C3DEngine +0x364 is the terrain: its sectors, 0x160 bytes each from +0x80, Terrain_SectorsPerSide to a row, cover 64
// units each (820B0A80). A loaded sector (+0x149 bit 3) keeps 65 x 65 vertices of 16 bytes at its data (+0x7C) +0x30,
// a u16 height in 128ths at +2 (82607E78). The shade covers the sectors around the camera's: for each unit the height
// over the sea below which the terrain hides the sun, swept a line at a time from the sun's side. It is built again
// when the camera enters another sector, a sector streams in or out, or the sun moves.
#include "game/terrain_shade.h"

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

#include "common/render_pass.h"
#include "kernel/memory.h"
#include "xdk/hle.h"

using namespace kernel;

namespace {

// C3DEngine's terrain and its sectors
constexpr uint32_t kEngineTerrain = 0x364;
constexpr uint32_t kTerrainSectors = 0x80;
constexpr uint32_t kSectorSize = 0x160;
constexpr uint32_t kSectorData = 0x7C;
constexpr uint32_t kSectorFlags = 0x149;
constexpr uint8_t kSectorLoaded = 8;
// sector data: 65 x 65 vertices one unit apart
constexpr uint32_t kDataVertices = 0x30;
constexpr uint32_t kVertexSize = 0x10;
constexpr uint32_t kVertexHeight = 2;
constexpr float kHeightUnit = 1.0f / 128;
constexpr int kSectorUnits = 64;
constexpr int kSectorVertices = kSectorUnits + 1;
// the shade: sectors either side of the camera's, a texel a unit
constexpr int kAround = 8;
constexpr int kSectors = 2 * kAround;
constexpr int kSide = kSectors * kSectorUnits + 1;
constexpr float kNoGround = -1e9f;
constexpr float kSunMoved = 1e-4f;

// what the shade was built from
struct Basis {
    int x = 0;  // its first sector
    int y = 0;
    uint64_t sectors = 0;  // the loaded sectors' vertices, hashed
    float sea = 0;
    float sun[3] = {};  // toward the sun
    bool built = false;
};

Basis g_basis;

bool Same(const Basis& a, const Basis& b) {
    bool sun = true;
    for (int i = 0; i < 3; i++) sun = sun && std::fabs(a.sun[i] - b.sun[i]) < kSunMoved;
    return a.built && b.built && sun && a.x == b.x && a.y == b.y && a.sectors == b.sectors && a.sea == b.sea;
}

// the terrain's sector at column x, row y, 0 off the terrain
uint32_t Sector(uint32_t terrain, int x, int y) {
    int per = int(R32(SYM_Terrain_SectorsPerSide));
    if (x < 0 || y < 0 || x >= per || y >= per) return 0;
    return R32(terrain + kTerrainSectors) + uint32_t(y * per + x) * kSectorSize;
}

// a loaded sector's vertices, 0 for none
uint32_t Vertices(uint32_t sector) {
    if (!sector || !(R8(sector + kSectorFlags) & kSectorLoaded)) return 0;
    uint32_t data = R32(sector + kSectorData);
    return data ? R32(data + kDataVertices) : 0;
}

uint64_t LoadedSectors(uint32_t terrain, int x0, int y0) {
    uint64_t hash = 14695981039346656037ull;
    for (int y = 0; y < kSectors; y++) {
        for (int x = 0; x < kSectors; x++) hash = (hash ^ Vertices(Sector(terrain, x0 + x, y0 + y))) * 1099511628211ull;
    }
    return hash;
}

// heights over the sea, kNoGround where no sector is loaded
std::vector<float> Heights(uint32_t terrain, int x0, int y0, float sea) {
    std::vector<float> heights(size_t(kSide) * kSide, kNoGround);
    for (int sy = 0; sy < kSectors; sy++) {
        for (int sx = 0; sx < kSectors; sx++) {
            uint32_t vertices = Vertices(Sector(terrain, x0 + sx, y0 + sy));
            if (!vertices) continue;
            for (int y = 0; y < kSectorVertices; y++) {
                float* row = &heights[size_t(sy * kSectorUnits + y) * kSide + sx * kSectorUnits];
                for (int x = 0; x < kSectorVertices; x++) {
                    uint32_t at = vertices + uint32_t(y * kSectorVertices + x) * kVertexSize + kVertexHeight;
                    row[x] = R16(at) * kHeightUnit - sea;
                }
            }
        }
    }
    return heights;
}

// each texel raised to the sun's ray over the line before it on the sun's side
void Sweep(std::vector<float>& shade, const float sun[3]) {
    if (sun[2] <= 0 || (sun[0] == 0 && sun[1] == 0)) return;
    bool along_x = std::fabs(sun[0]) >= std::fabs(sun[1]);
    float main = along_x ? sun[0] : sun[1];
    float slide = (along_x ? sun[1] : sun[0]) / std::fabs(main);  // texels across per line toward the sun
    float climb = sun[2] / std::fabs(main);                        // the ray's rise per line
    int toward = main > 0 ? 1 : -1;
    auto at = [&](int a, int b) -> float& {
        return along_x ? shade[size_t(b) * kSide + a] : shade[size_t(a) * kSide + b];
    };
    for (int a = toward > 0 ? kSide - 2 : 1; a >= 0 && a < kSide; a -= toward) {
        for (int b = 0; b < kSide; b++) {
            float across = b + slide;
            int lo = int(std::floor(across));
            if (lo < 0 || lo >= kSide) continue;
            int hi = std::min(lo + 1, kSide - 1);
            float f = across - lo;
            float ray = at(a + toward, lo) * (1 - f) + at(a + toward, hi) * f - climb;
            at(a, b) = std::max(at(a, b), ray);
        }
    }
}

}  // namespace

void fcip::UpdateTerrainShade(const float world[16], float sea, gpu::FogView& view) {
    uint32_t engine = R32(SYM_C3DEngine_pointer);
    uint32_t terrain = engine ? R32(engine + kEngineTerrain) : 0;
    float clip[16];
    if (!terrain || !SunClip(clip)) return;
    // the sun map's depth grows toward the sun
    float length = std::sqrt(clip[2] * clip[2] + clip[6] * clip[6] + clip[10] * clip[10]);
    if (!(length > 0)) return;
    Basis now;
    now.x = int(std::floor(world[12] / kSectorUnits)) - kAround;
    now.y = int(std::floor(world[13] / kSectorUnits)) - kAround;
    now.sectors = LoadedSectors(terrain, now.x, now.y);
    now.sea = sea;
    for (int i = 0; i < 3; i++) now.sun[i] = clip[i * 4 + 2] / length;
    now.built = true;
    if (!Same(now, g_basis)) {
        gpu::TerrainShade shade{kSide, Heights(terrain, now.x, now.y, sea)};
        Sweep(shade.heights, now.sun);
        d3d::PublishTerrainShade(std::move(shade));
        g_basis = now;
    }
    for (int r = 0; r < 4; r++) {
        view.terrain[r] = world[r * 4];
        view.terrain[4 + r] = world[r * 4 + 1];
    }
    view.terrain[3] -= float(g_basis.x * kSectorUnits);
    view.terrain[7] -= float(g_basis.y * kSectorUnits);
    view.shaded = true;
}
