// a level value edited with the widget its kind takes, in the solo editor's panels (fields.cpp); window thread
#pragma once
#include <span>
#include <string>
#include <vector>

namespace fcip::editor {

// Colour is "r,g,b" from 0 to 1, Colour255 from 0 to 255
enum class FieldKind { Text, Bool, Int, Float, Colour, Colour255, Vector, Vector2 };

// true once an edit ends, with the value to keep in out; the value given shows until then
bool DrawField(const char* id, FieldKind kind, const std::string& value, std::string& out);
// the widget a script property takes by the type its name starts with (bEnabled, fSpeed, iSoundType, nGroupID,
// clrDiffuse, vector_Dim)
FieldKind KindOfKey(const std::string& key);
// a script property's value with the widget its name asks for: its choice list, a picker, or KindOfKey's field
bool DrawPropertyValue(const char* id, const std::string& key, const std::string& value, std::string& out);
// a selX property's choices, the script's enumX list it picks from by index; empty when it has none
std::vector<std::string> ChoicesOf(const std::string& key);
// the files or effects a property may name, from what the campaign names; empty for any other property
std::span<const char* const> PickerItemsOf(const std::string& key, const std::string& value);
// a combo of names, the value an index into them
bool DrawChoice(const char* id, const std::vector<std::string>& names, const std::string& value, std::string& out);
// typed text, or one of items from a filtered list
bool DrawPicker(const char* id, std::span<const char* const> items, const std::string& value, std::string& out);

}  // namespace fcip::editor
