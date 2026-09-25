// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// tools/fidelity/main.cpp
//
// The fidelity runner's host tool. Three subcommands, each a separate surface
// so the heavy lifting can be dropped into a make/Ninja graph instead of a
// long-lived process:
//
//   fidelity list     --config <json> [--samples <dir>] [--fidelity <dir>]
//                           [--out-dir <dir>]
//                     merges every scenario over the Khronos defaults (the
//                     generator's Object.assign + deep dimensions/target/orbit
//                     merge), resolves each model/lighting path, and writes
//                       <out-dir>/scenarios.tsv        (TSV, one row each)
//                       <out-dir>/<name>.json          (harness handoff)
//
//   fidelity compare  --candidate <ppm> --goldens-dir <dir> --name <name>
//                           --out-dir <dir>
//                     computes the pixelmatch YIQ rmsDistanceRatio (dB) against
//                     every <goldens-dir>/<name>/<renderer>-golden.png that
//                     exists, writes <out-dir>/<name>.db (TSV: renderer<TAB>dB),
//                     and diff PNGs for the candidate (which is also converted
//                     to <name>_zahlen.png). The closest renderer's golden is
//                     copied in and its diff image is the one written.
//
//   fidelity report   --out-dir <dir>
//                     folds every <name>.db into report.md / report.html.
//
// Everything is stdout/quiet file I/O; the Bash driver scripts/build.ninja the
// graph and Ninja parallelises + caches it (mtime decides whether a scenario
// re-renders).

#include "FidelityCore.hpp"
#include "ImageIO.hpp"

#include <json/JSONSchema.hpp>

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <optional>
#include <print>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

namespace {

// ---------------------------------------------------------------------------
// Scenario model and the Khronos default merge.
// ---------------------------------------------------------------------------

struct Extent2 {
    uint32_t width  = 768;
    uint32_t height = 768;
};

struct Orbit3 {
    float theta  = 0.0f;   // azimuth / yaw, degrees around +Y
    float phi    = 90.0f;  // polar / inclination from +Y, degrees
    float radius = 1.0f;   // camera distance, metres
};

struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct Scenario {
    std::string name;
    std::string model;      // resolved absolute path
    std::string lighting;   // resolved absolute path (forward-compat with HDR)
    Extent2     dimensions;
    Vec3        target;
    Orbit3      orbit;
    float       verticalFov = 45.0f;
};

const char* kGoldenRenderers[] = {"filament", "blender-cycles", "gltf-sample-viewer", "model-viewer", "babylon"};

// The generator's src/config-reader.ts defaults (deep merge for the three
// nested objects, shallow Object.assign for the scalars).
float GetFloat(const ZHLN::ReflectJSON::ValueReader& reader, std::string_view key, float def) {
    const auto f = reader.GetKey(key);
    if (!f || f->IsNull()) {
        return def;
    }
    return static_cast<float>(f->GetDouble().value_or(static_cast<double>(def)));
}

uint32_t GetUInt(const ZHLN::ReflectJSON::ValueReader& reader, std::string_view key, uint32_t def) {
    const auto f = reader.GetKey(key);
    if (!f) {
        return def;
    }
    return static_cast<uint32_t>(f->GetUInt().value_or(static_cast<uint64_t>(def)));
}

std::string GetString(const ZHLN::ReflectJSON::ValueReader& reader, std::string_view key) {
    const auto f = reader.GetKey(key);
    if (!f) {
        return {};
    }
    return std::string(f->GetString().value_or(""));
}

std::vector<Scenario> ReadScenarios(std::string_view configPath) {
    std::ifstream in {std::string(configPath), std::ios::binary};
    if (!in) {
        std::println(stderr, "[fidelity] cannot open config '{}'", configPath);
        std::exit(1);
    }
    const std::string json {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};

    auto doc = ZHLN::ReflectJSON::Document::Parse(json);
    if (!doc) {
        std::println(stderr, "[fidelity] config '{}' is not valid JSON", configPath);
        std::exit(1);
    }
    const ZHLN::ReflectJSON::ValueReader root = doc->GetRoot();

    std::vector<Scenario> out;
    const auto scenarios = root.GetKey("scenarios");
    if (!scenarios) {
        std::println(stderr, "[fidelity] config has no 'scenarios' array");
        std::exit(1);
    }
    const size_t n = scenarios->GetArraySize();
    out.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        const auto raw = scenarios->GetArrayElement(i);
        if (!raw) {
            continue;
        }
        Scenario s;
        s.name     = GetString(*raw, "name");
        s.model    = GetString(*raw, "model");
        s.lighting = GetString(*raw, "lighting");
        if (s.lighting.empty()) {
            s.lighting = "../../../environments/lightroom_14b.hdr";
        }

        s.verticalFov = GetFloat(*raw, "verticalFov", 45.0f);

        if (const auto dims = raw->GetKey("dimensions"); dims) {
            s.dimensions.width  = GetUInt(*dims, "width", 768);
            s.dimensions.height = GetUInt(*dims, "height", 768);
        }
        if (const auto target = raw->GetKey("target"); target) {
            s.target.x = GetFloat(*target, "x", 0.0f);
            s.target.y = GetFloat(*target, "y", 0.0f);
            s.target.z = GetFloat(*target, "z", 0.0f);
        }
        if (const auto orbit = raw->GetKey("orbit"); orbit) {
            s.orbit.theta  = GetFloat(*orbit, "theta", 0.0f);
            s.orbit.phi    = GetFloat(*orbit, "phi", 90.0f);
            s.orbit.radius = GetFloat(*orbit, "radius", 1.0f);
        }
        out.push_back(std::move(s));
    }
    return out;
}

