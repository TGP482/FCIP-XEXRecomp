// The achievements dialog, as Valve's games show theirs over the menu: the profile's campaign and multiplayer
// achievements on two tabs, a count of those earned, the earned ones first and the rest greyed out, each with its
// picture from the SPA, and a reset.
#include "common/achievements.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <iterator>
#include <map>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <imgui.h>

#include "common/panel_colors.h"
#include "d3d/ui_images.h"
#include "xam/xam.h"

namespace fcip::achievements {

namespace {

using Microsoft::WRL::ComPtr;

// FCIP's SPA numbers its multiplayer achievements 40 to 83, its campaign ones the rest
constexpr uint16_t kFirstMultiplayer = 40;
constexpr uint16_t kLastMultiplayer = 83;
const char* const kTabs[] = {"CAMPAIGN", "MULTIPLAYER"};

constexpr ImVec4 kRow{0.12f, 0.05f, 0.05f, 1.0f};
constexpr ImVec4 kLockedTint{0.35f, 0.35f, 0.35f, 1.0f};

std::atomic<bool> g_open{false};
std::atomic<bool> g_opened{false};  // focus the dialog on its first frame
int g_tab = 0;
bool g_confirm = false;
std::map<uint32_t, ImTextureID> g_icons;  // by SPA picture, null when it could not be made

bool Multiplayer(const xam::Achievement& a) { return a.id >= kFirstMultiplayer && a.id <= kLastMultiplayer; }

bool Decode(const std::vector<uint8_t>& png, std::vector<uint8_t>& rgba, UINT& w, UINT& h) {
    static const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    (void)com;
    ComPtr<IWICImagingFactory> wic;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> converter;
    if (png.empty()) return false;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
    if (SUCCEEDED(hr)) hr = wic->CreateStream(&stream);
    if (SUCCEEDED(hr)) hr = stream->InitializeFromMemory(const_cast<BYTE*>(png.data()), DWORD(png.size()));
    if (SUCCEEDED(hr)) hr = wic->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad, &decoder);
    if (SUCCEEDED(hr)) hr = decoder->GetFrame(0, &frame);
    if (SUCCEEDED(hr)) hr = wic->CreateFormatConverter(&converter);
    if (SUCCEEDED(hr)) {
        hr = converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0,
                                   WICBitmapPaletteTypeCustom);
    }
    if (SUCCEEDED(hr)) hr = converter->GetSize(&w, &h);
    if (FAILED(hr)) return false;
    rgba.resize(size_t(w) * h * 4);
    return SUCCEEDED(converter->CopyPixels(nullptr, w * 4, UINT(rgba.size()), rgba.data()));
}

ImTextureID Icon(uint32_t image) {
    auto it = g_icons.find(image);
    if (it != g_icons.end()) return it->second;
    std::vector<uint8_t> rgba;
    UINT w = 0;
    UINT h = 0;
    ImTextureID icon = Decode(xam::AchievementImage(image), rgba, w, h) ? gpu::UiImage(rgba.data(), w, h) : nullptr;
    g_icons[image] = icon;
    return icon;
}

// the tab's achievements, the earned ones first
std::vector<xam::Achievement> TabList() {
    std::vector<xam::Achievement> list = xam::Achievements();
    std::erase_if(list, [](const xam::Achievement& a) { return Multiplayer(a) != (g_tab == 1); });
    std::stable_partition(list.begin(), list.end(), [](const xam::Achievement& a) { return a.unlocked != 0; });
    return list;
}

// LB / RB as the game's menus, Left / Right on the keyboard
void DrawTabs() {
    int step = ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false) - ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false);
    step += ImGui::IsKeyPressed(ImGuiKey_RightArrow, false) - ImGui::IsKeyPressed(ImGuiKey_LeftArrow, false);
    bool set = step && !g_confirm;
    if (set) g_tab = std::clamp(g_tab + step, 0, int(std::size(kTabs)) - 1);
    ImGui::PushStyleColor(ImGuiCol_Tab, kTab);
    ImGui::PushStyleColor(ImGuiCol_TabHovered, kTabHovered);
    ImGui::PushStyleColor(ImGuiCol_TabSelected, kHighlight);
    ImGui::PushStyleColor(ImGuiCol_TabSelectedOverline, kHeading);
    ImGui::PushStyleVar(ImGuiStyleVar_TabRounding, 0.0f);
    if (ImGui::BeginTabBar("##tabs")) {
        int count = int(std::size(kTabs));
        float gaps = ImGui::GetStyle().ItemInnerSpacing.x * float(count - 1);
        float w = (ImGui::GetContentRegionAvail().x - gaps) / float(count);
        for (int i = 0; i < count; i++) {
            ImGui::SetNextItemWidth(w);
            if (ImGui::BeginTabItem(kTabs[i], nullptr, set && i == g_tab ? ImGuiTabItemFlags_SetSelected : 0)) {
                if (!set) g_tab = i;
                ImGui::EndTabItem();
            }
        }
        ImGui::EndTabBar();
    }
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(4);
}

