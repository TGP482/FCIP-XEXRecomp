// Ambient occlusion over the world only. The renderer's pass submit draws one list, radix sorted on key bits 0 to 19:
// the layer in bits 16 to 19, then the pipeline in bits 11 to 15, or the depth in the layers drawn far to near (5, 6,
// 9, 10 and 13 by table 0x8204AEB8; the ocean is on 7 and 11). In the camera's pass the characters (skinned items and
// the characters' pipeline) move to the end of layer 4 and occlusion goes in before the first item past the world, so
// the characters, the blended layers and the ocean land on top. The fog is linear over view distance, from the
// renderer's constant block. RTAO traces the world's draws from the start of the camera's list to that point. Global
// illumination traces every world layer, the characters included, and is added after the list's last item wherever a
// draw left its albedo, the first person weapon's past the world too but not the ocean's. Volumetric fog replaces the
// game's: the list draws without it, then the fog goes in through its own sun map and the terrain's heights before the
// first item on the camera's side of the water, layer 9 on. Those are mostly blended and leave no depth for the fog to
// read, so each of their pixels takes the fog in front of it as it draws, until the list ends. Volumetric clouds go in
// after the sky box's draw (82372698, the Sky box pipeline's item 82508720), in the camera's pass and the water's
// reflections, over the pixels nothing but sky covers. The box turns about the world's up axis by the degrees 825EE578
// hands it at renderer +0x92A4, RotationSpeed (engine +0xA2C) times the render time plus SkyBoxAngle (+0xA28), and the
// clouds turn by the same. The level's Rain node (+0xB60), set by the StartRain and StopRain script calls, thickens and
// darkens them over kRainFade. Caustics go in before the camera's list draws its first water item, the Ocean
// pipeline's (Ocean.fx) or the Lake pipeline's (lakes and rivers, Lake.fx, layer 8), and each water item takes them
// through the screen it shows behind it; with the camera under the sea they go straight onto what lies below it, before
// the first item past the world. The ocean's layers hold the quads over and under the water, not the water itself.
#include <algorithm>
#include <bit>
#include <cmath>
#include <utility>

#include "common/render_pass.h"
#include "common/settings.h"
#include "core/log.h"
#include "d3d/occlusion.h"
#include "game/fpsfixes.h"
#include "game/motion_vectors.h"
#include "game/ocean_reflection.h"
#include "game/scene.h"
#include "game/terrain_shade.h"
#include "kernel/memory.h"
#include "xdk/hle.h"

using namespace kernel;
using namespace fcip::scene;