// Resolve a Khronos-relative path (../../../glTF-Sample-Assets/... or
// ../../../environments/...) against the fidelity repo's test/ directory, then
// the stand-alone samples clone. Error-code overloads only: this TU compiles
// under the engine's PUBLIC -fno-exceptions, so no throwing forms may appear.
std::string ResolveAsset(std::string_view rel, const fs::path& fidelityRepo, const fs::path& samplesDir) {
    std::error_code ec;
    const auto      direct = (fidelityRepo / "test" / rel).lexically_normal();
    if (fs::exists(direct, ec)) {
        return direct.string();
    }
    const std::string marker = "glTF-Sample-Assets/";
    if (rel.find(marker) != std::string_view::npos) {
        const std::string_view tail = rel.substr(rel.find(marker) + marker.size());
        const auto            alt   = (samplesDir / tail).lexically_normal();
        if (fs::exists(alt, ec)) {
            return alt.string();
        }
    }
    return direct.string();  // canonical even if absent; the harness reports
}

// Write `path` with `content`, but skip the write (preserving mtime) when the
// file already holds byte-identical content. This is what makes the Ninja
// mtime cache work: `list` runs every time, but a scenario's .json only
// changes when the config/model actually did, so an unchanged render stays
// skipped.
void WriteIfChanged(const fs::path& path, std::string_view content) {
    std::ifstream in {path, std::ios::binary};
    if (in) {
        const std::string existing {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
        if (existing == content) {
            return;
        }
    }
    std::ofstream out {path, std::ios::binary | std::ios::trunc};
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
}

// ---------------------------------------------------------------------------
// list
// ---------------------------------------------------------------------------

void CmdList(
    std::string_view config, std::string_view fidelityRepo, const std::optional<std::string>& samplesArg, std::string_view outDir
) {
    const fs::path out {std::string(outDir)};
    std::error_code ec;
    fs::create_directories(out, ec);

    const fs::path samples = samplesArg ? fs::path(*samplesArg) : (fs::path(fidelityRepo) / "glTF-Sample-Assets");

    std::vector<Scenario> scenarios = ReadScenarios(config);

    std::string tsv;
    for (auto& s: scenarios) {
        s.model    = ResolveAsset(s.model, fidelityRepo, samples);
        s.lighting = ResolveAsset(s.lighting, fidelityRepo, samples);

        tsv += std::format(
            "{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\n",
            s.name, s.model, s.dimensions.width, s.dimensions.height, s.orbit.theta, s.orbit.phi, s.orbit.radius, s.verticalFov,
            s.lighting
        );

        // Harness handoff: the fields FidelityHarness reads verbatim. Written
        // only when changed so its mtime stays a stable cache key.
        const std::string js = std::format(
            R"({{"name": "{}", "model": "{}", "lighting": "{}", "dimensions": {{"width": {}, "height": {}}}, "target": {{"x": {}, "y": {}, "z": {}}}, "orbit": {{"theta": {}, "phi": {}, "radius": {}}}, "verticalFov": {}}})",
            s.name, s.model, s.lighting, s.dimensions.width, s.dimensions.height, s.target.x, s.target.y, s.target.z, s.orbit.theta,
            s.orbit.phi, s.orbit.radius, s.verticalFov
        );
        WriteIfChanged(out / (s.name + ".json"), js);
    }
    WriteIfChanged(out / "scenarios.tsv", tsv);

    std::println("[fidelity] listed {} scenarios -> {}", scenarios.size(), (out / "scenarios.tsv").string());
}

// ---------------------------------------------------------------------------
// compare
// ---------------------------------------------------------------------------

struct GoldensArg {
    const std::string& name;
    const fs::path&    goldensDir;
};

void CmdCompare(std::string_view candidatePpm, std::string_view goldensDir, std::string_view name, std::string_view outDir) {
    auto cand = ZHLN::Fidelity::ReadPPM(candidatePpm);
    if (!cand) {
        std::println(stderr, "[fidelity] cannot read candidate '{}'", candidatePpm);
        std::exit(1);
    }
    const fs::path out {std::string(outDir)};
    const fs::path goldenDir = fs::path(goldensDir) / name;

    // Write the candidate as a PNG for the report.
    if (!ZHLN::Fidelity::WritePng((out / (std::string(name) + "_zahlen.png")).string(), cand->width, cand->height, cand->rgba.data())) {
        std::println(stderr, "[fidelity] failed to write {}_zahlen.png", name);
        std::exit(1);
    }

    struct Result {
        std::string          renderer;
        double               db;
        uint32_t             width;
        uint32_t             height;
        std::vector<uint8_t> candRGBA;  // reconciled (downscaled) candidate view
        std::vector<uint8_t> goldRGBA;  // reconciled golden view
    };

    std::vector<Result> results;
    std::ofstream       dbFile {out / (std::string(name) + ".db")};

    for (const char* renderer: kGoldenRenderers) {
        std::error_code   ec;
        const fs::path goldenPath = goldenDir / (std::string(renderer) + "-golden.png");
        if (!fs::exists(goldenPath, ec)) {
            continue;
        }
        int                  gw = 0, gh = 0;
        std::vector<uint8_t> goldRGBA = ZHLN::Fidelity::ReadPngRGBA(goldenPath.string(), &gw, &gh);
        if (goldRGBA.empty()) {
            continue;
        }

        // Reconcile sizes: downscale whichever is larger to the smaller (the
        // harness renders at 2x, so this is normally a size-identical no-op).
        const uint32_t tw = std::min(cand->width, static_cast<uint32_t>(gw));
        const uint32_t th = std::min(cand->height, static_cast<uint32_t>(gh));
        std::vector<uint8_t> cView = cand->rgba;
        std::vector<uint8_t> gView = goldRGBA;
        if (cand->width != tw || cand->height != th) {
            cView = ZHLN::Fidelity::AreaDownscale(cand->rgba, cand->width, cand->height, tw, th);
        }
        if (static_cast<uint32_t>(gw) != tw || static_cast<uint32_t>(gh) != th) {
            gView = ZHLN::Fidelity::AreaDownscale(goldRGBA, gw, gh, tw, th);
        }
        if (cView.empty() || gView.empty()) {
            continue;
        }

        const double rms = ZHLN::Fidelity::RmsDistanceRatio(cView, gView);
        const double db  = ZHLN::Fidelity::ToDecibel(rms);
        results.push_back({renderer, db, tw, th, std::move(cView), std::move(gView)});
        dbFile << renderer << '\t' << std::format("{:.2f}", db) << '\n';
    }

    if (results.empty()) {
        std::println(stderr, "[fidelity] no golden found for '{}' under '{}'", name, goldenDir.string());
        std::exit(1);
    }

    const Result& closest = *std::min_element(results.begin(), results.end(), [](const Result& a, const Result& b) { return a.db < b.db; });

    // Copy the closest golden in for the report.
    {
        std::error_code ec;
        const fs::path  src = goldenDir / (closest.renderer + "-golden.png");
        fs::copy_file(src, out / (std::string(name) + "-golden-" + closest.renderer + ".png"), fs::copy_options::overwrite_existing, ec);
    }

    // Diff image against the closest golden (reconciled views, so sizes match).
    {
        double              rms2;
        std::vector<double> deltas;
        ZHLN::Fidelity::RmsAndDeltas(closest.candRGBA, closest.goldRGBA, rms2, deltas);
        const uint32_t        dw = closest.width, dh = closest.height;
        std::vector<uint8_t>  rgba(static_cast<size_t>(dw) * dh * 4);
        for (size_t i = 0; i < deltas.size(); ++i) {
            const uint8_t mag  = static_cast<uint8_t>(std::clamp(
                255.0 * std::min(deltas[i] / ZHLN::Fidelity::kMaxColorDistance, 1.0) + 0.5, 0.0, 255.0
            ));
            const size_t  c    = i * 4;
            const bool    red  = (closest.candRGBA[c] + closest.candRGBA[c + 1] + closest.candRGBA[c + 2]) >=
                                (closest.goldRGBA[c] + closest.goldRGBA[c + 1] + closest.goldRGBA[c + 2]);
            rgba[i * 4 + 0] = red ? 255 : mag;
            rgba[i * 4 + 1] = mag;
            rgba[i * 4 + 2] = red ? mag : 255;
            rgba[i * 4 + 3] = 255;
        }
        ZHLN::Fidelity::WritePng((out / (std::string(name) + "_diff.png")).string(), dw, dh, rgba.data());
    }

    std::println("[fidelity] {} -> closest {} ({:.2f} dB)", name, closest.renderer, closest.db);
}

// ---------------------------------------------------------------------------
// report
// ---------------------------------------------------------------------------

std::string HtmlEscape(std::string_view s) {
    std::string out;
    for (char c: s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            default: out += c;
        }
    }
    return out;
}

