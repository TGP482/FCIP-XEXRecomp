// ige.xex, the solo level editor: CREATE A NEW LEVEL makes a solo level from the template picked, laid out as a
// campaign level is so every editor stage works on it. Its terrain is a campaign level's 16x16 sectors
// (HeightmapSize 512, which 82469E50 doubles into metres): the template's 4x4 keep their files and coordinates, the
// others are copies of its empty seabed sector numbered and placed for their spot, and terrain.sdat is laid out from
// them as 821099A0 writes it. Its mission is the template's Daytime one made solo: the campaign's player equip pack
// and every campaign equip pack its enemies may name, the campaign's weapons and formation templates, an
// EnclosingArea over the map in place of the multiplayer FenceArea, a player start on the island, its own
// Mission.lua as the mission script and its own string table.
#include "leveleditor/new_level.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <set>

#include "leveleditor/campaign_levels.h"

using fcip::editor::LevelFile;
using fcip::editor::ReadGameFile;

namespace {

using Bytes = std::vector<uint8_t>;

constexpr const char* kTemplateMission = "Mission_Daytime.xml";  // the mission 821124B0 starts a template with
constexpr const char* kCampaignMission = "Mission_Mission0.xml";
constexpr const char* kSoloSource = "01_BEACH";  // the campaign level whose player pack and weapons a new level takes
constexpr const char* kCopiedFiles[] = {"Brush.lst", "vegbank.dat", "moviedata.xml", "GraphManager.bai", "DPVS.dat"};

constexpr uint32_t kSectorMetres = 64;
constexpr uint32_t kHeightmapSize = 512;
constexpr uint32_t kSectorsPerSide = kHeightmapSize * 2 / kSectorMetres;
constexpr uint32_t kSide = kSectorsPerSide * kSectorMetres + 1;  // terrain.sdat's vertices a row
// sd%d.sdat: the sector's number (low 16 bits) and its brushes
constexpr uint32_t kSectorNumber = 0x0C;
constexpr uint32_t kSectorBrushes = 0x14;
// ShortRange%d.sdat: the sector's origin, then past its objects "FLAO", a byte a sector saying what streams from this
// one, some fields, and the collision walls 823747E0 makes: a count, then x1, y1, x2, y2, bottom, top, bottom, top
constexpr uint32_t kShortRangeX = 0x04;
constexpr uint32_t kShortRangeY = 0x08;
constexpr char kStreamMark[] = "FLAO";
constexpr size_t kMarkBytes = 4;
constexpr size_t kStreamTableBytes = 0x100;
constexpr size_t kWallCount = 0x21;  // from the table's end
constexpr size_t kWallBytes = 32;
constexpr size_t kWallBottom = 16;
constexpr size_t kWallTop = 20;
constexpr uint8_t kStreamDrawn = 0x7F;
constexpr uint8_t kStreamLoaded = 0x08;
constexpr uint32_t kDrawnRing = 3;  // sectors around one drawn from it
constexpr uint32_t kLoadedRing = 5;
// vegbank.dat: the far vegetation sprites 8236C0D0 loads, sprites\sprite%03d.xbt each
constexpr size_t kSpriteCount = 0x10;
// terrain.sdat: a little endian header, then big endian 1, the side and the vertices as the terrain keeps them
constexpr uint32_t kTerrainMagic = 0x54524E4B;
constexpr uint32_t kTerrainHeaderBytes = 0x14;
constexpr uint32_t kTerrainVersion = 0x19;
constexpr uint32_t kTerrainSide = 0x18;
constexpr uint32_t kTerrainVertices = 0x1C;
constexpr uint32_t kVertexBytes = 16;
constexpr uint32_t kVertexHeight = 2;  // u16, 1/128 m
constexpr float kHeightScale = 1.0f / 128.0f;
constexpr float kAreaInset = 1.0f;  // the EnclosingArea's corners from the map's edge, metres

// a sector's four files, in the order NewLevelFiles names them
constexpr const char* kSectorFiles[] = {"sdat\\sd%u.sdat", "sdat\\ShortRange%u.sdat", "sdat\\sd%u_diffuse_hi.xbt",
                                        "sdat\\ShortRange%u_shadow_hi.xbt"};
constexpr int kSd = 0;
constexpr int kShortRange = 1;
using Sector = std::array<Bytes, std::size(kSectorFiles)>;

// the theme's ambience bank, as 821124B0 picks it by the template's name
struct Ambience {
    const char* theme;
    const char* bank;
};
constexpr Ambience kAmbiences[] = {
    {"BEACH", "Amb_ME_Beach"},
    {"JUNGLE", "Amb_ME_Jungle"},
    {"SWAMP", "Amb_ME_Swamp"},
    {"FOREST", "FC15_Amb_ME_Forest"},
};

uint32_t Le32(const Bytes& b, size_t at) {
    return uint32_t(b[at]) | uint32_t(b[at + 1]) << 8 | uint32_t(b[at + 2]) << 16 | uint32_t(b[at + 3]) << 24;
}
uint32_t Be32(const Bytes& b, size_t at) {
    return uint32_t(b[at]) << 24 | uint32_t(b[at + 1]) << 16 | uint32_t(b[at + 2]) << 8 | uint32_t(b[at + 3]);
}
void PutLe32(Bytes& b, uint32_t v) {
    for (int i = 0; i < 4; i++) b.push_back(uint8_t(v >> (i * 8)));
}
void PutBe32(Bytes& b, uint32_t v) {
    for (int i = 3; i >= 0; i--) b.push_back(uint8_t(v >> (i * 8)));
}
void SetLe16(Bytes& b, size_t at, uint32_t v) {
    b[at] = uint8_t(v);
    b[at + 1] = uint8_t(v >> 8);
}
float LeFloat(const Bytes& b, size_t at) {
    uint32_t v = Le32(b, at);
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}
void SetLeFloat(Bytes& b, size_t at, float f) {
    uint32_t v;
    std::memcpy(&v, &f, 4);
    for (int i = 0; i < 4; i++) b[at + i] = uint8_t(v >> (i * 8));
}

std::string Text(const Bytes& b) { return {b.begin(), b.end()}; }
Bytes Data(const std::string& s) { return {s.begin(), s.end()}; }

std::string Upper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::toupper(c)); });
    return s;
}

