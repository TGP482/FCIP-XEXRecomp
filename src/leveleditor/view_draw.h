// the level editor's view: world space lines, boxes and labels over the frame (view_draw.cpp)
#pragma once
#include <imgui.h>

#include "common/aspect.h"

namespace fcip::editor {

// window thread, while the ImGui frame is built
struct View {
    float clip[16] = {};
    fcip::Box frame;
    ImDrawList* draw = nullptr;
    bool has_eye = false;
    float eye[3] = {};  // the camera
};

bool BeginView(View& v);  // false before the camera drew a frame
void EndView(const View& v);

bool Project(const View& v, const float p[3], ImVec2& out);  // false behind the camera
void Line(const View& v, const float a[3], const float b[3], ImU32 colour);
void AxisBox(const View& v, const float min[3], const float max[3], ImU32 colour);
void BoxEdges(const View& v, const float corner[8][3], ImU32 colour);  // corner bits 1, 2, 4 are its x, y, z sides
void Arrow(const View& v, const float from[3], const float to[3], ImU32 colour);
void Square(const View& v, const float p[3], ImU32 colour);
void Label(const View& v, const float p[3], const char* text, ImU32 colour);
bool NearCamera(const View& v, const float p[3]);  // close enough to be named

// the rotation the engine builds from an entity's angles in degrees (820BFE38): axes[i] is its local axis i
void AxesFromAngles(const float degrees[3], float axes[3][3]);

}  // namespace fcip::editor
