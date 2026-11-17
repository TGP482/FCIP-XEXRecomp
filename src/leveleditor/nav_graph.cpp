// AI navigation for a custom level. The walkable ground of each EnclosingArea is cut again from its outline, the
// ForbiddenAreas and the obstacles the static geometry left in the graphs the level loaded with; a waypoint building's
// nodes, its exits, paths and hide points are kept as the level loaded them. Files are little endian.
#include "leveleditor/nav_graph.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <set>

namespace fcip::editor {
namespace {

constexpr uint32_t kVersion = 0x2C;
constexpr uint32_t kManagerVersion = 1;
constexpr float kXyScale = 1024.0f;  // a Vec's x and y, 24 bit
constexpr float kZScale = 32.0f;     // its z, 16 bit
constexpr float kDefaultSizes[3] = {1.0f, 1.955f, 0.51f};
constexpr uint16_t kNone = 0xFFFF;
constexpr uint8_t kOutdoor = 0x01;
constexpr uint8_t kWaypointNode = 0x20;  // a waypoint building's node, no corners
constexpr uint16_t kObstacleLeaf = 16;  // most elements a quad tree node holds before it splits
constexpr uint16_t kPolygonLeaf = 16;
constexpr uint16_t kIslandLeaf = 32;
constexpr int kMaxDepth = 8;
constexpr float kMinNode = 4.0f;  // metres
constexpr float kSameRing = 0.05f;
constexpr double kMinWidth = 0.05;  // metres; thinner is a seam between edges that nearly touch
constexpr double kEps = 1e-9;

struct Vec3 {
    float x = 0, y = 0, z = 0;
};
struct Exit {
    uint16_t id = 0;
    std::vector<Vec3> points;
};
struct Hide {
    uint16_t id = 0, second = 0;
    uint32_t value = 0;
    Vec3 pos;
    uint16_t angle = 0;
    uint8_t type = 0;
};
struct Polygon {
    std::vector<uint32_t> links;
    std::vector<uint32_t> corners;
    Vec3 pos;
    uint16_t building = kNone, island = kNone;
    uint8_t flags = kOutdoor;
    uint32_t radius2 = 0;  // float bits
};
struct Path {
    uint16_t from = 0, to = 0;
    uint32_t cost = 0;
    std::vector<uint32_t> polygons;
};
struct Group {
    uint32_t key = 0;
    std::vector<Path> paths;
};
struct Graph {
    float sizes[3] = {};
    std::vector<Vec3> verts;
    std::vector<uint32_t> boundary;
    std::vector<Exit> exits;
    std::vector<Hide> hides;
    std::vector<std::vector<uint32_t>> obstacles;
    std::vector<Polygon> polygons;
    std::vector<Group> groups;
};

// reading

struct Reader {
    const NavFile& b;
    size_t p = 0;
    bool ok = true;
    bool Need(size_t n) {
        ok = ok && p + n <= b.size();
        return ok;
    }
    uint32_t Int(size_t n) {
        if (!Need(n)) return 0;
        uint32_t v = 0;
        for (size_t i = 0; i < n; i++) v |= uint32_t(b[p + i]) << (8 * i);
        p += n;
        return v;
    }
    void Skip(size_t n) {
        if (Need(n)) p += n;
    }
    uint8_t U8() { return uint8_t(Int(1)); }
    uint16_t U16() { return uint16_t(Int(2)); }
    uint32_t U32() { return Int(4); }
    float F32() {
        uint32_t v = U32();
        float f;
        std::memcpy(&f, &v, 4);
        return f;
    }
    Vec3 V() {
        uint32_t x = Int(3), y = Int(3), z = Int(2);
        return {x / kXyScale, y / kXyScale, z / kZScale};
    }
    std::vector<uint32_t> Indices() {
        uint32_t n = U32();
        uint8_t width = U8();
        std::vector<uint32_t> out;
        if (width < 1 || width > 3) {
            ok = ok && n == 0;
            return out;
        }
        for (uint32_t i = 0; ok && i < n; i++) out.push_back(Int(width == 3 ? 4 : width));
        return out;
    }
};

// a pool's elements; its quad tree is built again on writing
template <typename F>
void ReadPool(Reader& r, F&& element) {
    uint16_t count = r.U16();
    r.U32();
    uint16_t nodes = r.U16();
    r.U16();
    if (r.U32() == 0) {
        for (uint16_t i = 0; r.ok && i < count; i++) element(r);
    }
    std::vector<uint16_t> lengths;
    for (uint16_t i = 0; r.ok && i < nodes; i++) {
        r.Skip(10);  // children, then the owned count
        lengths.push_back(r.U16());
        r.Skip(16);
    }
    for (uint16_t n : lengths) r.Skip(2 * size_t(n));
}

bool Fit(const std::vector<uint32_t>& indices, size_t size) {
    return std::all_of(indices.begin(), indices.end(), [&](uint32_t i) { return i < size; });
}

bool IndicesFit(const Graph& g) {
    bool fit = Fit(g.boundary, g.verts.size());
    for (const auto& o : g.obstacles) fit = fit && Fit(o, g.verts.size());
    for (const Polygon& p : g.polygons) fit = fit && Fit(p.corners, g.verts.size()) && Fit(p.links, g.polygons.size());
    for (const Group& grp : g.groups) {
        for (const Path& p : grp.paths) fit = fit && Fit(p.polygons, g.polygons.size());
    }
    return fit;
}

bool ReadGraph(const NavFile& file, Graph& g) {
    Reader r{file};
    if (r.U32() != kVersion) return false;
    r.U32();
    for (float& s : g.sizes) s = r.F32();
    for (uint32_t i = 0, n = r.U32(); r.ok && i < n; i++) g.verts.push_back(r.V());
    g.boundary = r.Indices();
    for (uint32_t i = 0, n = r.U32(); r.ok && i < n; i++) {
        Exit e;
        e.id = r.U16();
        for (uint32_t k = 0, m = r.U32(); r.ok && k < m; k++) e.points.push_back(r.V());
        r.Indices();  // its islands, found again
        g.exits.push_back(e);
    }
    for (uint32_t i = 0, n = r.U32(); r.ok && i < n; i++) {
        Hide h;
        h.id = r.U16();
        h.second = r.U16();
        h.value = r.U32();
        h.pos = r.V();
        h.angle = r.U16();
        h.type = r.U8();
        g.hides.push_back(h);
    }
    r.Skip(24);  // the bounds, found again
    ReadPool(r, [&](Reader& e) {
        e.Skip(12);
        g.obstacles.push_back(e.Indices());
    });
    ReadPool(r, [&](Reader& e) {
        Polygon p;
        for (uint16_t k = 0, m = e.U16(); e.ok && k < m; k++) p.links.push_back(e.U16());
        p.corners = e.Indices();
        p.pos = e.V();
        p.building = e.U16();
        p.island = e.U16();
        p.flags = e.U8();
        p.radius2 = e.U32();
        g.polygons.push_back(p);
    });
    for (uint32_t i = 0, n = r.U32(); r.ok && i < n; i++) ReadPool(r, [](Reader&) {});
    for (uint32_t i = 0, n = r.U32(); r.ok && i < n; i++) {
        Group grp;
        grp.key = r.U32();
        for (uint32_t k = 0, m = r.U32(); r.ok && k < m; k++) {
            Path p;
            p.from = r.U16();
            p.to = r.U16();
            p.cost = r.U32();
            for (uint32_t j = 0, c = r.U32(); r.ok && j < c; j++) p.polygons.push_back(r.U16());
            grp.paths.push_back(p);
        }
        g.groups.push_back(grp);
    }
    return r.ok && r.p == file.size() && IndicesFit(g);
}

// writing

struct Writer {
    NavFile b;
    void Int(uint32_t v, size_t n) {
        for (size_t i = 0; i < n; i++) b.push_back(uint8_t(v >> (8 * i)));
    }
    void U8(uint32_t v) { Int(v, 1); }
    void U16(uint32_t v) { Int(v, 2); }
    void U32(uint32_t v) { Int(v, 4); }
    void F32(float f) {
        uint32_t v;
        std::memcpy(&v, &f, 4);
        U32(v);
    }
    void V(const Vec3& p) {
        Int(uint32_t(std::lround(p.x * kXyScale)) & 0xFFFFFF, 3);
        Int(uint32_t(std::lround(p.y * kXyScale)) & 0xFFFFFF, 3);
        Int(uint32_t(std::lround(p.z * kZScale)) & 0xFFFF, 2);
    }
    void Indices(const std::vector<uint32_t>& v) {
        uint32_t top = v.empty() ? 0 : *std::max_element(v.begin(), v.end());
        uint8_t width = top < 0x100 ? 1 : top < 0x10000 ? 2 : 3;
        U32(uint32_t(v.size()));
        U8(width);
        for (uint32_t i : v) Int(i, width == 3 ? 4 : width);
    }
};

struct Box {
    float x0 = 1e30f, x1 = -1e30f, y0 = 1e30f, y1 = -1e30f;  // empty until grown
    void Grow(float x, float y) {
        x0 = std::min(x0, x);
        x1 = std::max(x1, x);
        y0 = std::min(y0, y);
        y1 = std::max(y1, y);
    }
    bool Overlaps(const Box& o) const { return x0 <= o.x1 && o.x0 <= x1 && y0 <= o.y1 && o.y0 <= y1; }
};

struct Node {
    uint16_t children[4] = {kNone, kNone, kNone, kNone};
    uint16_t owned = 0;  // set to the list's length on a leaf, 0 above, as the stock graphs mostly do
    Box box{};
    std::vector<uint16_t> list;
};

void Split(std::vector<Node>& nodes, size_t at, const std::vector<Box>& boxes, uint16_t leaf, int depth) {
    Box b = nodes[at].box;
    float mx = (b.x0 + b.x1) * 0.5f, my = (b.y0 + b.y1) * 0.5f;
    bool split = nodes[at].list.size() > leaf && depth < kMaxDepth && b.x1 - b.x0 > kMinNode;
    if (!split) {
        nodes[at].owned = uint16_t(nodes[at].list.size());
        return;
    }
    const Box quads[4] = {{b.x0, mx, b.y0, my}, {mx, b.x1, b.y0, my}, {b.x0, mx, my, b.y1}, {mx, b.x1, my, b.y1}};
    for (int q = 0; q < 4; q++) {
        Node child;
        child.box = quads[q];
        for (uint16_t e : nodes[at].list) {
            if (boxes[e].Overlaps(quads[q])) child.list.push_back(e);
        }
        if (child.list.empty()) continue;
        nodes[at].children[q] = uint16_t(nodes.size());
        nodes.push_back(std::move(child));
        Split(nodes, nodes[at].children[q], boxes, leaf, depth + 1);
    }
}

// a pool's header, its elements when written, its quad tree over the members
void WritePool(Writer& w, const Box& bounds, const std::vector<Box>& boxes, const std::vector<uint16_t>& members,
               uint16_t leaf, const std::function<void(Writer&)>& elements) {
    std::vector<Node> nodes;
    if (!members.empty()) {
        nodes.push_back({});
        nodes[0].box = bounds;
        nodes[0].list = members;
        Split(nodes, 0, boxes, leaf, 0);
    }
    uint32_t total = 0;
    for (const Node& n : nodes) total += uint32_t(n.list.size());
    w.U16(uint32_t(boxes.size()));
    w.U32(total);
    w.U16(uint32_t(nodes.size()));
    w.U16(leaf);
    w.U32(elements ? 0 : 1);
    if (elements) elements(w);
    for (const Node& n : nodes) {
        for (uint16_t c : n.children) w.U16(c);
        w.U16(n.owned);
        w.U16(uint32_t(n.list.size()));
        w.F32(n.box.x0);
        w.F32(n.box.x1);
        w.F32(n.box.y0);
        w.F32(n.box.y1);
    }
    for (const Node& n : nodes) {
        for (uint16_t e : n.list) w.U16(e);
    }
}

// a waypoint node has no corners, only its place
Box BoxOf(const std::vector<Vec3>& verts, const std::vector<uint32_t>& corners, const Vec3& place) {
    Box b;
    if (corners.empty()) b.Grow(place.x, place.y);
    for (uint32_t c : corners) b.Grow(verts[c].x, verts[c].y);
    return b;
}

std::vector<uint16_t> AllOf(size_t n) {
    std::vector<uint16_t> out(n);
    for (size_t i = 0; i < n; i++) out[i] = uint16_t(i);
    return out;
}

// the vertices' centre and the largest squared distance from it, in the plane
std::pair<Vec3, float> Spread(const std::vector<Vec3>& verts, const std::vector<uint32_t>& corners) {
    Vec3 c;
    for (uint32_t i : corners) {
        c.x += verts[i].x / float(corners.size());
        c.y += verts[i].y / float(corners.size());
    }
    float r2 = 0;
    for (uint32_t i : corners) {
        float dx = verts[i].x - c.x, dy = verts[i].y - c.y;
        r2 = std::max(r2, dx * dx + dy * dy);
    }
    return {c, r2};
}

uint32_t Bits(float f) {
    uint32_t v;
    std::memcpy(&v, &f, 4);
    return v;
}

NavFile WriteGraph(const Graph& g, uint32_t id) {
    Writer w;
    w.U32(kVersion);
    w.U32(id);
    for (float s : g.sizes) w.F32(s);
    w.U32(uint32_t(g.verts.size()));
    for (const Vec3& v : g.verts) w.V(v);
    w.Indices(g.boundary);
    uint16_t islands = 0;
    for (const Polygon& p : g.polygons) {
        if (p.island != kNone) islands = std::max<uint16_t>(islands, p.island + 1);
    }
    w.U32(uint32_t(g.exits.size()));
    for (const Exit& e : g.exits) {
        w.U16(e.id);
        w.U32(uint32_t(e.points.size()));
        for (const Vec3& p : e.points) w.V(p);
        std::set<uint32_t> in;
        for (const Polygon& p : g.polygons) {
            if (p.building == e.id) in.insert(p.island == kNone ? 0 : p.island);
        }
        w.Indices({in.begin(), in.end()});
    }
    w.U32(uint32_t(g.hides.size()));
    for (const Hide& h : g.hides) {
        w.U16(h.id);
        w.U16(h.second);
        w.U32(h.value);
        w.V(h.pos);
        w.U16(h.angle);
        w.U8(h.type);
    }
    Box bounds;
    for (uint32_t i : g.boundary) bounds.Grow(g.verts[i].x, g.verts[i].y);
    for (const Exit& e : g.exits) {
        for (const Vec3& p : e.points) bounds.Grow(p.x, p.y);
    }
    if (bounds.x0 > bounds.x1) bounds = {0, 0, 0, 0};
    for (float f : {bounds.x0, bounds.y0, 0.0f, bounds.x1, bounds.y1, 0.0f}) w.F32(f);

    std::vector<Box> obstacle_boxes;
    for (const auto& o : g.obstacles) obstacle_boxes.push_back(BoxOf(g.verts, o, {}));
    WritePool(w, bounds, obstacle_boxes, AllOf(g.obstacles.size()), kObstacleLeaf, [&](Writer& e) {
        for (const auto& o : g.obstacles) {
            auto [c, r2] = Spread(g.verts, o);
            e.F32(r2);
            e.F32(c.x);
            e.F32(c.y);
            e.Indices(o);
        }
    });
    std::vector<Box> polygon_boxes;
    for (const Polygon& p : g.polygons) polygon_boxes.push_back(BoxOf(g.verts, p.corners, p.pos));
    WritePool(w, bounds, polygon_boxes, AllOf(g.polygons.size()), kPolygonLeaf, [&](Writer& e) {
        for (const Polygon& p : g.polygons) {
            e.U16(uint32_t(p.links.size()));
            for (uint32_t l : p.links) e.U16(l);
            e.Indices(p.corners);
            e.V(p.pos);
            e.U16(p.building);
            e.U16(p.island);
            e.U8(p.flags);
            e.U32(p.radius2);
        }
    });
    w.U32(islands);
    for (uint16_t i = 0; i < islands; i++) {
        std::vector<uint16_t> members;
        for (size_t k = 0; k < g.polygons.size(); k++) {
            if (g.polygons[k].island == i) members.push_back(uint16_t(k));
        }
        WritePool(w, bounds, polygon_boxes, members, kIslandLeaf, nullptr);
    }
    w.U32(uint32_t(g.groups.size()));
    for (const Group& grp : g.groups) {
        w.U32(grp.key);
        w.U32(uint32_t(grp.paths.size()));
        for (const Path& p : grp.paths) {
            w.U16(p.from);
            w.U16(p.to);
            w.U32(p.cost);
            w.U32(uint32_t(p.polygons.size()));
            for (uint32_t k : p.polygons) w.U16(k);
        }
    }
    return std::move(w.b);
}

// cutting the ground: vertical slabs between the corners' x, a trapezoid for each walkable stretch of a slab, joined
// along slabs while they keep the same two edges, then neighbours merged while they stay convex

using P2 = std::array<double, 2>;
using Ring2 = std::vector<P2>;

double Snap(double v) { return std::round(v * kXyScale) / kXyScale; }

struct Edge {
    size_t ring;
    double xa, ya, xb, yb;
    double At(double x) const { return ya + (yb - ya) * (x - xa) / (xb - xa); }
};
struct Trapezoid {
    double xl, xr, bl, tl, br, tr;  // bottom and top y on the left and right
};

std::vector<Trapezoid> Trapezoids(const std::vector<Ring2>& rings) {
    std::vector<Edge> edges;
    std::vector<double> xs;
    for (size_t r = 0; r < rings.size(); r++) {
        const Ring2& ring = rings[r];
        for (size_t i = 0; i < ring.size(); i++) {
            P2 a = ring[(i + ring.size() - 1) % ring.size()], b = ring[i];
            xs.push_back(b[0]);
            if (a[0] == b[0]) continue;
            if (a[0] > b[0]) std::swap(a, b);
            edges.push_back({r, a[0], a[1], b[0], b[1]});
        }
    }
    std::sort(xs.begin(), xs.end());
    xs.erase(std::unique(xs.begin(), xs.end()), xs.end());
    std::vector<size_t> order(edges.size());
    for (size_t i = 0; i < order.size(); i++) order[i] = i;
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return edges[a].xa < edges[b].xa; });

