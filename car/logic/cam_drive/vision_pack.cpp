#include "vision_pack.hpp"

#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>

namespace rumicar::vision {

namespace {

struct Reader {
    const std::vector<uint8_t>& b;
    size_t pos = 0;

    void need(size_t n, const char* what) {
        if (pos + n > b.size()) throw std::runtime_error(std::string("vision.bin: truncated at ") + what);
    }
    uint8_t u8(const char* what) { need(1, what); return b[pos++]; }
    uint16_t u16(const char* what) {
        need(2, what);
        const uint16_t v = static_cast<uint16_t>(b[pos] | (b[pos + 1] << 8));
        pos += 2;
        return v;
    }
    uint32_t u32(const char* what) {
        need(4, what);
        const uint32_t v = static_cast<uint32_t>(b[pos]) | (static_cast<uint32_t>(b[pos + 1]) << 8) |
                           (static_cast<uint32_t>(b[pos + 2]) << 16) | (static_cast<uint32_t>(b[pos + 3]) << 24);
        pos += 4;
        return v;
    }
    int32_t i32(const char* what) { return static_cast<int32_t>(u32(what)); }
    float f32(const char* what) {
        const uint32_t u = u32(what);
        float f;
        std::memcpy(&f, &u, 4);
        if (!std::isfinite(f)) throw std::runtime_error(std::string("vision.bin: non-finite value in ") + what);
        return f;
    }
    void bytes(uint8_t* dst, size_t n, const char* what) {
        need(n, what);
        std::memcpy(dst, b.data() + pos, n);
        pos += n;
    }
};

struct Writer {
    std::vector<uint8_t> b;
    void u8(uint8_t v) { b.push_back(v); }
    void u16(uint16_t v) { b.push_back(static_cast<uint8_t>(v & 0xFF)); b.push_back(static_cast<uint8_t>(v >> 8)); }
    void u32(uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF)); }
    void i32(int32_t v) { u32(static_cast<uint32_t>(v)); }
    void f32(float f) { uint32_t u; std::memcpy(&u, &f, 4); u32(u); }
    void bytes(const uint8_t* src, size_t n) { b.insert(b.end(), src, src + n); }
};

const uint32_t* crc_table() {
    static uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        init = true;
    }
    return table;
}

} // namespace

uint32_t crc32(const uint8_t* data, size_t len, uint32_t seed) {
    const uint32_t* t = crc_table();
    uint32_t c = seed ^ 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) c = t[(c ^ data[i]) & 0xFFu] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

