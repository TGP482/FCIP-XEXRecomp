// the MONITOR setting: the host sizes the window on the monitor it is on, so it is moved to the chosen one first
#pragma once
#include <string>
#include <vector>

#include "app/app.h"

namespace fcip {

struct Display {
    std::string device;  // \\.\DISPLAY1
    int number;          // the device's number, as Windows numbers it
    app::Mode desktop;
    bool primary;
};
// the connected monitors, in their device numbers' order
std::vector<Display> Displays();
// window thread: the game window onto the setting's monitor, before the host's display apply
void UpdateWindowMonitor(void* hwnd);
// the host's splash, already up when the game window is created, centred on the setting's monitor
void UpdateSplashMonitor();

}  // namespace fcip