std::string Format(const char* pattern, uint32_t n) {
    char text[64];
    std::snprintf(text, sizeof(text), pattern, n);
    return text;
}

std::string Number(float f) {
    char text[32];
    std::snprintf(text, sizeof(text), "%g", f);
    return text;
}

// XML as the game's files write it: an element from its "<tag" to its "/>" or "</tag>", none nested in itself
size_t ElementEnd(const std::string& text, const std::string& tag, size_t begin) {
    size_t gt = text.find('>', begin);
    if (gt == std::string::npos) return gt;
    if (text[gt - 1] == '/') return gt + 1;
    std::string close = "</" + tag + ">";
    size_t at = text.find(close, gt);
    return at == std::string::npos ? at : at + close.size();
}

std::vector<std::string> Elements(const std::string& text, const std::string& tag) {
    std::vector<std::string> out;
    std::string open = "<" + tag;
    for (size_t at = text.find(open); at != std::string::npos; at = text.find(open, at + 1)) {
        char next = at + open.size() < text.size() ? text[at + open.size()] : 0;
        if (next != ' ' && next != '>' && next != '/' && next != '\t') continue;
        size_t end = ElementEnd(text, tag, at);
        if (end == std::string::npos) break;
        out.push_back(text.substr(at, end - at));
    }
    return out;
}

std::string Element(const std::string& text, const std::string& tag) {
    std::vector<std::string> all = Elements(text, tag);
    return all.empty() ? std::string() : all.front();
}

std::string Attribute(const std::string& element, const std::string& key) {
    std::string open = " " + key + "=\"";
    size_t at = element.find(open);
    if (at == std::string::npos || at > element.find('>')) return {};
    at += open.size();
    return element.substr(at, element.find('"', at) - at);
}

// a level's file, empty when it has none
Bytes LevelFileOf(const ReadGameFile& read, const std::string& level, const std::string& file) {
    Bytes data;
    if (!read("levels\\" + level + "\\" + file, data)) data.clear();
    return data;
}

