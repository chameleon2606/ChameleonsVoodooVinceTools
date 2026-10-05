#include "animation_applier.h"
#include <filesystem>
#include <set>
//#include "vince_anim_import.hpp"
#include "imgui.h"
#include "main_window.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <atomic>
#include <mutex>
#include <thread>
#include <functional>

// IMPORTANT INFORMATION!!
// the animation applier is NOT my work
// it is completely vibe-coded and I do NOT take credit for that part.

struct Json {
    enum Type { Null, Bool, Number, String, Array, Object } type = Null;
    bool b = false;
    std::string s;  // Number: raw text; String: raw (still escaped) contents
    std::vector<Json> a;
    std::vector<std::pair<std::string, Json>> o;
 
    Json* find(const std::string& k) {
        for (auto& p : o) if (p.first == k) return &p.second;
        return nullptr;
    }
    const Json* find(const std::string& k) const {
        for (auto& p : o) if (p.first == k) return &p.second;
        return nullptr;
    }
    Json& operator[](const std::string& k) {  // creates the key if missing
        if (Json* v = find(k)) return *v;
        o.emplace_back(k, Json());
        return o.back().second;
    }
    void erase(const std::string& k) {
        for (size_t i = 0; i < o.size(); ++i)
            if (o[i].first == k) { o.erase(o.begin() + i); return; }
    }
    double num() const { return type == Number ? std::strtod(s.c_str(), nullptr) : 0.0; }
 
    static Json makeArray() { Json j; j.type = Array; return j; }
    static Json makeObject() { Json j; j.type = Object; return j; }
    static Json makeNumber(double d) {
        Json j; j.type = Number;
        char buf[40];
        if (d == std::floor(d) && std::fabs(d) < 1e15) std::snprintf(buf, sizeof buf, "%.0f", d);
        else std::snprintf(buf, sizeof buf, "%.9g", d);  // round-trips float32 exactly
        j.s = buf;
        return j;
    }
    static Json makeRawString(const std::string& escaped) { Json j; j.type = String; j.s = escaped; return j; }
    static Json makeString(const std::string& plain) {
        std::string e;
        for (unsigned char c : plain) {
            if (c == '"' || c == '\\') { e += '\\'; e += char(c); }
            else if (c < 0x20) { char buf[8]; std::snprintf(buf, sizeof buf, "\\u%04x", c); e += buf; }
            else e += char(c);
        }
        return makeRawString(e);
    }
};
 
class JsonParser {
public:
    JsonParser(const char* b, const char* e) : p(b), end(e) {}
    Json parse() {
        Json v = value();
        ws();
        if (p != end) fail("trailing characters");
        return v;
    }
private:
    const char* p; const char* end;
    [[noreturn]] void fail(const char* m) { throw std::runtime_error(std::string("JSON parse error: ") + m); }
    void ws() { while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p; }
    bool lit(const char* w) {
        size_t n = std::strlen(w);
        if (size_t(end - p) >= n && std::memcmp(p, w, n) == 0) { p += n; return true; }
        return false;
    }
    std::string rawString() {
        if (p >= end || *p != '"') fail("expected string");
        const char* s = ++p;
        while (p < end && *p != '"') { if (*p == '\\') ++p; ++p; }
        if (p >= end) fail("unterminated string");
        return std::string(s, p++);
    }
    Json value() {
        ws();
        if (p >= end) fail("unexpected end");
        Json v;
        char c = *p;
        if (c == '{') {
            v.type = Json::Object; ++p; ws();
            if (p < end && *p == '}') { ++p; return v; }
            for (;;) {
                ws(); std::string k = rawString(); ws();
                if (p >= end || *p != ':') fail("expected ':'");
                ++p;
                v.o.emplace_back(std::move(k), value());
                ws();
                if (p < end && *p == ',') { ++p; continue; }
                if (p < end && *p == '}') { ++p; return v; }
                fail("expected ',' or '}'");
            }
        }
        if (c == '[') {
            v.type = Json::Array; ++p; ws();
            if (p < end && *p == ']') { ++p; return v; }
            for (;;) {
                v.a.push_back(value()); ws();
                if (p < end && *p == ',') { ++p; continue; }
                if (p < end && *p == ']') { ++p; return v; }
                fail("expected ',' or ']'");
            }
        }
        if (c == '"') { v.type = Json::String; v.s = rawString(); return v; }
        if (lit("true")) { v.type = Json::Bool; v.b = true; return v; }
        if (lit("false")) { v.type = Json::Bool; return v; }
        if (lit("null")) return v;
        const char* s = p;
        while (p < end && (std::strchr("+-.eE", *p) || (*p >= '0' && *p <= '9'))) ++p;
        if (s == p) fail("unexpected character");
        v.type = Json::Number; v.s.assign(s, p);
        return v;
    }
};
 
inline void writeJson(const Json& v, std::string& out) {
    switch (v.type) {
    case Json::Null: out += "null"; break;
    case Json::Bool: out += v.b ? "true" : "false"; break;
    case Json::Number: out += v.s; break;
    case Json::String: out += '"'; out += v.s; out += '"'; break;
    case Json::Array:
        out += '[';
        for (size_t i = 0; i < v.a.size(); ++i) { if (i) out += ','; writeJson(v.a[i], out); }
        out += ']';
        break;
    case Json::Object:
        out += '{';
        for (size_t i = 0; i < v.o.size(); ++i) {
            if (i) out += ',';
            out += '"'; out += v.o[i].first; out += "\":";
            writeJson(v.o[i].second, out);
        }
        out += '}';
        break;
    }
}
 
// ---------------------------------------------------------------- math
struct Quat { double x = 0, y = 0, z = 0, w = 1; };
inline Quat qmul(const Quat& a, const Quat& b) {
    return { a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
             a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
             a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
             a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z };
}
inline Quat qinv(const Quat& q) { return { -q.x, -q.y, -q.z, q.w }; }
inline Quat qnorm(Quat q) {
    double n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (n > 0) { q.x /= n; q.y /= n; q.z /= n; q.w /= n; }
    return q;
}
inline double qdot(const Quat& a, const Quat& b) { return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w; }
inline Quat qaxis(int axis, double ang) {
    Quat q; double s = std::sin(ang * 0.5);
    q.w = std::cos(ang * 0.5);
    (axis == 0 ? q.x : axis == 1 ? q.y : q.z) = s;
    return q;
}
// X applied first, then Y, then Z  (R = Rz * Ry * Rx)
inline Quat eulerXYZ(double rx, double ry, double rz) {
    return qmul(qaxis(2, rz), qmul(qaxis(1, ry), qaxis(0, rx)));
}
// rotation angle in [0, pi] and unit axis of a quaternion
inline double qangle(const Quat& q) {
    double w = std::fabs(q.w) > 1 ? 1 : std::fabs(q.w);
    return 2 * std::acos(w);
}
inline void qaxisOf(const Quat& q, double v[3]) {
    double s = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z), sg = q.w < 0 ? -1 : 1;
    if (s < 1e-12) { v[0] = 1; v[1] = v[2] = 0; return; }
    v[0] = sg * q.x / s; v[1] = sg * q.y / s; v[2] = sg * q.z / s;
}
// shortest-arc rotation taking unit vector a onto unit vector b
inline Quat qfromTo(const double a[3], const double b[3]) {
    double d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    if (d < -0.999999) {  // opposite: rotate 180deg about any perpendicular axis
        double p[3] = { 0, -a[2], a[1] };
        if (p[1] * p[1] + p[2] * p[2] < 1e-12) { p[0] = -a[1]; p[1] = a[0]; p[2] = 0; }
        return qnorm({ p[0], p[1], p[2], 0 });
    }
    return qnorm({ a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0], 1 + d });
}
 
