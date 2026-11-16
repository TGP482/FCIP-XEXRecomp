// the mission's entity layers as the solo editor edits them: a hidden layer hides its entities' markers and models in
// the editor view, and each entity's layer is saved as its Layer in the mission (layers.cpp)
#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct PPCContext;

namespace fcip::editor {

inline constexpr const char* kNoLayer = "(no layer)";

// any thread
std::string LayerOf(uint32_t entity_id);  // empty for none
std::vector<std::string> ViewLayers();    // sorted, kNoLayer last when some entity has none
bool LayerShown(const std::string& layer);

// window thread
void ShowLayer(const std::string& layer, bool shown);
void ShowAllLayers(bool shown);
// each one step of undo
void CreateLayer(const std::string& name);
void RenameLayer(const std::string& from, const std::string& to);  // onto an existing one merges them
void DeleteLayer(const std::string& name);                         // its entities keep no layer
void MoveToLayer(uint32_t entity_id, const std::string& layer);    // empty or kNoLayer for none
void DrawLayerPanel();                                             // layer_panel.cpp

// editor thread
void UpdateLayers(PPCContext& ctx);     // each editing frame
void ShowLayerModels(PPCContext& ctx);  // play test shows every model
bool ModelHiddenByLayer(uint32_t entity_id);

}  // namespace fcip::editor
