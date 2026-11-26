#include "common/keys.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <mutex>
#include <utility>

#include <toml++/toml.hpp>

#include "app/app.h"

namespace fcip::keys {

namespace {

struct ActionInfo {
    const char* label;
    const char* key;  // settings file name
    uint8_t first;
    uint8_t second;
};

constexpr ActionInfo kActions[kActionCount] = {
    {"MOVE FORWARD", "move_forward", 'W', 0},
    {"MOVE BACK", "move_back", 'S', 0},
    {"STRAFE LEFT", "strafe_left", 'A', 0},
    {"STRAFE RIGHT", "strafe_right", 'D', 0},
    {"WALK", "walk", VK_MENU, 0},
    {"JUMP", "jump", VK_SPACE, 0},
    {"CROUCH", "crouch", 'C', 0},
    {"FIRE", "fire", VK_LBUTTON, 0},
    {"AIM", "aim", VK_RBUTTON, 0},
    {"MELEE", "melee", 'V', 0},
    {"RELOAD", "reload", 'R', 0},
    {"INTERACT", "interact", 'F', 0},
    {"FERAL RUN", "feral_run", VK_SHIFT, 0},
    {"FERAL VISION", "feral_vision", 'X', 0},
    {"THROW", "throw", 'G', 0},
    {"GRENADES / ROCKS", "grenades", 'Q', 0},
    {"WEAPONS / TRAPS", "weapons", 'E', 0},
    {"BINOCULARS", "binoculars", '1', 0},
    {"PISTOL", "pistol", '2', 0},
    {"RIFLE", "rifle", '3', 0},
    {"HEAVY WEAPON", "heavy_weapon", '4', 0},
    {"NEXT WEAPON", "next_weapon", kWheelDown, 0},
    {"PREVIOUS WEAPON", "previous_weapon", kWheelUp, 0},
    {"OBJECTIVES", "objectives", VK_TAB, 0},
    {"TOGGLE HUD", "toggle_hud", 0, 0},
    {"TOGGLE TEXTURE REPLACEMENT", "toggle_textures", 0, 0},
    {"PUSH TO TALK", "push_to_talk", 'T', 0},
    {"PAUSE MENU", "pause_menu", VK_ESCAPE, 0},
    {"SETTINGS PANEL", "settings_panel", VK_F10, 0},
    {"NOCLIP KEY", "noclip", VK_F1, 0},
    {"LOOK UP", "look_up", VK_UP, 0},
    {"LOOK DOWN", "look_down", VK_DOWN, 0},
    {"LOOK LEFT", "look_left", VK_LEFT, 0},
    {"LOOK RIGHT", "look_right", VK_RIGHT, 0},
};

struct Name {
    uint8_t vk;
    const char* name;
};

constexpr Name kNames[] = {
    {VK_LBUTTON, "LMB"}, {VK_RBUTTON, "RMB"}, {VK_MBUTTON, "MMB"}, {VK_XBUTTON1, "MB4"}, {VK_XBUTTON2, "MB5"},
    {kWheelUp, "WHEEL UP"}, {kWheelDown, "WHEEL DOWN"},
    {VK_BACK, "BACKSPACE"}, {VK_TAB, "TAB"}, {VK_RETURN, "ENTER"}, {VK_SHIFT, "SHIFT"}, {VK_CONTROL, "CTRL"},
    {VK_MENU, "ALT"}, {VK_PAUSE, "PAUSE"}, {VK_CAPITAL, "CAPS LOCK"}, {VK_ESCAPE, "ESCAPE"}, {VK_SPACE, "SPACE"},
    {VK_PRIOR, "PAGE UP"}, {VK_NEXT, "PAGE DOWN"}, {VK_END, "END"}, {VK_HOME, "HOME"}, {VK_LEFT, "LEFT"},
    {VK_UP, "UP"}, {VK_RIGHT, "RIGHT"}, {VK_DOWN, "DOWN"}, {VK_INSERT, "INSERT"}, {VK_DELETE, "DELETE"},
    {VK_MULTIPLY, "NUM *"}, {VK_ADD, "NUM +"}, {VK_SUBTRACT, "NUM -"}, {VK_DECIMAL, "NUM ."}, {VK_DIVIDE, "NUM /"},
    {VK_OEM_1, ";"}, {VK_OEM_PLUS, "="}, {VK_OEM_COMMA, ","}, {VK_OEM_MINUS, "-"}, {VK_OEM_PERIOD, "."},
    {VK_OEM_2, "/"}, {VK_OEM_3, "`"}, {VK_OEM_4, "["}, {VK_OEM_5, "\\"}, {VK_OEM_6, "]"}, {VK_OEM_7, "'"},
};

// US layout scan codes of the keys that move with the layout
struct Position {
    uint8_t vk;
    UINT scan;
};
constexpr Position kPositions[] = {
    {'1', 0x02}, {'2', 0x03}, {'3', 0x04}, {'4', 0x05}, {'5', 0x06}, {'6', 0x07}, {'7', 0x08}, {'8', 0x09},
    {'9', 0x0A}, {'0', 0x0B}, {VK_OEM_MINUS, 0x0C}, {VK_OEM_PLUS, 0x0D},
    {'Q', 0x10}, {'W', 0x11}, {'E', 0x12}, {'R', 0x13}, {'T', 0x14}, {'Y', 0x15}, {'U', 0x16}, {'I', 0x17},
    {'O', 0x18}, {'P', 0x19}, {VK_OEM_4, 0x1A}, {VK_OEM_6, 0x1B},
    {'A', 0x1E}, {'S', 0x1F}, {'D', 0x20}, {'F', 0x21}, {'G', 0x22}, {'H', 0x23}, {'J', 0x24}, {'K', 0x25},
    {'L', 0x26}, {VK_OEM_1, 0x27}, {VK_OEM_7, 0x28}, {VK_OEM_3, 0x29}, {VK_OEM_5, 0x2B},
    {'Z', 0x2C}, {'X', 0x2D}, {'C', 0x2E}, {'V', 0x2F}, {'B', 0x30}, {'N', 0x31}, {'M', 0x32},
    {VK_OEM_COMMA, 0x33}, {VK_OEM_PERIOD, 0x34}, {VK_OEM_2, 0x35},
};

UINT ScanOf(uint8_t vk) {
    for (const Position& p : kPositions) {
        if (p.vk == vk) return p.scan;
    }
    return 0;
}

// the layout the player types with, switched per window thread
HKL Layout() { return GetKeyboardLayout(GetWindowThreadProcessId(static_cast<HWND>(app::Window()), nullptr)); }

std::mutex g_bind_lock;
uint8_t g_binds[kActionCount][2];

// window thread writes, guest threads read
std::atomic<bool> g_down[256];
std::atomic<int64_t> g_mouse_x{0};
std::atomic<int64_t> g_mouse_y{0};
std::atomic<int> g_wheel{0};  // window thread adds, pad poll takes whole notches
std::atomic<uint64_t> g_pad_ms{0};
std::atomic<uint64_t> g_kbm_ms{0};
std::atomic<uint64_t> g_delete_ms{0};
std::atomic<bool> g_text{false};
std::mutex g_text_lock;
std::u16string g_typed;
std::atomic<bool> g_upper{false};

// window thread: the key as the layout types it with the keyboard's state, Caps Lock and Shift included; a dead key
// types nothing, and the layout's own dead key state is left as it was
void Type(uintptr_t vk, intptr_t lp) {
    constexpr UINT kKeepState = 0x4;
    BYTE state[256];
    wchar_t typed[4];
    if (!GetKeyboardState(state)) return;
    g_upper = bool(state[VK_CAPITAL] & 1) != bool(state[VK_SHIFT] & 0x80);
    UINT scan = UINT(lp >> 16) & 0xFF;
    int n = ToUnicodeEx(UINT(vk), scan, state, typed, int(std::size(typed)), kKeepState, Layout());
    std::lock_guard lock(g_text_lock);
    for (int i = 0; i < n; i++) g_typed += char16_t(typed[i]);
}

// 0 idle, 1 waiting for a key, 2 key taken, 3 cancelled
std::atomic<int> g_capture{0};
std::atomic<uint8_t> g_captured{0};

// pad poll thread: a new key waits one poll, so a pad press reported a pump late still taints it; tainted keys are
// ignored until released
bool g_prev[256];
bool g_fresh[256];
bool g_tainted[256];

// pad poll thread: notches waiting to press their key, and the polls the pressed one has left
int g_notches[2];
int g_notch_polls[2];
constexpr int kMaxNotches = 4;  // a fast spin stops soon after the wheel does
constexpr int kNotchPolls = 2;  // fresh then held, so every reader sees it once

constexpr uint64_t kPadHoldMs = 150;  // covers a key up from Steam's pad mapping landing after the pad let go

uint64_t NowMs() {
    auto since = std::chrono::steady_clock::now().time_since_epoch();
    return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(since).count());
}

uint8_t ParseName(const std::string& name) {
    if (name.size() == 1 && ((name[0] >= 'A' && name[0] <= 'Z') || (name[0] >= '0' && name[0] <= '9')))
        return uint8_t(name[0]);
    for (const Name& n : kNames)
        if (name == n.name) return n.vk;
    if (name.size() > 1 && name[0] == 'F') {
        int f = std::atoi(name.c_str() + 1);
        if (f >= 1 && f <= 24) return uint8_t(VK_F1 + f - 1);
    }
    if (name.rfind("NUM ", 0) == 0 && name.size() == 5 && name[4] >= '0' && name[4] <= '9')
        return uint8_t(VK_NUMPAD0 + name[4] - '0');
    return 0;
}

void Set(uint8_t vk, bool down) {
    if (!down || g_capture != 1) {
        g_down[vk] = down;
        return;
    }
    // a capture takes the press, the game never sees it
    g_captured = vk;
    g_capture = vk == VK_ESCAPE ? 3 : 2;
}

uint8_t MouseKey(uint32_t msg, uintptr_t wp) {
    switch (msg) {
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP: return VK_LBUTTON;
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP: return VK_RBUTTON;
        case WM_MBUTTONDOWN:
        case WM_MBUTTONUP: return VK_MBUTTON;
        case WM_XBUTTONDOWN:
        case WM_XBUTTONUP: return HIWORD(wp) == XBUTTON1 ? VK_XBUTTON1 : VK_XBUTTON2;
        default: return 0;
    }
}

bool IsDown(uint32_t msg) {
    return msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN || msg == WM_MBUTTONDOWN || msg == WM_XBUTTONDOWN;
}

void RawMouse(intptr_t lp) {
    RAWINPUT raw;
    UINT size = sizeof(raw);
    if (GetRawInputData(HRAWINPUT(lp), RID_INPUT, &raw, &size, sizeof(RAWINPUTHEADER)) == UINT(-1)) return;
    if (raw.header.dwType != RIM_TYPEMOUSE || (raw.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE)) return;
    g_mouse_x += raw.data.mouse.lLastX;
    g_mouse_y += raw.data.mouse.lLastY;
}

bool Usable(uint8_t vk, bool fresh) { return vk && g_prev[vk] && (fresh || !g_fresh[vk]) && !g_tainted[vk]; }

}  // namespace