inline void matFromQuat(const Quat& q, double m[3][3]) {
    double x = q.x, y = q.y, z = q.z, w = q.w;
    m[0][0] = 1 - 2 * (y * y + z * z); m[0][1] = 2 * (x * y - z * w);     m[0][2] = 2 * (x * z + y * w);
    m[1][0] = 2 * (x * y + z * w);     m[1][1] = 1 - 2 * (x * x + z * z); m[1][2] = 2 * (y * z - x * w);
    m[2][0] = 2 * (x * z - y * w);     m[2][1] = 2 * (y * z + x * w);     m[2][2] = 1 - 2 * (x * x + y * y);
}
 
inline Quat quatFromMatrix(const double m[3][3]) {  // m[row][col], orthonormal
    Quat q; double tr = m[0][0] + m[1][1] + m[2][2];
    if (tr > 0) {
        double s = std::sqrt(tr + 1.0) * 2;
        q.w = 0.25 * s; q.x = (m[2][1] - m[1][2]) / s; q.y = (m[0][2] - m[2][0]) / s; q.z = (m[1][0] - m[0][1]) / s;
    } else if (m[0][0] > m[1][1] && m[0][0] > m[2][2]) {
        double s = std::sqrt(1.0 + m[0][0] - m[1][1] - m[2][2]) * 2;
        q.w = (m[2][1] - m[1][2]) / s; q.x = 0.25 * s; q.y = (m[0][1] + m[1][0]) / s; q.z = (m[0][2] + m[2][0]) / s;
    } else if (m[1][1] > m[2][2]) {
        double s = std::sqrt(1.0 + m[1][1] - m[0][0] - m[2][2]) * 2;
        q.w = (m[0][2] - m[2][0]) / s; q.x = (m[0][1] + m[1][0]) / s; q.y = 0.25 * s; q.z = (m[1][2] + m[2][1]) / s;
    } else {
        double s = std::sqrt(1.0 + m[2][2] - m[0][0] - m[1][1]) * 2;
        q.w = (m[1][0] - m[0][1]) / s; q.x = (m[0][2] + m[2][0]) / s; q.y = (m[1][2] + m[2][1]) / s; q.z = 0.25 * s;
    }
    return qnorm(q);
}
 
// ---------------------------------------------------------------- .anim reader
class AnimFile {
public:
    enum { CH_UNKNOWN, CH_PX, CH_PY, CH_PZ, CH_RX, CH_RY, CH_RZ, CH_SX, CH_SY, CH_SZ };
    struct Channel { uint32_t count; float rest; uint32_t offset; };
    struct Event { std::string name; int bone; std::vector<float> times; };
 
    float duration = 0;
    uint32_t boneCount = 0;
 
    explicit AnimFile(const std::vector<uint8_t>& bytes) : d(bytes) {
        if (d.size() < 52 || std::memcmp(d.data(), "ANIM", 4) != 0) throw std::runtime_error("not an ANIM file");
        version   = rd<uint32_t>(4);
        duration  = rd<float>(8);
        boneCount = rd<uint32_t>(12);
        eventCount = rd<uint32_t>(16);
        stringCount = rd<uint32_t>(20);
        bonesOffset = rd<uint32_t>(28);
        eventTable = rd<uint32_t>(32);
        stringIndex = rd<uint32_t>(40);
        stringsOffset = rd<uint32_t>(44);
        if (version != 11) std::cerr << "warning: ANIM version " << version << " (tested with 11)\n";
    }
 
    Channel channel(uint32_t bone, int c) const {
        size_t o = size_t(bonesOffset) + 128u * bone;
        return { rd<uint32_t>(o + 8 * c), rd<float>(o + 8 * c + 4), rd<uint32_t>(o + 80 + 4 * c) };
    }
 
    // clampNegativeScale: scale curves that overshoot below zero between two keys are clamped
    // to the segment's smaller key value (authoring artifact, e.g. a popped balloon part)
    double evaluate(uint32_t bone, int c, double t, bool clampNegativeScale = false) const {
        Channel ch = channel(bone, c);
        if (ch.count == 0) return ch.rest;
        size_t off = ch.offset;
        if (ch.count == 1 && c >= CH_RX && c <= CH_RZ) {
            // "SpinKeys" {start, end, cubic, quad, total}: not present in tested data; linear guess
            double s = rd<float>(off), e = rd<float>(off + 4), total = rd<float>(off + 16);
            if (e <= s) return 0;
            double u = (t - s) / (e - s);
            return total * (u < 0 ? 0 : u > 1 ? 1 : u);
        }
        uint32_t n = ch.count;
        double start = rd<float>(off);
        double last = rd<float>(off + 4 + 4 * (n - 1));
        if ((t < start || t > last) && last > start) t = start + std::fmod(t - start + 100 * (last - start), last - start);
        double prev = start;
        for (uint32_t k = 0; k < n; ++k) {
            double tk = rd<float>(off + 4 + 4 * k);
            if (t <= tk + 1e-7 || k == n - 1) {
                double u = tk > prev ? (t - prev) / (tk - prev) : 0.0;
                size_t r = off + 4 + 4 * n + 16 * k;
                double a = rd<float>(r), b = rd<float>(r + 4), cc = rd<float>(r + 8), v = rd<float>(r + 12);
                double val = ((a * u + b) * u + cc) * u + v;
                if (clampNegativeScale && c >= CH_SX && val < 0) {
                    double lo = std::min(v, a + b + cc + v);
                    if (lo >= 0) val = lo;
                }
                return val;
            }
            prev = tk;
        }
        return ch.rest;
    }
 
    // every key time of every curve channel (used so baking hits the keys exactly)
    std::vector<double> keyTimes() const {
        std::vector<double> out;
        for (uint32_t b = 0; b < boneCount; ++b)
            for (int c = CH_PX; c <= CH_SZ; ++c) {
                Channel ch = channel(b, c);
                if (ch.count == 0 || (ch.count == 1 && c >= CH_RX && c <= CH_RZ)) continue;
                out.push_back(rd<float>(ch.offset));
                for (uint32_t k = 0; k < ch.count; ++k) out.push_back(rd<float>(ch.offset + 4 + 4 * k));
            }
        return out;
    }
 
    std::vector<Event> events() const {
        std::vector<std::string> names;
        for (uint32_t i = 0; i < stringCount; ++i) {
            size_t s = size_t(stringsOffset) + rd<uint32_t>(size_t(stringIndex) + 4 * i), e = s;
            while (e < d.size() && d[e]) ++e;
            names.emplace_back(reinterpret_cast<const char*>(d.data()) + s, e - s);
        }
        std::vector<Event> out;
        if (!eventTable) return out;
        for (uint32_t k = 0; k < eventCount; ++k) {
            size_t t = size_t(eventTable) + 16 * k;
            uint32_t timesOff = rd<uint32_t>(t), nameIdx = rd<uint32_t>(t + 4), bone = rd<uint32_t>(t + 8);
            Event ev;
            ev.name = nameIdx < names.size() ? names[nameIdx] : std::to_string(nameIdx);
            ev.bone = int(bone);
            uint32_t cnt = rd<uint32_t>(timesOff);
            for (uint32_t j = 0; j < cnt; ++j)
                if (rd<float>(timesOff + 8 + 8 * j) > 0.5f) ev.times.push_back(rd<float>(timesOff + 4 + 8 * j));
            out.push_back(ev);
        }
        return out;
    }
 
private:
    const std::vector<uint8_t>& d;
    uint32_t version = 0, eventCount = 0, stringCount = 0, bonesOffset = 0, eventTable = 0, stringIndex = 0, stringsOffset = 0;
    template <class T> T rd(size_t off) const {
        if (off + sizeof(T) > d.size()) throw std::runtime_error("ANIM read past end of file");
        T v; std::memcpy(&v, d.data() + off, sizeof v); return v;
    }
};
 