// the campaign's levels, the solo source first
std::vector<std::string> CampaignOrder() {
    std::vector<std::string> levels{kSoloSource};
    for (const char* level : fcip::editor::kCampaignLevels) {
        if (level != levels.front()) levels.push_back(level);
    }
    return levels;
}

// every element of a tag the campaign's missions hold, the first of each name
std::string UnionOf(const std::vector<std::string>& missions, const std::string& tag, const std::string& indent) {
    std::set<std::string> names;
    std::string out;
    for (const std::string& mission : missions) {
        for (const std::string& e : Elements(mission, tag)) {
            if (names.insert(Attribute(e, "name")).second) out += indent + e + "\n";
        }
    }
    return out;
}

struct Template {
    uint32_t side = 0;     // terrain.sdat's vertices a row
    uint32_t sectors = 0;  // a side
    Bytes terrain;
    std::vector<Sector> sector;
    uint32_t seabed = 0;
    bool walls = false;  // its edge has collision walls, from wall_bottom to wall_top
    float wall_bottom = 0;
    float wall_top = 0;
};

// a ShortRange file's wall count, npos when the file does not hold its table and walls
size_t WallCountAt(const Bytes& b) {
    auto mark = std::find_end(b.begin(), b.end(), kStreamMark, kStreamMark + kMarkBytes);
    if (mark == b.end()) return std::string::npos;
    size_t at = size_t(mark - b.begin()) + kMarkBytes + kStreamTableBytes + kWallCount;
    if (at + 4 > b.size() || at + 4 + Le32(b, at) * kWallBytes > b.size()) return std::string::npos;
    return at;
}

bool ReadTemplate(const ReadGameFile& read, const std::string& base, Template& t) {
    t.terrain = LevelFileOf(read, base, "sdat\\terrain.sdat");
    if (t.terrain.size() < kTerrainVertices) return false;
    t.side = Be32(t.terrain, kTerrainSide);
    t.sectors = (t.side - 1) / kSectorMetres;
    size_t vertices = size_t(t.side) * t.side * kVertexBytes;
    if (t.sectors == 0 || t.sectors > kSectorsPerSide || t.terrain.size() != kTerrainVertices + vertices) return false;
    t.sector.resize(size_t(t.sectors) * t.sectors);
    for (uint32_t i = 0; i < t.sector.size(); i++) {
        for (size_t f = 0; f < std::size(kSectorFiles); f++) {
            t.sector[i][f] = LevelFileOf(read, base, Format(kSectorFiles[f], i));
            if (t.sector[i][f].empty()) return false;
        }
        const Bytes& short_range = t.sector[i][kShortRange];
        size_t walls = WallCountAt(short_range);
        if (t.sector[i][kSd].size() <= kSectorBrushes + 4 || walls == std::string::npos) return false;
        if (!t.walls && Le32(short_range, walls) != 0) {
            t.walls = true;
            t.wall_bottom = LeFloat(short_range, walls + 4 + kWallBottom);
            t.wall_top = LeFloat(short_range, walls + 4 + kWallTop);
        }
    }
    // its empty seabed: no brushes and the fewest short range objects
    auto size = [&](uint32_t i) { return t.sector[i][kShortRange].size(); };
    bool found = false;
    for (uint32_t i = 0; i < t.sector.size(); i++) {
        if (Le32(t.sector[i][kSd], kSectorBrushes) != 0) continue;
        if (!found || size(i) < size(t.seabed)) t.seabed = i;
        found = true;
    }
    return found;
}

// the new sector at x, y is one of the template's own
bool OwnSector(const Template& t, uint32_t x, uint32_t y) { return x < t.sectors && y < t.sectors; }

uint8_t StreamState(uint32_t ring) {
    if (ring <= kDrawnRing) return kStreamDrawn;
    if (ring <= kLoadedRing) return kStreamLoaded;
    return 0;
}

uint32_t Apart(uint32_t a, uint32_t b) { return a > b ? a - b : b - a; }

struct Wall {
    float x1;
    float y1;
    float x2;
    float y2;
};

