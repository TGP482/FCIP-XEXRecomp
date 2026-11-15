// the solo editor's own undo steps (gizmo moves, mission object edits), undone with the editor's own newest first
// (undo.cpp); editor thread
#pragma once
#include <functional>
#include <string>

struct PPCContext;

namespace fcip::editor {

using UndoAction = std::function<void(PPCContext& ctx)>;

// undo puts the edit back; forget releases what undo would have needed, once the step is dropped unused
void PushUndo(PPCContext& ctx, std::string label, UndoAction undo, UndoAction forget = {});
void ClearUndo(PPCContext& ctx);  // a level starts

}  // namespace fcip::editor