// ---------------------------------------------------------------- file helpers
inline std::vector<uint8_t> readFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open " + path);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
inline void writeFile(const std::string& path, const std::vector<uint8_t>& data) {
    std::ofstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot write " + path);
    f.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
}
inline uint32_t rdU32(const std::vector<uint8_t>& b, size_t o) { uint32_t v; std::memcpy(&v, b.data() + o, 4); return v; }
inline void putU32(std::vector<uint8_t>& b, uint32_t v) { uint8_t t[4]; std::memcpy(t, &v, 4); b.insert(b.end(), t, t + 4); }
inline std::string baseName(const std::string& p) { size_t s = p.find_last_of("/\\"); return s == std::string::npos ? p : p.substr(s + 1); }
inline std::string stripExt(const std::string& p) {
    size_t dot = p.find_last_of('.'), sl = p.find_last_of("/\\");
    return (dot == std::string::npos || (sl != std::string::npos && dot < sl)) ? p : p.substr(0, dot);
}
 
// ---------------------------------------------------------------- importer
// Rigidly attaches a prop bone to another bone for a time range, the way the game's
// cutscenes hold props (e.g. Vince holding a pin): the prop keeps the offset it has
// relative to `target` at `gripTime` and follows the target, blending in/out smoothly.
struct PropAttach {
    std::string prop;          // node name of the prop bone, e.g. "pin1"
    std::string target;        // node name of the bone holding it, e.g. "palmr"
    double start = 0;          // seconds: fully attached from here ...
    double end = 0;            // ... to here
    double blendIn = 0.1;      // seconds of blending before start
    double blendOut = 0.1;     // seconds of blending after end
    double gripTime = -1;      // when the prop's offset in the target is taken (default: start)
};
 
struct ImportOptions {
    std::string outPath;              // default "<model>_anim.glb" / "<model>_anim.gltf"
    double fps = 60.0;                // bake rate
    bool clampNegativeScale = true;   // see header comment
    std::vector<PropAttach> attach;   // optional prop attachments
    bool replaceExisting = false;     // delete the model's existing animations before adding this one
    std::atomic<float>* progress = nullptr;  // optional, set to 0..1 while importing (for a progress bar on another thread)
};

// largest number stored under `key` anywhere in v (-1 if none)
inline void maxIndexOfKey(const Json& v, const char* key, int& maxIdx) {
    if (v.type == Json::Object) {
        for (const auto& [k, c] : v.o) {
            if (k == key && c.type == Json::Number) maxIdx = std::max(maxIdx, int(c.num()));
            maxIndexOfKey(c, key, maxIdx);
        }
    } else if (v.type == Json::Array) {
        for (const Json& c : v.a) maxIndexOfKey(c, key, maxIdx);
    }
}

// Removes all animations, then trims the accessors / bufferViews / buffers / GLB binary
// bytes at the end that nothing references anymore. Animation data is always appended
// last, so trimming from the end frees it without renumbering anything; data that
// something else still uses stops the trim and stays.
inline void removeAnimations(Json& doc, std::vector<uint8_t>& bin, bool glbEmbeddedBin) {
    doc["animations"].a.clear();

    // accessors used by meshes and skins
    int maxAccessor = -1;
    auto use = [&](const Json* j) { if (j && j->type == Json::Number) maxAccessor = std::max(maxAccessor, int(j->num())); };
    if (const Json* meshes = doc.find("meshes"))
        for (const Json& mesh : meshes->a)
            if (const Json* prims = mesh.find("primitives"))
                for (const Json& prim : prims->a) {
                    use(prim.find("indices"));
                    if (const Json* attrs = prim.find("attributes")) for (const auto& [k, a] : attrs->o) use(&a);
                    if (const Json* targets = prim.find("targets"))
                        for (const Json& t : targets->a) for (const auto& [k, a] : t.o) use(&a);
                }
    if (const Json* skins = doc.find("skins"))
        for (const Json& skin : skins->a) use(skin.find("inverseBindMatrices"));
    Json& accessors = doc["accessors"];
    while (int(accessors.a.size()) > maxAccessor + 1) accessors.a.pop_back();

    // bufferViews used by the remaining accessors, images, extensions...
    int maxView = -1;
    maxIndexOfKey(doc, "bufferView", maxView);
    Json& views = doc["bufferViews"];
    while (int(views.a.size()) > maxView + 1) views.a.pop_back();

    // buffers used by the remaining bufferViews (buffer 0 is always kept)
    int maxBuffer = 0;
    for (const Json& v : views.a)
        if (const Json* b = v.find("buffer")) maxBuffer = std::max(maxBuffer, int(b->num()));
    Json& buffers = doc["buffers"];
    while (int(buffers.a.size()) > maxBuffer + 1) buffers.a.pop_back();

    // cut the embedded binary down to the end of the last view that still lives in it
    if (glbEmbeddedBin && !buffers.a.empty()) {
        size_t end = 0;
        for (const Json& v : views.a) {
            const Json* b = v.find("buffer");
            if (b && int(b->num()) != 0) continue;
            const Json* off = v.find("byteOffset");
            const Json* len = v.find("byteLength");
            end = std::max(end, size_t(off ? off->num() : 0) + size_t(len ? len->num() : 0));
        }
        if (end < bin.size()) bin.resize(end);
        buffers.a[0]["byteLength"] = Json::makeNumber(double(bin.size()));
    }
}
 
