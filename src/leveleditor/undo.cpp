// the solo editor's undo steps, kept beside the editor's own stack (tools.cpp undoes whichever is newer)
#include "leveleditor/undo.h"

#include <deque>
#include <mutex>
#include <vector>

#include "editor/tools.h"

using namespace fcip::editor;

namespace {

constexpr size_t kKept = 32;

struct Step {
    UndoStep info;
    UndoAction undo;
    UndoAction forget;
};

std::mutex g_lock;
std::deque<Step> g_steps;  // oldest first

std::vector<UndoStep> Steps() {
    std::lock_guard lock(g_lock);
    std::vector<UndoStep> out;
    for (const Step& s : g_steps) out.push_back(s.info);
    return out;
}

void UndoNewest(PPCContext& ctx) {
    Step step;
    {
        std::lock_guard lock(g_lock);
        if (g_steps.empty()) return;
        step = std::move(g_steps.back());
        g_steps.pop_back();
    }
    step.undo(ctx);
    NoteMapChanged();
}

const bool g_registered = (g_extra_undo = {Steps, UndoNewest}, true);

}  // namespace

namespace fcip::editor {

void PushUndo(PPCContext& ctx, std::string label, UndoAction undo, UndoAction forget) {
    std::deque<Step> dropped;
    {
        std::lock_guard lock(g_lock);
        g_steps.push_back({{UndoStamp(), std::move(label)}, std::move(undo), std::move(forget)});
        while (g_steps.size() > kKept) {
            dropped.push_back(std::move(g_steps.front()));
            g_steps.pop_front();
        }
    }
    for (Step& s : dropped) {
        if (s.forget) s.forget(ctx);
    }
}

void ClearUndo(PPCContext& ctx) {
    std::deque<Step> dropped;
    {
        std::lock_guard lock(g_lock);
        dropped.swap(g_steps);
    }
    for (Step& s : dropped) {
        if (s.forget) s.forget(ctx);
    }
}

}  // namespace fcip::editor
