// Motion vectors of the camera's pass. The title marks the pass's world with clears no D3DCLEAR value uses and hands
// over the camera; the render thread matches each world draw to last frame's, whose vertex constants it runs again
#pragma once
#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

namespace gpu {

constexpr uint32_t kClearMotionBegin = 0x400;
constexpr uint32_t kClearMotionEnd = 0x800;
constexpr uint32_t kClearSceneBegin = 0x1000;  // the camera's pass, for FSR
constexpr uint32_t kClearSceneEnd = 0x2000;

// a camera's matrices as the title keeps them, row major, before its pass flips depth
struct MotionCamera {
    float view[16] = {};
    float proj[16] = {};
};

// title thread: the camera of the next kClearMotionBegin; render thread: takes it there
void PushMotionCamera(const MotionCamera& c);
bool TakeMotionCamera(MotionCamera& c);

// the title's clip space of a camera: its view times projection with the pass's depth flip, z = w - z (823B0580)
void ClipMatrix(const MotionCamera& c, float out[16]);
bool InvertMatrix(const float m[16], float out[16]);
// a clip position this frame to last frame's, for what stood still
bool Reprojection(const float now[16], const float last[16], float out[16]);

// last frame's world draws by signature and object position, with the vertex constant blocks their shaders read
class MotionHistory {
public:
    struct Draw {
        uint64_t signature = 0;
        float pos[3] = {};
        bool placed = false;
        bool taken = false;
        uint32_t at = 0;  // first block in blocks
        uint64_t read[2] = {};
        uint32_t next = UINT32_MAX;  // the next draw of its signature
    };

    void Begin();
    // the blocks read marks, from the title's big endian bank; pos null for a draw without an object position
    const Draw& Record(uint64_t signature, const float* pos, const uint64_t (&read)[2], const uint8_t* be);
    // last frame's draw for a recorded one, null when none: of its signature and near its position, the one whose
    // constants are nearest, since instances of one mesh share both and differ only in their constants
    const Draw* Match(const Draw& now);
    // a draw's blocks at their places in a 4 KB bank
    void Copy(const Draw& d, uint8_t* bank) const;

private:
    // a signature's draws in draw order; kept across frames, so only a frame's own count
    struct Chain {
        uint32_t first = 0;
        uint32_t last = 0;
        uint32_t frame = 0;
        uint64_t varying[2] = {};  // a bit per block of the draws' own, set where they differ
    };
    static void MarkVarying(Chain& c, const uint8_t* first, const uint8_t* draw, size_t blocks);
    struct Frame {
        std::vector<Draw> draws;
        std::vector<uint8_t> blocks;
        std::unordered_map<uint64_t, Chain> by_signature;
        uint32_t frame = 0;
    };
    Frame frames_[2];
    int now_ = 0;
};

}  // namespace gpu

namespace d3d {
// on the title's thread at the start and end of the camera pass's world
void MotionBegin(uint32_t dev, const gpu::MotionCamera& c);
void MotionEnd(uint32_t dev);
// on the title's thread at the start and end of the camera's pass
void SceneBegin(uint32_t dev);
void SceneEnd(uint32_t dev);
}  // namespace d3d