// Bakes the animation and writes the result. Returns false and fills *error on failure.
inline bool ImportAnimIntoGltf(const std::string& animPath, const std::string& gltfPath,
                               const ImportOptions& opt, std::string* error = nullptr) {
    const std::string& outPathIn = opt.outPath;
    double fps = opt.fps;
    bool clampNegativeScale = opt.clampNegativeScale;
    // the three per-bone passes each make up a third of the progress
    auto reportProgress = [&](int pass, uint32_t bone, uint32_t boneCount) {
        if (opt.progress && boneCount > 0) opt.progress->store((pass + float(bone) / float(boneCount)) / 3.0f);
    };
    if (opt.progress) opt.progress->store(0.0f);
    try {
        // --- load model ---
        std::vector<uint8_t> model = readFile(gltfPath);
        bool isGlb = model.size() >= 12 && std::memcmp(model.data(), "glTF", 4) == 0;
        std::string jsonText;
        std::vector<uint8_t> bin;
        bool hasBin = false;
        if (isGlb) {
            size_t total = std::min<size_t>(rdU32(model, 8), model.size()), o = 12;
            while (o + 8 <= total) {
                uint32_t len = rdU32(model, o), type = rdU32(model, o + 4);
                if (o + 8 + len > total) throw std::runtime_error("corrupt GLB chunk");
                if (type == 0x4E4F534A) jsonText.assign(reinterpret_cast<const char*>(model.data()) + o + 8, len);
                else if (type == 0x004E4942 && !hasBin) { bin.assign(model.begin() + o + 8, model.begin() + o + 8 + len); hasBin = true; }
                o += 8 + len;
            }
            if (jsonText.empty()) throw std::runtime_error("GLB has no JSON chunk");
        } else {
            jsonText.assign(model.begin(), model.end());
        }
        Json doc = JsonParser(jsonText.data(), jsonText.data() + jsonText.size()).parse();
        if (doc.type != Json::Object) throw std::runtime_error("glTF root is not an object");
 
        std::string outPath = outPathIn.empty() ? stripExt(gltfPath) + (isGlb ? "_anim.glb" : "_anim.gltf") : outPathIn;
 
        // make sure every top-level array exists before taking references into doc
        for (const char* k : { "nodes", "buffers", "bufferViews", "accessors", "animations" })
            if (!doc.find(k)) doc[k] = Json::makeArray();
        Json& nodes = doc["nodes"];
        Json& buffers = doc["buffers"];
        Json& views = doc["bufferViews"];
        Json& accessors = doc["accessors"];
        Json& animations = doc["animations"];

        // must run before buffer0Len is read below, since it can shrink the binary
        if (opt.replaceExisting && !animations.a.empty())
            removeAnimations(doc, bin, isGlb && hasBin && !buffers.a.empty() && !buffers.a[0].find("uri"));

        // where the new binary data goes
        bool appendToGlbBin = false;
        size_t buffer0Len = 0;
        if (isGlb && hasBin && !buffers.a.empty() && !buffers.a[0].find("uri")) {
            appendToGlbBin = true;
            if (const Json* bl = buffers.a[0].find("byteLength")) buffer0Len = size_t(bl->num());
            if (buffer0Len > bin.size()) buffer0Len = bin.size();
        } else if (isGlb && !hasBin && buffers.a.empty()) {
            appendToGlbBin = true;
            Json b = Json::makeObject(); b["byteLength"] = Json::makeNumber(0);
            buffers.a.push_back(b);
        }
        size_t baseOffset = (buffer0Len + 3) & ~size_t(3);
        int targetBuffer = appendToGlbBin ? 0 : int(buffers.a.size());
        std::vector<uint8_t> data;
 
        auto addAccessor = [&](const std::vector<float>& v, const char* type, int comps, bool minmax) {
            size_t off = data.size();
            data.resize(off + v.size() * 4);
            std::memcpy(data.data() + off, v.data(), v.size() * 4);
            Json bv = Json::makeObject();
            bv["buffer"] = Json::makeNumber(targetBuffer);
            bv["byteOffset"] = Json::makeNumber(double((appendToGlbBin ? baseOffset : 0) + off));
            bv["byteLength"] = Json::makeNumber(double(v.size() * 4));
            views.a.push_back(bv);
            Json acc = Json::makeObject();
            acc["bufferView"] = Json::makeNumber(double(views.a.size() - 1));
            acc["componentType"] = Json::makeNumber(5126);
            acc["count"] = Json::makeNumber(double(v.size() / comps));
            acc["type"] = Json::makeRawString(type);
            if (minmax) {
                float mn = v[0], mx = v[0];
                for (float f : v) { mn = std::min(mn, f); mx = std::max(mx, f); }
                Json a1 = Json::makeArray(), a2 = Json::makeArray();
                a1.a.push_back(Json::makeNumber(mn)); a2.a.push_back(Json::makeNumber(mx));
                acc["min"] = a1; acc["max"] = a2;
            }
            accessors.a.push_back(acc);
            return int(accessors.a.size() - 1);
        };
 
        // --- load animation ---
        std::vector<uint8_t> animBytes = readFile(animPath);
        AnimFile anim(animBytes);
        size_t modelBones = 0;  // nodes, not counting helper nodes added by an earlier import
        for (const Json& n : nodes.a) {
            const Json* ex = n.find("extras");
            if (!(ex && ex->find("vvanimFrameOf"))) ++modelBones;
        }
        if (anim.boneCount != modelBones)
            std::cerr << "warning: anim has " << anim.boneCount << " bones, model has " << modelBones
                      << " nodes; matching by index\n";
        uint32_t boneCount = std::min<uint32_t>(anim.boneCount, uint32_t(nodes.a.size()));
 
        // sample times: fixed-rate grid plus every original key time, sorted, near-duplicates merged
        if (fps <= 0) fps = 60;
        int frames = std::max(1, int(std::lround(anim.duration * fps)));
        std::vector<double> grid;
        for (int k = 0; k < frames; ++k) grid.push_back(k / fps);
        for (double t : anim.keyTimes()) if (t > 0 && t < anim.duration) grid.push_back(t);
        for (const PropAttach& at : opt.attach)
            for (double t : { at.start - at.blendIn, at.start, at.end, at.end + at.blendOut, at.gripTime < 0 ? at.start : at.gripTime })
                if (t > 0 && t < anim.duration) grid.push_back(t);
        grid.push_back(anim.duration);
        std::sort(grid.begin(), grid.end());
        std::vector<float> times;
        for (double t : grid)
            if (times.empty() || t - times.back() > 1e-4) times.push_back(float(t));
        times.back() = anim.duration;
        int timeAcc = addAccessor(times, "SCALAR", 1, true);
 
        Json animation = Json::makeObject();
        std::string animName = stripExt(baseName(animPath));
        animation["name"] = Json::makeString(animName);
        Json samplers = Json::makeArray(), channels = Json::makeArray();
 
        auto addChannel = [&](uint32_t node, const char* path, int acc) {
            Json s = Json::makeObject();
            s["input"] = Json::makeNumber(timeAcc);
            s["output"] = Json::makeNumber(acc);
            s["interpolation"] = Json::makeRawString("LINEAR");
            samplers.a.push_back(s);
            Json c = Json::makeObject(), tgt = Json::makeObject();
            c["sampler"] = Json::makeNumber(double(samplers.a.size() - 1));
            tgt["node"] = Json::makeNumber(node);
            tgt["path"] = Json::makeRawString(path);
            c["target"] = tgt;
            channels.a.push_back(c);
        };
 
        // ---- scale handling -------------------------------------------------------
        // The game does not inherit scale (like Maya's segment scale compensate): a bone's
        // scale sizes its own part and the offsets to its children, not the children
        // themselves. glTF always inherits scale.
        //  * uniformly scaled bone: its children get scale / parentScale (exact)
        //  * bone whose scale is ever non-uniform and that has children: it gets an
        //    unscaled helper node "<name>_frame" carrying its translation / rotation. Its
        //    children hang under the helper (offsets pre-multiplied by the bone's scale) and
        //    the bone node keeps only its scale, so the stretch can't skew the children.
        //    Exact. Joint indices, skin and mesh stay unchanged; the helper is reused when
        //    further animations are imported into the same file.
        auto readTRS = [](const Json& node, double t[3], Quat& q, double sc[3]) {
            t[0] = t[1] = t[2] = 0; sc[0] = sc[1] = sc[2] = 1; q = Quat();
            bool hadMatrix = false;
            if (const Json* m = node.find("matrix"); m && m->a.size() == 16) {
                hadMatrix = true;
                double M[3][3];
                for (int c = 0; c < 3; ++c) {
                    double len = 0;
                    for (int r = 0; r < 3; ++r) len += m->a[c * 4 + r].num() * m->a[c * 4 + r].num();
                    sc[c] = std::sqrt(len);
                    for (int r = 0; r < 3; ++r) M[r][c] = sc[c] > 0 ? m->a[c * 4 + r].num() / sc[c] : 0;
                }
                for (int r = 0; r < 3; ++r) t[r] = m->a[12 + r].num();
                q = quatFromMatrix(M);
            } else {
                if (const Json* v = node.find("translation"); v && v->a.size() == 3) for (int k = 0; k < 3; ++k) t[k] = v->a[k].num();
                if (const Json* v = node.find("rotation"); v && v->a.size() == 4) q = { v->a[0].num(), v->a[1].num(), v->a[2].num(), v->a[3].num() };
                if (const Json* v = node.find("scale"); v && v->a.size() == 3) for (int k = 0; k < 3; ++k) sc[k] = v->a[k].num();
            }
            q = qnorm(q);
            return hadMatrix;
        };
        auto vec = [](const double* v, int n) {
            Json a = Json::makeArray();
            for (int k = 0; k < n; ++k) a.a.push_back(Json::makeNumber(v[k]));
            return a;
        };
        auto writeTRS = [&](Json& node, const double t[3], const Quat& q, const double sc[3]) {
            node.erase("matrix");
            double qa[4] = { q.x, q.y, q.z, q.w };
            node["translation"] = vec(t, 3); node["rotation"] = vec(qa, 4); node["scale"] = vec(sc, 3);
        };
 
        std::vector<int> parentOf(nodes.a.size(), -1), owner(nodes.a.size(), -1), frameOf(nodes.a.size(), -1);
        for (size_t n = 0; n < nodes.a.size(); ++n) {
            if (const Json* ch = nodes.a[n].find("children"))
                for (const Json& c : ch->a) {
                    size_t ci = size_t(c.num());
                    if (ci < parentOf.size()) parentOf[ci] = int(n);
                }
            if (const Json* ex = nodes.a[n].find("extras"))
                if (const Json* f = ex->find("vvanimFrameOf"); f && f->type == Json::Number) {
                    size_t j = size_t(f->num());
                    if (j < nodes.a.size()) { owner[n] = int(j); frameOf[j] = int(n); }
                }
        }
        // parent in the game's hierarchy (skips helper nodes)
        auto logicalParent = [&](int n) {
            int p = parentOf[n];
            if (p >= 0 && owner[p] == n) p = parentOf[p];
            if (p >= 0 && owner[p] >= 0) p = owner[p];
            return p;
        };
 
        // the game's own scale of every bone at every sample time
        size_t T = times.size();
        std::vector<std::vector<double>> gs(boneCount, std::vector<double>(3 * T));
        std::vector<bool> nonUniform(boneCount, false);
        for (uint32_t b = 0; b < boneCount; ++b)
            for (size_t k = 0; k < T; ++k) {
                for (int c = 0; c < 3; ++c) gs[b][3 * k + c] = anim.evaluate(b, AnimFile::CH_SX + c, times[k], clampNegativeScale);
                double* v = &gs[b][3 * k];
                double m = std::max({ std::fabs(v[0]), std::fabs(v[1]), std::fabs(v[2]), 1e-6 });
                if (std::fabs(v[0] - v[1]) > 1e-4 * m || std::fabs(v[0] - v[2]) > 1e-4 * m) nonUniform[b] = true;
            }
        auto gameScaleAt = [&](int b, size_t k, int c) { return (b >= 0 && uint32_t(b) < boneCount) ? gs[b][3 * k + c] : 1.0; };
 
        std::vector<bool> hasChildren(nodes.a.size(), false);
        for (size_t n = 0; n < nodes.a.size(); ++n)
            if (owner[n] < 0) { int p = logicalParent(int(n)); if (p >= 0) hasChildren[p] = true; }
 
        // create helper nodes where needed
        for (uint32_t b = 0; b < boneCount; ++b) {
            if (frameOf[b] >= 0 || !nonUniform[b] || !hasChildren[b]) continue;
            double jt[3], js[3]; Quat jq;
            readTRS(nodes.a[b], jt, jq, js);
            int F = int(nodes.a.size()), P = parentOf[b];
            Json frame = Json::makeObject();
            std::string jname = "bone" + std::to_string(b);
            if (const Json* nm = nodes.a[b].find("name"); nm && nm->type == Json::String) jname = nm->s;
            frame["name"] = Json::makeRawString(jname + "_frame");
            double qa[4] = { jq.x, jq.y, jq.z, jq.w };
            frame["translation"] = vec(jt, 3);
            frame["rotation"] = vec(qa, 4);
            Json kids = Json::makeArray();
            if (const Json* ch = nodes.a[b].find("children")) kids = *ch;
            for (const Json& c : kids.a) {  // keep the children's bind pose: they no longer inherit the bone's bind scale
                size_t ci = size_t(c.num());
                if (ci >= nodes.a.size() || (std::fabs(js[0] - 1) < 1e-5 && std::fabs(js[1] - 1) < 1e-5 && std::fabs(js[2] - 1) < 1e-5)) continue;
                double ct[3], cs[3]; Quat cq;
                readTRS(nodes.a[ci], ct, cq, cs);
                for (int k = 0; k < 3; ++k) { ct[k] *= js[k]; cs[k] *= js[k]; }
                writeTRS(nodes.a[ci], ct, cq, cs);
            }
            kids.a.push_back(Json::makeNumber(b));
            frame["children"] = kids;
            Json ex = Json::makeObject(); ex["vvanimFrameOf"] = Json::makeNumber(b);
            frame["extras"] = ex;
            nodes.a.push_back(std::move(frame));
            // the bone itself keeps only its scale
            double zero[3] = { 0, 0, 0 };
            nodes.a[b].erase("children");
            writeTRS(nodes.a[b], zero, Quat(), js);
            // hook the helper in where the bone was
            if (P >= 0) {
                Json& pch = nodes.a[P]["children"];
                for (Json& c : pch.a) if (int(c.num()) == int(b)) c = Json::makeNumber(F);
            } else if (Json* scenes = doc.find("scenes")) {
                for (Json& sc : scenes->a)
                    if (Json* sn = sc.find("nodes"))
                        for (Json& c : sn->a) if (int(c.num()) == int(b)) c = Json::makeNumber(F);
            }
            parentOf.push_back(P); owner.push_back(int(b)); frameOf.push_back(-1); hasChildren.push_back(true);
            for (const Json& c : kids.a) { size_t ci = size_t(c.num()); if (ci < parentOf.size()) parentOf[ci] = F; }
            frameOf[b] = F;
            // earlier animations in this file move the bone: retarget them to the helper
            for (Json& an : animations.a)
                if (Json* chs = an.find("channels"))
                    for (Json& c : chs->a)
                        if (Json* tg = c.find("target"))
                            if (const Json* nd = tg->find("node"); nd && int(nd->num()) == int(b))
                                if (const Json* pa = tg->find("path"); pa && (pa->s == "translation" || pa->s == "rotation"))
                                    (*tg)["node"] = Json::makeNumber(F);
        }
 
        // ---- phase A: local transform of every bone at every sample (game semantics) ----
        struct BoneInfo { int trNode; bool restructured, parentRestructured, hadMatrix; int lp; double bt[3], bs[3], fs[3]; Quat bq; };
        std::vector<BoneInfo> info(boneCount);
        std::vector<std::vector<double>> gP(boneCount, std::vector<double>(3 * T));  // translation before parent scale
        std::vector<std::vector<Quat>> gQ(boneCount, std::vector<Quat>(T));         // final local rotation
        for (uint32_t i = 0; i < boneCount; ++i) {
            reportProgress(0, i, boneCount);
            BoneInfo& bi = info[i];
            bi.restructured = frameOf[i] >= 0;
            bi.trNode = bi.restructured ? frameOf[i] : int(i);
            bi.lp = logicalParent(int(i));
            bi.parentRestructured = bi.lp >= 0 && frameOf[bi.lp] >= 0;
            // bind TRS: translation / rotation from the node that carries them, scale from the bone
            double dummyT[3]; Quat dummyQ;
            bi.hadMatrix = readTRS(nodes.a[bi.trNode], bi.bt, bi.bq, bi.fs);
            if (bi.restructured) readTRS(nodes.a[i], dummyT, dummyQ, bi.bs);
            else for (int k = 0; k < 3; ++k) bi.bs[k] = bi.fs[k];
        }
 
        // Mirrored rotation axes. Some models (e.g. Kosmo) have a right side whose bind is a
        // 180deg flip combined with a turn about Y, while the left side has an identity bind.
        // In the game the left bones still rotate about the mirror image of the right
        // side's axes (an axis frame, like the eyelids'); the model export lost it because
        // the rest rotation is zero. Detect such pairs by mirrored bind positions and give
        // the identity-side bone the mirrored axis frame. (Vince: pure flip -> no change.)
        std::vector<char> hasMirrorFrame(boneCount, 0);
        std::vector<Quat> mirrorFrame(boneCount);
        {
            std::vector<double> wpos(3 * boneCount), wrot(9 * boneCount);
            std::vector<char> wdone(boneCount, 0);
            std::function<void(uint32_t)> bindWorld = [&](uint32_t b) {
                if (wdone[b]) return;
                double R[3][3]; matFromQuat(info[b].bq, R);
                int lp = info[b].lp;
                if (lp >= 0 && uint32_t(lp) < boneCount) {
                    bindWorld(uint32_t(lp));
                    const double* PR = &wrot[9 * lp]; const double* PP = &wpos[3 * lp];
                    for (int r = 0; r < 3; ++r) {
                        wpos[3 * b + r] = PP[r];
                        for (int c = 0; c < 3; ++c) {
                            wpos[3 * b + r] += PR[3 * r + c] * info[b].bt[c];
                            double v = 0;
                            for (int k = 0; k < 3; ++k) v += PR[3 * r + k] * R[k][c];
                            wrot[9 * b + 3 * r + c] = v;
                        }
                    }
                } else {
                    for (int r = 0; r < 3; ++r) { wpos[3 * b + r] = info[b].bt[r]; for (int c = 0; c < 3; ++c) wrot[9 * b + 3 * r + c] = R[r][c]; }
                }
                wdone[b] = 1;
            };
            for (uint32_t b = 0; b < boneCount; ++b) bindWorld(b);
            for (uint32_t a = 0; a < boneCount; ++a) {
                if (std::fabs(std::fabs(info[a].bq.w) - 1) > 1e-4) continue;   // identity-bind side only
                const double* pa = &wpos[3 * a];
                if (std::fabs(pa[0]) < 1e-4) continue;
                double norm = std::sqrt(pa[0] * pa[0] + pa[1] * pa[1] + pa[2] * pa[2]);
                int best = -1; double bestD = 1e30;
                for (uint32_t b = 0; b < boneCount; ++b) {
                    if (b == a) continue;
                    const double* pb = &wpos[3 * b];
                    double d = std::sqrt((pb[0] + pa[0]) * (pb[0] + pa[0]) + (pb[1] - pa[1]) * (pb[1] - pa[1]) + (pb[2] - pa[2]) * (pb[2] - pa[2]));
                    if (d < bestD) { bestD = d; best = int(b); }
                }
                if (best < 0 || bestD > 1e-3 * std::max(1.0, norm)) continue;
                const double* Rb = &wrot[9 * best];
                if (Rb[3 * 1 + 1] > -0.5) continue;                             // partner must be on a flipped chain
                const double* Ra = &wrot[9 * a];
                // C = Ra^T * (-S * Rb),  S = diag(-1, 1, 1)
                double A[3][3], C[3][3];
                for (int r = 0; r < 3; ++r) for (int c = 0; c < 3; ++c) A[r][c] = (r == 0 ? 1.0 : -1.0) * Rb[3 * r + c];
                for (int r = 0; r < 3; ++r) for (int c = 0; c < 3; ++c) {
                    double v = 0; for (int k = 0; k < 3; ++k) v += Ra[3 * k + r] * A[k][c]; C[r][c] = v;
                }
                Quat cq = quatFromMatrix(C);
                if (qangle(cq) < 0.5 * 3.14159265358979 / 180) continue;
                hasMirrorFrame[a] = 1; mirrorFrame[a] = cq;
            }
        }
 
        for (uint32_t i = 0; i < boneCount; ++i) {
            reportProgress(1, i, boneCount);
            BoneInfo& bi = info[i];
            const Quat bq = bi.bq;
 
            // How the model's bind rotation combines with the animated Euler rotation:
            //  a) bind is identity / 180deg about X (mirrored right side): local = bind * R(anim)
            //  b) bind is a rotation by the same angle as the anim's rest pose, about a
            //     tilted axis (e.g. eyelids hinged along the eye): the bind defines a
            //     rotation frame C, local = C * R(anim) * C^-1
            //  c) otherwise: local = (bind * R(rest)^-1) * R(anim)
            Quat jo = bq, axisFrame;  // local = jo * C * R * C^-1, C = axisFrame
            bool useAxisFrame = false;
            bool simple = std::fabs(std::fabs(bq.w) - 1) < 1e-4 || std::fabs(std::fabs(bq.x) - 1) < 1e-4;
            if (hasMirrorFrame[i]) {  // identity bind, axes mirrored from the flipped partner
                axisFrame = mirrorFrame[i];
                jo = Quat();
                useAxisFrame = true;
            } else if (!simple) {
                Quat rest = eulerXYZ(anim.channel(i, AnimFile::CH_RX).rest, -anim.channel(i, AnimFile::CH_RY).rest,
                                     -anim.channel(i, AnimFile::CH_RZ).rest);
                double angRest = qangle(rest), angBind = qangle(bq);
                if (angRest > 1e-3 && std::fabs(angRest - angBind) < 2e-3) {
                    double ar[3], ab[3];
                    qaxisOf(rest, ar); qaxisOf(bq, ab);
                    axisFrame = qfromTo(ar, ab);
                    jo = Quat();
                    useAxisFrame = true;
                } else {
                    jo = qmul(bq, qinv(rest));
                }
            }
            for (size_t k = 0; k < T; ++k) {
                double t = times[k];
                gP[i][3 * k + 0] = -anim.evaluate(i, AnimFile::CH_PX, t);
                gP[i][3 * k + 1] = anim.evaluate(i, AnimFile::CH_PY, t);
                gP[i][3 * k + 2] = anim.evaluate(i, AnimFile::CH_PZ, t);
                Quat r = eulerXYZ(anim.evaluate(i, AnimFile::CH_RX, t), -anim.evaluate(i, AnimFile::CH_RY, t),
                                  -anim.evaluate(i, AnimFile::CH_RZ, t));
                if (useAxisFrame) r = qmul(axisFrame, qmul(r, qinv(axisFrame)));
                gQ[i][k] = qnorm(qmul(jo, r));
            }
        }
 
        // ---- phase B: optional prop attachments ----
        if (!opt.attach.empty()) {
            auto findNode = [&](const std::string& name) {
                for (uint32_t n = 0; n < boneCount; ++n)
                    if (const Json* nm = nodes.a[n].find("name"); nm && nm->type == Json::String && nm->s == name) return int(n);
                throw std::runtime_error("attach: no animated bone named '" + name + "'");
            };
            auto rotate = [](const Quat& q, const double v[3], double out[3]) {
                Quat r = qmul(q, qmul(Quat{ v[0], v[1], v[2], 0 }, qinv(q)));
                out[0] = r.x; out[1] = r.y; out[2] = r.z;
            };
            // unscaled world frame (origin + rotation) of a bone at sample k, from the keyed data
            struct Frame { double o[3]; Quat q; };
            std::vector<std::vector<Frame>> wf(boneCount, std::vector<Frame>(T));
            std::vector<std::vector<char>> done(boneCount, std::vector<char>(T, 0));
            std::function<Frame(int, size_t)> world = [&](int b, size_t k) -> Frame {
                if (done[b][k]) return wf[b][k];
                Frame f;
                int lp = info[b].lp;
                double pl[3] = { gP[b][3 * k], gP[b][3 * k + 1], gP[b][3 * k + 2] };
                if (lp >= 0 && uint32_t(lp) < boneCount) {
                    Frame pf = world(lp, k);
                    double sc[3], d[3];
                    for (int c = 0; c < 3; ++c) sc[c] = pl[c] * gameScaleAt(lp, k, c);
                    rotate(pf.q, sc, d);
                    for (int c = 0; c < 3; ++c) f.o[c] = pf.o[c] + d[c];
                    f.q = qnorm(qmul(pf.q, gQ[b][k]));
                } else {
                    for (int c = 0; c < 3; ++c) f.o[c] = pl[c];
                    f.q = gQ[b][k];
                }
                wf[b][k] = f; done[b][k] = 1;
                return f;
            };
            auto smooth = [](double x) { x = x < 0 ? 0 : x > 1 ? 1 : x; return x * x * (3 - 2 * x); };
            for (const PropAttach& at : opt.attach) {
                int prop = findNode(at.prop), tgt = findNode(at.target);
                int pp = info[prop].lp;
                double gt = at.gripTime < 0 ? at.start : at.gripTime;
                size_t gk = 0;
                for (size_t k = 0; k < T; ++k) if (std::fabs(times[k] - gt) < std::fabs(times[gk] - gt)) gk = k;
                // grip: prop frame expressed in the target's frame (offset in target's unscaled units)
                Frame pf = world(prop, gk), tf = world(tgt, gk);
                Quat gripQ = qnorm(qmul(qinv(tf.q), pf.q));
                double d[3] = { pf.o[0] - tf.o[0], pf.o[1] - tf.o[1], pf.o[2] - tf.o[2] }, gripO[3];
                rotate(qinv(tf.q), d, gripO);
                for (int c = 0; c < 3; ++c) { double s = gameScaleAt(tgt, gk, c); gripO[c] = std::fabs(s) > 1e-6 ? gripO[c] / s : gripO[c]; }
                // new local transforms (computed from the keyed frames, then applied)
                std::vector<double> newP(gP[prop]); std::vector<Quat> newQ(gQ[prop]);
                for (size_t k = 0; k < T; ++k) {
                    double t = times[k];
                    double w = t < at.start ? (at.blendIn > 0 ? smooth((t - (at.start - at.blendIn)) / at.blendIn) : 0.0)
                             : t > at.end ? (at.blendOut > 0 ? 1.0 - smooth((t - at.end) / at.blendOut) : 0.0) : 1.0;
                    if (w <= 0) continue;
                    Frame tk = world(tgt, k);
                    double so[3], od[3], o[3];
                    for (int c = 0; c < 3; ++c) so[c] = gripO[c] * gameScaleAt(tgt, k, c);
                    rotate(tk.q, so, od);
                    for (int c = 0; c < 3; ++c) o[c] = tk.o[c] + od[c];
                    Quat q = qnorm(qmul(tk.q, gripQ));
                    // into the prop's parent frame
                    double pl[3]; Quat ql;
                    if (pp >= 0 && uint32_t(pp) < boneCount) {
                        Frame par = world(pp, k);
                        double rel[3] = { o[0] - par.o[0], o[1] - par.o[1], o[2] - par.o[2] };
                        rotate(qinv(par.q), rel, pl);
                        for (int c = 0; c < 3; ++c) { double s = gameScaleAt(pp, k, c); if (std::fabs(s) > 1e-6) pl[c] /= s; }
                        ql = qnorm(qmul(qinv(par.q), q));
                    } else { for (int c = 0; c < 3; ++c) pl[c] = o[c]; ql = q; }
                    // blend with the keyed transform
                    Quat kq = gQ[prop][k];
                    if (qdot(kq, ql) < 0) ql = { -ql.x, -ql.y, -ql.z, -ql.w };
                    double dotq = std::min(1.0, qdot(kq, ql)), th = std::acos(dotq);
                    Quat bq2;
                    if (th < 1e-6) bq2 = ql;
                    else {
                        double a0 = std::sin((1 - w) * th) / std::sin(th), a1 = std::sin(w * th) / std::sin(th);
                        bq2 = qnorm({ a0 * kq.x + a1 * ql.x, a0 * kq.y + a1 * ql.y, a0 * kq.z + a1 * ql.z, a0 * kq.w + a1 * ql.w });
                    }
                    newQ[k] = bq2;
                    for (int c = 0; c < 3; ++c) newP[3 * k + c] = (1 - w) * gP[prop][3 * k + c] + w * pl[c];
                }
                gP[prop] = newP; gQ[prop] = newQ;
                for (size_t k = 0; k < T; ++k) done[prop][k] = 0;
            }
        }
 
        // ---- phase C: write channels ----
        for (uint32_t i = 0; i < boneCount; ++i) {
            reportProgress(2, i, boneCount);
            const BoneInfo& bi = info[i];
            bool restructured = bi.restructured, parentRestructured = bi.parentRestructured;
            int trNode = bi.trNode, lp = bi.lp;
            const double *bt = bi.bt, *bs = bi.bs, *fs = bi.fs;
            Quat bq = bi.bq;
 
            std::vector<float> P, Q, S, FS;
            double maxTDiff = 0, maxRDiffPos = 0, maxRDiffNeg = 0, maxSDiff = 0, maxFSDiff = 0;
            Quat prev; bool first = true;
            for (size_t k = 0; k < T; ++k) {
                const double* p = &gP[i][3 * k];
                Quat q = gQ[i][k];
                if (!first && qdot(prev, q) < 0) q = { -q.x, -q.y, -q.z, -q.w };
                prev = q; first = false;
                // scale is unaffected by the X mirror
                for (int c = 0; c < 3; ++c) {
                    double ps = gameScaleAt(lp, k, c);              // parent's own scale
                    double gp = parentRestructured ? 1.0 : ps;      // scale glTF already applies from above
                    bool div = std::fabs(gp) > 1e-6;
                    double pos = p[c] * (parentRestructured ? ps : 1.0);
                    double own = gameScaleAt(int(i), k, c);
                    double boneS = restructured ? own : (div ? own / gp : own);
                    double frameS = div ? 1.0 / gp : 1.0;
                    P.push_back(float(pos)); maxTDiff = std::max(maxTDiff, std::fabs(pos - bt[c]));
                    S.push_back(float(boneS)); maxSDiff = std::max(maxSDiff, std::fabs(boneS - bs[c]));
                    FS.push_back(float(frameS)); maxFSDiff = std::max(maxFSDiff, std::fabs(frameS - fs[c]));
                }
                double qa[4] = { q.x, q.y, q.z, q.w }, qb[4] = { bq.x, bq.y, bq.z, bq.w };
                for (int c = 0; c < 4; ++c) {
                    Q.push_back(float(qa[c]));
                    maxRDiffPos = std::max(maxRDiffPos, std::fabs(qa[c] - qb[c]));
                    maxRDiffNeg = std::max(maxRDiffNeg, std::fabs(qa[c] + qb[c]));
                }
            }
            bool animT = false, animR = false, animS = false;
            for (int c = AnimFile::CH_PX; c <= AnimFile::CH_PZ; ++c) animT |= anim.channel(i, c).count > 0;
            for (int c = AnimFile::CH_RX; c <= AnimFile::CH_RZ; ++c) animR |= anim.channel(i, c).count > 0;
            for (int c = AnimFile::CH_SX; c <= AnimFile::CH_SZ; ++c) animS |= anim.channel(i, c).count > 0;
            bool wantT = animT || maxTDiff > 1e-5;
            bool wantR = animR || std::min(maxRDiffPos, maxRDiffNeg) > 1e-5;
            bool wantS = animS || maxSDiff > 1e-5;
            bool wantFS = restructured && maxFSDiff > 1e-5;
 
            if ((wantT || wantR || wantS) && bi.hadMatrix) writeTRS(nodes.a[trNode], bt, bq, bs);  // animated nodes must use TRS
            if (wantT) addChannel(uint32_t(trNode), "translation", addAccessor(P, "VEC3", 3, false));
            if (wantR) addChannel(uint32_t(trNode), "rotation", addAccessor(Q, "VEC4", 4, false));
            if (wantFS) addChannel(uint32_t(trNode), "scale", addAccessor(FS, "VEC3", 3, false));
            if (wantS) addChannel(i, "scale", addAccessor(S, "VEC3", 3, false));
        }
        if (channels.a.empty()) throw std::runtime_error("animation produced no channels");
        animation["samplers"] = samplers;
        animation["channels"] = channels;
 
        // extras: source, duration, sound / footprint events
        Json extras = Json::makeObject(), evs = Json::makeArray();
        extras["source"] = Json::makeString(baseName(animPath));
        extras["duration"] = Json::makeNumber(anim.duration);
        for (const auto& e : anim.events()) {
            Json je = Json::makeObject(), jt = Json::makeArray();
            je["event"] = Json::makeString(e.name);
            for (float t : e.times) jt.a.push_back(Json::makeNumber(t));
            je["times"] = jt;
            if (e.bone > 0 && size_t(e.bone) < nodes.a.size())
                if (const Json* nm = nodes.a[e.bone].find("name")) je["bone"] = *nm;
            evs.a.push_back(je);
        }
        extras["events"] = evs;
        animation["extras"] = extras;
        animations.a.push_back(std::move(animation));
 
        // --- write ---
        std::vector<uint8_t> extBin;
        std::string extBinPath;
        if (appendToGlbBin) {
            bin.resize(baseOffset, 0);
            bin.insert(bin.end(), data.begin(), data.end());
            buffers.a[0]["byteLength"] = Json::makeNumber(double(bin.size()));
        } else {  // separate buffer file next to the output
            extBinPath = stripExt(outPath) + "_" + animName + ".bin";
            Json b = Json::makeObject();
            b["uri"] = Json::makeString(baseName(extBinPath));
            b["byteLength"] = Json::makeNumber(double(data.size()));
            buffers.a.push_back(b);
            extBin = data;
        }
        // drop empty top-level arrays we may have created
        for (const char* k : { "animations", "accessors", "bufferViews", "buffers" })
            if (Json* v = doc.find(k); v && v->a.empty()) doc.erase(k);
 
        std::string js;
        writeJson(doc, js);
        std::vector<uint8_t> out;
        if (isGlb) {
            while (js.size() % 4) js += ' ';
            while (bin.size() % 4) bin.push_back(0);
            bool writeBin = !bin.empty();
            uint32_t total = uint32_t(12 + 8 + js.size() + (writeBin ? 8 + bin.size() : 0));
            out.insert(out.end(), { 'g', 'l', 'T', 'F' });
            putU32(out, 2); putU32(out, total);
            putU32(out, uint32_t(js.size())); putU32(out, 0x4E4F534A);
            out.insert(out.end(), js.begin(), js.end());
            if (writeBin) { putU32(out, uint32_t(bin.size())); putU32(out, 0x004E4942); out.insert(out.end(), bin.begin(), bin.end()); }
        } else {
            out.assign(js.begin(), js.end());
        }
        writeFile(outPath, out);
        if (!extBinPath.empty()) writeFile(extBinPath, extBin);
        if (opt.progress) opt.progress->store(1.0f);
        return true;
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        else std::cerr << "error: " << e.what() << "\n";
        return false;
    }
}

