// Button prompts. The token expander turns {X} into a pad icon glyph. On keyboard and mouse the text element draws
// a copy with the bound key, "Press and Hold (R) to swap weapon for", around every measure and draw, so prompts follow
// the device live and wrap and align as the game lays them out. The verb before a key follows the bind: Click for
// mouse buttons, Press for keys. Localised text loses its Xbox wording at lookup.
#include <algorithm>
#include <atomic>
#include <string>
#include <string_view>

#include "common/input.h"
#include "common/keys.h"
#include "kernel/memory.h"
#include "xdk/hle.h"

using namespace kernel;
namespace K = fcip::keys;

namespace {

// the 31 pad glyphs, u16 each, in token order
constexpr int kGlyphCount = 31;
constexpr int kGlyphA = 0;
constexpr int kGlyphB = 1;
constexpr int kGlyphX = 2;
constexpr int kGlyphStart = 6;
constexpr uint8_t kEnter = 0x0D;  // VK_RETURN

struct Bind {
    int first;
    int second;  // two direction tokens show both keys
};

constexpr Bind kNone = {-1, -1};

// A B X Y BL WH START BACK AS ASL ASG ASI ASS ASR ASD DP DPLR DPUD DPU DPD DPL DPR DPLRU DPLRD TRIG TRIGL TRIGG TRIGI
// TRIGS TRIGR TRIGD; none keeps the icon
constexpr Bind kBinds[kGlyphCount] = {
    {K::kJump, -1},       {K::kMelee, -1},  {K::kInteract, -1},      {K::kFeralRun, -1},  {K::kWeapons, -1},
    {K::kGrenades, -1},   {K::kPause, -1},  {K::kObjectives, -1},    kNone,               {K::kCrouch, -1},
    {K::kCrouch, -1},     {K::kCrouch, -1}, {K::kCrouch, -1},        {K::kAim, -1},       {K::kAim, -1},
    kNone,                {K::kRifle, K::kPistol},                   {K::kBinoculars, K::kHeavy},
    {K::kBinoculars, -1}, {K::kHeavy, -1},  {K::kRifle, -1},         {K::kPistol, -1},    kNone,
    kNone,                kNone,            {K::kThrow, -1},         {K::kThrow, -1},     {K::kThrow, -1},
    {K::kThrow, -1},      {K::kFire, -1},   {K::kFire, -1},
};

// std::wstring: buffer or pointer +0x04, length +0x14, capacity +0x18. The element's own text is at +0x0C, but a
// container drawing a HUD panel pushes its variable text onto a stack the draw reads first: global, depth u8 +0,
// slot pointers from +4. The key version goes on top as our own string; the game thread may free the element's.
constexpr uint32_t kElemText = 0x0C;
constexpr uint32_t kStringSize = 0x20;
constexpr uint32_t kStringData = 0x04;
constexpr uint32_t kStringLength = 0x14;
constexpr uint32_t kStringCapacity = 0x18;
constexpr uint32_t kMaxUnits = 2048;
constexpr uint32_t kSlots = 4;  // measure runs inside draw

uint32_t TextData(uint32_t str) { return R32(str + kStringCapacity) >= 8 ? R32(str + kStringData) : str + kStringData; }

bool IsMouse(uint8_t vk) { return vk == 0x01 || vk == 0x02 || vk == 0x04 || vk == 0x05 || vk == 0x06; }

std::string KeyText(const Bind& bind, bool& mouse) {
    if (bind.first < 0) return {};
    uint8_t vk = K::Key(bind.first, 0);
    std::string key = K::KeyLabel(vk);
    mouse = IsMouse(vk);
    if (bind.second >= 0) {
        std::string other = K::KeyLabel(K::Key(bind.second, 0));
        if (key.empty() || other.empty()) return {};
        key += "/" + other;
    }
    return key.empty() ? key : "(" + key + ")";
}

// the menus take Enter as A and the pause key as B
std::string MenuKeyText(int index) {
    uint8_t vk = 0;
    if (index == kGlyphA || (index == kGlyphStart && fcip::OnTitle())) vk = kEnter;
    if (index == kGlyphB) vk = K::Key(K::kPause, 0);
    std::string key = vk && fcip::InMenu() ? K::KeyLabel(vk) : std::string();
    return key.empty() ? key : "(" + key + ")";
}

bool IsLetter(char16_t c) { return (c | 0x20) >= 'a' && (c | 0x20) <= 'z'; }

// the letters of the word ending before end, trailing spaces skipped
size_t WordStart(const std::u16string& s, size_t& end) {
    while (end && s[end - 1] == ' ') end--;
    size_t begin = end;
    while (begin && IsLetter(s[begin - 1])) begin--;
    return begin;
}

bool Is(const std::u16string& s, size_t begin, size_t end, std::string_view word) {
    if (end - begin != word.size()) return false;
    for (size_t i = 0; i < word.size(); i++)
        if ((s[begin + i] | 0x20) != word[i]) return false;
    return true;
}

// "Pull and Hold" before a key becomes "Click and Hold" or "Press and Hold", case kept
void FixVerb(std::u16string& s, bool mouse) {
    size_t end = s.size();
    size_t begin = WordStart(s, end);
    if (Is(s, begin, end, "the")) {
        end = begin;
        begin = WordStart(s, end);
    }
    if (Is(s, begin, end, "hold") || Is(s, begin, end, "holding")) {
        end = begin;
        begin = WordStart(s, end);
        if (!Is(s, begin, end, "and")) return;
        end = begin;
        begin = WordStart(s, end);
    }
    bool ing = Is(s, begin, end, "pulling") || Is(s, begin, end, "pressing") || Is(s, begin, end, "clicking");
    if (!ing && !Is(s, begin, end, "pull") && !Is(s, begin, end, "press") && !Is(s, begin, end, "click")) return;
    std::string_view to = mouse ? (ing ? "clicking" : "click") : (ing ? "pressing" : "press");
    bool first_upper = s[begin] < 'a';
    bool all_upper = first_upper && s[begin + 1] < 'a';
    std::u16string word(to.begin(), to.end());
    for (size_t i = 0; i < word.size(); i++)
        if (all_upper || (i == 0 && first_upper)) word[i] -= 0x20;
    s.replace(begin, end - begin, word);
}

// every entry shrinks, so the lookup's own copy is edited in place
struct Swap {
    std::u16string_view from, to;
};
constexpr Swap kPcText[] = {
    {u"turn off your Xbox 360 console", u"close the game"},
    {u"turn off your console", u"close the game"},
    {u"TURN OFF YOUR CONSOLE", u"CLOSE THE GAME"},
    {u"Xbox 360 video game systems", u"PCs"},
    {u"Xbox 360 consoles", u"PCs"},
    {u"Xbox 360 console", u"PC"},
    {u"XBOX 360 CONSOLE", u"PC"},
    {u"XBOX COMMUNICATOR", u"HEADSET"},
    {u"XBOX LIVE", u"ONLINE"},
    {u"Xbox Live", u"Online"},
    {u"XBOXES", u"PCS"},
    {u"YOUR XBOX", u"YOUR PC"},
    {u"ANOTHER XBOX 360", u"ANOTHER PC"},
    {u"ANOTHER XBOX", u"ANOTHER PC"},
    {u"SINGLE XBOX 360", u"SINGLE PC"},
    {u"SINGLE XBOX", u"SINGLE PC"},
    {u"Xbox 360", u"PC"},
};

std::u16string ReadText(uint32_t data, uint32_t length) {
    std::u16string text(length, u'\0');
    for (uint32_t i = 0; i < length; i++) text[i] = char16_t(R16(data + i * 2));
    return text;
}

void WriteText(uint32_t data, const std::u16string& text) {
    for (size_t i = 0; i < text.size(); i++) W16(data + uint32_t(i) * 2, uint16_t(text[i]));
    W16(data + uint32_t(text.size()) * 2, 0);
}

// the wstring the lookup returned
void ToPc(uint32_t str) {
    uint32_t length = R32(str + kStringLength);
    uint32_t data = TextData(str);
    if (length < 6 || length > kMaxUnits || data < 0x10000u) return;
    std::u16string text = ReadText(data, length);
    bool changed = false;
    for (const Swap& swap : kPcText) {
        for (size_t at; (at = text.find(swap.from)) != std::u16string::npos;) {
            text.replace(at, swap.from.size(), swap.to);
            changed = true;
        }
    }
    if (!changed) return;
    WriteText(data, text);
    W32(str + kStringLength, uint32_t(text.size()));
}

// X is interact, unless the text is about reloading ("To reload Press {X}")
bool MentionsReload(const std::u16string& text) {
    constexpr std::string_view kWord = "reload";
    for (size_t i = 0; i + kWord.size() <= text.size(); i++) {
        size_t n = 0;
        while (n < kWord.size() && (text[i + n] | 0x20) == kWord[n]) n++;
        if (n == kWord.size()) return true;
    }
    return false;
}

uint32_t Block() {
    static const uint32_t block = mem::AllocSystem(kSlots * (kStringSize + kMaxUnits * 2));
    return block;
}

int GlyphIndex(char16_t unit) {
    for (int g = 0; g < kGlyphCount; g++)
        if (R16(SYM_UIGlyph_table + uint32_t(g) * 2) == unit) return g;
    return -1;
}

// the text with each bound icon swapped for its key; empty when nothing was swapped
std::u16string WithKeyText(const std::u16string& text) {
    std::u16string out;
    bool swapped = false;
    for (size_t i = 0; i < text.size(); i++) {
        int index = GlyphIndex(text[i]);
        bool mouse = false;
        std::string key = index >= 0 ? MenuKeyText(index) : std::string();
        if (key.empty() && index == kGlyphX && MentionsReload(text)) key = KeyText({K::kReload, -1}, mouse);
        else if (key.empty() && index >= 0) key = KeyText(kBinds[index], mouse);
        if (key.empty()) {
            out.push_back(text[i]);
            continue;
        }
        FixVerb(out, mouse);
        out.append(key.begin(), key.end());
        swapped = true;
        // "the {WH} button" names a key on the keyboard
        constexpr std::u16string_view kButton = u" button";
        if (!mouse && text.compare(i + 1, kButton.size(), kButton) == 0) {
            out += u" key";
            i += kButton.size();
        }
    }
    return swapped ? out : std::u16string();
}

// calls the original with the text it will read swapped for the key version, if it has icons
void WithKeys(PPCContext& ctx, uint8_t* base, PPCFunc& original) {
    uint32_t elem = uint32_t(ctx.r[3]);
    uint32_t stack = R32(SYM_UISystem_pointer);
    if (!K::KbmActive() || elem < 0x10000u || stack < 0x10000u || !Block()) return original(ctx, base);
    uint32_t top_slot = stack + 4u * (R8(stack) + 1u);
    uint32_t top = R32(top_slot);
    uint32_t str = top >= 0x10000u ? top : elem + kElemText;
    uint32_t length = R32(str + kStringLength);
    uint32_t data = TextData(str);
    if (!length || length > kMaxUnits || data < 0x10000u) return original(ctx, base);
    std::u16string out = WithKeyText(ReadText(data, length));
    if (out.empty() || out.size() >= kMaxUnits) return original(ctx, base);

    static std::atomic<uint32_t> next{0};
    uint32_t own = Block() + (next++ % kSlots) * (kStringSize + kMaxUnits * 2);
    uint32_t slot = own + kStringSize;
    WriteText(slot, out);
    uint32_t units = uint32_t(out.size());
    std::memset(HostPtr(own), 0, kStringSize);
    W32(own + kStringData, slot);
    W32(own + kStringLength, units);
    W32(own + kStringCapacity, std::max<uint32_t>(units, 8) | 7);
    W32(top_slot, own);
    original(ctx, base);
    W32(top_slot, top);
}

}  // namespace

PPC_EXTERN_FUNC(GUEST_ORIG(UIText_Draw));
PPC_EXTERN_FUNC(GUEST_ORIG(UIText_Measure));
PPC_EXTERN_FUNC(GUEST_ORIG(Localise_ByCrc));
PPC_EXTERN_FUNC(GUEST_ORIG(Localise_ByName));

// text element draw (vtable +8) and measure (vtable +0x54), r3 the element
PPC_FUNC(GUEST(UIText_Draw)) { WithKeys(ctx, base, GUEST_ORIG(UIText_Draw)); }
PPC_FUNC(GUEST(UIText_Measure)) { WithKeys(ctx, base, GUEST_ORIG(UIText_Measure)); }

// string table lookups by crc and by name, both return a wstring copy in r3
PPC_FUNC(GUEST(Localise_ByCrc)) {
    uint32_t out = uint32_t(ctx.r[3]);
    GUEST_ORIG(Localise_ByCrc)(ctx, base);
    ToPc(out);
}

PPC_FUNC(GUEST(Localise_ByName)) {
    uint32_t out = uint32_t(ctx.r[3]);
    GUEST_ORIG(Localise_ByName)(ctx, base);
    ToPc(out);
}