namespace {

// renderer
constexpr uint32_t kRendererPipelines = 0x10;  // the pipeline registry
// shader constant block, as the pass submit fills it
constexpr uint32_t kBlockFogScale = 0x21D4;  // fog amount / (end - start)
constexpr uint32_t kBlockFogBias = 0x21D8;   // -scale * start
constexpr uint32_t kBlockFogColour = 0x21E4;  // r, g, b from 0 to 1
// pipeline registry 82503538: entries of 0x18 bytes from +8 to +0xC, the name second
constexpr uint32_t kRegistryFirst = 0x8;
constexpr uint32_t kRegistryEnd = 0xC;
constexpr uint32_t kEntrySize = 0x18;
constexpr uint32_t kEntryName = 0x4;
constexpr uint32_t kEntryPipeline = 0x8;
constexpr uint32_t kCharactersName = 0x82070D10;  // "PerPixel Phong Color (Characters)"
constexpr uint32_t kOceanName = 0x8207D1C0;       // "Ocean"
constexpr uint32_t kLakeName = 0x82070D80;        // "Lake"
constexpr uint32_t kNoPipeline = 0xFFFFFFFF;
// pipeline: vtable first, the item draw at +0xC; the PerPixel Phong draws all end in 8250DF28
constexpr uint32_t kPipelineDraw = 0xC;
constexpr uint32_t kPhongDraws[] = {0x8250DF28, 0x82504FB8, 0x825050F0, 0x825052B0};
// render item: its mesh, which 8250DF28 draws SKINNED when the byte at +0x40 is set
constexpr uint32_t kItemMesh = 0x18;
constexpr uint32_t kMeshSkinned = 0x40;
// Cry3DEngine: the flat ambient its objects take, as 825D1AB0 builds it less the sun it adds for particles
constexpr uint32_t kEngineVisAreas = 0x37C;  // VisArea_Find's first argument
constexpr uint32_t kEngineOutdoorAmbient = 0xB50;
constexpr uint32_t kEngineAmbientScale = 0x11FC;
constexpr uint32_t kEngineAmbientTint = 0x1200;
constexpr uint32_t kEngineFade = 0x1424;  // its scale at +0x440
constexpr uint32_t kFadeScale = 0x440;
constexpr uint32_t kVisAreaAmbient = 0xDC;
constexpr uint32_t kAmbientFloor = 0xC;  // past the outdoor addition at Ambient_pointer
constexpr uint32_t kEngineSunRender = 0x51C;  // SunVectorRender as a point 10000 units out, the sky's sun
constexpr uint32_t kEngineRain = 0xB60;       // the Rain node's switch, a byte
// renderer: what 82371DD0 keeps for the sky box's draw
constexpr uint32_t kRendererSkyTurn = 0x92A4;  // degrees about the world's up axis
constexpr uint32_t kRendererSkyQueued = 0x92D4;  // a byte, set when the frame hands the sky box a draw
// device constant shadows, 16 bytes a register: GlobalVariables.fx's WaterLevel and SunLight colour
constexpr uint32_t kDevVsConstants = 0x780;
constexpr uint32_t kDevPsConstants = 0x1780;
constexpr uint32_t kVsWaterLevel = 129;
constexpr uint32_t kPsSunColour = 9;
// keys
constexpr uint32_t kPipelineShift = 11;
constexpr uint32_t kLastPipeline = 0x1F;
constexpr uint32_t kLayerAndPipeline = 0xFF800;  // key bits 11 to 19
constexpr uint32_t kMovedKey = kLastWorldLayer << kLayerShift | kLastPipeline << kPipelineShift;
// 82367D68 and the grass put what lies on the camera's side of the water from 9 on, the far side below
constexpr uint32_t kFirstNearLayer = 9;

// the registry's pipelines whose items may be characters
struct Pipelines {
    uint32_t characters = kLastPipeline;
    uint32_t phong = 0;  // a bit per pipeline
};

thread_local uint32_t t_sort_list = 0;  // the camera pass's list while its keys go in
thread_local Pipelines t_pipelines;
thread_local uint32_t t_draw_list = 0;  // the camera pass's list while it is drawn, until occlusion went in
thread_local uint32_t t_world_list = 0;  // the same while its world layers stream into global illumination's scene
thread_local uint32_t t_past_list = 0;   // the same past its world, whose draws but the ocean's keep their albedo
thread_local bool t_ocean = false;

// the camera pass's list until the volumetric fog went in, and the game's fog the list draws without
struct FogSplit {
    uint32_t list = 0;
    uint32_t scale = 0;
    uint32_t bias = 0;
};
thread_local FogSplit t_fog;

bool Enabled() { return fcip::Get().ambient_occlusion && fcip::SubmittedPass() == fcip::kPassCamera; }
bool Illuminated() { return fcip::Get().global_illumination && fcip::SubmittedPass() == fcip::kPassCamera; }
bool Fogged() { return fcip::Get().volumetric_fog && fcip::SubmittedPass() == fcip::kPassCamera; }
bool Caustic() { return fcip::Get().caustics && fcip::SubmittedPass() == fcip::kPassCamera; }

bool PhongDraw(uint32_t draw) {
    for (uint32_t d : kPhongDraws) {
        if (draw == d) return true;
    }
    return false;
}

// the characters' pipeline and the PerPixel Phong ones among the first kLastPipeline
Pipelines FindPipelines() {
    Pipelines p;
    uint32_t renderer = R32(SYM_Renderer_pointer);
    uint32_t registry = renderer ? R32(renderer + kRendererPipelines) : 0;
    if (!registry) return p;
    uint32_t first = R32(registry + kRegistryFirst);
    uint32_t end = R32(registry + kRegistryEnd);
    for (uint32_t i = 0; i < kLastPipeline && first + i * kEntrySize < end; i++) {
        uint32_t entry = first + i * kEntrySize;
        if (R32(entry + kEntryName) == kCharactersName) p.characters = i;
        uint32_t pipeline = R32(entry + kEntryPipeline);
        uint32_t vtable = pipeline ? R32(pipeline) : 0;
        if (vtable && PhongDraw(R32(vtable + kPipelineDraw))) p.phong |= 1u << i;
    }
    return p;
}

// the item drawn with this pipeline is a character
bool Character(uint32_t pipeline, uint32_t item) {
    if (pipeline == t_pipelines.characters) return true;
    if (!(t_pipelines.phong >> pipeline & 1)) return false;
    uint32_t mesh = R32(item + kItemMesh);
    return mesh && R8(mesh + kMeshSkinned);
}

// the item about to be popped lies past the world; the moved characters count as past it unless asked for
bool PastWorld(uint32_t list, bool characters) {
    uint32_t key = 0;
    if (!NextKey(list, key)) return false;
    uint32_t layer = key >> kLayerShift & kLayerMask;
    return layer > kLastWorldLayer || (!characters && (key & kLayerAndPipeline) == kMovedKey);
}

// a fog colour channel from 0 to 1 as a byte
uint32_t ColourByte(uint32_t addr) {
    float c = std::fmin(std::fmax(GF(addr), 0.0f), 1.0f);
    return uint32_t(std::lround(c * 255));
}

// the camera's projection and fog; false without a perspective camera
bool SceneArgs(gpu::OcclusionArgs& a) {
    uint32_t renderer = R32(SYM_Renderer_pointer);
    uint32_t camera = Camera();
    uint32_t block = renderer ? R32(renderer + kRendererBlock) : 0;
    if (!camera || !block) return false;
    uint32_t m = camera + kCameraProjection;
    float x_scale = GF(m);
    float y_scale = GF(m + 5 * 4);
    bool perspective = GF(m + 11 * 4) == -1.0f && GF(m + 15 * 4) == 0.0f;
    if (!perspective || x_scale == 0 || y_scale == 0) return false;
    a.proj[0] = 1 / x_scale;
    a.proj[1] = 1 / y_scale;
    a.proj[2] = -GF(m + 8 * 4);
    a.proj[3] = -GF(m + 9 * 4);
    // the pass draws with depth flipped, z = w - z (823B0580)
    a.m22 = GF(m + 11 * 4) - GF(m + 10 * 4);
    a.m32 = GF(m + 15 * 4) - GF(m + 14 * 4);
    a.fog_scale = GF(block + kBlockFogScale);
    a.fog_bias = GF(block + kBlockFogBias);
    uint32_t r = ColourByte(block + kBlockFogColour);
    uint32_t g = ColourByte(block + kBlockFogColour + 4);
    uint32_t b = ColourByte(block + kBlockFogColour + 8);
    a.fog_colour = r << 16 | g << 8 | b;
    return true;
}

void Occlude() {
    t_draw_list = 0;
    gpu::OcclusionArgs a;
    if (!SceneArgs(a)) return;
    a.method = fcip::Get().ambient_occlusion;
    d3d::Occlusion(R32(SYM_Device_pointer), a);
}

// the ambient at the camera, of the vis area holding it or outdoors
bool Ambient(PPCContext& ctx, float out[3]) {
    uint32_t engine = R32(SYM_C3DEngine_pointer);
    uint32_t camera = Camera();
    uint32_t outdoor = R32(SYM_Ambient_pointer);
    if (!engine || !camera || !outdoor) return false;
    static const uint32_t pos = mem::AllocSystem(16);
    // the view is row major: the camera sits at minus its translation through the rotation's transpose
    uint32_t v = camera + kCameraView;
    for (uint32_t i = 0; i < 3; i++) {
        float t = GF(v + 48) * GF(v + i * 16) + GF(v + 52) * GF(v + i * 16 + 4) + GF(v + 56) * GF(v + i * 16 + 8);
        W32(pos + i * 4, std::bit_cast<uint32_t>(-t));
    }
    // the front end's scenes have no vis areas: VisArea_Find reads its manager unchecked
    uint32_t areas = R32(engine + kEngineVisAreas);
    uint32_t area = areas ? uint32_t(CallGuest(ctx, SYM_VisArea_Find, {areas, pos})) : 0;
    float scale = GF(engine + kEngineAmbientScale);
    uint32_t fade = R32(engine + kEngineFade);
    if (fade) scale *= GF(fade + kFadeScale);
    for (uint32_t i = 0; i < 3; i++) {
        float base = GF(outdoor + i * 4) + GF(engine + kEngineOutdoorAmbient + i * 4);
        if (area) base = GF(area + kVisAreaAmbient + i * 4);
        float floor = GF(outdoor + kAmbientFloor + i * 4);
        out[i] = std::fmax(base * scale * GF(engine + kEngineAmbientTint + i * 4), floor);
    }
    static float logged[3] = {-1, -1, -1};
    if (std::fabs(out[0] - logged[0]) + std::fabs(out[1] - logged[1]) + std::fabs(out[2] - logged[2]) > 0.02f) {
        LOGR("global illumination: ambient %.3f %.3f %.3f, %s", out[0], out[1], out[2], area ? "vis area" : "outdoors");
        std::copy(out, out + 3, logged);
    }
    return true;
}

// the world ends where the first item past it is popped, else with the list; the ambient goes with it
void EndWorld(PPCContext& ctx) {
    t_past_list = std::exchange(t_world_list, 0);
    float ambient[3] = {};
    Ambient(ctx, ambient);
    d3d::IlluminationWorld(R32(SYM_Device_pointer), ambient);
}

// past the world, the ocean's layers clear the albedo the others write
void UpdateOcean(uint32_t list) {
    uint32_t key = 0;
    if (!NextKey(list, key)) return;
    bool ocean = kOceanLayers >> (key >> kLayerShift & kLayerMask) & 1;
    if (ocean == t_ocean) return;
    t_ocean = ocean;
    d3d::IlluminationOcean(R32(SYM_Device_pointer), ocean);
}

// the camera's view inverted, a rotation and a translation, with the renderer's z, the distance, turned back into the
// view's, which runs the other way: the renderer's view space into the world, row major
bool ViewToWorld(float out[16]) {
    uint32_t camera = Camera();
    if (!camera) return false;
    uint32_t v = camera + kCameraView;
    std::fill(out, out + 16, 0.0f);
    for (uint32_t i = 0; i < 3; i++) {
        for (uint32_t j = 0; j < 3; j++) out[i * 4 + j] = GF(v + (j * 4 + i) * 4);
        float t = GF(v + 48) * GF(v + i * 16) + GF(v + 52) * GF(v + i * 16 + 4) + GF(v + 56) * GF(v + i * 16 + 8);
        out[12 + i] = -t;
    }
    out[15] = 1;
    for (uint32_t j = 0; j < 3; j++) out[8 + j] = -out[8 + j];
    return true;
}

// the renderer's view space into the far sun map's clip space and up to the height over the sea, by column
void FogMatrices(gpu::FogView& v) {
    float world[16];
    float sun[16];
    if (!ViewToWorld(world)) return;
    uint32_t dev = R32(SYM_Device_pointer);
    float sea = GF(dev + kDevVsConstants + kVsWaterLevel * 16);
    for (uint32_t r = 0; r < 4; r++) v.height[r] = world[r * 4 + 2];
    v.height[3] -= sea;
    if (!fcip::SunClip(sun)) return;
    for (uint32_t r = 0; r < 4; r++) {
        for (uint32_t c = 0; c < 4; c++) {
            float sum = 0;
            for (uint32_t k = 0; k < 4; k++) sum += world[r * 4 + k] * sun[k * 4 + c];
            v.sun[c * 4 + r] = sum;
        }
    }
    v.shadowed = true;
}

// the sun as the receivers read its colour
gpu::FogView SunView() {
    gpu::FogView v;
    FogMatrices(v);
    uint32_t dev = R32(SYM_Device_pointer);
    for (uint32_t i = 0; i < 3; i++) v.sun_colour[i] = GF(dev + kDevPsConstants + kPsSunColour * 16 + i * 4);
    return v;
}

void Illuminate(PPCContext& ctx) {
    if (t_world_list) EndWorld(ctx);
    gpu::OcclusionArgs a;
    if (SceneArgs(a)) d3d::Illumination(R32(SYM_Device_pointer), a, SunView());
}

// over the camera's list drawn so far, lit by the sun, as thick as the game's fog
void Fog(const FogSplit& f) {
    gpu::OcclusionArgs a;
    if (!SceneArgs(a)) return;
    a.fog_scale = std::bit_cast<float>(f.scale);
    a.fog_bias = std::bit_cast<float>(f.bias);
    gpu::FogView v = SunView();
    uint32_t dev = R32(SYM_Device_pointer);
    float world[16];
    if (ViewToWorld(world)) fcip::UpdateTerrainShade(world, GF(dev + kDevVsConstants + kVsWaterLevel * 16), v);
    static float logged[3] = {-1, -1, -1};
    const float* c = v.sun_colour;
    if (std::fabs(c[0] - logged[0]) + std::fabs(c[1] - logged[1]) + std::fabs(c[2] - logged[2]) > 0.02f) {
        LOGR("volumetric fog: sun %.3f %.3f %.3f, sea level %.2f, %s", c[0], c[1], c[2],
             GF(dev + kDevVsConstants + kVsWaterLevel * 16), v.shadowed ? "sun map" : "no sun map");
        std::copy(c, c + 3, logged);
    }
    d3d::VolumetricFog(dev, a, v);
}

// the clouds' coverage, clear and in rain, and the seconds rain takes to build or clear
constexpr float kClearCoverage = 0.38f;
constexpr float kRainCoverage = 0.9f;
constexpr float kRainFade = 12.0f;
constexpr float kRadiansPerDegree = 3.14159265f / 180.0f;

// title thread: the weather the clouds show, eased toward the level's, and their wind's clock
struct Weather {
    float rain = 0;
    float seconds = 0;
    bool raining = false;
};
Weather g_weather;

bool Clouded() {
    uint32_t pass = fcip::SubmittedPass();
    return fcip::Get().volumetric_clouds && (pass == fcip::kPassCamera || pass <= fcip::kPassReflection);
}

// once a frame, in the camera's pass
void UpdateWeather(uint32_t engine) {
    float dt = float(fcip::FrameTime());
    g_weather.seconds += dt;
    bool raining = R8(engine + kEngineRain) != 0;
    if (raining != g_weather.raining) LOGR("volumetric clouds: rain %s", raining ? "starts" : "stops");
    g_weather.raining = raining;
    float step = dt / kRainFade;
    float target = raining ? 1.0f : 0.0f;
    g_weather.rain += std::clamp(target - g_weather.rain, -step, step);
}

// toward the sun in the world, as the sky draws it
void SunToward(uint32_t engine, float out[3]) {
    float sun[3];
    for (uint32_t i = 0; i < 3; i++) sun[i] = GF(engine + kEngineSunRender + i * 4);
    float length = std::sqrt(sun[0] * sun[0] + sun[1] * sun[1] + sun[2] * sun[2]);
    if (length <= 0) {
        sun[2] = 1;
        length = 1;
    }
    for (uint32_t i = 0; i < 3; i++) out[i] = sun[i] / length;
}

// over the sky box just drawn, turned as it is
void Clouds(uint32_t renderer) {
    uint32_t engine = R32(SYM_C3DEngine_pointer);
    gpu::OcclusionArgs a;
    float world[16];
    if (!engine || !SceneArgs(a) || !ViewToWorld(world)) return;
    if (fcip::SubmittedPass() == fcip::kPassCamera) UpdateWeather(engine);
    gpu::CloudView v;
    for (uint32_t r = 0; r < 3; r++) {
        for (uint32_t c = 0; c < 3; c++) v.to_world[r * 4 + c] = world[r * 4 + c];
    }
    SunToward(engine, v.sun);
    uint32_t dev = R32(SYM_Device_pointer);
    for (uint32_t i = 0; i < 3; i++) v.sun_colour[i] = GF(dev + kDevPsConstants + kPsSunColour * 16 + i * 4);
    float turn = GF(renderer + kRendererSkyTurn) * kRadiansPerDegree;
    float sea = GF(dev + kDevVsConstants + kVsWaterLevel * 16);
    v.sky[0] = std::cos(turn);
    v.sky[1] = std::sin(turn);
    v.sky[2] = world[14] - sea;
    v.sky[3] = g_weather.seconds;
    v.weather[0] = kClearCoverage + (kRainCoverage - kClearCoverage) * g_weather.rain;
    v.weather[1] = g_weather.rain;
    d3d::VolumetricClouds(dev, a, v);
}

// the camera pass's list while the caustics may go in, and the water item being drawn
struct CausticsSplit {
    uint32_t list = 0;
    uint32_t ocean = kNoPipeline;  // the water pipelines' registry indices
    uint32_t lake = kNoPipeline;
    bool under = false;           // the camera under the sea
    bool done = false;
    bool water = false;
};
thread_local CausticsSplit t_caustics;
float g_caustics_seconds = 0;

uint32_t PipelineNamed(uint32_t name) {
    uint32_t renderer = R32(SYM_Renderer_pointer);
    uint32_t registry = renderer ? R32(renderer + kRendererPipelines) : 0;
    if (!registry) return kNoPipeline;
    uint32_t first = R32(registry + kRegistryFirst);
    uint32_t end = R32(registry + kRegistryEnd);
    for (uint32_t i = 0; first + i * kEntrySize < end; i++) {
        if (R32(first + i * kEntrySize + kEntryName) == name) return i;
    }
    return kNoPipeline;
}

// the item is the ocean's or a lake's
bool Water(uint32_t item) {
    if (!item) return false;
    uint32_t pipeline = R32(item + kItemPipeline);
    return pipeline == t_caustics.ocean || pipeline == t_caustics.lake;
}

bool HoldsWater(uint32_t list) {
    uint32_t count = R32(list);
    uint32_t entries = R32(list + kListEntries);
    for (uint32_t i = 0; i < count; i++) {
        uint32_t entry = entries + i * kListEntrySize;
        if (Water(R32(entry + kEntryItem))) return true;
    }
    return false;
}

// before the camera's list: the caustics go in when it holds water or the camera is under the sea
void StartCaustics(uint32_t list) {
    t_caustics.ocean = PipelineNamed(kOceanName);
    t_caustics.lake = PipelineNamed(kLakeName);
    t_caustics.under = fcip::SubmittedUnderWater();
    bool water = HoldsWater(list);
    if (t_caustics.under || water) t_caustics.list = list;
    static int logged = -1;
    int state = int(t_caustics.under) | int(water) << 1;
    if (state == logged) return;
    logged = state;
    LOGR("caustics: camera %s the sea, water %s, pipelines ocean %d lake %d", t_caustics.under ? "under" : "over",
         water ? "in view" : "out of view", int(t_caustics.ocean), int(t_caustics.lake));
}

// the caustics' light over what the camera's list drew so far, lit by the sun as the receivers are
void CausticsIn() {
    t_caustics.done = true;
    uint32_t engine = R32(SYM_C3DEngine_pointer);
    gpu::OcclusionArgs a;
    float world[16];
    if (!engine || !SceneArgs(a) || !ViewToWorld(world)) return;
    gpu::CausticsView v;
    v.light = SunView();
    for (uint32_t r = 0; r < 4; r++) {
        v.world_x[r] = world[r * 4];
        v.world_y[r] = world[r * 4 + 1];
    }
    SunToward(engine, v.toward_sun);
    g_caustics_seconds += float(fcip::FrameTime());
    v.toward_sun[3] = g_caustics_seconds;
    v.under = t_caustics.under;
    d3d::Caustics(R32(SYM_Device_pointer), a, v);
}

// before each item: the caustics before the first water item, under the sea before the first past the world; the
// water items take them
void UpdateCaustics(uint32_t list) {
    uint32_t key = 0;
    uint32_t item = 0;
    if (!NextItem(list, key, item)) return;
    bool water = Water(item);
    bool past_world = (key >> kLayerShift & kLayerMask) > kLastWorldLayer;
    if (!t_caustics.done && (water || (t_caustics.under && past_world))) CausticsIn();
    if (water == t_caustics.water) return;
    t_caustics.water = water;
    d3d::CausticsWater(R32(SYM_Device_pointer), water);
}

// after the list's last item
void EndCaustics() {
    if (!t_caustics.done && t_caustics.under) CausticsIn();
    if (t_caustics.water) d3d::CausticsWater(R32(SYM_Device_pointer), false);
    t_caustics = {};
}

// the item about to be popped lies on the camera's side of the water
bool NearLayer(uint32_t list) {
    uint32_t key = 0;
    return NextKey(list, key) && (key >> kLayerShift & kLayerMask) >= kFirstNearLayer;
}

// the fog over what is drawn so far; the draws past it take it per pixel
void FogIn() {
    Fog(t_fog);
    t_fog.list = 0;
}

}  // namespace