// selections persist across frames
static std::set<std::string> selected_anim_files;
static std::string selected_model_file;
int fps = 60;

// state shared between the UI and the import thread
static std::jthread apply_thread;                   // jthread joins on destruction, so closing the app mid-import is safe
static std::atomic<bool> applying = false;
static std::atomic<int> files_done = 0;
static std::atomic<float> file_progress = 0.0f;     // progress inside the file currently being imported
static int files_total = 0;
static int files_failed = 0;                        // only written by the worker, read once applying is false
static std::mutex error_mutex;
static std::vector<std::string> apply_errors;

static void start_applying()
{
    // copy the selection so the worker doesn't read containers the UI could change
    std::vector<std::string> anims(selected_anim_files.begin(), selected_anim_files.end());
    std::string model = selected_model_file;
    double bake_fps = fps;

    files_total = static_cast<int>(anims.size());
    files_failed = 0;
    files_done = 0;
    file_progress = 0.0f;
    {
        std::lock_guard lock(error_mutex);
        apply_errors.clear();
    }
    applying = true;

    apply_thread = std::jthread([anims = std::move(anims), model = std::move(model), bake_fps]
    {
        // the first successful import of the batch clears the old animations, the rest add to it
        bool cleared = false;
        for (const auto& animation : anims)
        {
            ImportOptions options;
            options.fps = bake_fps;
            options.outPath = model;
            options.replaceExisting = !cleared;
            options.progress = &file_progress;

            std::string error_message;
            if (ImportAnimIntoGltf(animation, model, options, &error_message))
            {
                cleared = true;
            }
            else
            {
                std::string error = std::filesystem::path(animation).filename().string() + ": " + error_message;
                printf("failed to import animation %s\n", error.c_str());
                files_failed++;
                std::lock_guard lock(error_mutex);
                apply_errors.push_back(std::move(error));
            }
            files_done++;
        }
        applying = false;
    });
}
// Short form (kept for existing callers).
inline bool ImportAnimIntoGltf(const std::string& animPath, const std::string& gltfPath,
                               const std::string& outPath = "", double fps = 60.0, std::string* error = nullptr,
                               bool clampNegativeScale = true) {
    ImportOptions opt;
    opt.outPath = outPath; opt.fps = fps; opt.clampNegativeScale = clampNegativeScale;
    return ImportAnimIntoGltf(animPath, gltfPath, opt, error);
}

