#include "game/achievement_rows.h"

#include <algorithm>
#include <cstdio>
#include <vector>

#include "xam/xam.h"

namespace fcip::achievement_rows {

namespace {

using rows::Kind;
using rows::Row;

// FCIP's SPA numbers its multiplayer achievements 40 to 83, its campaign ones the rest
constexpr uint16_t kFirstMultiplayer = 40;
constexpr uint16_t kLastMultiplayer = 83;
constexpr int kCampaign = 0;
constexpr int kMultiplayer = 1;
constexpr const char* kSectionHelp = "Earned achievements first, the rest greyed out";
constexpr const char* kResetHelp = "Lock every achievement this profile has earned";

std::vector<xam::Achievement> g_list;  // the SPA's order, the rows' labels and help point into it
std::vector<Row> g_rows;

int Section(const xam::Achievement& a) {
    bool multiplayer = a.id >= kFirstMultiplayer && a.id <= kLastMultiplayer;
    return multiplayer ? kMultiplayer : kCampaign;
}

const xam::Achievement* Find(int id) {
    for (const xam::Achievement& a : g_list) {
        if (a.id == id) return &a;
    }
    return nullptr;
}

void AddSection(int section, const char* label) {
    g_rows.push_back({Kind::Heading, section, label, kSectionHelp});
    std::vector<const xam::Achievement*> list;
    for (const xam::Achievement& a : g_list) {
        if (Section(a) == section) list.push_back(&a);
    }
    std::stable_partition(list.begin(), list.end(), [](const xam::Achievement* a) { return a->unlocked != 0; });
    for (const xam::Achievement* a : list) {
        const std::string& help = a->unlocked ? a->description : a->locked_description;
        g_rows.push_back({Kind::Achievement, a->id, a->name.c_str(), help.c_str()});
    }
}

}  // namespace

void Refresh() {
    g_list = xam::Achievements();
    g_rows.clear();
    AddSection(kCampaign, "CAMPAIGN");
    AddSection(kMultiplayer, "MULTIPLAYER");
    g_rows.push_back({Kind::Defaults, 0, "RESET ACHIEVEMENTS", kResetHelp});
}

std::span<const Row> Rows() { return g_rows; }

std::string Value(const Row& row) {
    if (row.kind != Kind::Heading) return {};
    int earned = 0;
    int total = 0;
    for (const xam::Achievement& a : g_list) {
        if (Section(a) != row.id) continue;
        total++;
        if (a.unlocked) earned++;
    }
    char text[32];
    std::snprintf(text, sizeof(text), "%d / %d (%d%%)", earned, total, total ? earned * 100 / total : 0);
    return text;
}

bool Locked(const Row& row) {
    const xam::Achievement* a = row.kind == Kind::Achievement ? Find(row.id) : nullptr;
    return a && !a->unlocked;
}

uint32_t Image(const Row& row) {
    const xam::Achievement* a = row.kind == Kind::Achievement ? Find(row.id) : nullptr;
    return a ? a->image : 0;
}

void Reset() {
    xam::ResetAchievements();
    Refresh();
}

}  // namespace fcip::achievement_rows
