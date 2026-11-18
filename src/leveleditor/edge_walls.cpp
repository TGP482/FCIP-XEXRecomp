// ige.xex, the solo level editor: edge walls. A sector's ShortRange%d.sdat ends with "FLAO", the 256 byte table of
// the sectors streamed from it, 33 bytes of fields, then its collision walls: a count and 32 bytes each, x1, y1, x2,
// y2, bottom, top, bottom, top, of which 823747E0 makes a physics box from bottom to top along the segment. The
// walls are read from every sector's file as the level opens, edited as one list here, and the save writes each
// sector's from the walls whose middle lies in it.
#include "leveleditor/edge_walls.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <imgui.h>

#include "common/panel_colors.h"
#include "editor/tools.h"
#include "kernel/memory.h"
#include "leveleditor/campaign_levels.h"
#include "leveleditor/fields.h"
#include "leveleditor/undo.h"
#include "leveleditor/view_draw.h"
#include "xdk/hle.h"

using namespace kernel;
using namespace fcip::editor;

namespace {

constexpr char kMark[] = "FLAO";
constexpr size_t kMarkBytes = 4;
constexpr size_t kStreamTableBytes = 0x100;
constexpr size_t kWallCount = 0x21;  // from the table's end
constexpr size_t kWallBytes = 32;
constexpr float kSectorMetres = 64.0f;
constexpr float kEdgeInset = 1.0f;     // the map edge's walls from its border, metres
constexpr float kNewWallLength = 10.0f;
constexpr float kNewWallBelow = 5.0f;  // a wall added at the cursor, from below it to above it
constexpr float kNewWallAbove = 50.0f;
constexpr float kDefaultBottom = -70.0f;  // 01_BEACH's edge walls, for a level with none
constexpr float kDefaultTop = 130.0f;
constexpr ImU32 kWallColour = IM_COL32(255, 170, 60, 200);

struct Wall {
    float x1 = 0;
    float y1 = 0;
    float x2 = 0;
    float y2 = 0;
    float bottom = 0;
    float top = 0;
};
using Walls = std::vector<Wall>;
using Edit = std::function<void(PPCContext&)>;

std::mutex g_lock;
Walls g_walls;
std::vector<Edit> g_edits;
// editor thread, until the level opens again: each save writes the sectors' walls as they loaded
bool g_edited = false;
// window thread
bool g_show = false;

// where a sector file's wall count sits, npos when it has none
size_t WallCountAt(const std::vector<uint8_t>& f) {
    auto mark = std::find_end(f.begin(), f.end(), kMark, kMark + kMarkBytes);
    if (mark == f.end()) return std::string::npos;
    size_t at = size_t(mark - f.begin()) + kMarkBytes + kStreamTableBytes + kWallCount;
    if (at + 4 > f.size()) return std::string::npos;
    uint32_t count = 0;
    std::memcpy(&count, f.data() + at, 4);
    return at + 4 + size_t(count) * kWallBytes <= f.size() ? at : std::string::npos;
}

void ReadWalls(const std::vector<uint8_t>& f, Walls& out) {
    size_t at = WallCountAt(f);
    if (at == std::string::npos) return;
    uint32_t count = 0;
    std::memcpy(&count, f.data() + at, 4);
    for (uint32_t i = 0; i < count; i++) {
        float v[8];
        std::memcpy(v, f.data() + at + 4 + i * kWallBytes, sizeof(v));
        out.push_back({v[0], v[1], v[2], v[3], v[4], v[5]});
    }
}

std::string SectorFile(uint32_t sector) {
    char name[48];
    std::snprintf(name, sizeof(name), "sdat\\ShortRange%u.sdat", sector);
    return name;
}

uint32_t SectorsPerSide() { return R32(SYM_Terrain_SectorsPerSide); }

uint32_t SectorOf(const Wall& w, uint32_t side) {
    int sx = std::clamp(int((w.x1 + w.x2) * 0.5f / kSectorMetres), 0, int(side) - 1);
    int sy = std::clamp(int((w.y1 + w.y2) * 0.5f / kSectorMetres), 0, int(side) - 1);
    return uint32_t(sy) * side + uint32_t(sx);
}

// the list replaced; one step of undo
void SetWalls(PPCContext& ctx, Walls walls, const char* label) {
    Walls old;
    {
        std::lock_guard lock(g_lock);
        old = std::exchange(g_walls, std::move(walls));
    }
    g_edited = true;
    NoteMapChanged();
    PushUndo(ctx, label, [old](PPCContext&) {
        std::lock_guard lock(g_lock);
        g_walls = old;
    });
}

// a wall along each side of every border sector, as a new level's are made
Walls MapEdgeWalls(float bottom, float top) {
    uint32_t side = SectorsPerSide();
    float far = float(side) * kSectorMetres - kEdgeInset;
    Walls out;
    for (uint32_t i = 0; i < side; i++) {
        float a = float(i) * kSectorMetres;
        float b = a + kSectorMetres;
        out.push_back({a, kEdgeInset, b, kEdgeInset, bottom, top});
        out.push_back({a, far, b, far, bottom, top});
        out.push_back({kEdgeInset, a, kEdgeInset, b, bottom, top});
        out.push_back({far, a, far, b, bottom, top});
    }
    return out;
}

Walls Snapshot() {
    std::lock_guard lock(g_lock);
    return g_walls;
}

void Queue(Edit edit) {
    std::lock_guard lock(g_lock);
    g_edits.push_back(std::move(edit));
}

bool EditPoint(const char* label, float& x, float& y, float& z) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(ImGui::GetFontSize() * 8);
    ImGui::SetNextItemWidth(-1);
    char text[96];
    std::snprintf(text, sizeof(text), "%g,%g,%g", x, y, z);
    std::string out;
    if (!DrawField(label, FieldKind::Vector, text, out)) return false;
    return std::sscanf(out.c_str(), "%f,%f,%f", &x, &y, &z) == 3;
}

}  // namespace

