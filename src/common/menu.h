// the F10 panel and the rows the game's own menus show
#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

struct PPCContext;

namespace fcip {

constexpr int kSettingsPage = 0;
constexpr int kKeyboardPage = 1;
constexpr int kDebugPage = 2;

bool MenuOpen();
void ToggleMenu();
void OpenMenuPage(int page);

// GameInfo hooks
void DrawUi(double fps);
bool MenuMessage(void* hwnd, uint32_t msg, uintptr_t wp, intptr_t lp, intptr_t* result);

// default.xex's native pages, each frame on the menu thread after the menus' own input
inline void (*g_menu_tick)(PPCContext& ctx, uint32_t mgr) = nullptr;
// default.xex's debug page: queues a story level by folder name, false before START at the title screen
inline bool (*g_load_level)(const char* folder) = nullptr;
// window thread: the cursor and buttons for the game's menus, a left click skips a skippable movie
void MenuMouseMessage(void* hwnd, uint32_t msg, uintptr_t wp, intptr_t lp);
bool SkippableMovie();

namespace rows {

enum class Kind : uint8_t { Heading, Value, Bind, Level, Defaults };

struct Row {
    Kind kind;
    int id;  // a bind's action, a level's index
    const char* label;
    const char* help;  // the game's help bar
};

std::span<const Row> Page(int page);
// any thread: the value as shown, stepped first when step is not zero
std::string Text(const Row& row, int step = 0);
void Defaults(int page);
void SetBind(int action, uint8_t vk);  // the action's key, its alternate kept
void ClearBind(int action);            // both keys

}  // namespace rows

}  // namespace fcip