    std::map<std::pair<size_t, size_t>, Trapezoid> open;
    std::vector<std::pair<std::pair<size_t, size_t>, Trapezoid>> done;
    std::vector<size_t> active;
    std::vector<int> odd(rings.size());
    size_t next = 0;
    for (size_t s = 0; s + 1 < xs.size(); s++) {
        double x0 = xs[s], x1 = xs[s + 1], xm = (x0 + x1) * 0.5;
        while (next < order.size() && edges[order[next]].xa <= x0) active.push_back(order[next++]);
        std::erase_if(active, [&](size_t e) { return edges[e].xb < x1; });
        std::vector<size_t> cross = active;
        std::sort(cross.begin(), cross.end(), [&](size_t a, size_t b) { return edges[a].At(xm) < edges[b].At(xm); });
        std::fill(odd.begin(), odd.end(), 0);
        int blocked = 0;  // obstacles the stretch is inside
        std::map<std::pair<size_t, size_t>, Trapezoid> still;
        for (size_t j = 0; j + 1 < cross.size(); j++) {
            size_t r = edges[cross[j]].ring;
            odd[r] ^= 1;
            if (r) blocked += odd[r] ? 1 : -1;
            if (!odd[0] || blocked) continue;
            std::pair<size_t, size_t> key{cross[j], cross[j + 1]};
            auto it = open.find(key);
            Trapezoid t = it != open.end() ? it->second : Trapezoid{x0, x0, 0, 0, 0, 0};
            if (it != open.end()) open.erase(it);
            t.xr = x1;
            still[key] = t;
        }
        for (auto& kv : open) done.push_back(kv);
        open = std::move(still);
    }
    for (auto& kv : open) done.push_back(kv);
    std::vector<Trapezoid> out;
    for (auto& [key, t] : done) {
        const Edge &b = edges[key.first], &top = edges[key.second];
        Trapezoid q{t.xl, t.xr, Snap(b.At(t.xl)), Snap(top.At(t.xl)), Snap(b.At(t.xr)), Snap(top.At(t.xr))};
        if (std::max(q.tl - q.bl, q.tr - q.br) > kMinWidth) out.push_back(q);
    }
    return out;
}

