// a level value edited with the widget its kind takes, in the solo editor's panels (fields.cpp); window thread
#pragma once
#include <string>

namespace fcip::editor {

enum class FieldKind { Text, Bool, Int, Float, Colour, Vector };

// true once an edit ends, with the value to keep in out; the value given shows until then
bool DrawField(const char* id, FieldKind kind, const std::string& value, std::string& out);

}  // namespace fcip::editor
