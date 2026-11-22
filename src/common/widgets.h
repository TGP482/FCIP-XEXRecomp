// the game's menu widgets as both images lay them out (CryUI): reading, captioning and cloning them
#pragma once
#include <cstdint>
#include <initializer_list>

struct PPCContext;

namespace fcip::ui {

// widget names, crc32 of the layout's names
constexpr uint32_t kCrcRecompSettings = 0x7B8654F7u;  // B_RecompSettings
constexpr uint32_t kCrcMouseKeyboard = 0xFF898DA7u;   // B_MouseKeyboard
constexpr uint32_t kCrcControllerSettings = 0x582765A9u;  // B_ControllerSettings
constexpr uint32_t kCrcMultiplayerSetup = 0x5FC27D09u;
constexpr uint32_t kCrcExit = 0x5C6EA6F2u;  // B_Exit
constexpr uint32_t kCrcCustomCampaign = 0x16D94647u;  // B_CustomCampaign
constexpr uint32_t kCrcOnline = 0x9B680E53u;  // B_Live, MULTIPLAYER's ONLINE
constexpr uint32_t kCrcStatistics = 0x02CF2666u;  // B_Statistics, the Live menu's, our MULTIPLAYER entry
constexpr uint32_t kCrcAchievements = 0xED50CBB6u;  // B_Achievements

// CUIWidget
constexpr uint32_t kWidgetNameCrc = 0x04u;
constexpr uint32_t kWidgetElement = 0x08u;
constexpr uint32_t kWidgetVisible = 0x18u;  // bit 0x80
constexpr uint32_t kWidgetUp = 0x3Cu;       // then down, left, right
constexpr uint32_t kWidgetDown = 0x40u;
constexpr uint32_t kWidgetEnabled = 0x4Cu;  // bit 0x40
constexpr uint32_t kPageNameCrc = 0x14u;

// guest memory with a guard: menu objects sit on the heap below 0xC0000000
bool AddrOk(uint32_t addr);
uint32_t Read32(uint32_t addr);
void Write32(uint32_t addr, uint32_t value);
int16_t ReadS16(uint32_t addr);
uint8_t Read8(uint32_t addr);
void Write8(uint32_t addr, uint8_t value);

// the container's child named name_crc, 0 if none
uint32_t FindWidget(uint32_t container, uint32_t name_crc);
// a UTF-16 copy of text in guest memory, and a std::wstring view of it the callee only reads; one buffer for all
uint32_t Text(const char* text);
uint32_t View(const char* text);
void SetCaption(PPCContext& ctx, uint32_t element, const char* text);
// a row taken out of its list: not drawn, not focused, its neighbours linked past it
void HideWidget(uint32_t widget);
// widget moved to where place rests
void MoveWidget(uint32_t widget, uint32_t place);
// a copy of any widget pushed into the container, 0 when it failed
uint32_t CloneChild(PPCContext& ctx, uint32_t container, uint32_t source);
// a text or picture widget's left, right, top and bottom
void SetRect(uint32_t widget, const int16_t (&rect)[4]);
void ShiftRect(uint32_t widget, int dx);  // left and right
// a placed widget width wide moved to the other side of a screen that wide; twice puts it back
void MirrorX(uint32_t widget, int width, int screen);
// a picture widget's colour on all four corners
void SetPictureColour(uint32_t widget, uint32_t argb);

// our entries: caption, help and the panel page they open, by widget name; nullptr / -1 when not ours
const char* CaptionFor(uint32_t crc);
// the entry that opens a panel page
uint32_t EntryFor(int page);
const char* HelpFor(uint32_t crc);
int PageFor(uint32_t crc);
void SetEntryCaptions(PPCContext& ctx, uint32_t page);

// Adds the entries below `last`, one row apart, once per page object (the pages live in the layout, so a later menu
// instance finds them there). Clones copy `source`, `above` sits a row over it, `push` moves a row down per clone.
void AddEntries(PPCContext& ctx, uint32_t page, uint32_t last, uint32_t source_crc, uint32_t above_crc,
                std::initializer_list<uint32_t> crcs, uint32_t push = 0);

}  // namespace fcip::ui
