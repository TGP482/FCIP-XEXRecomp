// the solo level editor's save: objective markers and the extraction. A waypoint's marker is a ParticleEffect of
// 01_BEACH's objective marker its waypoint switches on as it becomes current and off as it closes, as 01_BEACH's
// waypoints do. The extraction waypoint starts absent, with an EndMapTrigger over it; the rules file watches the
// objectives once a second (Game:SetTimer calls its table's OnEvent): an objective closes from what happened to the
// entity whose link closes it (a destroyable's currentStage, an enemy's IsDeath) or from the player reaching it, as
// its own links close it too, and once all are closed the extraction opens and the player there ends the level. The
// game's Lua is 4.0: no local functions, for k, v in t, and GetPos hands back one shared table.
#include "leveleditor/level_rules.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

using namespace fcip::editor;

namespace {

constexpr const char* kMarkerEffect = "gfx.GFX_Objective_Marker.GFX_Objective_Marker";
constexpr const char* kMarkerAngles = "90,0,170";
constexpr const char* kMarkerSuffix = "_Marker";
constexpr const char* kTriggerSuffix = "_EndMapTrigger";
constexpr int kExtractionSize = 20;  // the end trigger's box, metres a side
constexpr int kExtractionReach = 10;
constexpr const char* kCurrent = "2";  // selStatus
constexpr const char* kAbsent = "0";
constexpr const char* kMarkerLink = "Event=\"Enable\" SourceEvent=\"SetCurrent\"";  // a waypoint with its marker

struct Block {
    size_t begin = 0;
    size_t end = 0;  // past its close
    std::string text;
};

// the mission's entities, each from its "<Entity" to its "/>" or past its "</Entity>"
std::vector<Block> Entities(const std::string& mission) {
    constexpr const char* kOpen = "<Entity ";
    constexpr const char* kClose = "</Entity>";
    std::vector<Block> out;
    for (size_t at = mission.find(kOpen); at != std::string::npos; at = mission.find(kOpen, at + 1)) {
        size_t tag = mission.find('>', at);
        if (tag == std::string::npos) break;
        size_t end = tag + 1;
        if (mission[tag - 1] != '/') {
            end = mission.find(kClose, tag);
            if (end == std::string::npos) break;
            end += std::char_traits<char>::length(kClose);
        }
        out.push_back({at, end, mission.substr(at, end - at)});
    }
    return out;
}

std::string Attr(const std::string& text, const std::string& key) {
    std::string open = " " + key + "=\"";
    size_t at = text.find(open);
    if (at == std::string::npos) return {};
    at += open.size();
    return text.substr(at, text.find('"', at) - at);
}

void SetAttr(std::string& text, const std::string& key, const std::string& value) {
    std::string open = " " + key + "=\"";
    size_t at = text.find(open);
    if (at == std::string::npos) return;
    at += open.size();
    text.replace(at, text.find('"', at) - at, value);
}

std::string EventTarget(uint32_t target, const std::string& event, const std::string& source) {
    return "\n\t\t\t\t<EventTarget Target=\"" + std::to_string(target) + "\" Event=\"" + event +
           "\" SourceEvent=\"" + source + "\" />";
}

void AddEvents(std::string& entity, const std::string& events) {
    constexpr const char* kTargets = "<EventTargets>";
    size_t at = entity.find(kTargets);
    if (at != std::string::npos) {
        entity.insert(at + std::char_traits<char>::length(kTargets), events);
        return;
    }
    entity.insert(entity.find('>') + 1, "\n\t\t\t<EventTargets>" + events + "\n\t\t\t</EventTargets>");
}

std::string EntityOpen(const std::string& name, const std::string& cls, const Block& at, uint32_t id,
                       const char* angles) {
    return "\t\t<Entity Type=\"Entity\" Name=\"" + name + "\" Pos=\"" + Attr(at.text, "Pos") + "\" Angles=\"" +
           angles + "\" CastShadowMaps=\"0\" RecvShadowMaps=\"0\" CastShadows=\"0\" ReceiveLightmap=\"0\" "
           "LodRatio2=\"1\" ViewDistanceRatio=\"1\" HiddenInGame=\"0\" AmbientColorBoost=\"0,0,0\" "
           "ConditionalVisibility=\"0\" EntityClass=\"" + cls + "\" EntityId=\"" + std::to_string(id) +
           "\" Layer=\"" + Attr(at.text, "Layer") + "\" >\n";
}

std::string Marker(const Block& waypoint, uint32_t id, bool active) {
    return EntityOpen(Attr(waypoint.text, "Name") + kMarkerSuffix, "ParticleEffect", waypoint, id, kMarkerAngles) +
           "\t\t\t<Properties bActive=\"" + (active ? "1" : "0") +
           "\" clrEntityColor=\"1,1,1\" bFeralVisionOnly=\"0\" Scale=\"1\" SpawnPeriod=\"1.5\" "
           "SpawnVariation=\"0\" >\n"
           "\t\t\t\t<ParticlesSystem effectParticleEffect=\"" + kMarkerEffect + "\" />\n"
           "\t\t\t</Properties>\n\t\t</Entity>\n";
}

std::string EndTrigger(const Block& waypoint, uint32_t id, const std::string& next_map) {
    std::string size = std::to_string(kExtractionSize);
    return EntityOpen(Attr(waypoint.text, "Name") + kTriggerSuffix, "EndMapTrigger", waypoint, id, "0,0,0") +
           "\t\t\t<Properties DimX=\"" + size + "\" DimY=\"" + size + "\" DimZ=\"10\" bEnabled=\"0\" Message=\"\" "
           "NextMap=\"" + next_map + "\" RestartDelay=\"0\" />\n\t\t</Entity>\n";
}

// the entity a link of "SetClosed" on the waypoint comes from, empty for none
std::string Closer(const std::vector<Block>& entities, const Block& waypoint) {
    std::string link = "Target=\"" + Attr(waypoint.text, "EntityId") + "\" Event=\"SetClosed\"";
    for (const Block& e : entities) {
        if (&e != &waypoint && e.text.find(link) != std::string::npos) return Attr(e.text, "Name");
    }
    return {};
}

std::string Goal(const std::vector<Block>& entities, const Block& waypoint) {
    std::string line = "\t\t{waypoint = \"" + Attr(waypoint.text, "Name") + "\"";
    std::string target = Closer(entities, waypoint);
    if (!target.empty()) line += ", target = \"" + target + "\"";
    if (Attr(waypoint.text, "bTriggeredOnEnter") == "1") line += ", reach = " + Attr(waypoint.text, "Radius");
    return line + "},\n";
}

std::string Script(const std::string& goals, const std::string& extraction) {
    return "-- written by the level editor as the level saves: the objectives watched, then the extraction\n"
           "\n"
           "LevelRules = {\n"
           "\tGoals = {\n" + goals + "\t},\n"
           "\tExtraction = \"" + extraction + "\",\n"
           "\tTrigger = \"" + extraction + kTriggerSuffix + "\",\n"
           "\tReach = " + std::to_string(kExtractionReach) + ",\n"
           "\tClosed = 3,\n"
           "}\n"
           "\n"
           "function LevelRules:Near(entity, radius)\n"
           "\tlocal player = Game:GetLocalPlayer()\n"
           "\tif not player then return nil end\n"
           "\tlocal p = player:GetPos()\n"
           "\tlocal px = p.x\n"
           "\tlocal py = p.y\n"
           "\tp = entity:GetPos()\n"
           "\tlocal dx = px - p.x\n"
           "\tlocal dy = py - p.y\n"
           "\treturn dx * dx + dy * dy <= radius * radius\n"
           "end\n"
           "\n"
           "function LevelRules:Done(goal, waypoint)\n"
           "\tif goal.reach and self:Near(waypoint, goal.reach) then return 1 end\n"
           "\tif not goal.target then return nil end\n"
           "\tlocal target = System:GetEntityByName(goal.target)\n"
           "\tif not target then return nil end\n"
           "\tif target.IsDeath then return target.IsDeath ~= 0 end\n"
           "\treturn target.currentStage and target.currentStage > 0\n"
           "end\n"
           "\n"
           "function LevelRules:Watch()\n"
           "\tlocal open = 0\n"
           "\tlocal i = 1\n"
           "\twhile self.Goals[i] do\n"
           "\t\tlocal goal = self.Goals[i]\n"
           "\t\tlocal waypoint = System:GetEntityByName(goal.waypoint)\n"
           "\t\tif waypoint and waypoint.Properties.selStatus ~= self.Closed then\n"
           "\t\t\tif self:Done(goal, waypoint) then\n"
           "\t\t\t\twaypoint:Event_SetClosed(waypoint)\n"
           "\t\t\telse\n"
           "\t\t\t\topen = open + 1\n"
           "\t\t\tend\n"
           "\t\tend\n"
           "\t\ti = i + 1\n"
           "\tend\n"
           "\tif open > 0 then return end\n"
           "\tlocal extraction = System:GetEntityByName(self.Extraction)\n"
           "\tlocal trigger = System:GetEntityByName(self.Trigger)\n"
           "\tif not extraction or not trigger then return end\n"
           "\tif extraction.Properties.selStatus == 0 then\n"
           "\t\textraction:Event_SetCurrent(extraction)\n"
           "\t\ttrigger:Event_Enable(trigger)\n"
           "\telseif not self.Leaving and self:Near(extraction, self.Reach) then\n"
           "\t\tself.Leaving = 1\n"
           "\t\ttrigger:Event_Trigger(trigger)\n"
           "\tend\n"
           "end\n"
           "\n"
           "function LevelRules:OnEvent(id, param)\n"
           "\tself:Watch()\n"
           "\tGame:SetTimer(LevelRules, 1000)\n"
           "end\n"
           "\n"
           "Game:SetTimer(LevelRules, 1000)\n";
}

}  // namespace