void DrawCount(const std::vector<xam::Achievement>& list) {
    int earned = int(std::count_if(list.begin(), list.end(), [](const xam::Achievement& a) { return a.unlocked; }));
    int total = int(list.size());
    int percent = total ? earned * 100 / total : 0;
    char count[32];
    std::snprintf(count, sizeof(count), "%d / %d ( %d%% )", earned, total, percent);
    ImGui::TextUnformatted("ACHIEVEMENTS EARNED");
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(count).x + ImGui::GetCursorPosX());
    ImGui::TextUnformatted(count);
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, kHeading);
    ImGui::ProgressBar(total ? float(earned) / float(total) : 0.0f, ImVec2(-FLT_MIN, ImGui::GetFontSize() * 0.6f), "");
    ImGui::PopStyleColor();
}

// a selectable row so the pad and arrows scroll through the list, the picture and text drawn over it
void DrawRow(const xam::Achievement& a) {
    float em = ImGui::GetFontSize();
    float line = ImGui::GetTextLineHeight();
    float pad = em * 0.5f;
    float icon = em * 3.0f;
    float height = std::max(icon, line * 3) + pad * 2;
    ImVec2 min = ImGui::GetCursorScreenPos();
    float width = ImGui::GetContentRegionAvail().x;
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(min, ImVec2(min.x + width, min.y + height), ImGui::GetColorU32(kRow));
    ImGui::PushID(a.id);
    ImGui::Selectable("##row", false, 0, ImVec2(width, height));
    ImGui::PopID();
    bool earned = a.unlocked != 0;
    ImVec2 at(min.x + pad, min.y + (height - icon) * 0.5f);
    if (ImTextureID tex = Icon(a.image)) {
        ImU32 tint = ImGui::GetColorU32(earned ? ImVec4(1, 1, 1, 1) : kLockedTint);
        draw->AddImage(tex, at, ImVec2(at.x + icon, at.y + icon), ImVec2(0, 0), ImVec2(1, 1), tint);
    }
    float text_x = at.x + icon + pad;
    float wrap = min.x + width - pad - text_x;
    const std::string& about = earned ? a.description : a.locked_description;
    draw->AddText(ImVec2(text_x, min.y + pad), ImGui::GetColorU32(earned ? kText : kDim), a.name.c_str());
    draw->AddText(nullptr, 0.0f, ImVec2(text_x, min.y + pad + line), ImGui::GetColorU32(kDim), about.c_str(), nullptr,
                  wrap);
}

void ConfirmReset() {
    if (!g_confirm) return;
    constexpr ImGuiWindowFlags kFlags =
        ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove;
    ImGui::OpenPopup("##reset");
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal("##reset", nullptr, kFlags)) return;
    ImGui::TextUnformatted("Reset every achievement this profile has earned? This cannot be undone.");
    float bw = ImGui::GetFontSize() * 8;
    if (ImGui::Button("Yes", ImVec2(bw, 0))) {
        xam::ResetAchievements();
        g_confirm = false;
    }
    ImGui::SameLine();
    if (ImGui::Button("No", ImVec2(bw, 0))) g_confirm = false;
    if (!g_confirm) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void DrawDialog() {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    float em = ImGui::GetFontSize();
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(em * 44, em * 34), ImGuiCond_Always);
    if (g_opened.exchange(false)) ImGui::SetNextWindowFocus();
    for (const auto& [col, color] : kPanelColors) ImGui::PushStyleColor(col, color);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 2.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(em * 1.6f, em * 1.2f));
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
                                        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoTitleBar;
    if (ImGui::Begin("##achievements", nullptr, kFlags)) {
        const char* title = "ACHIEVEMENTS";
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(title).x) * 0.5f);
        ImGui::TextUnformatted(title);
        DrawTabs();
        std::vector<xam::Achievement> list = TabList();
        DrawCount(list);
        float footer = ImGui::GetFrameHeightWithSpacing() + em;
        ImGui::BeginChild("##list", ImVec2(0, -footer), ImGuiChildFlags_NavFlattened);
        for (const xam::Achievement& a : list) DrawRow(a);
        ImGui::EndChild();
        ImGui::Separator();
        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        ImGui::TextUnformatted("LB and RB or Left and Right switch, Escape closes");
        ImGui::PopStyleColor();
        float bw = em * 8;
        ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - bw * 2 - ImGui::GetStyle().ItemSpacing.x);
        if (ImGui::Button("Reset", ImVec2(bw, 0))) g_confirm = true;
        ImGui::SameLine();
        if (ImGui::Button("Back", ImVec2(bw, 0))) Close();
        // on release, so the game's menu never sees the press that closed it
        if (!g_confirm && ImGui::IsKeyReleased(ImGuiKey_GamepadFaceRight)) Close();
        ConfirmReset();
    }
    ImGui::End();
    ImGui::PopStyleVar(4);
    ImGui::PopStyleColor(int(std::size(kPanelColors)));
}

}  // namespace

void Open() {
    g_open = true;
    g_opened = true;
}

void Close() {
    g_open = false;
    g_confirm = false;
}

bool IsOpen() { return g_open; }

void Draw() {
    if (g_open) DrawDialog();
}

}  // namespace fcip::achievements
