// entities' own archetypes (a Merc's tplCreatureType), kept by entity id as levels load and written back by the save
// (archetypes.cpp); editor thread
#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct PPCContext;

namespace fcip::editor {

void ForgetArchetypes();  // a level starts
std::string ArchetypeOf(uint32_t entity_id);  // the one it loaded or was set with, empty for none
// the full names of the editor library's archetypes made for a class
std::vector<std::string> ArchetypesOfClass(PPCContext& ctx, const std::string& cls);
// the entity takes the archetype's Properties and is saved with it; one step of undo
void SetArchetype(PPCContext& ctx, uint32_t entity, const std::string& name);
// the save wrote an entity (node): its archetype, and its whole Properties table as LocalProperties over it
void SaveArchetype(PPCContext& ctx, uint32_t entity, uint32_t node);
// every archetype the saved mission's entities name, into its EntityLibrary from the editor's library
void AddSavedPrototypes(PPCContext& ctx, uint32_t mission);

}  // namespace fcip::editor
