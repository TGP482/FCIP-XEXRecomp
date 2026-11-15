// the level editor's view: what the editor draws over the frame is projected with the camera the last frame drew.
// Lines are cut where they pass behind the camera, so a box the camera stands in keeps its far side.
#include "leveleditor/view_draw.h"

#include <algorithm>
#include <cmath>
#include <initializer_list>

#include "common/view_clip.h"

namespace fcip::editor {

namespace {

constexpr float kNearW = 0.01f;  // clip space w at the camera's near side
constexpr float kLineWidth = 2.0f;
constexpr float kSquareHalf = 6.0f;  // pixels
constexpr float kHeadPixels = 10.0f;
constexpr float kHeadSpread = 0.45f;  // radians either side of the shaft
constexpr float kRadians = 3.14159265f / 180.0f;
constexpr float kNamedWithin = 120.0f;  // metres
constexpr int kCircleSegments = 24;
constexpr float kTau = 6.28318531f;

struct Clip {
    float x;
    float y;
    float w;
};

Clip ToClip(const View& v, const float p[3]) {
    const float* c = v.clip;
    return {p[0] * c[0] + p[1] * c[4] + p[2] * c[8] + c[12], p[0] * c[1] + p[1] * c[5] + p[2] * c[9] + c[13],
            p[0] * c[3] + p[1] * c[7] + p[2] * c[11] + c[15]};
}

ImVec2 ToScreen(const View& v, const Clip& c) {
    float x = (c.x / c.w * 0.5f + 0.5f) * v.frame.w;
    float y = (0.5f - c.y / c.w * 0.5f) * v.frame.h;
    return ImVec2(v.frame.x + x, v.frame.y + y);
}

// where the line from inside to outside meets the near side
Clip Cut(const Clip& inside, const Clip& outside) {
    float t = (inside.w - kNearW) / (inside.w - outside.w);
    return {inside.x + (outside.x - inside.x) * t, inside.y + (outside.y - inside.y) * t, kNearW};
}

// false when the whole line is behind the camera
bool ScreenLine(const View& v, const float a[3], const float b[3], ImVec2& sa, ImVec2& sb) {
    Clip ca = ToClip(v, a);
    Clip cb = ToClip(v, b);
    if (ca.w < kNearW && cb.w < kNearW) return false;
    if (ca.w < kNearW) ca = Cut(cb, ca);
    if (cb.w < kNearW) cb = Cut(ca, cb);
    sa = ToScreen(v, ca);
    sb = ToScreen(v, cb);
    return true;
}

}  // namespace

bool BeginView(View& v) {
    if (!ViewClip(v.clip)) return false;
    v.has_eye = ViewPosition(v.eye);
    ImVec2 size = ImGui::GetIO().DisplaySize;
    v.frame = FrameBox(size.x, size.y);
    v.draw = ImGui::GetBackgroundDrawList();
    v.draw->PushClipRect(ImVec2(v.frame.x, v.frame.y), ImVec2(v.frame.x + v.frame.w, v.frame.y + v.frame.h));
    return true;
}

void EndView(const View& v) { v.draw->PopClipRect(); }

bool Project(const View& v, const float p[3], ImVec2& out) {
    Clip c = ToClip(v, p);
    if (c.w < kNearW) return false;
    out = ToScreen(v, c);
    return true;
}

void Line(const View& v, const float a[3], const float b[3], ImU32 colour) {
    ImVec2 sa;
    ImVec2 sb;
    if (ScreenLine(v, a, b, sa, sb)) v.draw->AddLine(sa, sb, colour, kLineWidth);
}

void AxisBox(const View& v, const float min[3], const float max[3], ImU32 colour) {
    float corner[8][3];
    for (int i = 0; i < 8; i++) {
        corner[i][0] = (i & 1) ? max[0] : min[0];
        corner[i][1] = (i & 2) ? max[1] : min[1];
        corner[i][2] = (i & 4) ? max[2] : min[2];
    }
    BoxEdges(v, corner, colour);
}

void BoxEdges(const View& v, const float corner[8][3], ImU32 colour) {
    // corners one bit apart share an edge
    for (int i = 0; i < 8; i++) {
        for (int bit = 1; bit < 8; bit <<= 1) {
            if (!(i & bit)) Line(v, corner[i], corner[i | bit], colour);
        }
    }
}

void Arrow(const View& v, const float from[3], const float to[3], ImU32 colour) {
    ImVec2 a;
    ImVec2 b;
    if (!ScreenLine(v, from, to, a, b)) return;
    v.draw->AddLine(a, b, colour, kLineWidth);
    float dx = a.x - b.x;
    float dy = a.y - b.y;
    float length = std::sqrt(dx * dx + dy * dy);
    if (length < 1.0f) return;
    float angle = std::atan2(dy, dx);
    for (float side : {-kHeadSpread, kHeadSpread}) {
        ImVec2 head(b.x + std::cos(angle + side) * kHeadPixels, b.y + std::sin(angle + side) * kHeadPixels);
        v.draw->AddLine(b, head, colour, kLineWidth);
    }
}

void Square(const View& v, const float p[3], ImU32 colour) {
    ImVec2 at;
    if (!Project(v, p, at)) return;
    ImVec2 min(at.x - kSquareHalf, at.y - kSquareHalf);
    ImVec2 max(at.x + kSquareHalf, at.y + kSquareHalf);
    v.draw->AddRect(min, max, colour, 0.0f, 0, kLineWidth);
}

void Circle(const View& v, const float centre[3], float radius, ImU32 colour) {
    float last[3] = {centre[0] + radius, centre[1], centre[2]};
    for (int s = 1; s <= kCircleSegments; s++) {
        float angle = kTau * float(s) / float(kCircleSegments);
        float next[3] = {centre[0] + std::cos(angle) * radius, centre[1] + std::sin(angle) * radius, centre[2]};
        Line(v, last, next, colour);
        std::copy(next, next + 3, last);
    }
}

void Prism(const View& v, const std::vector<std::array<float, 3>>& points, float height, bool closed, ImU32 colour) {
    size_t n = points.size();
    for (size_t i = 0; i < n; i++) {
        const float* a = points[i].data();
        float a_top[3] = {a[0], a[1], a[2] + height};
        if (height > 0.0f) Line(v, a, a_top, colour);
        if (i + 1 == n && !closed) break;
        const float* b = points[(i + 1) % n].data();
        float b_top[3] = {b[0], b[1], b[2] + height};
        Line(v, a, b, colour);
        if (height > 0.0f) Line(v, a_top, b_top, colour);
    }
}

// shadowed, so it reads over sky and sand alike
void Label(const View& v, const float p[3], const char* text, ImU32 colour) {
    ImVec2 at;
    if (!Project(v, p, at)) return;
    ImVec2 pos(at.x + kSquareHalf * 1.5f, at.y - kSquareHalf);
    v.draw->AddText(ImVec2(pos.x + 1.0f, pos.y + 1.0f), IM_COL32(0, 0, 0, 200), text);
    v.draw->AddText(pos, colour, text);
}

bool NearCamera(const View& v, const float p[3]) {
    float dx = p[0] - v.eye[0];
    float dy = p[1] - v.eye[1];
    float dz = p[2] - v.eye[2];
    return v.has_eye && dx * dx + dy * dy + dz * dz < kNamedWithin * kNamedWithin;
}

void AxesFromAngles(const float degrees[3], float axes[3][3]) {
    float sx = std::sin(degrees[0] * kRadians);
    float cx = std::cos(degrees[0] * kRadians);
    float sy = std::sin(degrees[1] * kRadians);
    float cy = std::cos(degrees[1] * kRadians);
    float sz = std::sin(degrees[2] * kRadians);
    float cz = std::cos(degrees[2] * kRadians);
    float x[3] = {cz * cy, sz * cy, -sy};
    float y[3] = {cz * sy * sx - sz * cx, sz * sy * sx + cz * cx, cy * sx};
    float z[3] = {sz * sx + cz * sy * cx, sz * sy * cx - cz * sx, cy * cx};
    for (int i = 0; i < 3; i++) {
        axes[0][i] = x[i];
        axes[1][i] = y[i];
        axes[2][i] = z[i];
    }
}

}  // namespace fcip::editor
