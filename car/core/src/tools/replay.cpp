// rumicar-replay — runs a recorded session through a driving logic and a pack,
// exactly as the car would (same .so, same pack, same frame order and timing),
// and prints one JSON line per frame with the logic's decisions.  Used to
// compare pack versions before sending one to the car (docs 4.5).
//
//   rumicar-replay <logic dir> <pack dir> <session dir> [--out replay.jsonl] [--no-image]
//
// <logic dir> and <pack dir> are directories with manifest.json (as installed
// under /var/lib/rumicar or as staged by the build).  The session is a folder
// written by the drive manager (meta.jsonl + frames/).  Each line:
//   {"index":1,"seq":10,"t_us":...,"tof_mm":[..],"image":true,"state":17,"state_name":"SIGNAL_STOP",
//    "steer":0,"drive_permille":0,"flags":1,"aux_format":1,"aux":"<base64>","rc":0,
//    "recorded":{"state":..,"steer":..,"drive_permille":..}}
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "camera/jpeg.hpp"
#include "logic_loader.hpp"
#include "rumicar/logic_api.h"
#include "upload.hpp"

using json = nlohmann::json;

namespace {

std::vector<uint8_t> read_bytes(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open " + path);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

int usage() {
    std::fprintf(stderr, "usage: rumicar-replay <logic dir> <pack dir> <session dir> [--out replay.jsonl] [--no-image]\n");
    return 2;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 4) return usage();
    const std::string logic_dir = argv[1], pack_dir = argv[2], session = argv[3];
    std::string out_path;
    bool use_image = true;
    for (int i = 4; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--out" && i + 1 < argc) out_path = argv[++i];
        else if (a == "--no-image") use_image = false;
        else return usage();
    }
    try {
        auto lib = std::make_shared<rumicar::LogicLibrary>(logic_dir);
        rumicar::LogicInstance inst(lib, pack_dir);
        std::fprintf(stderr, "logic %s %s, pack %s %s", lib->manifest().id.c_str(), lib->manifest().version.c_str(),
                     inst.pack_manifest().id.c_str(), inst.pack_manifest().version.c_str());
        const auto names = inst.aux_names();
        if (!names.empty()) {
            std::fprintf(stderr, ", aux:");
            for (const auto& n : names) std::fprintf(stderr, " %s", n.c_str());
        }
        std::fprintf(stderr, "\n");

        std::ifstream meta(session + "/meta.jsonl");
        if (!meta) throw std::runtime_error("not a session folder (meta.jsonl is missing): " + session);
        std::ofstream out_file;
        if (!out_path.empty()) {
            out_file.open(out_path);
            if (!out_file) throw std::runtime_error("cannot write " + out_path);
        }
        std::ostream& out = out_path.empty() ? std::cout : out_file;

        std::string line;
        unsigned frames = 0, images = 0, errors = 0, missing = 0;
        std::vector<unsigned> state_frames(256, 0);
        rumicar::cam::DecodedImage img;
        while (std::getline(meta, line)) {
            if (line.empty()) continue;
            json rec;
            try { rec = json::parse(line); } catch (const json::exception&) { continue; }
            if (!rec.is_object() || !rec.contains("header") || !rec["header"].is_object()) continue;
            const json& h = rec["header"];
            rumi_sensor_frame f{};
            f.size = sizeof(f);
            f.seq = h.value("seq", 0u);
            f.t_us = h.value("t_us", 0ull);
            if (h.contains("tof_mm") && h["tof_mm"].is_array() && h["tof_mm"].size() == 3) {
                for (size_t i = 0; i < 3; ++i) f.tof_mm[i] = h["tof_mm"][i].get<int32_t>();
            }
            bool has_image = false;
            if (use_image && rec.contains("image") && rec["image"].is_string()) {
                try {
                    img = rumicar::cam::jpeg_decode(read_bytes(session + "/frames/" + rec["image"].get<std::string>()));
                    f.image = img.rgb.data();
                    f.image_width = img.width;
                    f.image_height = img.height;
                    f.image_stride = img.width * 3;
                    has_image = true;
                    ++images;
                } catch (const std::exception& e) {
                    ++missing;
                    if (missing <= 3) std::fprintf(stderr, "frame %s: %s\n", rec["image"].get<std::string>().c_str(), e.what());
                }
            }
            rumi_command c{};
            const int rc = inst.step(f, c);
            ++frames;
            if (rc != 0) ++errors;
            else ++state_frames[c.state];
            json o = {
                {"index", rec.value("index", 0)},
                {"seq", f.seq},
                {"t_us", f.t_us},
                {"tof_mm", {f.tof_mm[0], f.tof_mm[1], f.tof_mm[2]}},
                {"image", has_image},
                {"rc", rc},
                {"state", rc == 0 ? c.state : 0},
                {"state_name", rc == 0 ? inst.state_name(c.state) : "ERROR"},
                {"steer", rc == 0 ? c.steer : 0},
                {"drive_permille", rc == 0 ? c.drive_permille : 0},
                {"flags", rc == 0 ? c.flags : 0u},
                {"recorded", {{"state", h.value("state", 0)}, {"state_name", h.value("state_name", "")},
                              {"steer", h.value("logic_steer", 0)}, {"drive_permille", h.value("logic_drive_permille", 0)}}},
            };
            if (rc == 0 && c.aux_len > 0) {
                o["aux_format"] = c.aux_format;
                o["aux"] = rumicar::base64_encode(c.aux, std::min<size_t>(c.aux_len, RUMI_AUX_BYTES));
            }
            out << o.dump() << '\n';
        }
        std::fprintf(stderr, "%u frames (%u with image, %u image files unreadable), %u errors\n", frames, images, missing, errors);
        for (unsigned s = 0; s < 256; ++s) {
            if (state_frames[s]) std::fprintf(stderr, "  %-18s %u frames\n", inst.state_name(static_cast<uint8_t>(s)), state_frames[s]);
        }
        return errors == 0 ? 0 : 1;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