// the walls along the map's edge where the sector at x, y meets it, as the template's along its own
std::vector<Wall> EdgeWalls(uint32_t x, uint32_t y) {
    float x0 = float(x * kSectorMetres);
    float y0 = float(y * kSectorMetres);
    float x1 = x0 + kSectorMetres;
    float y1 = y0 + kSectorMetres;
    float low = kAreaInset;
    float high = float(kSide - 1) - kAreaInset;
    uint32_t last = kSectorsPerSide - 1;
    std::vector<Wall> walls;
    if (y == 0) walls.push_back({x0, low, x1, low});
    if (y == last) walls.push_back({x0, high, x1, high});
    if (x == 0) walls.push_back({low, y0, low, y1});
    if (x == last) walls.push_back({high, y0, high, y1});
    return walls;
}

// the sector's ShortRange file with what streams from it and its walls for its spot in the new level: a copied seabed
// keeps the seabed's, and the template's own were made for its 4x4 sectors
Bytes PlacedShortRange(const Bytes& file, uint32_t x, uint32_t y, const Template& t) {
    size_t count = WallCountAt(file);
    size_t table = count - kWallCount - kStreamTableBytes;
    size_t rest = count + 4 + Le32(file, count) * kWallBytes;
    Bytes out(file.begin(), file.begin() + table);
    for (uint32_t i = 0; i < kStreamTableBytes; i++) {
        uint32_t ring = std::max(Apart(i % kSectorsPerSide, x), Apart(i / kSectorsPerSide, y));
        out.push_back(StreamState(ring));
    }
    out.insert(out.end(), file.begin() + table + kStreamTableBytes, file.begin() + count);
    std::vector<Wall> walls = t.walls ? EdgeWalls(x, y) : std::vector<Wall>();
    PutLe32(out, uint32_t(walls.size()));
    for (const Wall& w : walls) {
        for (float f : {w.x1, w.y1, w.x2, w.y2, t.wall_bottom, t.wall_top, t.wall_bottom, t.wall_top}) {
            uint32_t v;
            std::memcpy(&v, &f, 4);
            PutLe32(out, v);
        }
    }
    out.insert(out.end(), file.begin() + rest, file.end());
    return out;
}

void AddSectors(const Template& t, std::vector<LevelFile>& files) {
    for (uint32_t y = 0; y < kSectorsPerSide; y++) {
        for (uint32_t x = 0; x < kSectorsPerSide; x++) {
            bool own = OwnSector(t, x, y);
            Sector s = t.sector[own ? y * t.sectors + x : t.seabed];
            uint32_t number = y * kSectorsPerSide + x;
            SetLe16(s[kSd], kSectorNumber, number);
            SetLeFloat(s[kShortRange], kShortRangeX, float(x * kSectorMetres));
            SetLeFloat(s[kShortRange], kShortRangeY, float(y * kSectorMetres));
            s[kShortRange] = PlacedShortRange(s[kShortRange], x, y, t);
            for (size_t f = 0; f < s.size(); f++) {
                files.push_back({Format(kSectorFiles[f], number), std::move(s[f])});
            }
        }
    }
}

// a vertex of the new terrain from the template: its own where its sectors lie, else the seabed's at the same spot in
// the sector, the sector of the last row and column being the one before as 823255E8 takes it
const uint8_t* VertexAt(const Template& t, uint32_t x, uint32_t y) {
    uint32_t sx = std::min(x, kSide - 2) / kSectorMetres;
    uint32_t sy = std::min(y, kSide - 2) / kSectorMetres;
    if (!OwnSector(t, sx, sy)) {
        x = t.seabed % t.sectors * kSectorMetres + (x - sx * kSectorMetres);
        y = t.seabed / t.sectors * kSectorMetres + (y - sy * kSectorMetres);
    }
    return t.terrain.data() + kTerrainVertices + (size_t(y) * t.side + x) * kVertexBytes;
}

float HeightAt(const Template& t, uint32_t x, uint32_t y) {
    const uint8_t* v = VertexAt(t, x, y) + kVertexHeight;
    return float(uint32_t(v[0]) << 8 | v[1]) * kHeightScale;
}

