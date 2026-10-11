#pragma once
// RSV1: Tricky donor ResetZone collision surfaces, checked at the EE rider update boundary.
// The sidecar holds donor-space triangles and the course placement. A swept
// triangle hit redirects the current guest call to SSX 3's
// own course-reset entry (0x11d390), which chooses the route respawn and fade.

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

namespace ps2_ssx3_tricky_resetvol
{
constexpr size_t kMaxTriangles = 2048u;
constexpr uint32_t kResetEntry = 0x0011d390u;
constexpr uint32_t kRiderUpdateSource = 0x00128ddcu;
constexpr uint32_t kRiderUpdateTarget = 0x001216e0u;

struct Triangle
{
    uint32_t donorOrdinal = 0;
    double vertex[3][3] = {};
};

struct Course
{
    std::string archive;
    int event = -1;
    double source[3] = {};
    double target[3] = {};
    double cosine = 1.0;
    double sine = 0.0;
    double scale = 1.0;
    std::vector<Triangle> triangles;
};

inline bool enabled() noexcept
{
    static const bool value = [] {
        const char *s = std::getenv("PS2X_SSX3_TRICKY_RESETVOL");
        return s && std::strcmp(s, "1") == 0;
    }();
    return value;
}

inline bool parseTableText(const std::string &text, std::vector<Course> &courses, std::string &error)
{
    courses.clear();
    std::istringstream input(text);
    std::string line;
    size_t total = 0;
    unsigned lineNo = 0;
    while (std::getline(input, line))
    {
        ++lineNo;
        if (line.empty() || line[0] == '#') continue;
        std::istringstream row(line);
        std::string kind, key, extra;
        row >> kind >> key;
        if (kind == "course")
        {
            Course c;
            c.archive = key;
            double yaw = 0.0;
            if (key.empty() || key.size() > 15u || !(row >> c.event >> c.source[0] >> c.source[1] >> c.source[2]
                >> c.target[0] >> c.target[1] >> c.target[2] >> yaw >> c.scale) || (row >> extra)
                || c.event < 0 || c.event > 31 || !std::isfinite(yaw) || !std::isfinite(c.scale)
                || c.scale <= 0.0)
            { error = "bad course at line " + std::to_string(lineNo); courses.clear(); return false; }
            bool valid = true;
            for (int i = 0; i < 3; ++i)
                valid &= std::isfinite(c.source[i]) && std::isfinite(c.target[i]);
            for (const Course &old : courses)
                valid &= old.archive != key && old.event != c.event;
            if (!valid)
            { error = "invalid/duplicate course at line " + std::to_string(lineNo); courses.clear(); return false; }
            constexpr double pi = 3.14159265358979323846;
            c.cosine = std::cos(yaw * pi / 180.0);
            c.sine = std::sin(yaw * pi / 180.0);
            courses.push_back(std::move(c));
        }
        else if (kind == "tri")
        {
            Course *course = nullptr;
            for (Course &c : courses)
                if (c.archive == key) { course = &c; break; }
            Triangle tri;
            if (!course || !(row >> tri.donorOrdinal >> tri.vertex[0][0] >> tri.vertex[0][1] >> tri.vertex[0][2]
                          >> tri.vertex[1][0] >> tri.vertex[1][1] >> tri.vertex[1][2]
                          >> tri.vertex[2][0] >> tri.vertex[2][1] >> tri.vertex[2][2])
                || (row >> extra) || total >= kMaxTriangles)
            { error = "bad triangle at line " + std::to_string(lineNo); courses.clear(); return false; }
            bool valid = true;
            for (const auto &vertex : tri.vertex)
                for (double x : vertex) valid &= std::isfinite(x);
            if (!valid)
            { error = "invalid triangle at line " + std::to_string(lineNo); courses.clear(); return false; }
            course->triangles.push_back(tri);
            ++total;
        }
        else
        { error = "unknown row at line " + std::to_string(lineNo); courses.clear(); return false; }
    }
    if (courses.empty() || total == 0u)
    { error = "empty table"; courses.clear(); return false; }
    error.clear();
    return true;
}

inline const std::vector<Course> &table()
{
    static const std::vector<Course> value = [] {
        std::vector<Course> out;
        const char *path = std::getenv("PS2X_SSX3_TRICKY_RESETVOL_TABLE");
        std::string error = "table path unset";
        if (path && *path)
        {
            FILE *f = std::fopen(path, "rb");
            if (!f) error = "open failed";
            else
            {
                std::string text;
                char buf[8192];
                size_t n = 0;
                while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0 && text.size() <= (1u << 20))
                    text.append(buf, n);
                std::fclose(f);
                if (text.size() > (1u << 20)) error = "table exceeds 1 MiB";
                else parseTableText(text, out, error);
            }
        }
        size_t count = 0;
        for (const Course &c : out) count += c.triangles.size();
        std::fprintf(stderr, "[rsv1] table '%s': %s\n", path ? path : "(unset)",
                     error.empty() ? (std::to_string(count) + " triangles").c_str() : error.c_str());
        return out;
    }();
    return value;
}