static void display_animation_files(const std::string& path)
{
    const bool busy = applying;
    // lock the selection while the worker is running
    ImGui::BeginDisabled(busy);

    // split the available width in half, minus the gap SameLine() inserts between the two
    const float half_width = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
    // negative height = fill the remaining space except this much, which keeps the button row visible
    const float list_height = -ImGui::GetFrameHeightWithSpacing();
    int animation_amount = 0;
    ImGui::BeginChild("Animation Files", ImVec2(half_width, list_height), ImGuiChildFlags_Borders);
    for (auto& entry : std::filesystem::directory_iterator(path))
    {
        if (entry.path().string().ends_with(".anim"))
        {
            std::string anim_file = entry.path().string();
            bool check = selected_anim_files.contains(anim_file);
            animation_amount++;
            if (ImGui::Checkbox(entry.path().filename().string().c_str(), &check))
            {
                if (check)
                    selected_anim_files.insert(anim_file);
                else
                    selected_anim_files.erase(anim_file);
            }
        }
    }
    if (animation_amount <= 1)
    {
        ImGui::Text("no animation data found yet!\nGo to the resource extractor\nand extract some animations");
    }
    ImGui::EndChild();
    ImGui::SameLine();
    
    ImGui::BeginChild("3D Models", ImVec2(half_width, list_height), ImGuiChildFlags_Borders);
    for (auto& entry : std::filesystem::directory_iterator(path))
    {
        if (entry.path().string().ends_with(".glb"))
        {
            std::string model_file = entry.path().string();
            if (ImGui::RadioButton(entry.path().filename().string().c_str(), selected_model_file == model_file))
            {
                selected_model_file = model_file;
            }
        }
    }
    ImGui::EndChild();
    ImGui::EndDisabled();

    ImGui::BeginChild("Apply Animation");
    // Begin/EndDisabled must be balanced inside each child window, so this is a second block
    ImGui::BeginDisabled(busy);
    // needs at least one animation and a model to apply to
    const bool can_apply = !selected_anim_files.empty() && !selected_model_file.empty();
    ImGui::BeginDisabled(!can_apply);
    if (ImGui::Button("Apply"))
    {
        start_applying();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Clear"))
    {
        selected_anim_files.clear();
        selected_model_file = "";
    }
    ImGui::SameLine();
    // wide enough for a few digits plus the +/- buttons, scales with the font
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7);
    if (ImGui::InputInt("FPS", &fps, 10))
    {
        fps = std::max(fps, 1);
    }
    ImGui::EndDisabled();

    // progress bar fills the rest of the button row, stays visible after finishing to show the result
    if (files_total > 0)
    {
        ImGui::SameLine();
        const int done = files_done;
        char overlay[64];
        float fraction;
        if (busy)
        {
            fraction = std::min((done + file_progress) / files_total, 1.0f);
            snprintf(overlay, sizeof(overlay), "%d / %d", done, files_total);
        }
        else
        {
            fraction = 1.0f;
            if (files_failed == 0)
                snprintf(overlay, sizeof(overlay), "Done (%d)", files_total);
            else
                snprintf(overlay, sizeof(overlay), "%d of %d failed", files_failed, files_total);
        }
        ImGui::ProgressBar(fraction, ImVec2(-FLT_MIN, 0), overlay);

        // hover the bar to see why imports failed
        if (!busy && files_failed > 0 && ImGui::IsItemHovered())
        {
            std::lock_guard lock(error_mutex);
            ImGui::BeginTooltip();
            for (const auto& error : apply_errors)
                ImGui::TextUnformatted(error.c_str());
            ImGui::EndTooltip();
        }
    }
    ImGui::EndChild();
}
void animation_loop()
{
    display_animation_files(combined_output_path);
}
