// event links between entities (a trigger's Enter enabling a door), kept by entity id; a mission saves them as the
// entity's <EventTargets>
#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct PPCContext;

namespace fcip::editor {

struct Link {
    std::string source;   // the event the entity sends
    uint32_t target = 0;  // entity id
    std::string event;    // the event the target gets
    std::string param;    // ExtraParam, mostly empty
};

std::vector<Link> LinksOf(uint32_t entity_id);
void ForgetLinks();  // a level starts
// editor thread: keeps the entity's links and rebuilds its script's Events table from them
void SetLinks(PPCContext& ctx, uint32_t entity, const std::vector<Link>& links);

}  // namespace fcip::editor