const char* Label(int action) { return kActions[action].label; }

uint8_t Key(int action, int slot) {
    std::lock_guard lock(g_bind_lock);
    return g_binds[action][slot];
}

void Bind(int action, uint8_t vk, int slot) {
    std::lock_guard lock(g_bind_lock);
    g_binds[action][slot] = vk;
}

void ResetBinds() {
    std::lock_guard lock(g_bind_lock);
    for (int i = 0; i < kActionCount; i++) {
        g_binds[i][0] = kActions[i].first;
        g_binds[i][1] = kActions[i].second;
    }
}

// the defaults stand until a settings file holds binds
const bool g_defaults = (ResetBinds(), true);

std::string KeyName(uint8_t vk) {
    if (!vk) return {};
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) return std::string(1, char(vk));
    for (const Name& n : kNames)
        if (n.vk == vk) return n.name;
    char text[16];
    if (vk >= VK_F1 && vk <= VK_F24) std::snprintf(text, sizeof(text), "F%d", vk - VK_F1 + 1);
    else if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) std::snprintf(text, sizeof(text), "NUM %d", vk - VK_NUMPAD0);
    else std::snprintf(text, sizeof(text), "KEY %02X", vk);
    return text;
}

std::string KeyLabel(uint8_t vk) {
    // shown short, the settings file keeps ESCAPE
    if (vk == VK_ESCAPE) return "ESC";
    UINT scan = ScanOf(vk);
    UINT live = scan ? MapVirtualKeyExW(scan, MAPVK_VSC_TO_VK_EX, Layout()) : 0;
    if (!live || live == vk) return KeyName(vk);
    if ((live >= 'A' && live <= 'Z') || (live >= '0' && live <= '9')) return std::string(1, char(live));
    // the top bit flags a dead key
    UINT c = MapVirtualKeyExW(live, MAPVK_VK_TO_CHAR, Layout()) & 0x7FFF;
    if (c > ' ' && c < 0x7F) return std::string(1, char(c));
    return KeyName(vk);
}

