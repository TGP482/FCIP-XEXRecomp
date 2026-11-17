// the level's mission script and its objectives' text, for the solo editor's Script and Objectives tabs
// (mission_text.cpp)
#pragma once
#include <string>

namespace fcip::editor {

// editor thread: the mission's string table as Language:LoadMissionStringTable loaded it, own when it is the custom
// level's copy
void KeepStringTable(const std::string& name, const std::string& text, bool own);

// window thread: a tag's text in a section of the table (Waypoints, Completions); set keeps it for the save
bool HasStringTable();
std::string MissionText(const std::string& section, const std::string& tag);
void SetMissionText(const std::string& section, const std::string& tag, const std::string& text);
void DrawScriptPanel();

// any thread, the save: the table's file name and its text with the edits; an empty name when there is nothing to
// write, the campaign's table unedited
struct StringTableFile {
    std::string name;
    std::string text;
};
StringTableFile SavedStringTable();

}  // namespace fcip::editor