struct Mesh {
    std::vector<P2> verts;
    std::vector<std::vector<uint32_t>> rings;  // counter clockwise
    std::vector<std::set<uint32_t>> links;
};

// trapezoids sharing a stretch of a vertical side link, each side gaining the other's corners so they share vertices
Mesh MeshOf(const std::vector<Trapezoid>& traps) {
    Mesh m;
    std::multimap<double, size_t> by_left, by_right;
    for (size_t i = 0; i < traps.size(); i++) {
        by_left.insert({traps[i].xl, i});
        by_right.insert({traps[i].xr, i});
    }
    std::vector<std::vector<double>> on_left(traps.size()), on_right(traps.size());
    m.links.resize(traps.size());
    for (auto& [x, i] : by_left) {
        const Trapezoid& a = traps[i];
        auto range = by_right.equal_range(x);
        for (auto it = range.first; it != range.second; ++it) {
            size_t j = it->second;
            const Trapezoid& b = traps[j];
            if (std::min(a.tl, b.tr) - std::max(a.bl, b.br) <= kEps) continue;
            m.links[i].insert(uint32_t(j));
            m.links[j].insert(uint32_t(i));
            for (double y : {b.br, b.tr}) {
                if (a.bl < y && y < a.tl) on_left[i].push_back(y);
            }
            for (double y : {a.bl, a.tl}) {
                if (b.br < y && y < b.tr) on_right[j].push_back(y);
            }
        }
    }
    std::map<P2, uint32_t> ids;
    auto id = [&](double x, double y) {
        auto [it, added] = ids.insert({P2{x, y}, uint32_t(m.verts.size())});
        if (added) m.verts.push_back({x, y});
        return it->second;
    };
    for (size_t i = 0; i < traps.size(); i++) {
        const Trapezoid& t = traps[i];
        std::vector<P2> pts{{t.xl, t.bl}, {t.xr, t.br}};
        std::sort(on_right[i].begin(), on_right[i].end());
        for (double y : on_right[i]) pts.push_back({t.xr, y});
        pts.push_back({t.xr, t.tr});
        pts.push_back({t.xl, t.tl});
        std::sort(on_left[i].rbegin(), on_left[i].rend());
        for (double y : on_left[i]) pts.push_back({t.xl, y});
        std::vector<uint32_t> ring;
        for (const P2& p : pts) {
            uint32_t v = id(p[0], p[1]);
            if (ring.empty() || ring.back() != v) ring.push_back(v);
        }
        if (ring.size() > 1 && ring.front() == ring.back()) ring.pop_back();
        m.rings.push_back(std::move(ring));
    }
    return m;
}

