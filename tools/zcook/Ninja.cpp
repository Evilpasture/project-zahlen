// tools/zcook/Ninja.cpp
// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Ninja.hpp"
#include "BinaryReader.hpp"
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace ZHLN {
namespace {

// --- The tree's vocabulary -------------------------------------------------------------

// Directories a .blend scan does not descend into, by lowercased name. Sources
// only live under the asset root; a build tree, a vendored submodule and a
// checkout's metadata directories are noise that would otherwise be walked on
// every configure.
constexpr std::array<std::string_view, 8> kPrunedByBlendScan = {
    "build", "cmake", ".git", ".github", "bin", "extern", "third_party", "build_assets",
};

// ... and the (shorter) list the loose-asset scan prunes. They differ because
// resources/assets is a directory a game owns: dropping a model into
// resources/assets/extern is not a mistake worth refusing.
constexpr std::array<std::string_view, 3> kPrunedByAssetScan = {"build", "cmake", ".git"};

// Path fragments whose contents are machine output, not sources: the exporter's
// own output directory, and anything a user marked as already exported.
constexpr std::array<std::string_view, 2> kSkippedPathFragments = {"resources/intermediate", "exported_assets"};

std::string Lower(std::string_view text) {
    std::string lowered(text);
    std::ranges::transform(lowered, lowered.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lowered;
}

bool EndsWith(std::string_view text, std::string_view suffix) {
    return text.size() >= suffix.size() && text.ends_with(suffix);
}

bool IsOneOf(std::string_view name, std::span<const std::string_view> names) {
    return std::ranges::find(names, name) != names.end();
}

// Ninja's metacharacters, escaped for a path: `$` starts an escape, and `:`
// ends a rule's output list, so a source file called `Caines$2: rig.png` is
// three syntax errors away from being ignored. A space -- the one this tree has
// actually hit -- is `$ `, the same spelling ninja uses for it in a variable
// expansion.
//
// Every path written into the graph goes through here, at the place where it is
// written. The lists that feed the pak's inputs hold *raw* paths and are escaped
// when they are joined: escaping on the way in and again on the way out is how
// the same path ends up spelled two ways, one of which no rule declares.
std::string Escape(std::string_view path) {
    std::string escaped;
    escaped.reserve(path.size());
    for (char c: path) {
        if (c == ' ') {
            escaped += "$ ";
        } else if (c == '$') {
            escaped += "$$";
        } else if (c == ':') {
            escaped += "$:";
        } else {
            escaped += c;
        }
    }
    return escaped;
}

std::string Join(const std::vector<std::string>& parts, std::string_view separator) {
    std::string joined;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) {
            joined += separator;
        }
        joined += parts[i];
    }
    return joined;
}

// Join raw paths into one ninja list, escaping each as it is written.
std::string JoinEscaped(const std::vector<std::string>& paths, std::string_view separator) {
    std::string joined;
    for (size_t i = 0; i < paths.size(); ++i) {
        if (i > 0) {
            joined += separator;
        }
        joined += Escape(paths[i]);
    }
    return joined;
}

std::string Slashed(const fs::path& path) {
    return path.generic_string();
}

// Absolute, forward-slashed, no trailing separator. The asset root arrives from
// CMake absolute already; normalizing means a generator invoked by hand from
// inside the tree writes the same graph the build does.
std::string Normalized(const std::string& raw) {
    std::error_code ec;
    fs::path        absolute = fs::absolute(fs::path(raw), ec);
    if (ec) {
        absolute = fs::path(raw);
    }
    std::string text = Slashed(absolute.lexically_normal());
    while (text.size() > 1 && text.back() == '/') {
        text.pop_back();
    }
    return text;
}

// The path of `full` relative to `root`, both already normalized. A path that is
// not under the root is returned unchanged: every caller here discovered its
// input under the root, and inventing a "../" spelling for a case that cannot
// happen would be a lie in the graph.
std::string RelativeTo(const std::string& full, const std::string& root) {
    if (full.size() > root.size() + 1 && full.starts_with(root) && full[root.size()] == '/') {
        return full.substr(root.size() + 1);
    }
    return full;
}

// A blend file's level name: its path under the asset root, extensionless, with
// separators folded into underscores -- "blender/props/crate.blend" becomes
// "blender_props_crate", which is the directory its intermediate files land in
// and the stem every cooked target is named after.
std::string LevelName(std::string_view relativePath) {
    const size_t slash = relativePath.find_last_of('/');
    const size_t dot   = relativePath.find_last_of('.');
    std::string  stem(
        relativePath.substr(0, (dot != std::string_view::npos && (slash == std::string_view::npos || dot > slash + 1)) ? dot : relativePath.size())
    );
    std::ranges::replace(stem, '/', '_');
    return stem;
}

std::vector<std::string> ListEntriesSorted(const fs::path& directory) {
    std::vector<std::string> names;
    std::error_code          ec;
    for (const fs::directory_entry& entry: fs::directory_iterator(directory, ec)) {
        if (ec) {
            break;
        }
        names.push_back(Slashed(entry.path().filename()));
    }
    std::ranges::sort(names);
    return names;
}

bool Exists(const fs::path& path) {
    std::error_code ec;
    return fs::exists(path, ec);
}

// Rewrite only on a real change. `data/base.pak` is packed from manifest.txt, so
// a manifest rewritten with identical content and a fresh mtime would make every
// build re-pack the archive and re-cook everything downstream of it.
bool WriteIfChanged(const fs::path& path, std::string_view content) {
    {
        std::ifstream existing(path, std::ios::binary);
        if (existing.is_open()) {
            std::string current((std::istreambuf_iterator<char>(existing)), std::istreambuf_iterator<char>());
            if (current == content) {
                return false;
            }
        }
    }
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    return true;
}

// --- Discovery -------------------------------------------------------------------------

std::vector<std::string> DiscoverBlendFiles(const std::string& sourceDir) {
    std::vector<std::string> found;
    std::error_code          ec;
    const fs::path           root(sourceDir);
    if (!Exists(root)) {
        return found;
    }

    for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec); it != fs::recursive_directory_iterator();
         it.increment(ec)) {
        if (ec) {
            break;
        }
        const std::string path = Slashed(it->path());
        std::error_code   kindEc;
        if (it->is_directory(kindEc)) {
            if (IsOneOf(Lower(Slashed(it->path().filename())), kPrunedByBlendScan)) {
                it.disable_recursion_pending();
            }
            continue;
        }
        std::error_code fileEc;
        if (!it->is_regular_file(fileEc)) {
            continue;
        }
        // The exporter's output is not a source, wherever it is spelled.
        const std::string parent = Lower(Slashed(it->path().parent_path()));
        if (std::ranges::any_of(kSkippedPathFragments, [&](std::string_view fragment) { return parent.find(fragment) != std::string::npos; })) {
            continue;
        }

        const std::string name = Slashed(it->path().filename());
        if (name.starts_with('.') || !EndsWith(name, ".blend")) {
            continue;
        }
        if (Lower(name).find("void") != std::string::npos) {
            continue;
        }
        found.push_back(path);
    }
    std::ranges::sort(found);
    return found;
}