void fcip::editor::UpdateEdgeWalls(PPCContext& ctx, bool level_opened) {
    std::vector<Edit> edits;
    {
        std::lock_guard lock(g_lock);
        edits.swap(g_edits);
    }
    for (Edit& e : edits) e(ctx);
    if (!level_opened) return;
    Walls walls;
    uint32_t side = SectorsPerSide();
    for (uint32_t i = 0; i < side * side; i++) {
        std::vector<uint8_t> f;
        if (ReadPackFile(ctx, "levels\\" + StartedLevel() + "\\" + SectorFile(i), f)) ReadWalls(f, walls);
    }
    g_edited = false;
    std::lock_guard lock(g_lock);
    g_walls = std::move(walls);
}

void fcip::editor::WriteWallEdits(const std::filesystem::path& folder) {
    if (!g_edited) return;
    Walls walls = Snapshot();
    uint32_t side = SectorsPerSide();
    std::vector<std::vector<Wall>> by_sector(size_t(side) * side);
    for (const Wall& w : walls) by_sector[SectorOf(w, side)].push_back(w);
    for (uint32_t i = 0; i < by_sector.size(); i++) {
        std::filesystem::path path = folder / SectorFile(i);
        std::ifstream in(path, std::ios::binary);
        std::vector<uint8_t> f(std::istreambuf_iterator<char>(in), {});
        in.close();
        size_t at = WallCountAt(f);
        if (at == std::string::npos) continue;
        uint32_t old = 0;
        std::memcpy(&old, f.data() + at, 4);
        size_t rest = at + 4 + size_t(old) * kWallBytes;
        std::vector<uint8_t> out(f.begin(), f.begin() + at);
        uint32_t count = uint32_t(by_sector[i].size());
        out.insert(out.end(), reinterpret_cast<uint8_t*>(&count), reinterpret_cast<uint8_t*>(&count) + 4);
        for (const Wall& w : by_sector[i]) {
            float v[8] = {w.x1, w.y1, w.x2, w.y2, w.bottom, w.top, w.bottom, w.top};
            out.insert(out.end(), reinterpret_cast<uint8_t*>(v), reinterpret_cast<uint8_t*>(v) + sizeof(v));
        }
        out.insert(out.end(), f.begin() + rest, f.end());
        std::ofstream file(path, std::ios::binary);
        file.write(reinterpret_cast<const char*>(out.data()), std::streamsize(out.size()));
    }
}

void fcip::editor::DrawEdgeWalls(const View& v) {
    if (!g_show) return;
    for (const Wall& w : Snapshot()) {
        float a0[3] = {w.x1, w.y1, w.bottom};
        float b0[3] = {w.x2, w.y2, w.bottom};
        float a1[3] = {w.x1, w.y1, w.top};
        float b1[3] = {w.x2, w.y2, w.top};
        Line(v, a0, b0, kWallColour);
        Line(v, a1, b1, kWallColour);
        Line(v, a0, a1, kWallColour);
        Line(v, b0, b1, kWallColour);
    }
}

void fcip::editor::DrawEdgeWallsPanel() {
    Walls walls = Snapshot();
    ImGui::SeparatorText("Edge walls");
    ImGui::TextDisabled("Invisible collision walls, each from its bottom to its top along its line");
    ImGui::Text("%zu walls", walls.size());
    ImGui::Checkbox("Show edge walls", &g_show);
    if (ImGui::Button("Add a wall at the tool cursor")) {
        State tools = Read();
        Wall w{tools.cursor[0], tools.cursor[1], tools.cursor[0] + kNewWallLength, tools.cursor[1],
               tools.cursor[2] - kNewWallBelow, tools.cursor[2] + kNewWallAbove};
        Walls next = walls;
        next.push_back(w);
        Queue([next](PPCContext& ctx) { SetWalls(ctx, next, "Add an edge wall"); });
    }
    ImGui::SameLine();
    if (ImGui::Button("Walls along the map edge")) {
        float bottom = kDefaultBottom;
        float top = kDefaultTop;
        if (!walls.empty()) {
            bottom = std::min_element(walls.begin(), walls.end(), [](auto& a, auto& b) { return a.bottom < b.bottom; })
                         ->bottom;
            top = std::max_element(walls.begin(), walls.end(), [](auto& a, auto& b) { return a.top < b.top; })->top;
        }
        Walls next = MapEdgeWalls(bottom, top);
        Queue([next](PPCContext& ctx) { SetWalls(ctx, next, "Walls along the map edge"); });
    }
    if (!ImGui::CollapsingHeader("Walls")) return;
    for (size_t i = 0; i < walls.size(); i++) {
        ImGui::PushID(int(i));
        Wall w = walls[i];
        bool changed = EditPoint("From", w.x1, w.y1, w.bottom);
        changed = EditPoint("To", w.x2, w.y2, w.top) || changed;
        if (changed) {
            Walls next = walls;
            next[i] = w;
            Queue([next](PPCContext& ctx) { SetWalls(ctx, next, "Place an edge wall"); });
        }
        if (ImGui::SmallButton("Remove")) {
            Walls next = walls;
            next.erase(next.begin() + std::ptrdiff_t(i));
            Queue([next](PPCContext& ctx) { SetWalls(ctx, next, "Remove an edge wall"); });
        }
        ImGui::Separator();
        ImGui::PopID();
    }
}