bool Convex(const std::vector<uint32_t>& ring, const std::vector<P2>& v) {
    size_t n = ring.size();
    for (size_t i = 0; i < n; i++) {
        const P2 &a = v[ring[i]], &b = v[ring[(i + 1) % n]], &c = v[ring[(i + 2) % n]];
        if ((b[0] - a[0]) * (c[1] - b[1]) - (b[1] - a[1]) * (c[0] - b[0]) < -kEps) return false;
    }
    return true;
}

// the edge a to b of ring i that ring j runs b to a, as positions in each
bool SharedEdge(const std::vector<uint32_t>& ri, const std::vector<uint32_t>& rj, size_t& at_i, size_t& at_j) {
    for (size_t k = 0; k < ri.size(); k++) {
        uint32_t a = ri[k], b = ri[(k + 1) % ri.size()];
        for (size_t l = 0; l < rj.size(); l++) {
            if (rj[l] == b && rj[(l + 1) % rj.size()] == a) {
                at_i = k;
                at_j = l;
                return true;
            }
        }
    }
    return false;
}

void Merge(Mesh& m) {
    std::vector<bool> alive(m.rings.size(), true);
    for (bool changed = true; changed;) {
        changed = false;
        for (size_t i = 0; i < m.rings.size(); i++) {
            if (!alive[i]) continue;
            for (uint32_t j : std::vector<uint32_t>(m.links[i].begin(), m.links[i].end())) {
                size_t at_i, at_j;
                const auto &ri = m.rings[i], &rj = m.rings[j];
                if (!SharedEdge(ri, rj, at_i, at_j)) continue;
                std::vector<uint32_t> u;
                for (size_t k = 1; k <= ri.size(); k++) u.push_back(ri[(at_i + k) % ri.size()]);  // b around to a
                for (size_t k = 2; k < rj.size(); k++) u.push_back(rj[(at_j + k) % rj.size()]);   // past a, before b
                if (!Convex(u, m.verts)) continue;
                m.rings[i] = std::move(u);
                alive[j] = false;
                for (uint32_t k : m.links[j]) {
                    m.links[k].erase(j);
                    if (k != i) {
                        m.links[k].insert(uint32_t(i));
                        m.links[i].insert(k);
                    }
                }
                m.links[i].erase(j);
                changed = true;
            }
        }
    }
    std::vector<uint32_t> index(m.rings.size());
    Mesh out;
    out.verts = m.verts;
    for (size_t i = 0; i < m.rings.size(); i++) {
        if (!alive[i]) continue;
        index[i] = uint32_t(out.rings.size());
        out.rings.push_back(m.rings[i]);
    }
    for (size_t i = 0; i < m.rings.size(); i++) {
        if (!alive[i]) continue;
        std::set<uint32_t> l;
        for (uint32_t k : m.links[i]) l.insert(index[k]);
        out.links.push_back(l);
    }
    m = std::move(out);
}

