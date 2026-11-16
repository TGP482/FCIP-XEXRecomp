// the kept mission's XML nodes for the solo editor's tabs: reads straight from guest memory, edits through the node's
// own methods, and an undo that keeps a deep copy of the edited node and trades the copy's content back
#include "leveleditor/node_edit.h"

#include "editor/tools.h"
#include "kernel/memory.h"
#include "leveleditor/guest.h"
#include "leveleditor/undo.h"
#include "xdk/hle.h"

using namespace kernel;

namespace {

// guest scratch: two texts and a node reference
constexpr uint32_t kScratchKey = 0x000;
constexpr uint32_t kScratchKeyBytes = 0x40;
constexpr uint32_t kScratchValue = 0x040;
constexpr uint32_t kScratchValueBytes = 0x100;
constexpr uint32_t kScratchRef = 0x140;
constexpr uint32_t kScratchBytes = 0x144;

uint32_t g_scratch = 0;  // editor thread

bool EnsureScratch() {
    if (!g_scratch) g_scratch = mem::AllocSystem(kScratchBytes);
    return g_scratch != 0;
}

uint32_t KeyText(const std::string& text) {
    return fcip::editor::WriteText(g_scratch + kScratchKey, text, kScratchKeyBytes);
}

uint32_t ValueText(const std::string& text) {
    return fcip::editor::WriteText(g_scratch + kScratchValue, text, kScratchValueBytes);
}

void Release(PPCContext& ctx, uint32_t node) {
    CallGuest(ctx, fcip::editor::Method(node, fcip::editor::kNodeRelease), {node});
}

// a deep copy, referenced once
uint32_t Clone(PPCContext& ctx, uint32_t node) {
    W32(g_scratch + kScratchRef, 0);
    CallGuest(ctx, fcip::editor::Method(node, fcip::editor::kNodeClone), {g_scratch + kScratchRef, node});
    return R32(g_scratch + kScratchRef);
}

}  // namespace

namespace fcip::editor {

std::vector<XmlAttribute> XmlAttributes(uint32_t node) {
    std::vector<XmlAttribute> out;
    if (!node) return out;
    for (uint32_t a = R32(node + kNodeAttrFirst); a && a < R32(node + kNodeAttrLast); a += kAttrBytes) {
        out.push_back({GuestString(a + kAttrKey), GuestString(a + kAttrValue)});
    }
    return out;
}

std::string XmlValue(uint32_t node, const std::string& key) {
    for (const XmlAttribute& a : XmlAttributes(node)) {
        if (a.key == key) return a.value;
    }
    return {};
}

uint32_t XmlChild(uint32_t node, const std::string& tag) {
    for (uint32_t i = 0; node && i < ChildCount(node); i++) {
        if (GuestString(ChildAt(node, i) + kNodeTag) == tag) return ChildAt(node, i);
    }
    return 0;
}

void SetXmlValue(PPCContext& ctx, uint32_t node, const std::string& key, const std::string& value) {
    if (!node || !EnsureScratch()) return;
    CallGuest(ctx, Method(node, kNodeSetAttr), {node, KeyText(key), ValueText(value)});
}

uint32_t AddXmlChild(PPCContext& ctx, uint32_t parent, const std::string& tag) {
    if (!parent || !EnsureScratch()) return 0;
    W32(g_scratch + kScratchRef, 0);
    CallGuest(ctx, Method(parent, kNodeNewChild), {g_scratch + kScratchRef, parent, KeyText(tag)});
    uint32_t child = R32(g_scratch + kScratchRef);
    if (child) Release(ctx, child);
    return child;
}

void RemoveXmlChild(PPCContext& ctx, uint32_t parent, uint32_t child) {
    if (!parent || !child || !EnsureScratch()) return;
    CallGuest(ctx, Method(child, kNodeAddRef), {child});
    W32(g_scratch + kScratchRef, child);
    CallGuest(ctx, Method(parent, kNodeRemoveChild), {parent, g_scratch + kScratchRef});
}

void EditMissionNode(PPCContext& ctx, const std::string& tag, const std::string& label, const NodeChange& change,
                     const std::function<void(PPCContext&)>& reload) {
    uint32_t node = XmlChild(KeptMission(), tag);
    if (!node || !EnsureScratch()) return;
    uint32_t before = Clone(ctx, node);
    if (!before) return;
    change(ctx, node);
    if (reload) reload(ctx);
    NoteMapChanged();
    auto undo = [node, before, reload](PPCContext& ctx) {
        SwapContent(node, before);
        Release(ctx, before);
        if (reload) reload(ctx);
    };
    PushUndo(ctx, label, undo, [before](PPCContext& ctx) { Release(ctx, before); });
}

}  // namespace fcip::editor