// Every .ztex input the pipeline knows about, and the reference models it packs
// byte-for-byte: a .glb is already a runtime container, so it is a pak entry
// with no cook step of its own. Fonts are consumed pre-baked only (see
// include/Zahlen/gui/FontLoader.hpp): TTFs are cooked into 'FNT0' containers
// by `zcook font`, while fontbm `.fnt`+`.png` pairs and pre-cooked `.zfont`
// files travel byte-for-byte under `fonts/`.
struct LooseAssets {
    std::vector<std::string> textures;
    std::vector<std::string> models;
    std::vector<std::string> fonts;        // TTFs under fonts/ to cook
    std::vector<std::string> fontPayloads; // .fnt/.png/.zfont under fonts/ to pack raw
};

LooseAssets DiscoverLooseAssets(const std::string& assetsRoot) {
    LooseAssets     assets;
    std::error_code ec;
    const fs::path  root(assetsRoot);
    if (!Exists(root)) {
        return assets;
    }

    for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec); it != fs::recursive_directory_iterator();
         it.increment(ec)) {
        if (ec) {
            break;
        }
        std::error_code kindEc;
        if (it->is_directory(kindEc)) {
            if (IsOneOf(Lower(Slashed(it->path().filename())), kPrunedByAssetScan)) {
                it.disable_recursion_pending();
            }
            continue;
        }
        std::error_code fileEc;
        if (!it->is_regular_file(fileEc)) {
            continue;
        }

        const std::string path  = Slashed(it->path());
        const std::string lower = Lower(Slashed(it->path().filename()));
        const std::string rel   = RelativeTo(path, assetsRoot);
        // Fonts are a separate lane: everything under fonts/ is font payload,
        // not a texture, even though a fontbm page is a PNG. A TTF is cooked
        // into a `.zfont`; `.fnt`/`.png`/`.zfont` travel byte-for-byte.
        if (rel.starts_with("fonts/")) {
            if (EndsWith(lower, ".ttf")) {
                assets.fonts.push_back(path);
            } else if (EndsWith(lower, ".fnt") || EndsWith(lower, ".png") || EndsWith(lower, ".zfont")) {
                assets.fontPayloads.push_back(path);
            }
            continue;
        }
        if (EndsWith(lower, ".png") || EndsWith(lower, ".jpg") || EndsWith(lower, ".jpeg") || EndsWith(lower, ".tga")) {
            assets.textures.push_back(path);
        } else if (EndsWith(lower, ".glb")) {
            assets.models.push_back(path);
        }
    }
    std::ranges::sort(assets.textures);
    std::ranges::sort(assets.models);
    std::ranges::sort(assets.fonts);
    std::ranges::sort(assets.fontPayloads);
    return assets;
}