// assembling a graph

double SignedArea(const NavRing& r) {
    double a = 0;
    for (size_t i = 0; i < r.size(); i++) {
        const NavPoint &p = r[(i + r.size() - 1) % r.size()], &q = r[i];
        a += double(p[0]) * q[1] - double(q[0]) * p[1];
    }
    return a * 0.5;
}

bool SameRing(const NavRing& a, const NavRing& b) {
    if (a.size() != b.size()) return false;
    for (const NavPoint& p : a) {
        bool found = false;
        for (const NavPoint& q : b) found = found || (std::fabs(p[0] - q[0]) + std::fabs(p[1] - q[1]) < kSameRing);
        if (!found) return false;
    }
    return true;
}

Box BoxOfRing(const NavRing& r) {
    Box b;
    for (const NavPoint& p : r) b.Grow(p[0], p[1]);
    return b;
}

bool Inside(const std::vector<P2>& verts, const std::vector<uint32_t>& ring, double x, double y) {
    bool in = false;
    for (size_t i = 0; i < ring.size(); i++) {
        const P2 &a = verts[ring[i]], &b = verts[ring[(i + ring.size() - 1) % ring.size()]];
        if ((a[1] > y) != (b[1] > y) && x < (b[0] - a[0]) * (y - a[1]) / (b[1] - a[1]) + a[0]) in = !in;
    }
    return in;
}