PPC_EXTERN_FUNC(GUEST_ORIG(Scene_Sort));
PPC_EXTERN_FUNC(GUEST_ORIG(Scene_Push));
PPC_EXTERN_FUNC(GUEST_ORIG(Scene_Draw));
PPC_EXTERN_FUNC(GUEST_ORIG(Scene_Pop));
PPC_EXTERN_FUNC(GUEST_ORIG(SkyBox_Draw));

// the list's keys made and pushed, r3 the constant block
PPC_FUNC(GUEST(Scene_Sort)) {
    if (Enabled()) {
        t_sort_list = uint32_t(ctx.r[3]) + kBlockList;
        t_pipelines = FindPipelines();
    }
    GUEST_ORIG(Scene_Sort)(ctx, base);
    t_sort_list = 0;
}

// (list, key, item): a character in the world layers sorts after every other world item
PPC_FUNC(GUEST(Scene_Push)) {
    uint32_t key = uint32_t(ctx.r[4]);
    bool world = (key >> kLayerShift & kLayerMask) <= kLastWorldLayer;
    uint32_t pipeline = key >> kPipelineShift & kLastPipeline;
    bool sorting = t_sort_list == uint32_t(ctx.r[3]);
    if (sorting && world && Character(pipeline, uint32_t(ctx.r[5]))) ctx.r[4] = (key & ~kLayerAndPipeline) | kMovedKey;
    GUEST_ORIG(Scene_Push)(ctx, base);
}