inline const Course *findCourse(const std::vector<Course> &courses, int event, const std::string &archive) noexcept
{
    for (const Course &c : courses)
        if (c.event == event && c.archive == archive) return &c;
    return nullptr;
}

inline void toDonor(const Course &course, const double host[3], double donor[3]) noexcept
{
    const double dx = (host[0] - course.target[0]) / course.scale;
    const double dy = (host[1] - course.target[1]) / course.scale;
    donor[0] = course.source[0] + course.cosine * dx + course.sine * dy;
    donor[1] = course.source[1] - course.sine * dx + course.cosine * dy;
    donor[2] = course.source[2] + (host[2] - course.target[2]) / course.scale;
}

inline double dot(const double a[3], const double b[3]) noexcept
{ return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

inline bool crosses(const Triangle &tri, const double from[3], const double to[3]) noexcept
{
    const auto &v = tri.vertex;
    const double ab[3] = {v[1][0] - v[0][0], v[1][1] - v[0][1], v[1][2] - v[0][2]};
    const double ac[3] = {v[2][0] - v[0][0], v[2][1] - v[0][1], v[2][2] - v[0][2]};
    const double normal[3] = {ab[1] * ac[2] - ab[2] * ac[1], ab[2] * ac[0] - ab[0] * ac[2],
                              ab[0] * ac[1] - ab[1] * ac[0]};
    const double f[3] = {from[0] - v[0][0], from[1] - v[0][1], from[2] - v[0][2]};
    const double t[3] = {to[0] - v[0][0], to[1] - v[0][1], to[2] - v[0][2]};
    const double df = dot(normal, f), dt = dot(normal, t);
    if ((df > 0.0 && dt > 0.0) || (df < 0.0 && dt < 0.0) || df == dt) return false;
    const double step = df / (df - dt);
    const double point[3] = {f[0] + step * (t[0] - f[0]), f[1] + step * (t[1] - f[1]),
                             f[2] + step * (t[2] - f[2])};
    const double aa = dot(ab, ab), bb = dot(ac, ac), abac = dot(ab, ac);
    const double denom = aa * bb - abac * abac;
    if (denom < 1.0) return false;
    const double pa = dot(point, ab), pb = dot(point, ac);
    const double u = (bb * pa - abac * pb) / denom;
    const double w = (aa * pb - abac * pa) / denom;
    // The donor quad is a collision surface. A small edge tolerance absorbs
    // PBD mesh quantization without turning its enclosing AABB into a trigger.
    constexpr double epsilon = 0.03;
    return u >= -epsilon && w >= -epsilon && u + w <= 1.0 + epsilon;
}

inline int firstCrossing(const Course &course, const double from[3], const double to[3]) noexcept
{
    for (const Triangle &tri : course.triangles)
        if (crosses(tri, from, to)) return static_cast<int>(tri.donorOrdinal);
    return -1;
}

struct RiderEdge
{
    uint32_t rider = 0;
    int event = -1;
    bool hasPrevious = false;
    double previous[3] = {};
};
struct State { std::array<RiderEdge, 6> riders{}; };

inline State &state() noexcept { static State value; return value; }

// Returns donor ordinal when the rider path crosses a donor collision triangle.
inline int poll(State &state, const std::vector<Course> &courses, const uint8_t *ram, size_t ramSize,
                bool tricky, int event, const std::string &archive, uint32_t riderPtr) noexcept
{
    if (!tricky)
    { state = State{}; return -1; }
    const Course *course = findCourse(courses, event, archive);
    if (!course || course->triangles.empty()) return -1;
    constexpr uint32_t mask = 0x01ffffffu;
    const uint32_t r = riderPtr & mask;
    if (!ram || r < 0x1000u || (r & 3u) || r + 0x884u < r || r + 0x884u > ramSize)
        return -1;
    uint32_t slot = 6u, riderMode = 0u;
    float xyz[3] = {};
    std::memcpy(&slot, ram + r + 0x86cu, 4);
    std::memcpy(&riderMode, ram + r + 0x880u, 4);
    std::memcpy(xyz, ram + r + 0x110u, sizeof xyz);
    if (slot >= state.riders.size() || !std::isfinite(xyz[0]) || !std::isfinite(xyz[1]) || !std::isfinite(xyz[2]))
        return -1;
    RiderEdge &edge = state.riders[slot];
    if (edge.rider != r || edge.event != event)
        edge = RiderEdge{r, event, false, {}};
    const double host[3] = {xyz[0], xyz[1], xyz[2]};
    double donor[3];
    toDonor(*course, host, donor);
    const double delta[3] = {donor[0] - edge.previous[0], donor[1] - edge.previous[1],
                             donor[2] - edge.previous[2]};
    const bool continuous = edge.hasPrevious && dot(delta, delta) < 4000.0 * 4000.0;
    const int hit = continuous && riderMode == 7u ? firstCrossing(*course, edge.previous, donor) : -1;
    std::memcpy(edge.previous, donor, sizeof donor);
    edge.hasPrevious = true;
    return hit;
}
} // namespace ps2_ssx3_tricky_resetvol