uint8_t PositionKey(uintptr_t vk) {
    if (vk >= 256) return 0;
    UINT scan = MapVirtualKeyExW(UINT(vk), MAPVK_VK_TO_VSC, Layout());
    for (const Position& p : kPositions) {
        if (p.scan == scan) return p.vk;
    }
    return uint8_t(vk);
}

bool Held(int action, bool fresh) { return Usable(Key(action, 0), fresh) || Usable(Key(action, 1), fresh); }

bool KeyHeld(uint8_t vk) { return Usable(vk, true); }

bool BindDown(int action) {
    uint8_t first = Key(action, 0);
    uint8_t second = Key(action, 1);
    return !g_text && ((first && g_down[first]) || (second && g_down[second]));
}

bool Message(void*, uint32_t msg, uintptr_t wp, intptr_t lp) {
    switch (msg) {
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            if (wp < 256) Set(PositionKey(wp), true);
            if (g_text) Type(wp, lp);
            // bit 30: a held key's repeat
            if (wp == VK_DELETE && !(lp & (1 << 30))) g_delete_ms = NowMs();
            // Alt and F10 open the system menu otherwise
            return msg == WM_SYSKEYDOWN && wp != VK_F4;
        case WM_KEYUP:
        case WM_SYSKEYUP:
            if (wp < 256) Set(PositionKey(wp), false);
            if (wp == VK_SHIFT) g_upper = bool(GetKeyState(VK_CAPITAL) & 1);
            return msg == WM_SYSKEYUP;
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP:
        case WM_MBUTTONDOWN:
        case WM_MBUTTONUP:
        case WM_XBUTTONDOWN:
        case WM_XBUTTONUP:
            Set(MouseKey(msg, wp), IsDown(msg));
            return false;
        case WM_MOUSEWHEEL:
            if (g_capture == 1) Set(GET_WHEEL_DELTA_WPARAM(wp) > 0 ? kWheelUp : kWheelDown, true);
            else g_wheel += GET_WHEEL_DELTA_WPARAM(wp);
            return false;
        case WM_INPUT:
            RawMouse(lp);
            return false;
        case WM_KILLFOCUS:
            for (auto& d : g_down) d = false;
            return false;
    }
    return false;
}