Bytes TerrainFile(const Template& t) {
    Bytes b;
    b.reserve(kTerrainVertices + size_t(kSide) * kSide * kVertexBytes);
    for (uint32_t v : {kTerrainMagic, kTerrainHeaderBytes, kTerrainVersion}) PutLe32(b, v);
    PutLe32(b, kSectorsPerSide * kSectorsPerSide);
    PutLe32(b, kSectorsPerSide * kSectorsPerSide);
    PutBe32(b, 1);
    PutBe32(b, kSide);
    for (uint32_t y = 0; y < kSide; y++) {
        for (uint32_t x = 0; x < kSide; x++) {
            const uint8_t* v = VertexAt(t, x, y);
            b.insert(b.end(), v, v + kVertexBytes);
        }
    }
    return b;
}

// an attribute's value in the element starting at begin replaced, false when it has none
bool SetAttribute(std::string& text, size_t begin, const std::string& key, const std::string& value) {
    size_t close = text.find('>', begin);
    std::string open = " " + key + "=\"";
    size_t at = text.find(open, begin);
    if (at == std::string::npos || at > close) return false;
    at += open.size();
    text.replace(at, text.find('"', at) - at, value);
    return true;
}

// the template's LevelData.xml at the campaign's size, named for the level and naming the template its assets come
// from, its missions the one Mission0 (82138EB0 finds the mission StartLevel names here, else takes the first)
std::string LevelDataText(std::string text, const std::string& name, const std::string& base) {
    size_t info = text.find("<LevelInfo");
    std::string missions = Element(text, "Missions");
    if (info == std::string::npos || missions.empty()) return {};
    SetAttribute(text, info, "HeightmapSize", std::to_string(kHeightmapSize));
    SetAttribute(text, info, "Name", name);
    text.insert(text.find("/>", info), "CampaignLevel=\"" + base + "\" ");
    std::string own = "<Missions>\n\t\t<Mission Name=\"Mission0\" File=\"" + std::string(kCampaignMission) +
                      "\" />\n\t</Missions>";
    text.replace(text.find(missions), missions.size(), own);
    return text;
}

std::string PointText(float x, float y, float z) { return Number(x) + "," + Number(y) + "," + Number(z); }

// the player start on the template's island and an EnclosingArea over the map for the AI
std::string ObjectsText(const Template& t) {
    float middle = float(t.sectors * kSectorMetres / 2);
    float start_z = HeightAt(t, uint32_t(middle), uint32_t(middle));
    float low = kAreaInset;
    float high = float(kSide - 1) - kAreaInset;
    float z = HeightAt(t, 0, 0);
    std::string corners[] = {PointText(low, low, z), PointText(high, low, z), PointText(high, high, z),
                             PointText(low, high, z)};
    std::string out = "\t<Objects>\n";
    out += "\t\t<Object Type=\"Respawn\" Name=\"Respawn0\" Pos=\"" + PointText(middle, middle, start_z) +
           "\" Angles=\"0,0,0\" Action=\"\" />\n";
    out += "\t\t<Object Type=\"EnclosingArea\" Name=\"EnclosingArea0\" Pos=\"" + corners[0] +
           "\" Width=\"0\" Height=\"0\" AreaId=\"0\" GroupId=\"0\" Closed=\"1\" Indoor=\"0\" "
           "SoundOcclusionFactor=\"0\" AudibilityAreaID=\"0\" ActivateWithBino=\"0\" >\n";
    out += "\t\t\t<Points>\n";
    for (const std::string& c : corners) out += "\t\t\t\t<Point Pos=\"" + c + "\" />\n";
    out += "\t\t\t</Points>\n\t\t</Object>\n\t</Objects>\n";
    return out;
}

