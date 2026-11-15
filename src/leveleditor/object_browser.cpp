// the solo level editor's Objects tab: everything a level places, by the editor view's categories. Entity classes
// are the Add Object tool's catalogue items, which a click in the view then places; mission objects (areas, AI
// objects, respawn points) go into the kept mission at the tool cursor.
#include <algorithm>
#include <string>
#include <vector>

#include <imgui.h>

#include "editor/tools.h"
#include "leveleditor/level_objects.h"
#include "leveleditor/mission.h"

using namespace fcip::editor;

namespace {

// the Add Object tool's categories
constexpr int kWeaponCategory = 1;
constexpr int kRespawnCategory = 3;  // multiplayer respawn points, where no mission is kept
constexpr int kFirstLaterCategory = 3;  // the game's categories from here go after the level's

char g_search[64] = "";

char Lower(char c) { return char(c >= 'A' && c <= 'Z' ? c + 32 : c); }

bool Matches(const std::string& name) {
    std::string search = g_search;
    auto same = [](char a, char b) { return Lower(a) == Lower(b); };
    return std::search(name.begin(), name.end(), search.begin(), search.end(), same) != name.end();
}

bool SameText(const std::string& a, const char* b) {
    std::string other = b;
    auto same = [](char x, char y) { return Lower(x) == Lower(y); };
    return std::equal(a.begin(), a.end(), other.begin(), other.end(), same);
}

// a catalogue item one of the level's categories lists instead
bool InLevelCategory(const std::string& caption) {
    for (const LevelClass& c : kLevelClasses) {
        if (c.category != kPickupsCategory && SameText(caption, c.caption)) return true;
    }
    return false;
}

int WeaponItem(const Category& weapons, const char* caption) {
    for (size_t i = 0; i < weapons.items.size(); i++) {
        if (SameText(weapons.items[i], caption)) return int(i);
    }
    return -1;
}

bool OpenHeader(const char* name) {
    if (g_search[0]) ImGui::SetNextItemOpen(true);
    return ImGui::CollapsingHeader(name);
}

void GameCategory(const State& s, const std::vector<Category>& catalogue, int category) {
    const std::vector<std::string>& items = catalogue[size_t(category)].items;
    std::vector<int> shown;
    for (int i = 0; i < int(items.size()); i++) {
        bool level = category == kWeaponCategory && InLevelCategory(items[size_t(i)]);
        if (!level && Matches(items[size_t(i)])) shown.push_back(i);
    }
    if (shown.empty() || !OpenHeader(CategoryName(category))) return;
    for (int i : shown) {
        bool picked = s.tool == kAddObject && s.category == category && s.item == i;
        ImGui::PushID(category * 4096 + i);
        if (ImGui::Selectable(items[size_t(i)].c_str(), picked)) PickObject(category, i);
        ImGui::PopID();
    }
}

void AddAtCursor(const char* type) {
    State s = Read();
    AddAiObject(type, {s.cursor[0], s.cursor[1], s.cursor[2]});
}

void LevelCategory(const State& s, const std::vector<Category>& catalogue, int view, bool kept) {
    std::vector<const LevelClass*> classes;
    std::vector<const LevelObject*> objects;
    for (const LevelClass& c : kLevelClasses) {
        if (c.category == view && Matches(c.caption)) classes.push_back(&c);
    }
    for (const LevelObject& o : kLevelObjects) {
        if (o.category == view && Matches(o.caption)) objects.push_back(&o);
    }
    if (classes.empty() && objects.empty()) return;
    ImGui::PushID(view);
    if (OpenHeader(ViewCategoryName(view))) {
        const Category& weapons = catalogue[kWeaponCategory];
        for (const LevelClass* c : classes) {
            int item = WeaponItem(weapons, c->caption);
            bool picked = s.tool == kAddObject && s.category == kWeaponCategory && s.item == item;
            ImGui::BeginDisabled(item < 0);
            if (ImGui::Selectable(c->caption, picked)) PickObject(kWeaponCategory, item);
            ImGui::EndDisabled();
        }
        ImGui::BeginDisabled(!kept);
        for (const LevelObject* o : objects) {
            if (ImGui::Selectable(o->caption)) AddAtCursor(o->type);
            const char* tip = kept ? "Added at the tool cursor" : "Needs a campaign or custom level";
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", tip);
        }
        ImGui::EndDisabled();
    }
    ImGui::PopID();
}

}  // namespace

void fcip::editor::DrawLevelObjects() {
    State s = Read();
    std::vector<Category> catalogue = Catalogue();
    catalogue.resize(kCategoryCount);
    bool kept = ReadAiObjects()->kept;
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##search", "Search", g_search, sizeof(g_search));
    ImGui::BeginChild("##objects");
    for (int c = 0; c < kFirstLaterCategory; c++) GameCategory(s, catalogue, c);
    for (int v = 0; v < kViewCategoryCount; v++) LevelCategory(s, catalogue, v, kept);
    for (int c = kFirstLaterCategory; c < kCategoryCount; c++) {
        if (c != kRespawnCategory || !kept) GameCategory(s, catalogue, c);
    }
    ImGui::EndChild();
}
