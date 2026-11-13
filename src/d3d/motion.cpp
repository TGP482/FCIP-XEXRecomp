#include "d3d/motion.h"

#include <bit>
#include <cmath>
#include <cstring>
#include <deque>
#include <mutex>
#include <utility>

namespace gpu {

namespace {

constexpr uint32_t kBlock = 32;          // two constants, a bit of ShaderInfo::constants
constexpr float kMatchRadius = 4.0f;     // farther than an object moves in a frame, nearer than two of one mesh
constexpr size_t kMaxCameras = 8;        // the render thread is at most a few frames behind

std::mutex g_cameras_lock;
std::deque<MotionCamera> g_cameras;

void Multiply(const float a[16], const float b[16], float out[16]) {
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            float v = 0;
            for (int k = 0; k < 4; k++) v += a[r * 4 + k] * b[k * 4 + c];
            out[r * 4 + c] = v;
        }
    }
}

float Distance2(const float a[3], const float b[3]) {
    float x = a[0] - b[0];
    float y = a[1] - b[1];
    float z = a[2] - b[2];
    return x * x + y * y + z * z;
}

}  // namespace

// Gauss Jordan with partial pivoting
bool InvertMatrix(const float m[16], float out[16]) {
    double a[4][8];
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            a[r][c] = m[r * 4 + c];
            a[r][c + 4] = r == c ? 1 : 0;
        }
    }
    for (int c = 0; c < 4; c++) {
        int pivot = c;
        for (int r = c + 1; r < 4; r++) {
            if (std::fabs(a[r][c]) > std::fabs(a[pivot][c])) pivot = r;
        }
        if (std::fabs(a[pivot][c]) < 1e-12) return false;
        std::swap(a[c], a[pivot]);
        double inv = 1 / a[c][c];
        for (double& v : a[c]) v *= inv;
        for (int r = 0; r < 4; r++) {
            if (r == c) continue;
            double f = a[r][c];
            for (int k = 0; k < 8; k++) a[r][k] -= f * a[c][k];
        }
    }
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) out[r * 4 + c] = float(a[r][c + 4]);
    }
    return true;
}

void PushMotionCamera(const MotionCamera& c) {
    std::lock_guard lock(g_cameras_lock);
    if (g_cameras.size() == kMaxCameras) g_cameras.pop_front();
    g_cameras.push_back(c);
}

bool TakeMotionCamera(MotionCamera& c) {
    std::lock_guard lock(g_cameras_lock);
    if (g_cameras.empty()) return false;
    c = g_cameras.front();
    g_cameras.pop_front();
    return true;
}

void ClipMatrix(const MotionCamera& c, float out[16]) {
    Multiply(c.view, c.proj, out);
    for (int r = 0; r < 4; r++) out[r * 4 + 2] = out[r * 4 + 3] - out[r * 4 + 2];
}

bool Reprojection(const float now[16], const float last[16], float out[16]) {
    float inv[16];
    if (!InvertMatrix(now, inv)) return false;
    Multiply(inv, last, out);
    return true;
}

void MotionHistory::Begin() {
    now_ ^= 1;
    Frame& f = frames_[now_];
    f.draws.clear();
    f.blocks.clear();
    f.by_signature.clear();
}

// summed distance of two draws' constants, given up once past limit
float Gap(const uint8_t* a, const uint8_t* b, size_t floats, float limit) {
    float sum = 0;
    for (size_t i = 0; i < floats && sum < limit; i++) {
        float x;
        float y;
        std::memcpy(&x, a + i * 4, 4);
        std::memcpy(&y, b + i * 4, 4);
        sum += std::fabs(x - y);
    }
    return sum;
}

const MotionHistory::Draw* MotionHistory::Match(const Draw& now) {
    Frame& last = frames_[now_ ^ 1];
    auto it = last.by_signature.find(now.signature);
    if (it == last.by_signature.end()) return nullptr;
    const uint8_t* mine = frames_[now_].blocks.data() + size_t(now.at) * kBlock;
    size_t floats = size_t(std::popcount(now.read[0]) + std::popcount(now.read[1])) * kBlock / 4;
    Draw* best = nullptr;
    float best_gap = INFINITY;
    for (uint32_t i : it->second) {
        Draw& d = last.draws[i];
        if (d.taken || d.placed != now.placed || d.read[0] != now.read[0] || d.read[1] != now.read[1]) continue;
        if (now.placed && Distance2(d.pos, now.pos) > kMatchRadius * kMatchRadius) continue;
        float gap = Gap(mine, last.blocks.data() + size_t(d.at) * kBlock, floats, best_gap);
        if (gap >= best_gap) continue;
        best_gap = gap;
        best = &d;
    }
    if (best) best->taken = true;
    return best;
}

const MotionHistory::Draw& MotionHistory::Record(uint64_t signature, const float* pos, const uint64_t (&read)[2],
                                                 const uint8_t* be) {
    Frame& f = frames_[now_];
    Draw d;
    d.signature = signature;
    d.placed = pos != nullptr;
    if (pos) std::memcpy(d.pos, pos, sizeof(d.pos));
    d.at = uint32_t(f.blocks.size() / kBlock);
    d.read[0] = read[0];
    d.read[1] = read[1];
    size_t blocks = size_t(std::popcount(read[0]) + std::popcount(read[1]));
    f.blocks.resize(f.blocks.size() + blocks * kBlock);
    uint8_t* dst = f.blocks.data() + size_t(d.at) * kBlock;
    for (uint32_t w = 0; w < 2; w++) {
        for (uint64_t m = read[w]; m; m &= m - 1) {
            const uint8_t* src = be + (w * 64 + uint32_t(std::countr_zero(m))) * kBlock;
            for (uint32_t i = 0; i < kBlock; i += 4) {
                uint32_t v;
                std::memcpy(&v, src + i, 4);
                v = __builtin_bswap32(v);
                std::memcpy(dst + i, &v, 4);
            }
            dst += kBlock;
        }
    }
    f.by_signature[signature].push_back(uint32_t(f.draws.size()));
    f.draws.push_back(d);
    return f.draws.back();
}

void MotionHistory::Copy(const Draw& d, uint8_t* bank) const {
    const Frame& last = frames_[now_ ^ 1];
    const uint8_t* src = last.blocks.data() + size_t(d.at) * kBlock;
    for (uint32_t w = 0; w < 2; w++) {
        for (uint64_t m = d.read[w]; m; m &= m - 1) {
            std::memcpy(bank + (w * 64 + uint32_t(std::countr_zero(m))) * kBlock, src, kBlock);
            src += kBlock;
        }
    }
}

}  // namespace gpu
