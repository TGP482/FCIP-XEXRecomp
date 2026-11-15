// ige.xex, the solo level editor: a mission area drawn as the engine tests a point against it (default.xex 821538B0).
// A Shape is its points' outline, from the lowest point up Height, or at any height when Height is 0; an AreaBox is
// Width by Length centred on Pos and Height up from it, turned by Angles; an AreaSphere is Radius around Pos. An area
// naming entities in its Entities tells them of the player entering (OnEnterArea).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <unordered_map>

#include "leveleditor/markers.h"

using namespace fcip::editor;

namespace {

constexpr int kCircleSegments = 24;
constexpr size_t kMinShapePoints = 3;  // the engine makes no area under this
constexpr float kTau = 6.28318531f;

float FloatOf(const AiObject& o, const char* key) { return std::strtof(AttributeOf(o, key).c_str(), nullptr); }

void DrawShape(const View& v, const AiObject& o, ImU32 colour) {
    if (o.points.size() < kMinShapePoints) return;
    float bottom = o.points.front()[2];
    for (const auto& p : o.points) bottom = std::min(bottom, p[2]);
    float height = FloatOf(o, "Height");
    size_t n = o.points.size();
    for (size_t i = 0; i < n; i++) {
        const auto& a = o.points[i];
        const auto& b = o.points[(i + 1) % n];
        float a0[3] = {a[0], a[1], bottom};
        float b0[3] = {b[0], b[1], bottom};
        Line(v, a0, b0, colour);
        if (height <= 0.0f) continue;
        float a1[3] = {a[0], a[1], bottom + height};
        float b1[3] = {b[0], b[1], bottom + height};
        Line(v, a1, b1, colour);
        Line(v, a0, a1, colour);
    }
}

void DrawAreaBox(const View& v, const AiObject& o, ImU32 colour) {
    float angles[3] = {};
    std::string text = AttributeOf(o, "Angles");
    std::sscanf(text.c_str(), "%f,%f,%f", &angles[0], &angles[1], &angles[2]);
    float axes[3][3];
    AxesFromAngles(angles, axes);
    float half_width = FloatOf(o, "Width") * 0.5f;
    float half_length = FloatOf(o, "Length") * 0.5f;
    float height = FloatOf(o, "Height");
    float corner[8][3];
    for (int i = 0; i < 8; i++) {
        float local[3] = {(i & 1) ? half_width : -half_width, (i & 2) ? half_length : -half_length,
                          (i & 4) ? height : 0.0f};
        for (int k = 0; k < 3; k++) {
            corner[i][k] = o.pos[k] + axes[0][k] * local[0] + axes[1][k] * local[1] + axes[2][k] * local[2];
        }
    }
    BoxEdges(v, corner, colour);
}

// a circle each in the xy, xz and yz planes
void DrawAreaSphere(const View& v, const AiObject& o, ImU32 colour) {
    float radius = FloatOf(o, "Radius");
    for (int plane = 0; plane < 3; plane++) {
        int u = plane == 2 ? 1 : 0;
        int w = plane == 0 ? 1 : 2;
        float last[3] = {o.pos[0], o.pos[1], o.pos[2]};
        last[u] += radius;
        for (int s = 1; s <= kCircleSegments; s++) {
            float angle = kTau * float(s) / float(kCircleSegments);
            float next[3] = {o.pos[0], o.pos[1], o.pos[2]};
            next[u] += std::cos(angle) * radius;
            next[w] += std::sin(angle) * radius;
            Line(v, last, next, colour);
            std::copy(next, next + 3, last);
        }
    }
}

// window thread: the areas by the entity they name, made again when the AI objects change
struct AreaIndex {
    std::shared_ptr<const AiObjects> source;
    std::unordered_multimap<std::string, const AiObject*> by_entity;
};
AreaIndex g_index;

const AreaIndex& Index() {
    std::shared_ptr<const AiObjects> ai = ReadAiObjects();
    if (ai == g_index.source) return g_index;
    g_index.source = ai;
    g_index.by_entity.clear();
    for (const AiObject& o : ai->objects) {
        for (const std::string& name : o.entities) g_index.by_entity.emplace(name, &o);
    }
    return g_index;
}

}  // namespace

void fcip::editor::DrawEntityAreas(const View& v, const std::string& name, const float pos[3], ImU32 colour) {
    auto [first, last] = Index().by_entity.equal_range(name);
    for (auto it = first; it != last; ++it) {
        const AiObject& o = *it->second;
        DrawArea(v, o, colour);
        const float* at = o.placed ? o.pos : (o.points.empty() ? nullptr : o.points.front().data());
        if (at) Line(v, at, pos, colour);
    }
}

void fcip::editor::DrawArea(const View& v, const AiObject& area, ImU32 colour) {
    if (area.type == "Shape") DrawShape(v, area, colour);
    else if (area.type == "AreaBox" && area.placed) DrawAreaBox(v, area, colour);
    else if (area.type == "AreaSphere" && area.placed) DrawAreaSphere(v, area, colour);
}