bool valid_detector_name(const std::string& name) {
    if (name.empty() || name.size() > kMaxNameLength) return false;
    for (const char ch : name) {
        const bool ok = (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_';
        if (!ok) return false;
    }
    return true;
}

VisionPack parse_vision_pack(const std::vector<uint8_t>& bytes) {
    if (bytes.size() < 124) throw std::runtime_error("vision.bin: too short");
    if (bytes.size() > kPackMaxBytes) throw std::runtime_error("vision.bin: larger than 16 MiB");
    if (std::memcmp(bytes.data(), "RCV1", 4) != 0) throw std::runtime_error("vision.bin: bad magic");
    const uint32_t stored = static_cast<uint32_t>(bytes[bytes.size() - 4]) | (static_cast<uint32_t>(bytes[bytes.size() - 3]) << 8) |
                            (static_cast<uint32_t>(bytes[bytes.size() - 2]) << 16) | (static_cast<uint32_t>(bytes[bytes.size() - 1]) << 24);
    if (crc32(bytes.data(), bytes.size() - 4) != stored) throw std::runtime_error("vision.bin: CRC mismatch (file damaged)");

    Reader r{bytes, 4};
    VisionPack p;
    const uint16_t version = r.u16("version");
    if (version != kPackVersion) throw std::runtime_error("vision.bin: unsupported version " + std::to_string(version));
    const uint16_t feature_set = r.u16("feature_set");
    if (feature_set != kFeatureSet) throw std::runtime_error("vision.bin: unsupported feature set " + std::to_string(feature_set));
    p.config.downscale = r.u16("downscale");
    p.config.grid_x = r.u16("grid_x");
    p.config.grid_y = r.u16("grid_y");
    p.config.s_min = r.u8("s_min");
    p.config.v_min = r.u8("v_min");
    p.config.v_white = r.u8("v_white");
    const uint8_t flags = r.u8("flags");
    if (flags & ~0x1u) throw std::runtime_error("vision.bin: unknown flags");
    const uint16_t n_det = r.u16("n_detectors");
    if (n_det > kMaxDetectors) throw std::runtime_error("vision.bin: too many detectors");
    p.image_w = r.u16("image_w");
    p.image_h = r.u16("image_h");
    const std::string cfg_err = validate_config(p.config, p.image_w, p.image_h);
    if (!cfg_err.empty()) throw std::runtime_error("vision.bin: " + cfg_err);
    for (unsigned i = 0; i < kFeaturesPerCell; ++i) p.mean[i] = r.f32("mean");
    for (unsigned i = 0; i < kFeaturesPerCell; ++i) {
        p.std[i] = r.f32("std");
        if (!(p.std[i] > 0.0f)) throw std::runtime_error("vision.bin: std must be positive");
    }
    const unsigned dims = p.config.dims();
    for (unsigned d = 0; d < n_det; ++d) {
        Detector det;
        const uint8_t kind = r.u8("detector kind");
        if (kind > 1) throw std::runtime_error("vision.bin: unknown detector kind");
        det.kind = static_cast<DetectorKind>(kind);
        const uint8_t name_len = r.u8("detector name length");
        det.name.resize(name_len);
        r.bytes(reinterpret_cast<uint8_t*>(det.name.data()), name_len, "detector name");
        if (!valid_detector_name(det.name)) throw std::runtime_error("vision.bin: invalid detector name");
        for (const auto& prev : p.detectors) {
            if (prev.name == det.name) throw std::runtime_error("vision.bin: duplicate detector name " + det.name);
        }
        const uint16_t wdims = r.u16("detector dims");
        const unsigned expected = det.kind == DetectorKind::Global ? dims : kLocalDims;
        if (wdims != expected)
            throw std::runtime_error("vision.bin: detector " + det.name + " has " + std::to_string(wdims) + " weights, expected " +
                                     std::to_string(expected));
        det.weights.resize(wdims);
        for (unsigned k = 0; k < wdims; ++k) det.weights[k] = r.f32("detector weights");
        det.bias = r.f32("bias");
        det.thr_on = r.f32("thr_on");
        det.thr_off = r.f32("thr_off");
        if (!(det.thr_on > 0.0f && det.thr_on < 1.0f && det.thr_off >= 0.0f && det.thr_off <= det.thr_on))
            throw std::runtime_error("vision.bin: thresholds of " + det.name + " must satisfy 0 <= thr_off <= thr_on < 1");
        det.confirm_frames = r.u16("confirm_frames");
        det.release_frames = r.u16("release_frames");
        if (det.confirm_frames < 1 || det.confirm_frames > 1000 || det.release_frames < 1 || det.release_frames > 1000)
            throw std::runtime_error("vision.bin: confirm/release frames of " + det.name + " must be 1..1000");
        det.min_cells = r.u16("min_cells");
        det.row_min = r.u8("row_min");
        det.row_max = r.u8("row_max");
        if (det.kind == DetectorKind::Local) {
            if (det.min_cells < 1 || det.min_cells > p.config.grid_x * p.config.grid_y)
                throw std::runtime_error("vision.bin: min_cells of " + det.name + " out of range");
            if (det.row_min > det.row_max || det.row_min >= p.config.grid_y)
                throw std::runtime_error("vision.bin: row range of " + det.name + " is invalid");
        }
        p.detectors.push_back(std::move(det));
    }
    if (flags & 0x1u) {
        p.selftest.present = true;
        p.selftest.image.width = r.u16("self-test width");
        p.selftest.image.height = r.u16("self-test height");
        const unsigned sw = p.image_w / p.config.downscale, sh = p.image_h / p.config.downscale;
        if (p.selftest.image.width != sw || p.selftest.image.height != sh)
            throw std::runtime_error("vision.bin: self-test image size does not match the configuration");
        p.selftest.image.rgb.resize(static_cast<size_t>(sw) * sh * 3);
        r.bytes(p.selftest.image.rgb.data(), p.selftest.image.rgb.size(), "self-test image");
        p.selftest.features.resize(dims);
        for (unsigned k = 0; k < dims; ++k) p.selftest.features[k] = r.i32("self-test features");
        p.selftest.scores.resize(n_det);
        for (unsigned d = 0; d < n_det; ++d) p.selftest.scores[d] = r.f32("self-test scores");
    }
    if (r.pos != bytes.size() - 4) throw std::runtime_error("vision.bin: trailing data");
    return p;
}

VisionPack load_vision_pack(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open " + path);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return parse_vision_pack(bytes);
}

std::vector<uint8_t> encode_vision_pack(const VisionPack& p) {
    Writer w;
    w.bytes(reinterpret_cast<const uint8_t*>("RCV1"), 4);
    w.u16(kPackVersion);
    w.u16(static_cast<uint16_t>(kFeatureSet));
    w.u16(static_cast<uint16_t>(p.config.downscale));
    w.u16(static_cast<uint16_t>(p.config.grid_x));
    w.u16(static_cast<uint16_t>(p.config.grid_y));
    w.u8(p.config.s_min);
    w.u8(p.config.v_min);
    w.u8(p.config.v_white);
    w.u8(p.selftest.present ? 1 : 0);
    w.u16(static_cast<uint16_t>(p.detectors.size()));
    w.u16(static_cast<uint16_t>(p.image_w));
    w.u16(static_cast<uint16_t>(p.image_h));
    for (unsigned i = 0; i < kFeaturesPerCell; ++i) w.f32(p.mean[i]);
    for (unsigned i = 0; i < kFeaturesPerCell; ++i) w.f32(p.std[i]);
    for (const auto& d : p.detectors) {
        w.u8(static_cast<uint8_t>(d.kind));
        w.u8(static_cast<uint8_t>(d.name.size()));
        w.bytes(reinterpret_cast<const uint8_t*>(d.name.data()), d.name.size());
        w.u16(static_cast<uint16_t>(d.weights.size()));
        for (const float f : d.weights) w.f32(f);
        w.f32(d.bias);
        w.f32(d.thr_on);
        w.f32(d.thr_off);
        w.u16(d.confirm_frames);
        w.u16(d.release_frames);
        w.u16(d.min_cells);
        w.u8(d.row_min);
        w.u8(d.row_max);
    }
    if (p.selftest.present) {
        w.u16(static_cast<uint16_t>(p.selftest.image.width));
        w.u16(static_cast<uint16_t>(p.selftest.image.height));
        w.bytes(p.selftest.image.rgb.data(), p.selftest.image.rgb.size());
        for (const int32_t f : p.selftest.features) w.i32(f);
        for (const float s : p.selftest.scores) w.f32(s);
    }
    w.u32(crc32(w.b.data(), w.b.size()));
    return std::move(w.b);
}

} // namespace rumicar::vision