bool InsideLoaded(const Graph& g, const std::vector<uint32_t>& ring, float x, float y) {
    bool in = false;
    for (size_t i = 0; i < ring.size(); i++) {
        const Vec3 &a = g.verts[ring[i]], &b = g.verts[ring[(i + ring.size() - 1) % ring.size()]];
        if ((a.y > y) != (b.y > y) && x < (b.x - a.x) * (y - a.y) / (b.y - a.y) + a.x) in = !in;
    }
    return in;
}

// ground the loaded graph had under a roof (its outdoor bit clear) stays so where the new ground lies on it
void KeepGroundFlags(const Graph& loaded, Graph& g, size_t outdoor) {
    for (const Polygon& old : loaded.polygons) {
        if (old.building != kNone || (old.flags & kOutdoor) || old.corners.empty()) continue;
        for (size_t i = 0; i < outdoor; i++) {
            const Vec3& at = g.polygons[i].pos;
            if (InsideLoaded(loaded, old.corners, at.x, at.y)) g.polygons[i].flags = old.flags;
        }
    }
}

// islands: polygons reached through links, numbered from the first polygon; none when there is one
void NumberIslands(Graph& g) {
    std::vector<int> island(g.polygons.size(), -1);
    int count = 0;
    for (size_t s = 0; s < g.polygons.size(); s++) {
        if (island[s] >= 0) continue;
        std::vector<size_t> stack{s};
        island[s] = count;
        while (!stack.empty()) {
            size_t u = stack.back();
            stack.pop_back();
            for (uint32_t v : g.polygons[u].links) {
                if (island[v] < 0) {
                    island[v] = count;
                    stack.push_back(v);
                }
            }
        }
        count++;
    }
    for (size_t i = 0; i < g.polygons.size(); i++) g.polygons[i].island = count > 1 ? uint16_t(island[i]) : kNone;
}