void CmdReport(std::string_view outDir) {
    const fs::path out {std::string(outDir)};

    struct Row {
        std::string                        name;
        std::vector<std::pair<std::string, double>> goldens;  // renderer, dB (sorted by list order)
        std::string                        closest;
        double                             closestDb = 0.0;
    };

    std::vector<Row> rows;
    std::error_code  ec;
    fs::directory_iterator it {out, ec};
    const fs::directory_iterator end {};
    for (; !ec && it != end; it.increment(ec)) {
        const auto& entry = *it;
        if (entry.path().extension() != ".db") {
            continue;
        }
        Row row;
        row.name = entry.path().stem().string();
        std::ifstream f {entry.path()};
        std::string line;
        double       best = 1e300;
        while (std::getline(f, line)) {
            const size_t tab = line.find('\t');
            if (tab == std::string::npos) {
                continue;
            }
            const std::string renderer = line.substr(0, tab);
            double            db;
            const auto [ptr, ec] = std::from_chars(line.data() + tab + 1, line.data() + line.size(), db);
            if (ec != std::errc {}) {
                continue;
            }
            row.goldens.emplace_back(renderer, db);
            if (db < best) {
                best         = db;
                row.closest   = renderer;
                row.closestDb = db;
            }
        }
        if (!row.goldens.empty()) {
            rows.push_back(std::move(row));
        }
    }
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.name < b.name; });

    // Unify the column order across rows (preference order, then alphabetical).
    std::vector<std::string> columns;
    for (const auto& row: rows) {
        for (const auto& [renderer, _]: row.goldens) {
            if (std::find(columns.begin(), columns.end(), renderer) == columns.end()) {
                columns.push_back(renderer);
            }
        }
    }

    std::string md = "# Zahlen glTF Render-Fidelity Results\n\n";
    md += "Threshold: -22.0 dB (generator convention); lower = closer to the golden. Each column is the pixelmatch YIQ RMS distance ratio in dB against that renderer's golden; the closest renderer for a scenario is bolded.\n\n";
    md += "| Scenario |";
    for (const auto& c: columns) {
        md += " " + c + " |";
    }
    md += " Closest |\n| --- |";
    for (size_t i = 0; i < columns.size(); ++i) {
        md += " --- |";
    }
    md += " --- |\n";

    std::string html =
        "<html><head><meta charset='utf-8'><title>Zahlen glTF Fidelity Results</title></head>"
        "<body style='background:#111;color:#eee;font-family:sans-serif;margin:24px'>"
        "<h2>Zahlen glTF Render-Fidelity Results</h2>"
        "<p>Threshold: <b>-22.0 dB</b> (generator convention); lower = closer to the golden.</p>"
        "<table border='1' cellpadding='8' style='border-collapse:collapse'>"
        "<tr><th>Scenario</th>";
    for (const auto& c: columns) {
        html += "<th>" + HtmlEscape(c) + "</th>";
    }
    html += "<th>Zahlen</th><th>Golden</th><th>Diff</th><th>Closest</th></tr>";

    for (const auto& row: rows) {
        md += "| " + row.name + " |";
        for (const auto& c: columns) {
            auto it = std::find_if(row.goldens.begin(), row.goldens.end(), [&](const auto& p) { return p.first == c; });
            if (it == row.goldens.end()) {
                md += " — |";
            } else if (c == row.closest) {
                md += std::format(" **{:.2f}** |", it->second);
            } else {
                md += std::format(" {:.2f} |", it->second);
            }
        }
        md += " " + row.closest + " |\n";

        html += "<tr><td>" + HtmlEscape(row.name) + "</td>";
        for (const auto& c: columns) {
            auto it = std::find_if(row.goldens.begin(), row.goldens.end(), [&](const auto& p) { return p.first == c; });
            if (it == row.goldens.end()) {
                html += "<td>—</td>";
            } else if (c == row.closest) {
                html += std::format("<td><b>{:.2f}</b></td>", it->second);
            } else {
                html += std::format("<td>{:.2f}</td>", it->second);
            }
        }
        html += std::format(
            "<td><img src='{}_zahlen.png' width='280'/></td><td><img src='{}-golden-{}.png' width='280'/></td>"
            "<td><img src='{}_diff.png' width='280'/></td><td>{}</td></tr>",
            HtmlEscape(row.name), HtmlEscape(row.name), HtmlEscape(row.closest), HtmlEscape(row.name), HtmlEscape(row.closest)
        );
    }

    html += "</table></body></html>";

    {
        std::ofstream f {out / "report.md"};
        f << md;
    }
    {
        std::ofstream f {out / "report.html"};
        f << html;
    }
    std::println("[fidelity] report -> {} / {}", (out / "report.md").string(), (out / "report.html").string());
}

