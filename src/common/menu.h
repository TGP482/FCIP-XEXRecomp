// the F10 panel and the rows the game's own menus show
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

struct PPCContext;

namespace fcip {

constexpr int kSettingsPage = 0;
constexpr int kKeyboardPage = 1;
constexpr int kControllerPage = 2;
constexpr int kDebugPage = 3;

bool MenuOpen();
void ToggleMenu();
void OpenMenuPage(int page);
uint32_t SettingsRevision();  // counts every change, from the panel or the game's menus

// GameInfo hooks
void DrawUi(double fps);
bool MenuMessage(void* hwnd, uint32_t msg, uintptr_t wp, intptr_t lp, intptr_t* result);

// default.xex's native pages, each frame on the menu thread after the menus' own input
inline void (*g_menu_tick)(PPCContext& ctx, uint32_t mgr) = nullptr;
// default.xex's debug page: queues a story level by folder name, false before START at the title screen
inline bool (*g_load_level)(const char* folder) = nullptr;
// FCIPEditor.exe's desktop editor around the 3D view, drawn first in the ImGui frame; true while it is up, when it
// takes the mouse from the game
inline bool (*g_editor_ui)(double fps) = nullptr;
// window thread: its messages, keys only while the settings panel is closed; true when taken
inline bool (*g_editor_message)(void* hwnd, uint32_t msg, uintptr_t wp) = nullptr;
// while the editor's panels take typing the game gets no keys, as with the settings panel
inline std::atomic<bool> g_editor_typing{false};
// while it is up the pointer stays free: the editor's player never mouse looks under it
inline std::atomic<bool> g_editor_shell{false};
// FCIPEditor.exe's launch question, set by the level editor's sources (GameInfo::init)
inline void (*g_editor_launch)() = nullptr;
// window thread: the cursor and buttons for the game's menus, a left click skips a skippable movie
void MenuMouseMessage(void* hwnd, uint32_t msg, uintptr_t wp, intptr_t lp);
bool SkippableMovie();

namespace rows {

enum class Kind : uint8_t { Heading, Value, Bind, Level, Defaults, Text };

struct Row {
    Kind kind;
    int id;  // a bind's action, a level's index
    const char* label;
    const char* help;  // the game's help bar
};

std::span<const Row> Page(int page);
// any thread: the value as shown, stepped first when step is not zero
std::string Text(const Row& row, int step = 0);
// any thread: the row's values as a list, empty when it only steps; current is the one set
std::vector<std::string> Choices(const Row& row, int* current);
void Choose(const Row& row, int index);
void Defaults(int page);
void SetBind(int action, uint8_t vk);  // the action's key, its alternate kept
void ClearBind(int action);            // both keys

}  // namespace rows

}  // namespace fcip
