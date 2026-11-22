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
constexpr float kMatchRadius = 4.0f;     // farther than an object moves in a frame
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

// the blocks a chain's draw differs from its first in
void MotionHistory::MarkVarying(Chain& c, const uint8_t* first, const uint8_t* draw, size_t blocks) {
    for (size_t k = 0; k < blocks; k++) {
        if (std::memcmp(first + k * kBlock, draw + k * kBlock, kBlock)) c.varying[k / 64] |= 1ull << (k % 64);
    }
}

void MotionHistory::Begin() {
    constexpr size_t kStaleSignatures = 8;  // signatures kept per draw before the table starts over
    MarkStarted();
    now_ ^= 1;
    Frame& f = frames_[now_];
    if (f.by_signature.size() > kStaleSignatures * (f.draws.size() + 64)) f.by_signature.clear();
    f.draws.clear();
    f.blocks.clear();
    f.frame++;
}

// summed distance of two draws' constants over the blocks marked, given up once past limit
float Gap(const uint8_t* a, const uint8_t* b, const uint64_t (&blocks)[2], float limit) {
    float sum = 0;
    for (uint32_t w = 0; w < 2; w++) {
        for (uint64_t m = blocks[w]; m && sum < limit; m &= m - 1) {
            size_t at = (w * 64 + size_t(std::countr_zero(m))) * kBlock;
            for (size_t i = 0; i < kBlock && sum < limit; i += 4) {
                float x;
                float y;
                std::memcpy(&x, a + at + i, 4);
                std::memcpy(&y, b + at + i, 4);
                sum += std::fabs(x - y);
            }
        }
    }
    return sum;
}

// of last frame's draws in c, the one whose constants are nearest now's: at its position, else moving near it
MotionHistory::Draw* MotionHistory::Nearest(const Draw& now, const Chain& c, bool here) {
    Frame& last = frames_[now_ ^ 1];
    const uint8_t* mine = frames_[now_].blocks.data() + size_t(now.at) * kBlock;
    // a block every candidate shares adds the same to each gap, so only the ones they differ in are compared
    const uint64_t(&varying)[2] = c.varying;
    Draw* best = nullptr;
    float best_gap = INFINITY;
    for (uint32_t i = c.first; i != UINT32_MAX; i = last.draws[i].next) {
        Draw& d = last.draws[i];
        if (d.taken || !d.placed || d.read[0] != now.read[0] || d.read[1] != now.read[1]) continue;
        float distance = Distance2(d.pos, now.pos);
        // a signature never drawn twice in a frame can't take a sibling's place
        bool lone = !c.multi && !frames_[now_].by_signature[now.signature].multi;
        bool fits = here ? distance == 0 : (d.moving || lone) && distance <= kMatchRadius * kMatchRadius;
        if (!fits) continue;
        float gap = Gap(mine, last.blocks.data() + size_t(d.at) * kBlock, varying, best_gap);
        if (gap >= best_gap) continue;
        best_gap = gap;
        best = &d;
    }
    return best;
}

const MotionHistory::Draw* MotionHistory::Match(Draw& now) {
    // without a position a draw can't be told from one that reused its buffers: the depth's reprojection takes it
    if (!now.placed) return nullptr;
    Frame& last = frames_[now_ ^ 1];
    auto it = last.by_signature.find(now.signature);
    if (it == last.by_signature.end() || it->second.frame != last.frame) return nullptr;
    bool shared = it->second.shared || frames_[now_].by_signature[now.signature].shared;
    // a shared position matches only a moving draw near it, the rest are left to the depth's reprojection
    Draw* best = shared ? nullptr : Nearest(now, it->second, true);
    if (!best) best = Nearest(now, it->second, false);
    if (!best) return nullptr;
    best->taken = true;
    now.matched = true;
    now.moving = best->moving || Distance2(best->pos, now.pos) > 0;
    return best;
}

// a placed draw left unmatched where one of last frame's went untaken nearby: the same object, started moving
void MotionHistory::MarkStarted() {
    Frame& f = frames_[now_];
    const Frame& last = frames_[now_ ^ 1];
    for (Draw& d : f.draws) {
        if (d.matched || !d.placed) continue;
        auto it = last.by_signature.find(d.signature);
        if (it == last.by_signature.end() || it->second.frame != last.frame) continue;
        for (uint32_t i = it->second.first; i != UINT32_MAX && !d.moving; i = last.draws[i].next) {
            const Draw& old = last.draws[i];
            float distance = Distance2(old.pos, d.pos);
            d.moving = !old.taken && old.placed && distance > 0 && distance <= kMatchRadius * kMatchRadius;
        }
    }
}

MotionHistory::Draw& MotionHistory::Record(uint64_t signature, const float* pos, const uint64_t (&read)[2],
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
    uint32_t index = uint32_t(f.draws.size());
    Chain& c = f.by_signature[signature];
    if (c.frame == f.frame) {
        f.draws[c.last].next = index;
        c.multi = true;
        MarkVarying(c, f.blocks.data() + size_t(f.draws[c.first].at) * kBlock, f.blocks.data() + size_t(d.at) * kBlock,
                    blocks);
        for (uint32_t i = c.first; i != index && d.placed && !c.shared; i = f.draws[i].next) {
            c.shared = Distance2(f.draws[i].pos, d.pos) == 0;
        }
    } else {
        bool shared = c.shared;
        bool multi = c.multi;
        c = {index, index, f.frame};
        c.shared = shared;
        c.multi = multi;
    }
    c.last = index;
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