void WindowUp(void* hwnd) {
    RAWINPUTDEVICE mouse{0x01, 0x02, 0, static_cast<HWND>(hwnd)};  // generic desktop, mouse
    RegisterRawInputDevices(&mouse, 1, sizeof(mouse));
}

void SetTextEntry(bool on) {
    if (g_text.exchange(on) == on) return;
    g_upper = bool(GetKeyState(VK_CAPITAL) & 1) != bool(GetAsyncKeyState(VK_SHIFT) & 0x8000);
    std::lock_guard lock(g_text_lock);
    g_typed.clear();
}

bool TextEntry() { return g_text; }

bool TypingUpper() { return g_upper; }

std::u16string TakeText() {
    std::lock_guard lock(g_text_lock);
    return std::exchange(g_typed, {});
}

bool TakeDelete() {
    uint64_t pressed = g_delete_ms.exchange(0);
    return pressed && NowMs() - pressed < 250;
}

void BeginCapture() {
    g_captured = 0;
    g_capture = 1;
}

bool Capturing() { return g_capture == 1; }

int Captured() {
    int state = g_capture;
    if (state < 2) return 0;
    g_capture = 0;
    return state == 3 ? -1 : g_captured.load();
}

bool PadRecent() {
    uint64_t pad = g_pad_ms;
    return pad && NowMs() - pad < kPadHoldMs;
}

