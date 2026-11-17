// the level's cutscenes and scripted sequences: moviedata.xml kept as the level loads it, edited, given back to the
// game's movie system and saved with a custom level (sequences.cpp)
#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "leveleditor/mission.h"

struct PPCContext;

namespace fcip::editor {

struct Key {
    float time = 0.0f;
    float pos[3] = {};
    std::vector<int> path;  // in its sequence's elements
};
struct MovieNode {
    std::string id;    // NodeData's
    std::string name;  // the entity it moves
    std::vector<Key> path;
    std::vector<std::string> events;  // "3s Enable"
};
struct Sequence {
    uint32_t node = 0;  // guest, its identity for edits
    std::string name;
    float start_time = 0.0f;
    float end_time = 0.0f;
    std::vector<MovieNode> nodes;
    bool has_start = false;
    float start[3] = {};
    std::vector<XmlElement> elements;  // the Sequence node and its children, depth first
};
using Movie = std::vector<Sequence>;

std::shared_ptr<const Movie> CurrentMovie();  // any thread
uint32_t KeptMovie();  // editor thread, moviedata.xml's root, 0 for none
bool MovieKept();      // any thread

// window thread: edits of a sequence (by its node) the editor thread applies, each one step of undo; the game's
// movie system then reads the sequences again
void SetMovieAttribute(uint32_t sequence, std::vector<int> path, std::string key, std::string value);
// index -1 adds it last
void AddMovieChild(uint32_t sequence, std::vector<int> path, std::string tag, std::vector<XmlAttribute> attributes,
                   int index = -1);
void RemoveMovieChild(uint32_t sequence, std::vector<int> path);
void AddSequence(std::string name);
void RemoveSequence(uint32_t sequence);
void AddSequenceEntity(uint32_t sequence, uint32_t entity_id);  // a node moving the entity, with position and
                                                                // rotation tracks

void UpdateSequences(PPCContext& ctx);  // editor thread, each frame: queued edits and the preview

// window thread: the sequence's entities placed as the game would have them at the time, until preview ends
void PreviewSequence(const std::string& name, float time);
void EndPreview();
// editor thread: the previewed entities back where they stood, before play test or a save; the preview comes back
// with the next editing frame
void StopPreview(PPCContext& ctx);

}  // namespace fcip::editor
