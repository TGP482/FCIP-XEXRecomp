#include "editor/icons.h"

#include <cmath>
#include <initializer_list>

namespace fcip::editor {

namespace {

constexpr float kPi = 3.14159265f;

// an icon's drawing in its own 0 to 1 square
class Pen {
public:
    Pen(ImDrawList* draw, ImVec2 min, float size, ImU32 colour)
        : draw_(draw), min_(min), size_(size), colour_(colour), width_(std::fmax(1.0f, size * 0.08f)) {}

    ImVec2 At(float x, float y) const { return ImVec2(min_.x + x * size_, min_.y + y * size_); }
    void Line(float x0, float y0, float x1, float y1) const { draw_->AddLine(At(x0, y0), At(x1, y1), colour_, width_); }
    void Lines(std::initializer_list<ImVec2> points, bool closed = false) const {
        for (ImVec2 p : points) draw_->PathLineTo(At(p.x, p.y));
        draw_->PathStroke(colour_, closed ? ImDrawFlags_Closed : ImDrawFlags_None, width_);
    }
    void Fill(std::initializer_list<ImVec2> points) const {
        for (ImVec2 p : points) draw_->PathLineTo(At(p.x, p.y));
        draw_->PathFillConvex(colour_);
    }
    void Circle(float x, float y, float r) const { draw_->AddCircle(At(x, y), r * size_, colour_, 0, width_); }
    void Arc(float x, float y, float r, float from, float to) const {
        draw_->PathArcTo(At(x, y), r * size_, from, to);
        draw_->PathStroke(colour_, ImDrawFlags_None, width_);
    }
    // an arrow head at (x, y) pointing up (dir -1) or down (dir 1)
    void Head(float x, float y, float dir) const {
        float back = y - dir * 0.12f;
        Lines({{x - 0.12f, back}, {x, y}, {x + 0.12f, back}});
    }
    void Ground() const { Line(0.1f, 0.85f, 0.9f, 0.85f); }

private:
    ImDrawList* draw_;
    ImVec2 min_;
    float size_;
    ImU32 colour_;
    float width_;
};

}  // namespace

void DrawIcon(ImDrawList* draw, Icon icon, ImVec2 min, float size, ImU32 colour) {
    Pen p(draw, min, size, colour);
    switch (icon) {
        case Icon::Undo:
            p.Arc(0.55f, 0.55f, 0.28f, kPi, kPi * 2.5f);
            p.Head(0.27f, 0.62f, 1.0f);
            break;
        case Icon::Terrain:
            p.Lines({{0.08f, 0.82f}, {0.38f, 0.3f}, {0.55f, 0.55f}, {0.7f, 0.4f}, {0.92f, 0.82f}}, true);
            break;
        case Icon::AddObject:
            p.Lines({{0.15f, 0.4f}, {0.65f, 0.4f}, {0.65f, 0.88f}, {0.15f, 0.88f}}, true);
            p.Line(0.8f, 0.08f, 0.8f, 0.4f);
            p.Line(0.64f, 0.24f, 0.96f, 0.24f);
            break;
        case Icon::EditObject:
            p.Fill({{0.25f, 0.1f}, {0.25f, 0.8f}, {0.42f, 0.64f}, {0.75f, 0.6f}});
            p.Line(0.42f, 0.64f, 0.6f, 0.92f);
            break;
        case Icon::Environment:
            p.Circle(0.5f, 0.5f, 0.17f);
            for (int i = 0; i < 8; i++) {
                float a = float(i) * kPi / 4.0f;
                p.Line(0.5f + std::cos(a) * 0.28f, 0.5f + std::sin(a) * 0.28f, 0.5f + std::cos(a) * 0.42f,
                       0.5f + std::sin(a) * 0.42f);
            }
            break;
        case Icon::Raise:
            p.Ground();
            p.Line(0.5f, 0.72f, 0.5f, 0.15f);
            p.Head(0.5f, 0.15f, -1.0f);
            break;
        case Icon::Lower:
            p.Ground();
            p.Line(0.5f, 0.15f, 0.5f, 0.68f);
            p.Head(0.5f, 0.68f, 1.0f);
            break;
        case Icon::Bump:
            p.Lines({{0.08f, 0.8f}, {0.22f, 0.5f}, {0.36f, 0.8f}, {0.5f, 0.35f}, {0.64f, 0.8f}, {0.78f, 0.55f},
                     {0.92f, 0.8f}});
            break;
        case Icon::Dig:
            p.Lines({{0.08f, 0.4f}, {0.3f, 0.4f}, {0.42f, 0.82f}, {0.58f, 0.82f}, {0.7f, 0.4f}, {0.92f, 0.4f}});
            break;
        case Icon::SetHeight:
            p.Line(0.08f, 0.5f, 0.92f, 0.5f);
            p.Line(0.5f, 0.1f, 0.5f, 0.38f);
            p.Head(0.5f, 0.38f, 1.0f);
            p.Line(0.5f, 0.9f, 0.5f, 0.62f);
            p.Head(0.5f, 0.62f, -1.0f);
            break;
        case Icon::Extend:
            p.Lines({{0.08f, 0.85f}, {0.25f, 0.45f}, {0.6f, 0.45f}});
            p.Line(0.6f, 0.45f, 0.92f, 0.45f);
            p.Lines({{0.8f, 0.33f}, {0.92f, 0.45f}, {0.8f, 0.57f}});
            break;
        case Icon::Smooth:
            p.Arc(0.5f, 1.1f, 0.6f, kPi * 1.2f, kPi * 1.8f);
            p.Ground();
            break;
        case Icon::Trim:
            p.Lines({{0.08f, 0.85f}, {0.25f, 0.45f}, {0.75f, 0.45f}, {0.92f, 0.85f}});
            p.Line(0.25f, 0.2f, 0.35f, 0.2f);
            p.Line(0.45f, 0.2f, 0.55f, 0.2f);
            p.Line(0.65f, 0.2f, 0.75f, 0.2f);
            break;
        case Icon::Paint:
            p.Line(0.85f, 0.12f, 0.45f, 0.52f);
            p.Fill({{0.45f, 0.42f}, {0.58f, 0.55f}, {0.3f, 0.88f}, {0.12f, 0.88f}});
            break;
        case Icon::Erase:
            p.Lines({{0.15f, 0.62f}, {0.5f, 0.27f}, {0.85f, 0.62f}, {0.62f, 0.85f}, {0.38f, 0.85f}}, true);
            p.Line(0.32f, 0.45f, 0.67f, 0.8f);
            break;
    }
}

}  // namespace fcip::editor