ObjectiveRules fcip::editor::AddObjectiveRules(const std::string& mission, const std::string& extraction,
                                               const std::string& next_map) {
    std::vector<Block> entities = Entities(mission);
    uint32_t id = 0;
    for (const Block& e : entities) {
        id = std::max(id, uint32_t(std::strtoul(Attr(e.text, "EntityId").c_str(), nullptr, 10)));
    }
    std::string out;
    std::string added;
    std::string goals;
    bool has_extraction = false;
    size_t copied = 0;
    for (const Block& e : entities) {
        if (Attr(e.text, "EntityClass") != "Waypoint" || Attr(e.text, "WaypointTag").empty()) continue;
        std::string waypoint = e.text;
        std::string name = Attr(waypoint, "Name");
        bool is_extraction = !extraction.empty() && name == extraction;
        if (is_extraction) {
            has_extraction = true;
            SetAttr(waypoint, "selStatus", kAbsent);
            SetAttr(waypoint, "enumStatus", kAbsent);
            added += EndTrigger(e, ++id, next_map);
        } else {
            goals += Goal(entities, e);
        }
        if (waypoint.find(kMarkerLink) == std::string::npos) {
            uint32_t marker = ++id;
            added += Marker(e, marker, !is_extraction && Attr(waypoint, "selStatus") == kCurrent);
            AddEvents(waypoint, EventTarget(marker, "Enable", "SetCurrent") +
                                    EventTarget(marker, "Disable", "SetClosed"));
        }
        out += mission.substr(copied, e.begin - copied) + waypoint;
        copied = e.end;
    }
    out += mission.substr(copied);
    size_t objects = out.rfind("</Objects>");
    if (objects != std::string::npos) out.insert(out.rfind('\n', objects) + 1, added);
    return {out, has_extraction ? Script(goals, extraction) : std::string()};
}