Graph BuildArea(const NavArea& area, const Graph* loaded, const std::vector<NavRing>& obstacles,
                const GroundHeight& height) {
    Graph g;
    std::copy(std::begin(kDefaultSizes), std::end(kDefaultSizes), g.sizes);
    NavRing outline = area.points;
    if (SignedArea(outline) < 0) std::reverse(outline.begin(), outline.end());
    std::map<P2, uint32_t> ids;
    auto vert = [&](double x, double y, float z) {
        P2 key{Snap(x), Snap(y)};
        auto [it, added] = ids.insert({key, uint32_t(g.verts.size())});
        if (added) g.verts.push_back({float(key[0]), float(key[1]), z});
        return it->second;
    };
    for (const NavPoint& p : outline) g.boundary.push_back(vert(p[0], p[1], p[2]));

    Box box = BoxOfRing(outline);
    std::vector<Ring2> rings{{}};
    for (const NavPoint& p : outline) rings[0].push_back({Snap(p[0]), Snap(p[1])});
    for (const NavRing& o : obstacles) {
        if (o.size() < 3 || !BoxOfRing(o).Overlaps(box)) continue;
        Ring2 r;
        std::vector<uint32_t> corners;
        for (const NavPoint& p : o) {
            r.push_back({Snap(p[0]), Snap(p[1])});
            corners.push_back(vert(p[0], p[1], height(p[0], p[1])));
        }
        rings.push_back(r);
        g.obstacles.push_back(corners);
    }
    Mesh mesh = MeshOf(Trapezoids(rings));
    Merge(mesh);
    for (size_t i = 0; i < mesh.rings.size(); i++) {
        Polygon p;
        for (uint32_t v : mesh.rings[i]) {
            const P2& q = mesh.verts[v];
            p.corners.push_back(vert(q[0], q[1], height(float(q[0]), float(q[1]))));
        }
        p.links.assign(mesh.links[i].begin(), mesh.links[i].end());
        auto [c, r2] = Spread(g.verts, p.corners);
        p.pos = {c.x, c.y, height(c.x, c.y)};
        p.radius2 = Bits(r2);
        g.polygons.push_back(std::move(p));
    }
    size_t outdoor = g.polygons.size();

    if (loaded) {
        std::copy(std::begin(loaded->sizes), std::end(loaded->sizes), g.sizes);
        KeepGroundFlags(*loaded, g, outdoor);
        g.exits = loaded->exits;
        g.hides = loaded->hides;
        // the buildings' nodes follow the ground, their links into the old ground going to the polygon under them
        std::vector<uint32_t> index(loaded->polygons.size(), UINT32_MAX);
        for (size_t i = 0; i < loaded->polygons.size(); i++) {
            if (loaded->polygons[i].building == kNone) continue;
            index[i] = uint32_t(g.polygons.size());
            g.polygons.push_back(loaded->polygons[i]);
        }
        for (size_t i = outdoor; i < g.polygons.size(); i++) {
            Polygon& p = g.polygons[i];
            bool door = false;
            std::vector<uint32_t> links;
            for (uint32_t l : p.links) {
                if (l < index.size() && index[l] != UINT32_MAX) links.push_back(index[l]);
                else door = true;
            }
            if (door && outdoor) {
                size_t best = 0;
                float best_d = 1e30f;
                for (size_t k = 0; k < outdoor; k++) {
                    float dx = g.polygons[k].pos.x - p.pos.x, dy = g.polygons[k].pos.y - p.pos.y;
                    float d = Inside(mesh.verts, mesh.rings[k], p.pos.x, p.pos.y) ? -1.0f : dx * dx + dy * dy;
                    if (d < best_d) {
                        best_d = d;
                        best = k;
                    }
                }
                links.push_back(uint32_t(best));
                g.polygons[best].links.push_back(uint32_t(i));
            }
            p.links = links;
        }
        for (const Group& grp : loaded->groups) {
            Group out;
            out.key = grp.key;
            for (Path path : grp.paths) {
                std::vector<uint32_t> kept;
                for (uint32_t k : path.polygons) {
                    if (k < index.size() && index[k] != UINT32_MAX) kept.push_back(index[k]);
                }
                path.polygons = kept;
                out.paths.push_back(path);
            }
            g.groups.push_back(out);
        }
    }
    NumberIslands(g);
    return g;
}

}  // namespace

