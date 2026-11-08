// the F10 panel and the rows the game's own menus show
#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace input {
struct Pad;
}

namespace fcip {

constexpr int kSettingsPage = 0;
constexpr int kKeyboardPage = 1;

bool MenuOpen();
void ToggleMenu();
void OpenMenuPage(int page);

// GameInfo hooks
void DrawUi(double fps);
bool MenuMessage(void* hwnd, uint32_t msg, uintptr_t wp, intptr_t lp, intptr_t* result);
void MenuPad(int port, input::Pad& pad);  // the game sees no pad while the panel is open

namespace rows {

enum class Kind : uint8_t { Heading, Value, Defaults };

struct Row {
    Kind kind;
    int id;
    const char* label;
    const char* help;  // the game's help bar
};

std::span<const Row> Page(int page);
// any thread: the value as shown, stepped first when step is not zero
std::string Text(const Row& row, int step = 0);
void Defaults(int page);

}  // namespace rows

}  // namespace fcip
