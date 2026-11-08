// the F10 panel's colours, the game's menu red
#pragma once
#include <utility>

#include <imgui.h>

namespace fcip {

inline constexpr ImVec4 kPanelBg{0.05f, 0.03f, 0.03f, 0.94f};
inline constexpr ImVec4 kBorder{0.88f, 0.84f, 0.76f, 0.80f};
inline constexpr ImVec4 kText{0.93f, 0.91f, 0.87f, 1.0f};
inline constexpr ImVec4 kHighlight{0.55f, 0.12f, 0.12f, 1.0f};
inline constexpr ImVec4 kHeading{0.82f, 0.28f, 0.22f, 1.0f};  // above the highlight red so it stays legible on it
inline constexpr ImVec4 kDim{0.62f, 0.59f, 0.55f, 1.0f};
inline constexpr ImVec4 kTab{0.16f, 0.06f, 0.06f, 1.0f};
inline constexpr ImVec4 kTabHovered{0.36f, 0.10f, 0.10f, 1.0f};
inline constexpr ImVec4 kButton{0.22f, 0.08f, 0.08f, 1.0f};
inline constexpr ImVec4 kButtonHovered{0.70f, 0.18f, 0.16f, 1.0f};  // lighter than the highlight, so it shows on a selected row
inline constexpr ImVec4 kLine{0.82f, 0.28f, 0.22f, 0.35f};

inline constexpr std::pair<ImGuiCol, ImVec4> kPanelColors[] = {
    {ImGuiCol_WindowBg, kPanelBg}, {ImGuiCol_Border, kBorder}, {ImGuiCol_Text, kText},
    {ImGuiCol_Header, kHighlight}, {ImGuiCol_HeaderHovered, kHighlight}, {ImGuiCol_HeaderActive, kHighlight},
    {ImGuiCol_Button, kButton}, {ImGuiCol_ButtonHovered, kButtonHovered}, {ImGuiCol_ButtonActive, kButtonHovered},
    {ImGuiCol_FrameBg, kTab}, {ImGuiCol_FrameBgHovered, kTabHovered}, {ImGuiCol_FrameBgActive, kHighlight},
    {ImGuiCol_ScrollbarGrab, kHighlight}, {ImGuiCol_ScrollbarGrabHovered, kButtonHovered},
    {ImGuiCol_ScrollbarGrabActive, kHeading}, {ImGuiCol_Separator, kLine}, {ImGuiCol_NavHighlight, kHeading},
    {ImGuiCol_TextSelectedBg, kButtonHovered}, {ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0)},
};

}  // namespace fcip