std::string MissionText(const std::string& name, const std::string& tpl, const std::vector<std::string>& campaign,
                        const Template& t) {
    std::string open = tpl.substr(0, tpl.find('>') + 1);
    std::string out = "<Mission Name=\"Mission0\" Description=\"\" Time=\"" + Attribute(open, "Time") +
                      "\" PlayerEquipPack=\"Player\" Script=\"\\Levels\\" + name + "\\Mission.lua\" >\n";
    out += "\t<EquipPacks>\n" + UnionOf(campaign, "EquipPack", "\t\t") + "\t</EquipPacks>\n";
    out += "\t" + Element(tpl, "Environment") + "\n";
    out += "\t" + Element(campaign.front(), "Weapons") + "\n";
    out += "\t" + Element(tpl, "Formations") + "\n";
    out += "\t" + Element(tpl, "EntityDescriptions") + "\n";
    out += ObjectsText(t);
    out += "\t" + Element(tpl, "EntityLibrary") + "\n";
    std::string templates = UnionOf(campaign, "formation_template", "\t\t");
    out += "\t<FormationTemplatesLibrary>\n" + templates + "\t</FormationTemplatesLibrary>\n";
    out += "</Mission>\n";
    return out;
}

// a whole mission script, as the campaign's are: MissionTools adds the mission calls to Mission after it runs, and
// the editor runs it again on a change, so Mission is kept
std::string ScriptText(const std::string& name, const std::string& base) {
    std::string bank;
    for (const Ambience& a : kAmbiences) {
        if (bank.empty() && Upper(base).find(a.theme) != std::string::npos) bank = a.bank;
    }
    std::string out = "-- " + name + ": the level's mission script, run as the level loads.\n\n"
                      "Mission = Mission or {}\n\n"
                      "function Mission:OnInit()\n"
                      "\tLanguage:LoadMissionStringTable(\"" + name + ".xml\")\n";
    if (!bank.empty()) out += "\tSound:LoadBank(\"" + bank + "\")\n";
    out += "end\n\n"
           "function Mission:OnUpdate()\n"
           "end\n\n"
           "function Mission:Finish()\n"
           "end\n";
    return out;
}

// the sections the Objectives tab and the Waypoint entities read
std::string StringTableText() {
    return "<stringtable language=\"english\">\n\n"
           "\t<section name=\"Completions\">\n"
           "\t</section>\n\n"
           "\t<section name=\"Waypoints\">\n"
           "\t</section>\n\n"
           "</stringtable>\n";
}

}  // namespace

std::vector<std::string> fcip::editor::SpriteFiles(const std::vector<uint8_t>& vegbank) {
    if (vegbank.size() < kSpriteCount + 4) return {};
    std::vector<std::string> files{"sprites\\clusters.xbt"};
    for (uint32_t i = 0; i < Le32(vegbank, kSpriteCount); i++) files.push_back(Format("sprites\\sprite%03u.xbt", i));
    return files;
}

std::vector<fcip::editor::LevelFile> fcip::editor::NewLevelFiles(const std::string& name, const std::string& base,
                                                                const ReadGameFile& read) {
    Template t;
    std::string data = Text(LevelFileOf(read, base, "LevelData.xml"));
    std::string mission = Text(LevelFileOf(read, base, kTemplateMission));
    std::vector<std::string> campaign;
    for (const std::string& level : CampaignOrder()) {
        campaign.push_back(Text(LevelFileOf(read, level, kCampaignMission)));
    }
    std::string level_data = LevelDataText(data, name, base);
    if (level_data.empty() || mission.empty() || campaign.front().empty() || !ReadTemplate(read, base, t)) return {};

    std::vector<LevelFile> files;
    files.push_back({"LevelData.xml", Data(level_data)});
    files.push_back({kCampaignMission, Data(MissionText(name, mission, campaign, t))});
    files.push_back({"Mission.lua", Data(ScriptText(name, base))});
    files.push_back({name + ".xml", Data(StringTableText())});
    for (const char* file : kCopiedFiles) {
        Bytes copy = LevelFileOf(read, base, file);
        if (!copy.empty()) files.push_back({file, std::move(copy)});
    }
    for (const std::string& file : SpriteFiles(LevelFileOf(read, base, "vegbank.dat"))) {
        Bytes copy = LevelFileOf(read, base, file);
        if (!copy.empty()) files.push_back({file, std::move(copy)});
    }
    files.push_back({"sdat\\terrain.sdat", TerrainFile(t)});
    AddSectors(t, files);
    return files;
}