bool KbmActive() { return g_kbm_ms > g_pad_ms; }

bool PadActive() { return g_pad_ms > g_kbm_ms; }

void NoteKbm() {
    if (!PadRecent()) g_kbm_ms = NowMs();
}

// each notch presses its wheel key for kNotchPolls polls, one after another
void PulseWheel() {
    int delta = g_wheel.exchange(0);
    int steps = delta / WHEEL_DELTA;
    // fine grained wheels add up to a notch first
    g_wheel += delta - steps * WHEEL_DELTA;
    int& queued = g_notches[steps > 0 ? 0 : 1];
    queued = std::min(queued + std::abs(steps), kMaxNotches);
    for (int d = 0; d < 2; d++) {
        uint8_t vk = d ? kWheelDown : kWheelUp;
        if (g_notch_polls[d] > 0) {
            if (--g_notch_polls[d] == 0) g_down[vk] = false;
        } else if (g_notches[d] > 0 && !g_prev[vk]) {
            g_notches[d]--;
            g_notch_polls[d] = kNotchPolls;
            g_down[vk] = true;
        }
    }
}

// edges are judged against the pad: a key that goes down while it is in use is its echo
void TrackKeys(bool pad_moved, bool menu_open) {
    PulseWheel();
    uint64_t now = NowMs();
    if (pad_moved) g_pad_ms = now;
    bool pad_recent = PadRecent();
    for (int i = 0; i < 256; i++) {
        bool down = g_down[i];
        if (g_fresh[i]) {
            g_fresh[i] = false;
            g_tainted[i] = g_tainted[i] || pad_recent;
            if (!g_tainted[i] || g_text) g_kbm_ms = now;
        }
        if (down && !g_prev[i]) {
            g_fresh[i] = true;
            g_tainted[i] = pad_recent || menu_open || g_text;
        }
        if (!down) g_fresh[i] = g_tainted[i] = false;
        g_prev[i] = down;
    }
}

void TakeMouse(double& dx, double& dy) {
    dx = double(g_mouse_x.exchange(0));
    dy = double(g_mouse_y.exchange(0));
}


void Load(const void* table) {
    ResetBinds();
    auto& t = *static_cast<const toml::table*>(table);
    const toml::table* binds = t["keys"].as_table();
    if (!binds) return;
    for (int i = 0; i < kActionCount; i++) {
        std::optional<std::string> v = (*binds)[kActions[i].key].value<std::string>();
        if (!v) continue;
        size_t comma = v->find(',');
        Bind(i, ParseName(v->substr(0, comma)), 0);
        Bind(i, comma == std::string::npos ? 0 : ParseName(v->substr(comma + 1)), 1);
    }
}

void Save(void* table) {
    toml::table binds;
    for (int i = 0; i < kActionCount; i++) {
        std::string v = KeyName(Key(i, 0));
        if (Key(i, 1)) v += "," + KeyName(Key(i, 1));
        binds.insert(kActions[i].key, v);
    }
    static_cast<toml::table*>(table)->insert_or_assign("keys", std::move(binds));
}

}  // namespace fcip::keys
