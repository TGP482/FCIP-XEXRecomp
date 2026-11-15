// entities' own archetypes (a Merc's tplCreatureType), kept by entity id as levels load and written back by the save
// (archetypes.cpp); editor thread
#pragma once
#include <cstdint>

struct PPCContext;

namespace fcip::editor {

void ForgetArchetypes();  // a level starts
// the save wrote an entity (node): its archetype, and its whole Properties table as LocalProperties over it
void SaveArchetype(PPCContext& ctx, uint32_t entity, uint32_t node);
// every archetype the saved mission's entities name, into its EntityLibrary from the editor's library
void AddSavedPrototypes(PPCContext& ctx, uint32_t mission);

}  // namespace fcip::editor
