// Text the recomp adds to the game's menus and the F10 panel, in the language the game booted with
#pragma once
#include <string>
#include <string_view>

namespace fcip {

// each phrase with an entry, the longest from each word start, translated; the rest kept, and all of it in English.
// UTF-8 in and out
std::string Translate(std::string_view english);
// UTF-8 as the game's UTF-16
std::u16string Utf16(std::string_view utf8);

}  // namespace fcip