// the list drawn in key order, r3 the constant block; occlusion goes in at the end when nothing past the world came
PPC_FUNC(GUEST(Scene_Draw)) {
    fcip::MotionSceneDraw(uint32_t(ctx.r[3]));
    fcip::OceanSceneDraw();
    if (Enabled()) {
        t_draw_list = uint32_t(ctx.r[3]) + kBlockList;
        if (fcip::Get().ambient_occlusion == gpu::kAoRtao) d3d::OcclusionScene(R32(SYM_Device_pointer));
    }
    bool lit = Illuminated();
    bool fogged = Fogged();
    if (lit) {
        t_world_list = uint32_t(ctx.r[3]) + kBlockList;
        d3d::IlluminationScene(R32(SYM_Device_pointer));
    }
    // the volumetric fog replaces the game's: 82502D18 hands the list's draws a fog scale and bias of 0
    uint32_t block = uint32_t(ctx.r[3]);
    if (fogged) {
        t_fog.list = block + kBlockList;
        t_fog.scale = R32(block + kBlockFogScale);
        t_fog.bias = R32(block + kBlockFogBias);
        W32(block + kBlockFogScale, 0);
        W32(block + kBlockFogBias, 0);
    }
    if (Caustic()) StartCaustics(block + kBlockList);
    GUEST_ORIG(Scene_Draw)(ctx, base);
    if (t_caustics.list) EndCaustics();
    if (t_draw_list) Occlude();
    if (lit) Illuminate(ctx);
    t_past_list = 0;
    t_ocean = false;
    if (fogged) {
        if (t_fog.list) FogIn();
        W32(block + kBlockFogScale, t_fog.scale);
        W32(block + kBlockFogBias, t_fog.bias);
        d3d::VolumetricFogEnd(R32(SYM_Device_pointer));
    }
    fcip::MotionSceneDone();
}

// the next item of a list, r3 the list
PPC_FUNC(GUEST(Scene_Pop)) {
    uint32_t list = uint32_t(ctx.r[3]);
    if (t_draw_list && t_draw_list == list && PastWorld(list, false)) Occlude();
    if (t_world_list && t_world_list == list && PastWorld(list, true)) EndWorld(ctx);
    if (t_past_list && t_past_list == list) UpdateOcean(list);
    if (t_fog.list && t_fog.list == list && NearLayer(list)) FogIn();
    if (t_caustics.list && t_caustics.list == list) UpdateCaustics(list);
    fcip::MotionScenePop(list);
    GUEST_ORIG(Scene_Pop)(ctx, base);
}

// the sky box's item draw, r3 the renderer
PPC_FUNC(GUEST(SkyBox_Draw)) {
    uint32_t renderer = uint32_t(ctx.r[3]);
    GUEST_ORIG(SkyBox_Draw)(ctx, base);
    if (Clouded() && R8(renderer + kRendererSkyQueued)) Clouds(renderer);
}