// --- The graph -------------------------------------------------------------------------

// One source file's worth of outputs. The intermediate directory is named after
// the level, so everything a .blend produced lives under one prefix and the
// cooked target names stay readable in a ninja log.
struct BlendUnit {
    std::string blend;
    std::string level;
    std::string levelDir;
    std::string meta;
};

} // namespace

int GenerateAssetNinja(int argc, char** argv) {
    std::string outPath;
    std::string sourceDir;
    std::string engineTools;
    std::string selfPath;
    for (int i = 0; i < argc; ++i) {
        std::string_view arg = argv[i];
        if (arg == "--out" && i + 1 < argc) {
            outPath = argv[++i];
        } else if (arg == "--source" && i + 1 < argc) {
            sourceDir = argv[++i];
        } else if (arg == "--engine-tools" && i + 1 < argc) {
            engineTools = argv[++i];
        } else if (arg == "--self" && i + 1 < argc) {
            selfPath = argv[++i];
        }
    }

    if (outPath.empty() || sourceDir.empty() || engineTools.empty()) {
        std::println(
            stderr, "[zcook] ERROR: Missing arguments for ninja subcommand.\n"
                    "  zcook ninja --out <assets.ninja> --source <asset-root> --engine-tools <dir> [--self <zcook>]"
        );
        return 1;
    }
    if (selfPath.empty()) {
        selfPath = argv[0];
    }
    // A bare name is resolved through PATH when ninja re-runs it; anything with a
    // separator is a path, and paths in a shared file have to be absolute.
    if (selfPath.find_first_of("/\\") != std::string::npos) {
        selfPath = Normalized(selfPath);
    }

    outPath     = Normalized(outPath);
    sourceDir   = Normalized(sourceDir);
    engineTools = Normalized(engineTools);

    const std::string intermediateRoot = sourceDir + "/resources/intermediate";
    const std::string assetsRoot       = sourceDir + "/resources/assets";
    const std::string blenderScript    = engineTools + "/export_metadata.py";
    const std::string blenderWrapper   = engineTools + "/run_blender.py";

    for (const std::string* required: {&blenderScript, &blenderWrapper}) {
        if (!Exists(*required)) {
            std::println(stderr, "[zcook] ERROR: '{}' does not exist; pass the engine's tools directory with --engine-tools.", *required);
            return 1;
        }
    }

    std::vector<BlendUnit> units;
    for (const std::string& blend: DiscoverBlendFiles(sourceDir)) {
        const std::string relative = RelativeTo(blend, sourceDir);
        const std::string level    = LevelName(relative);
        const std::string levelDir = intermediateRoot + "/" + level;
        units.push_back({blend, level, levelDir, levelDir + "/metadata.bin"});
    }

    const std::string escapedZcook  = Escape(selfPath);
    const std::string escapedScript = Escape(blenderScript);
    const std::string escapedWrap   = Escape(blenderWrapper);
    const std::string escapedOut    = Escape(outPath);
    const std::string escapedSource = Escape(sourceDir);
    const std::string escapedRoot   = Escape(intermediateRoot);

    // --- The rules. Every one of these is a zcook invocation except
    // blender_extract, which runs the exporter inside Blender because only
    // Blender's Python can read a .blend.
    std::string ninja = "# Automatically generated by zcook ninja -- do not edit\n"
                        "ninja_required_version = 1.3\n"
                        "builddir = build_assets\n"
                        "\n"
                        "rule blender_extract\n"
                        "  command = python3 \"" +
                        escapedWrap + "\" blender -b $in -P \"" + escapedScript + "\" -- $in \"" + escapedRoot +
                        "\"\n"
                        "  description = BLENDER $in\n"
                        "\n"
                        "rule zmesh\n"
                        "  command = \"" +
                        escapedZcook +
                        "\" mesh --meta \"$meta\" --id \"$id\" -i $in -o $out\n"
                        "  description = ZMESH $id\n"
                        "\n"
                        "rule zanim\n"
                        "  command = \"" +
                        escapedZcook +
                        "\" anim --meta \"$meta\" --id \"$id\" -o $out\n"
                        "  description = ZANIM $id\n"
                        "\n"
                        "rule ztex\n"
                        "  command = \"" +
                        escapedZcook +
                        "\" tex -i $in -o $out\n"
                        "  description = ZTEX $in\n"
                        "\n"
                        "rule zglb\n"
                        "  command = \"" +
                        escapedZcook +
                        "\" glb --meta $in -o $out\n"
                        "  description = ZGLB $in\n"
                        "\n"
                        "rule zfont\n"
                        "  command = \"" +
                        escapedZcook +
                        "\" font -i $in -o $out\n"
                        "  description = ZFONT $in\n"
                        "\n"
                        "rule zpak\n"
                        "  command = \"" +
                        escapedZcook +
                        "\" pak -o $out -i $in\n"
                        "  description = ZPAK $out\n"
                        "  pool = console\n";

    std::vector<std::string> compiledTargets;
    std::vector<std::string> glbTargets;
    std::vector<std::string> manifestEntries;
    std::vector<std::string> metaDependencies;

    metaDependencies.reserve(units.size());
    for (const BlendUnit& unit: units) {
        metaDependencies.push_back(unit.meta);
    }

    for (const BlendUnit& unit: units) {
        ninja += "\nbuild " + Escape(unit.meta) + ": blender_extract " + Escape(unit.blend) + " | " + escapedScript + " " + escapedWrap + "\n";

        // A .blend that has never been exported has nothing to cook yet: the
        // extraction rule above is the only edge, and ninja stops there until
        // Blender has run. A manifest that is there but unreadable is a warning
        // rather than an error, because the cook that follows will fail with the
        // parser's own message if it matters.
        if (!Exists(fs::path(unit.meta))) {
            continue;
        }

        Compiler::IRManifest manifest;
        if (auto parsed = Compiler::BinaryReader(unit.meta).Parse()) {
            manifest = std::move(*parsed);
        } else {
            std::println(stderr, "[zcook] WARNING: Failed to parse {}: {}", unit.meta, parsed.error());
        }

        for (const Compiler::IRMesh& mesh: manifest.meshes) {
            if (mesh.id.empty() || mesh.binFile.empty()) {
                continue;
            }
            const std::string input  = unit.levelDir + "/" + mesh.binFile;
            const std::string output = "build_assets/" + unit.level + "/" + mesh.id + ".zmesh";

            ninja += "\nbuild " + Escape(output) + ": zmesh " + Escape(input) + " | " + Escape(unit.meta) + " || " + escapedZcook + "\n";
            ninja += "  meta = " + unit.meta + "\n";
            ninja += "  id = " + mesh.id + "\n";
            compiledTargets.push_back(output);
            manifestEntries.push_back(mesh.id + ".zmesh=" + output);
        }

        for (const Compiler::IRAnimation& animation: manifest.animations) {
            if (animation.id.empty() || animation.samplers.empty() || animation.samplers.front().binFile.empty()) {
                continue;
            }
            const std::string input  = unit.levelDir + "/" + animation.samplers.front().binFile;
            const std::string output = "build_assets/" + unit.level + "/" + animation.id + ".zanim";

            ninja += "\nbuild " + Escape(output) + ": zanim " + Escape(input) + " | " + Escape(unit.meta) + " || " + escapedZcook + "\n";
            ninja += "  meta = " + unit.meta + "\n";
            ninja += "  id = " + animation.id + "\n";
            compiledTargets.push_back(output);
            manifestEntries.push_back(animation.id + ".zanim=" + output);
        }

        // Whatever the exporter dropped into this level's textures/ directory is
        // an input, entry for entry: a subdirectory is the exporter's business,
        // and refusing it here would be this generator inventing a rule the cook
        // step does not have.
        const fs::path textureDir = fs::path(unit.levelDir) / "textures";
        if (Exists(textureDir)) {
            for (const std::string& texture: ListEntriesSorted(textureDir)) {
                const std::string input  = unit.levelDir + "/textures/" + texture;
                const std::string output = "build_assets/" + unit.level + "/lvl_" + texture + ".ztex";

                ninja += "build " + Escape(output) + ": ztex " + Escape(input) + " | " + Escape(unit.meta) + " || " + escapedZcook + "\n";
                compiledTargets.push_back(output);
                manifestEntries.push_back("textures/" + texture + "=" + output);
            }
        }

        // The debug GLB is a view of the level, not an asset: it exists so a
        // cooked blend can be opened in a glTF viewer without Blender.
        std::vector<std::string> bins;
        for (const Compiler::IRMesh& mesh: manifest.meshes) {
            if (!mesh.binFile.empty()) {
                bins.push_back(unit.levelDir + "/" + mesh.binFile);
            }
        }
        std::ranges::sort(bins);
        auto [first, last] = std::ranges::unique(bins);
        bins.erase(first, bins.end());

        const std::string glbOutput = "build_assets/debug_glb/" + unit.level + ".glb";
        ninja += "\nbuild " + Escape(glbOutput) + ": zglb " + Escape(unit.meta) + " | " + JoinEscaped(bins, " ") + " || " + escapedZcook + "\n";
        glbTargets.push_back(glbOutput);
    }

    const LooseAssets loose = DiscoverLooseAssets(assetsRoot);
    for (const std::string& texture: loose.textures) {
        const std::string relative = RelativeTo(texture, assetsRoot);
        const std::string output   = "build_assets/raw/" + relative + ".ztex";

        ninja += "build " + Escape(output) + ": ztex " + Escape(texture) + " || " + escapedZcook + "\n";
        compiledTargets.push_back(output);
        manifestEntries.push_back(relative + "=" + output);
    }
    // A .glb is packed where it lies: it is already the runtime container the
    // engine loads, so there is nothing to compile and no target to build.
    for (const std::string& model: loose.models) {
        const std::string relative = RelativeTo(model, assetsRoot);
        compiledTargets.push_back(model);
        manifestEntries.push_back(relative + "=" + model);
    }

    // TrueType fonts are the one font input the pipeline cooks: the runtime
    // never parses an outline font, so `zcook font` bakes the SDF atlas and
    // the cooked container lands as fonts/<name>.zfont (the virtual path
    // PrimeDefaultBakedFont looks up).
    for (const std::string& font: loose.fonts) {
        const std::string relative    = RelativeTo(font, assetsRoot);
        const size_t      dot         = relative.find_last_of('.');
        const std::string virtualPath = (dot == std::string::npos) ? (relative + ".zfont") : (relative.substr(0, dot) + ".zfont");
        const std::string output      = "build_assets/raw/" + relative + ".zfont";

        ninja += "\nbuild " + Escape(output) + ": zfont " + Escape(font) + " || " + escapedZcook + "\n";
        compiledTargets.push_back(output);
        manifestEntries.push_back(virtualPath + "=" + output);
    }
    // fontbm pairs and pre-cooked containers are runtime-ready: pack them
    // byte-for-byte under their fonts/ virtual paths.
    for (const std::string& payload: loose.fontPayloads) {
        const std::string relative = RelativeTo(payload, assetsRoot);
        compiledTargets.push_back(payload);
        manifestEntries.push_back(relative + "=" + payload);
    }

    // Fallback: ensure fonts/default.zfont is valid even when no TTF is present.
    // The engine's zero-asset embedded font (resources/fonts/DefaultFont.zfont)
    // is the canonical fallback; packing it as fonts/default.zfont makes the
    // pak valid and eliminates the WARNING: BMFont/cooked font failed warnings
    // in standalone builds that have no game fonts. If an existing payload for
    // fonts/default.zfont exists but is invalid (not FNT0), replace it.
    auto isValidZFont = [](const std::string& path) -> bool {
        std::ifstream f(path, std::ios::binary);
        if (!f) {
            return false;
        }
        std::array<char, 4> magic {};
        f.read(magic.data(), 4);
        return f.gcount() == 4 && std::memcmp(magic.data(), "FNT0", 4) == 0;
    };

    bool hasValidDefaultZFont = false;
    for (const auto& e: manifestEntries) {
        if (e.starts_with("fonts/default.zfont=")) {
            std::string real = e.substr(std::string("fonts/default.zfont=").size());
            if (isValidZFont(real)) {
                hasValidDefaultZFont = true;
                break;
            }
        }
    }

    if (!hasValidDefaultZFont) {
        // Remove any existing invalid fonts/default.zfont entries
        std::erase_if(manifestEntries, [](const std::string& e) { return e.starts_with("fonts/default.zfont="); });
        std::erase_if(compiledTargets, [](const std::string& p) {
            return p.find("fonts/default.zfont") != std::string::npos || p.find("fonts/default.fnt") != std::string::npos;
        });

        const std::string embeddedSrc = sourceDir + "/resources/fonts/DefaultFont.zfont";
        if (Exists(fs::path(embeddedSrc)) && isValidZFont(embeddedSrc)) {
            compiledTargets.push_back(embeddedSrc);
            manifestEntries.push_back("fonts/default.zfont=" + embeddedSrc);
        }
    }

    // Also drop any fonts/default.fnt that is not valid JSON BMFont (would cause MissingMetrics warning)
    // For simplicity, if we have a valid default.zfont, we don't need default.fnt
    if (hasValidDefaultZFont || !manifestEntries.empty()) {
        // If we have valid zfont, remove any .fnt that would cause warnings
        bool haveZFontNow = false;
        for (const auto& e: manifestEntries) {
            if (e.starts_with("fonts/default.zfont=")) {
                haveZFontNow = true;
                break;
            }
        }
        if (haveZFontNow) {
            std::erase_if(manifestEntries, [](const std::string& e) { return e.starts_with("fonts/default.fnt="); });
        }
    }

    // --- The manifest. It is the only record of which cooked file answers which
    // virtual path, and zcook pak is its only reader.
    std::ranges::sort(manifestEntries);
    std::string manifestBody;
    for (size_t i = 0; i < manifestEntries.size(); ++i) {
        if (i > 0) {
            manifestBody += "\n";
        }
        manifestBody += manifestEntries[i];
    }
    const std::string manifestPath = (fs::path(outPath).parent_path() / "build_assets" / "manifest.txt").generic_string();
    WriteIfChanged(manifestPath, manifestBody);

    // --- The archive, the debug view, and the defaults.
    std::ranges::sort(compiledTargets);
    const std::string packedTargets = JoinEscaped(compiledTargets, " ");
    const std::string metaInputs    = JoinEscaped(metaDependencies, " ");

    ninja += "\nbuild data/base.pak: zpak " + Escape(manifestPath) + " | " + packedTargets + " " + metaInputs + " || " + escapedZcook + "\n";

    std::ranges::sort(glbTargets);
    ninja += "\nbuild debug_glbs: phony " + JoinEscaped(glbTargets, " ") + "\n";

    // --- Self-regeneration. `generator = 1` is what tells ninja that running
    // this rule may change the build graph itself, so it reloads the file and
    // re-plans instead of trusting the graph it started with. The dependencies
    // are what make the rewrite happen when it should: the generator binary (so
    // a rebuilt zcook regenerates the graph it describes), every .blend (a new
    // source file is a new target), every metadata.bin (a new mesh or animation
    // inside an existing blend is a new target), and the two Python files the
    // extraction rule names.
    ninja += "\nrule regenerate_ninja\n";
    // Quoted, unlike the rule commands above: this is the one command line
    // written by hand rather than carried over from the generator this
    // replaced, and a source directory with a space in it would otherwise
    // split into two arguments on the way back in.
    ninja += "  command = \"" + escapedZcook + "\" ninja --out \"" + escapedOut + "\" --source \"" + escapedSource + "\" --engine-tools \"" +
             Escape(engineTools) + "\" --self \"" + escapedZcook + "\"\n";
    ninja += "  description = Regenerating assets.ninja\n";
    ninja += "  generator = 1\n";

    std::vector<std::string> blendDependencies;
    blendDependencies.reserve(units.size());
    for (const BlendUnit& unit: units) {
        blendDependencies.push_back(unit.blend);
    }
    ninja += "\nbuild " + escapedOut + ": regenerate_ninja " + escapedZcook + " | " + JoinEscaped(blendDependencies, " ") + " " + metaInputs + " " +
             escapedScript + " " + escapedWrap + "\n";

    ninja += "\ndefault data/base.pak\n";

    // The graph is rewritten on every run: it is this command's output, and a
    // generator rule that returns without touching its own file is the kind of
    // thing a build system is entitled to be suspicious of. The manifest above
    // is the opposite case -- it is an *input* to the pack step, so its mtime
    // has to stay put when its content did.
    std::ofstream graph(outPath, std::ios::binary | std::ios::trunc);
    graph.write(ninja.data(), static_cast<std::streamsize>(ninja.size()));
    return 0;
}

} // namespace ZHLN