std::vector<NavFile> BuildNavGraphs(const NavInput& in, const GroundHeight& height) {
    std::vector<Graph> loaded(in.loaded_graphs.size());
    std::vector<bool> readable(loaded.size());
    for (size_t i = 0; i < loaded.size(); i++) readable[i] = ReadGraph(in.loaded_graphs[i], loaded[i]);

    // the static geometry's obstacles: those the loaded graphs hold that no loaded ForbiddenArea made
    std::vector<NavRing> obstacles;
    for (size_t i = 0; i < loaded.size(); i++) {
        if (!readable[i]) continue;
        for (const auto& o : loaded[i].obstacles) {
            NavRing r;
            for (uint32_t v : o) {
                if (v < loaded[i].verts.size()) r.push_back({loaded[i].verts[v].x, loaded[i].verts[v].y, 0});
            }
            auto same = [&](const NavRing& k) { return SameRing(k, r); };
            bool forbidden = std::any_of(in.loaded_forbidden.begin(), in.loaded_forbidden.end(), same);
            if (!forbidden && std::none_of(obstacles.begin(), obstacles.end(), same)) obstacles.push_back(r);
        }
    }
    obstacles.insert(obstacles.end(), in.forbidden.begin(), in.forbidden.end());

    std::vector<std::string> loaded_names = in.loaded_areas;
    std::sort(loaded_names.begin(), loaded_names.end());
    std::vector<NavArea> areas = in.areas;
    std::sort(areas.begin(), areas.end(), [](const NavArea& a, const NavArea& b) { return a.name < b.name; });

    std::vector<NavFile> out;
    for (const NavArea& area : areas) {
        auto it = std::find(loaded_names.begin(), loaded_names.end(), area.name);
        size_t at = size_t(it - loaded_names.begin());
        const Graph* from = it != loaded_names.end() && at < loaded.size() && readable[at] ? &loaded[at] : nullptr;
        out.push_back(WriteGraph(BuildArea(area, from, obstacles, height), uint32_t(out.size())));
    }
    // the waypoint buildings' own graphs, after the areas', as they loaded
    for (size_t i = loaded_names.size(); i < loaded.size(); i++) {
        NavFile f = in.loaded_graphs[i];
        uint32_t id = uint32_t(out.size());
        if (f.size() >= 8) std::memcpy(f.data() + 4, &id, 4);
        out.push_back(std::move(f));
    }
    return out;
}

NavShape ShapeOfGraph(const NavFile& file) {
    Graph g;
    NavShape s;
    if (!ReadGraph(file, g)) return s;
    for (const Vec3& v : g.verts) s.verts.push_back({v.x, v.y, v.z});
    s.boundary = g.boundary;
    s.obstacles = g.obstacles;
    for (const Polygon& p : g.polygons) {
        if (p.flags & kWaypointNode) s.nodes.push_back({p.pos.x, p.pos.y, p.pos.z});
        else s.polygons.push_back(p.corners);
    }
    return s;
}

NavFile NavManagerFile(uint32_t graphs) {
    Writer w;
    w.U32(kManagerVersion);
    w.U32(graphs);
    return std::move(w.b);
}

}  // namespace fcip::editor