// ---------------------------------------------------------------------------
// tiny arg parser
// ---------------------------------------------------------------------------

struct Args {
    std::optional<std::string> command;
    std::string                config;
    std::string                fidelityRepo;
    std::optional<std::string> samples;
    std::string                candidate;
    std::string                goldensDir;
    std::string                name;
    std::string                outDir {"build/fidelity_output"};
};

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::println(stderr, "usage: fidelity <list|compare|report> [args]");
        return 1;
    }

    Args args;
    std::string command {argv[1]};

    // Keep positionals-in-first-arg or explicit --command.
    for (int i = 2; i < argc; ++i) {
        const std::string_view a = argv[i];
        if (a == "--config") { if (i + 1 < argc) args.config = argv[++i]; }
        else if (a == "--fidelity") { if (i + 1 < argc) args.fidelityRepo = argv[++i]; }
        else if (a == "--samples") { if (i + 1 < argc) args.samples = argv[++i]; }
        else if (a == "--candidate") { if (i + 1 < argc) args.candidate = argv[++i]; }
        else if (a == "--goldens-dir") { if (i + 1 < argc) args.goldensDir = argv[++i]; }
        else if (a == "--name") { if (i + 1 < argc) args.name = argv[++i]; }
        else if (a == "--out-dir") { if (i + 1 < argc) args.outDir = argv[++i]; }
    }

    if (command == "list") {
        CmdList(args.config, args.fidelityRepo, args.samples, args.outDir);
    } else if (command == "compare") {
        CmdCompare(args.candidate, args.goldensDir, args.name, args.outDir);
    } else if (command == "report") {
        CmdReport(args.outDir);
    } else {
        std::println(stderr, "[fidelity] unknown command '{}'", command);
        return 1;
    }
    return 0;
}
