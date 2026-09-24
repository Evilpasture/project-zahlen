// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// samples/RemoteGLBSample.cpp
//
// A remote asset, end to end: fetch a .glb over HTTP, cache it on disk, import
// it, and put it on a turntable.
//
// The asset is Khronos' Damaged Helmet from glTF-Sample-Assets -- the model
// every other glTF viewer measures itself against, and a hard one: a single mesh
// carrying the full metallic-roughness set (base colour, normal, packed
// occlusion/roughness/metallic, emissive) behind a node transform that stands it
// upright. It is fetched at run time and never committed, which is the point:
// the subject of this sample is the fetch, not the file. It stays the CC-BY-NC
// material of the Khronos authors, and the cache directory it lands in is
// outside the repository.
//
// Three stages, each owned by the layer that knows about it:
//
//   1. extras/HTTP does the transfer. HTTP::Fetch is synchronous on purpose
//      ("an engine that must not stall a frame calls this from a worker"), so
//      the sample runs it on a std::jthread and keeps rendering while it goes:
//      the backdrop is up, the grade is set, and the HUD counts the seconds.
//   2. The bytes land in the engine's own cache directory --
//      ZHLN::FS::Paths::CacheDir(), which is build/cache inside a dev tree and
//      the per-user cache directory anywhere else, with ZHLN_CACHE_DIR over both
//      -- under http/<name>-<hash>.glb. The second run touches no network at
//      all. A cached file has to pass the 12-byte GLB container check before it
//      is trusted, so a truncated download, or an HTML error page written by an
//      earlier run, is re-fetched instead of parsed.
//   3. extras/glTF imports the bytes straight from memory
//      (GLTF::LoadGLBPrefabFromMemory, the call the inspector's drop handler
//      makes) and PrefabFactory::InstantiatePrefab spawns the parts.
//
// The camera is a turntable rather than core's free-cam: left-drag orbits,
// right-drag pans, the wheel zooms, and the framing comes from the imported
// bounds transformed through the node hierarchy, so any asset lands in shot. The
// render settings are the other half of the sample -- MakeStudioSettings says
// what each knob does and why the numbers are what they are.
//
//   ./build/samples/RemoteGLBSample
//   ZHLN_REMOTE_GLB_FRAMES=300 ./build/samples/RemoteGLBSample --headless
//
// Environment:
//   ZHLN_REMOTE_GLB_URL=<url>     another .glb to fetch. Self-contained only:
//                                 the in-memory importer cannot chase external
//                                 buffer or image URIs.
//   ZHLN_REMOTE_GLB_REFRESH=1     ignore the cache and fetch again
//   ZHLN_REMOTE_GLB_RTR=1         ray-traced reflections and sun shadow
//                                 (default off; MakeStudioSettings says why)
//   ZHLN_REMOTE_GLB_TIMEOUT=<s>   transfer budget, default 60
//   ZHLN_REMOTE_GLB_FRAMES=<n>    exit after n frames -- a headless smoke run
//
// Keys: 1-4 quality tiers, 0 back to the hand-tuned studio look, F re-frame,
// G hide/show the floor, R re-download.

#include <Zahlen/Camera.hpp>
#include <Zahlen/Clock.hpp>
#include <Zahlen/CommandLine.hpp>
#include <Zahlen/Components.hpp>
#include <Zahlen/Core/Hash.hpp>
#include <Zahlen/PrefabFactory.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Entity.hpp>
#include <Zahlen/FileSystem/Paths.hpp>
#include <Zahlen/GraphicsSettings.hpp>
#include <Zahlen/Input.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Math3D.hpp>
#include <Zahlen/ModelPrefab.hpp>
#include <Zahlen/PlatformHost.hpp>
#include <Zahlen/Render/Render.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <Zahlen/Window.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <Zahlen/gui/GUI.hpp>

// The two extras this sample is about. Both are optional targets -- no libcurl,
// no zahlen_http; no extras, no zahlen_gltf -- and samples/CMakeLists.txt skips
// a sample whose extras were not built, so neither include needs a guard here.
#include <HTTP/HTTP.hpp>
#include <glTF/GLTFImporter.hpp>

#if defined(ZHLN_HAS_FONTS)
#include <Fonts/Fonts.hpp>
#endif

#include <Jolt/Jolt.h>
#include <Jolt/Math/Vec3.h>
#include <Jolt/Math/Vec4.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

// ============================================================================
// WHAT TO FETCH
// ============================================================================

// The link a browser's address bar offers for a repository file is GitHub's HTML
// page *about* the file (/blob/); the file itself is the /raw/ redirect beside
// it. FetchCandidates rewrites one into the other, so the URL below is the URL to
// paste rather than a second spelling to maintain.
inline constexpr std::string_view kDefaultAssetURL =
    "https://github.com/KhronosGroup/glTF-Sample-Assets/blob/main/Models/DamagedHelmet/glTF-Binary/DamagedHelmet.glb";

inline constexpr std::string_view kUserAgent      = "project-zahlen/RemoteGLBSample";
inline constexpr std::string_view kAcceptGLB      = "model/gltf-binary, application/octet-stream, */*";
inline constexpr uint32_t         kFetchTimeout   = 60;
inline constexpr size_t           kMaxDetailChars = 400;
inline constexpr size_t           kMaxNameChars   = 48;

// ============================================================================
// THE LOOK
// ============================================================================

// The studio, in the units the shaders actually consume. blit.slang scales the
// HDR buffer by `exposure`, tonemaps with ACES and then grades, so every
// radiance number below is chosen against that one multiplier rather than in
// isolation: the sun is dim because the exposure is bright.
inline constexpr float kExposure        = 0.080f;
inline constexpr int   kTonemapperACES  = 1;      // blit.slang: 0 linear, 1 ACES, 2 Reinhard, 3 neutral
inline constexpr float kBloomStrength   = 0.08f;  // was 0.15 — emissive bloom was bleeding into blur chain, oil-painting look
inline constexpr float kGlowIntensity   = 0.12f;  // was 0.22 — helmet emissive decals should not smear
inline constexpr float kContrast        = 1.03f;
inline constexpr float kSaturation      = 1.05f;
inline constexpr float kVignette        = 0.25f; // was 0.35 — even lighter, keep corners readable
inline constexpr float kVignettePower   = 1.10f;
inline constexpr float kAmbientExposure = 4.00f; // scales the baked SH fill *and* the sky gradient below

// A softbox gradient rather than a night sky. The IBL cube is baked once, at
// renderer init, from the engine's default sky, so a metal surface's environment
// has to come from what it can actually see: the screen-space reflection pass
// reads the lit buffer, and the gradient below is what fills it (and what a
// ray-traced reflection paints where its ray leaves the scene). This is the lever
// that makes brushed metal read as metal.
inline const JPH::Vec4 kSkyZenith {0.35f, 0.42f, 0.55f, 1.0f};
inline const JPH::Vec4 kSkyHorizon {0.90f, 0.92f, 0.96f, 1.0f};
inline const JPH::Vec4 kSkyGround {0.06f, 0.06f, 0.07f, 1.0f};

// Key light. Directional, so it needs no distance compensation.
inline constexpr float kSunIntensity = 60.0f;
inline const JPH::Vec3 kSunColor {1.00f, 0.97f, 0.92f};
inline const JPH::Vec3 kSunDirection {0.45f, 1.00f, 0.30f};

// Two punctual fills, quoted per square metre of subject: a point light falls off
// as 1/(d^2 + 1) and the rig sits at a distance proportional to the subject's
// radius, so intensity scales with radius^2 to look the same on a helmet and on a
// car. At the helmet's ~0.5 m that is 25 and 40 — was 70/110 which blew the floor.
inline constexpr float kFillIntensityPerM2 = 100.0f;
inline constexpr float kRimIntensityPerM2  = 160.0f;
inline const JPH::Vec3 kFillColor {0.55f, 0.72f, 1.00f}; // cool, from the front-left
inline const JPH::Vec3 kRimColor {1.00f, 0.72f, 0.45f};  // warm, from behind-right

// Floor: dark and glossy enough to show SSR/RTR reflections without blowing out
// point-light specular. 0.22 was too sharp (blown highlights on RTX 3050),
// 0.45 killed reflections entirely (no floor reflection), 0.28 was VNDF
// (alpha=0.078) with 1 SPP sparkle like sparkling water. 0.03 is mirror tier
// (<0.04): warp-coherent full-res ray, no VNDF jitter, no half-res upsample,
// clean reflection. Keep dark color so mirror is not blown.
inline const JPH::Vec4 kFloorColor {0.08f, 0.09f, 0.11f, 1.0f};
inline constexpr float kFloorRoughness = 0.03f;
inline constexpr float kFloorExtent    = 5.0f; // wider to catch reflections at oblique angles

// Framing and feel of the turntable: the distance that puts the bounding sphere
// just inside the vertical field of view, times a margin so the subject does not
// touch the edge. Was 1.35 which at r=1.645 gave 5.36m and a tiny helmet.
inline constexpr float kFrameMargin    = 1.15f;
inline constexpr float kFollowRate     = 14.0f; // exponential settle for pan and zoom
inline constexpr float kOrbitSpeed     = 0.30f;
inline constexpr float kPanSpeed       = 0.0015f;
inline constexpr float kZoomSpeed      = 0.18f;
inline constexpr float kNearPlaneScale = 0.02f;

// HUD geometry. The camera ignores mouse input inside this strip, the way the
// glTF inspector ignores it inside its explorer panel.
inline constexpr float kPanelWidth  = 460.0f;
inline constexpr float kPanelMargin = 16.0f;

// ============================================================================
// ENVIRONMENT, URL AND CACHE
// ============================================================================

[[nodiscard]] auto EnvironmentString(const char* name, std::string_view fallback) -> std::string {
    const char* value = std::getenv(name);
    return (value != nullptr && value[0] != '\0') ? std::string(value) : std::string(fallback);
}

[[nodiscard]] auto EnvironmentFlag(const char* name) -> bool {
    const char* value = std::getenv(name);
    if (value == nullptr || value[0] == '\0') {
        return false;
    }
    const std::string_view text(value);
    return text != "0" && text != "false" && text != "FALSE" && text != "off" && text != "OFF";
}

[[nodiscard]] auto EnvironmentU32(const char* name, uint32_t fallback) -> uint32_t {
    const char* value = std::getenv(name);
    if (value == nullptr || value[0] == '\0') {
        return fallback;
    }
    char*          end    = nullptr;
    const unsigned parsed = static_cast<unsigned>(std::strtoul(value, &end, 10));
    return (end != value) ? static_cast<uint32_t>(parsed) : fallback;
}

[[nodiscard]] auto StripQuery(std::string_view url) noexcept -> std::string_view {
    const size_t cut = url.find_first_of("?#");
    return (cut == std::string_view::npos) ? url : url.substr(0, cut);
}

// The host and the absolute path of an http(s) URL, or two empty views when the
// string is not one. Nothing here needs a real URL parser: the only rewrite on
// offer moves "/blob/" inside the path.
[[nodiscard]] auto SplitHostAndPath(std::string_view url) noexcept -> std::pair<std::string_view, std::string_view> {
    const size_t scheme = url.find("://");
    if (scheme == std::string_view::npos) {
        return {};
    }
    const size_t hostStart = scheme + 3;
    const size_t pathStart = url.find('/', hostStart);
    if (pathStart == std::string_view::npos) {
        return {url.substr(hostStart), {}};
    }
    return {url.substr(hostStart, pathStart - hostStart), url.substr(pathStart)};
}

// Every spelling of one URL worth putting on the wire, in the order to try them.
// A plain https URL is its own only candidate; a GitHub /blob/ page link also
// gets the /raw/ redirect beside it (which raw.githubusercontent.com answers, and
// which HTTP::Fetch follows) and the raw host spelled out directly, for a network
// that resolves one and not the other.
[[nodiscard]] auto FetchCandidates(std::string_view url) -> std::vector<std::string> {
    constexpr std::string_view kBlob   = "/blob/";
    constexpr std::string_view kRawCDN = "https://raw.githubusercontent.com";

    std::vector<std::string> candidates {std::string(url)};
    const auto [host, path] = SplitHostAndPath(url);
    const bool   isGitHub   = (host == "github.com") || (host == "www.github.com");
    const size_t blob       = path.find(kBlob);
    if (!isGitHub || blob == std::string_view::npos) {
        return candidates;
    }

    // The offset of the path inside the whole URL, so the rewrite lands there.
    const size_t hostStart = url.find("://") + 3;
    const size_t blobAt    = url.find('/', hostStart) + blob;

    std::string raw = std::string(url);
    raw.replace(blobAt, kBlob.size(), "/raw/");
    candidates.push_back(std::move(raw));

    std::string cdn = std::string(kRawCDN);
    cdn += path.substr(0, blob);
    cdn += path.substr(blob + kBlob.size());
    candidates.push_back(std::move(cdn));
    return candidates;
}

[[nodiscard]] auto LastPathSegment(std::string_view url) noexcept -> std::string_view {
    const std::string_view path  = StripQuery(url);
    const size_t           slash = path.rfind('/');
    return (slash == std::string_view::npos) ? path : path.substr(slash + 1);
}

// A remote URL is not a filename. Everything that is not plainly safe in one is
// dropped, so the cache path can never climb out of its directory or carry a
// character some platform disagrees with.
[[nodiscard]] auto SanitizeFileName(std::string_view name) -> std::string {
    std::string out;
    out.reserve(name.size());
    for (const char c: name) {
        const bool plain = ((c >= 'a') && (c <= 'z')) || ((c >= 'A') && (c <= 'Z')) || ((c >= '0') && (c <= '9'));
        if (plain || (c == '-') || (c == '_')) {
            out.push_back(c);
        }
    }
    if (out.size() > kMaxNameChars) {
        out.resize(kMaxNameChars);
    }
    if (out.empty()) {
        out = "asset";
    }
    return out;
}

// The cache file name: the URL's last path segment without its extension, plus
// eight hex digits of the whole URL's hash, so two URLs that end in the same name
// cannot share a file. The .glb is appended here, by the thing that knows what
// the bytes are.
[[nodiscard]] auto CacheFileName(std::string_view url) -> std::string {
    const std::string_view segment = LastPathSegment(url);
    const size_t           dot     = segment.rfind('.');
    const std::string      stem    = SanitizeFileName((dot == std::string_view::npos) ? segment : segment.substr(0, dot));
    return std::format("{}-{:08x}.glb", stem, static_cast<uint32_t>(ZHLN::Hash64(url) & 0xFFFFFFFFULL));
}

// Where a downloaded asset lives between runs. The engine already answers that
// question for its own caches -- build/cache in a dev tree, the per-user cache
// directory otherwise, ZHLN_CACHE_DIR over both -- and a fetched model is the
// same kind of thing as a pipeline cache: regenerable, machine-local, never
// committed.
[[nodiscard]] auto CacheFileFor(std::string_view url) -> std::filesystem::path {
    return ZHLN::FS::Paths::CacheDir() / "http" / CacheFileName(url);
}

// The 12-byte GLB container header: the magic, the version, and the total byte
// length of the container. Read little-endian by hand, because the spec says
// little-endian and the host is not the spec. A cached file that fails this is a
// truncated download or an HTML error page, and is re-fetched rather than handed
// to a parser that would only answer "not a glTF".
[[nodiscard]] auto IsGLBContainer(std::span<const uint8_t> bytes) noexcept -> bool {
    if (bytes.size() < 12 || bytes.size() > ZHLN::HTTP::kMaxBodyBytes) {
        return false;
    }
    if (std::memcmp(bytes.data(), "glTF", 4) != 0) {
        return false;
    }
    const auto littleEndian32 = [](const uint8_t* p) noexcept -> uint32_t {
        return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
               (static_cast<uint32_t>(p[3]) << 24);
    };
    const uint32_t version = littleEndian32(bytes.data() + 4);
    const uint32_t length  = littleEndian32(bytes.data() + 8);
    return (version == 2) && (static_cast<size_t>(length) == bytes.size());
}

[[nodiscard]] auto ReadWholeFile(const std::filesystem::path& file) -> std::vector<uint8_t> {
    std::ifstream in(file, std::ios::binary | std::ios::ate);
    if (!in) {
        return {};
    }
    const auto size = in.tellg();
    if (size <= 0) {
        return {};
    }
    in.seekg(0, std::ios::beg);
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    in.read(reinterpret_cast<char*>(bytes.data()), size);
    if (!in) {
        return {}; // A short read is no file at all: the caller re-fetches.
    }
    return bytes;
}

// Writes through a sibling temp file and renames it into place, the way the
// pipeline cache does: an interrupted download then leaves no half-written file
// behind for the next run to mistake for a cache hit.
auto WriteCacheFile(const std::filesystem::path& file, std::span<const uint8_t> bytes) -> bool {
    std::error_code ec;
    if (const auto parent = file.parent_path(); !parent.empty()) {
        std::filesystem::create_directories(parent, ec);
    }

    const std::filesystem::path temp = std::filesystem::path(file).concat(".tmp");
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) {
            ZHLN::Log("[RemoteGLB] Could not open '{}' for writing.", temp.string());
            return false;
        }
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        out.close();
        if (out.fail()) {
            ZHLN::Log("[RemoteGLB] Writing '{}' failed.", temp.string());
            std::filesystem::remove(temp, ec);
            return false;
        }
    }

    std::filesystem::rename(temp, file, ec);
    if (ec) {
        ZHLN::Log("[RemoteGLB] Renaming '{}' into place failed: {}", temp.string(), ec.message());
        std::filesystem::remove(temp, ec);
        return false;
    }
    return true;
}

// ============================================================================
// THE FETCH
// ============================================================================

enum class FetchPhase : uint8_t { Idle, Running, Succeeded, Failed };

struct FetchOutcome {
    bool                 ok = false;
    std::vector<uint8_t> bytes;
    std::string          source; // which candidate answered
    std::string          detail; // every failure, for the log and the HUD
};

// One transfer, tried against each candidate in turn. An HTTP status is data and
// not an error (see HTTP.hpp), so a 404 is a reason to try the next candidate and
// a failed transfer is a reason to say what failed. Logging is left to the
// caller: this runs on the worker, and the frame loop reports the outcome once,
// in order, when it observes it.
[[nodiscard]] auto FetchGLB(const std::vector<std::string>& candidates, uint32_t timeoutSeconds, const std::stop_token& stop) -> FetchOutcome {
    FetchOutcome outcome;
    const auto   note = [&outcome](std::string text) -> void {
        if (outcome.detail.size() >= kMaxDetailChars) {
            return;
        }
        if (!outcome.detail.empty()) {
            outcome.detail += "; ";
        }
        outcome.detail += std::move(text);
    };

    for (const std::string& url: candidates) {
        if (stop.stop_requested()) {
            note("cancelled");
            return outcome;
        }

        const ZHLN::HTTP::Request request {
            .url            = url,
            .headers        = {ZHLN::HTTP::Header {.name = "User-Agent", .value = std::string(kUserAgent)},
                               ZHLN::HTTP::Header {.name = "Accept", .value = std::string(kAcceptGLB)}},
            .timeoutSeconds = timeoutSeconds,
        };

        auto response = ZHLN::HTTP::Fetch(request);
        if (!response) {
            const ZHLN::Error err = response.error();
            note(std::format("{}: {} ({})", url, err.Category(), err.Message()));
            continue;
        }
        if (response->statusCode < 200 || response->statusCode >= 300) {
            note(std::format("{}: HTTP {}", url, response->statusCode));
            continue;
        }
        if (!IsGLBContainer(response->body)) {
            note(std::format("{}: {} byte(s) that are not a GLB container", url, response->body.size()));
            continue;
        }

        outcome.bytes  = std::move(response->body);
        outcome.source = url;
        outcome.ok     = true;
        return outcome;
    }
    return outcome;
}

// A download in flight. `worker` is declared last on purpose: members are
// destroyed in reverse order, so the jthread joins before the strings and the
// byte vector its body writes are torn down.
//
// The synchronisation is one release/acquire pair. The worker owns `bytes`,
// `source` and `detail` until it stores the phase with release; the frame loop
// reads them only after an acquire load reports Succeeded or Failed, and the
// worker never touches them again. No lock, because nothing else is shared.
struct RemoteAsset {
    std::string             url;
    std::filesystem::path   cacheFile;
    std::vector<uint8_t>    bytes;
    std::string             source;
    std::string             detail;
    double                  startedAt = 0.0;
    bool                    usedCache = false;
    std::atomic<FetchPhase> phase {FetchPhase::Idle};
    std::jthread            worker;
};

// Cache first, network second. The cache read stays on the calling thread -- it
// is a few megabytes from a local disk, and a hit means the model is on screen in
// the first frame. Only the transfer goes to a worker, because that is the part
// that can take a minute.
void StartFetch(RemoteAsset& asset, bool bypassCache, uint32_t timeoutSeconds, double now) {
    if (asset.phase.load(std::memory_order::acquire) == FetchPhase::Running) {
        return;
    }
    asset.bytes.clear();
    asset.source.clear();
    asset.detail.clear();
    asset.usedCache = false;
    asset.startedAt = now;

    if (!bypassCache) {
        std::vector<uint8_t> cached = ReadWholeFile(asset.cacheFile);
        if (IsGLBContainer(cached)) {
            asset.bytes     = std::move(cached);
            asset.source    = asset.cacheFile.string();
            asset.usedCache = true;
            asset.phase.store(FetchPhase::Succeeded, std::memory_order::release);
            return;
        }
        if (!cached.empty()) {
            std::error_code ec;
            std::filesystem::remove(asset.cacheFile, ec);
            asset.detail = std::format("'{}' held {} byte(s) of something that is not a GLB; removed it", asset.cacheFile.string(), cached.size());
        }
    }

    const std::vector<std::string> candidates = FetchCandidates(asset.url);
    asset.phase.store(FetchPhase::Running, std::memory_order::release);
    asset.worker = std::jthread([&asset, candidates, timeoutSeconds](std::stop_token stop) -> void {
        FetchOutcome outcome = FetchGLB(candidates, timeoutSeconds, stop);
        if (!outcome.ok) {
            asset.detail = outcome.detail.empty() ? std::string("no candidate answered") : std::move(outcome.detail);
            asset.phase.store(FetchPhase::Failed, std::memory_order::release);
            return;
        }
        if (WriteCacheFile(asset.cacheFile, outcome.bytes)) {
            // From here on the honest source is the cache: that is what the next
            // run reads, and what a device-lost rebuild would re-import.
            outcome.source = asset.cacheFile.string();
        }
        asset.bytes = std::move(outcome.bytes);
        asset.source = std::move(outcome.source);
        asset.phase.store(FetchPhase::Succeeded, std::memory_order::release);
    });
}

// ============================================================================
// THE SUBJECT
// ============================================================================

struct Subject {
    ZHLN::ModelPrefab*        prefab    = nullptr;
    std::vector<ZHLN::Entity> instances {};
    JPH::Vec3                 center    = JPH::Vec3::sZero();
    JPH::Vec3                 boundsMin = JPH::Vec3::sZero();
    JPH::Vec3                 boundsMax = JPH::Vec3::sZero();
    float                     radius    = 1.0f;
    uint32_t                  triangles = 0;
    uint32_t                  textures  = 0;
    bool                      loaded    = false;
};

// A part's node transform, accumulated up the hierarchy -- the same product
// PrefabFactory::InstantiatePrefab places the part with, and the same walk
// ProceduralAnimationSample uses to estimate a character's bounds.
[[nodiscard]] auto NodeModelTransform(const ZHLN::ModelPrefab& prefab, int32_t nodeIndex) -> JPH::Mat44 {
    if (nodeIndex < 0 || nodeIndex >= static_cast<int32_t>(prefab.nodes.size())) {
        return JPH::Mat44::sIdentity();
    }
    JPH::Mat44 transform = prefab.nodes[static_cast<size_t>(nodeIndex)].localTransform;
    int32_t    parent    = prefab.nodes[static_cast<size_t>(nodeIndex)].parentIndex;
    for (size_t depth = 0; depth < prefab.nodes.size() && parent >= 0 && parent < static_cast<int32_t>(prefab.nodes.size()); ++depth) {
        transform = prefab.nodes[static_cast<size_t>(parent)].localTransform * transform;
        parent    = prefab.nodes[static_cast<size_t>(parent)].parentIndex;
    }
    return transform;
}

// The subject's world-space bounds. A part's localMin/localMax live in the part's
// own space, so the node chain above them has to be applied first -- for the
// Damaged Helmet that chain is a -90 degree turn about X that stands the model up,
// and a union of untransformed boxes would be a different box. The world AABB of
// an oriented box is its centre plus the absolute contributions of its three
// transformed half axes, which is exact for a rotation and a uniform scale.
void ComputeBounds(const ZHLN::ModelPrefab& prefab, JPH::Vec3& outMin, JPH::Vec3& outMax) {
    outMin = JPH::Vec3(1e9f, 1e9f, 1e9f);
    outMax = JPH::Vec3(-1e9f, -1e9f, -1e9f);

    for (const ZHLN::ModelPart& part: prefab.parts) {
        const JPH::Mat44 model = NodeModelTransform(prefab, part.nodeIndex) * part.localTransform;
        const JPH::Vec3  localMin(part.localMin[0], part.localMin[1], part.localMin[2]);
        const JPH::Vec3  localMax(part.localMax[0], part.localMax[1], part.localMax[2]);
        const JPH::Vec3  localCenter = (localMin + localMax) * 0.5f;
        const JPH::Vec3  halfExtent  = (localMax - localMin) * 0.5f;

        const JPH::Vec3 center = model.Multiply3x3(localCenter) + model.GetTranslation();
        const JPH::Vec3 extent = (model.GetColumn3(0) * halfExtent.GetX()).Abs() + (model.GetColumn3(1) * halfExtent.GetY()).Abs() +
                                 (model.GetColumn3(2) * halfExtent.GetZ()).Abs();

        outMin = JPH::Vec3::sMin(outMin, center - extent);
        outMax = JPH::Vec3::sMax(outMax, center + extent);
    }

    if (prefab.parts.empty()) {
        outMin = JPH::Vec3(-1.0f, -1.0f, -1.0f);
        outMax = JPH::Vec3(1.0f, 1.0f, 1.0f);
    }
}

void ClearSubject(ZHLN::Engine& engine, Subject& subject) {
    auto& reg = engine.GetRegistry();
    for (const ZHLN::Entity entity: subject.instances) {
        if (entity != ZHLN::Entity::Null() && reg.IsAlive(entity)) {
            ZHLN::DespawnEntity(engine, entity);
        }
    }
    subject.instances.clear();
    subject.loaded = false;
}

// Imports bytes and spawns them. The prefab is cached under the name the file has
// on disk, so a second run and a device-lost rebuild both find the same entry (see
// GLTF::RebuildCachedPrefabs). GLTF::InstantiatePrefabFromMemory makes these two
// calls in one; they stay apart here because the prefab is what the bounds, the
// triangle count and the texture count come from.
auto ImportSubject(ZHLN::Engine& engine, Subject& subject, std::span<const uint8_t> bytes, std::string_view virtualPath) -> bool {
    ClearSubject(engine, subject);

    ZHLN::ModelPrefab* prefab = ZHLN::GLTF::LoadGLBPrefabFromMemory(engine.GetRenderContext(), engine.GetAssetManager(), bytes, virtualPath);
    if (prefab == nullptr) {
        ZHLN::Log("[RemoteGLB] '{}' is not a glTF this importer can read.", virtualPath);
        return false;
    }

    // A prefab emits one root, one entity per part and at most one emissive
    // virtual light per part, so the output buffer is sized for that and then
    // truncated to what was written.
    const uint32_t capacity = 1U + (static_cast<uint32_t>(prefab->parts.size()) * 2U);
    subject.instances.resize(capacity);
    const uint32_t written = ZHLN::PrefabFactory::InstantiatePrefab(
        engine, *prefab,
        ZHLN::PrefabFactory::SpawnParams {
            .position      = JPH::RVec3(0.0, 0.0, 0.0),
            .createPhysics = false,
            .isAnimated    = !prefab->animations.empty(),
        },
        subject.instances.data(), capacity
    );
    subject.instances.resize(std::min(written, capacity));

    ComputeBounds(*prefab, subject.boundsMin, subject.boundsMax);
    subject.center = (subject.boundsMin + subject.boundsMax) * 0.5f;
    subject.radius = std::max((subject.boundsMax - subject.boundsMin).Length() * 0.5f, 0.01f);

    // What the HUD reports: the triangles the parts draw, and the distinct texture
    // handles they bind. Distinct, because a handle is one GPU image and several
    // parts may share it -- the Damaged Helmet is a single part, but a multi-part
    // asset would otherwise be counted once per part.
    uint64_t                         triangles = 0;
    std::vector<ZHLN::TextureHandle> uniqueTextures;
    for (const ZHLN::ModelPart& part: prefab->parts) {
        triangles += (part.mesh.indexCount > 0) ? part.mesh.indexCount / 3U : part.mesh.vertexCount / 3U;
        const std::array slots {part.defaultMaterial.albedoMap, part.defaultMaterial.normalMap, part.defaultMaterial.pbrMap, part.defaultMaterial.emissiveMap};
        for (const ZHLN::TextureHandle handle: slots) {
            if (handle == ZHLN::TextureHandle::Invalid || std::ranges::find(uniqueTextures, handle) != uniqueTextures.end()) {
                continue;
            }
            uniqueTextures.push_back(handle);
        }
    }
    subject.triangles = static_cast<uint32_t>(triangles);
    subject.textures  = static_cast<uint32_t>(uniqueTextures.size());

    subject.prefab = prefab;
    subject.loaded = true;
    ZHLN::Log(
        "[RemoteGLB] Imported '{}': {} part(s), {} node(s), {} triangle(s), {} texture(s), centre ({:.3f}, {:.3f}, {:.3f}), radius {:.3f} m, {} instance(s).",
        virtualPath, prefab->parts.size(), prefab->nodes.size(), subject.triangles, subject.textures, subject.center.GetX(), subject.center.GetY(),
        subject.center.GetZ(), subject.radius, written
    );
    return true;
}

// ============================================================================
// THE STUDIO
// ============================================================================

struct Studio {
    ZHLN::Entity sun   = ZHLN::Entity::Null();
    ZHLN::Entity fill  = ZHLN::Entity::Null();
    ZHLN::Entity rim   = ZHLN::Entity::Null();
    ZHLN::Entity floor = ZHLN::Entity::Null();
    bool         floorOn = true;
};

[[nodiscard]] auto MakeLight(
    ZHLN::Engine&    engine,
    std::string_view name,
    ZHLN::LightType  type,
    JPH::Vec3Arg     position,
    JPH::Vec3Arg     color,
    float            intensity,
    float            radius,
    float            range,
    JPH::Vec3Arg     direction
) -> ZHLN::Entity {
    auto&            reg   = engine.GetRegistry();
    const JPH::Mat44 world = ZHLN::Math::CreateTransform(position, JPH::Quat::sIdentity());
    return reg.Create(
        ZHLN::Components::NameComponent {.name = ZHLN::String64(name)},
        ZHLN::Components::TransformComponent {.position = position, .rotation = JPH::Quat::sIdentity(), .scale = JPH::Vec3::sReplicate(1.0f)},
        ZHLN::Components::WorldTransformComponent {.world = world, .previous = world},
        ZHLN::Components::LightComponent {.type = type, .color = color, .intensity = intensity, .radius = radius, .direction = direction, .range = range}
    );
}

void ClearStudio(ZHLN::Engine& engine, Studio& studio) {
    auto& reg = engine.GetRegistry();
    for (const ZHLN::Entity entity: {studio.sun, studio.fill, studio.rim, studio.floor}) {
        if (entity != ZHLN::Entity::Null() && reg.IsAlive(entity)) {
            ZHLN::DespawnEntity(engine, entity);
        }
    }
    studio.sun   = ZHLN::Entity::Null();
    studio.fill  = ZHLN::Entity::Null();
    studio.rim   = ZHLN::Entity::Null();
    studio.floor = ZHLN::Entity::Null();
}

void SetFloorVisible(ZHLN::Engine& engine, const Studio& studio) {
    engine.GetRegistry().Patch<ZHLN::Components::MeshComponent>(studio.floor, [&studio](auto& mesh) -> auto {
        if (studio.floorOn) {
            mesh.flags &= ~ZHLN::DrawFlags::Hidden;
        } else {
            mesh.flags |= ZHLN::DrawFlags::Hidden;
        }
    });
}

// Key plus two fills plus a floor, laid out around the subject's bounds. The sun
// needs no subject and could be built earlier; keeping the whole rig in one
// function is what lets a device-lost recovery rebuild it in one call.
void BuildStudio(ZHLN::Engine& engine, Studio& studio, const Subject& subject) {
    ClearStudio(engine, studio);
    auto& reg = engine.GetRegistry();

    const float     radius = subject.radius;
    const JPH::Vec3 center = subject.center;

    studio.sun = MakeLight(engine, "StudioSun", ZHLN::LightType::Sun, center + JPH::Vec3(radius * 10.0f, radius * 20.0f, radius * 10.0f), kSunColor,
                           kSunIntensity, 0.0f, 0.0f, kSunDirection.Normalized());

    // Cool fill from the front-left, warm rim from behind-right: the two specular
    // highlights that separate a metal surface from a silhouette. Neither casts --
    // ShadowSettings::maxPunctualShadows is 0, so no cube map is rendered for them.
    studio.fill = MakeLight(engine, "StudioFill", ZHLN::LightType::Point, center + JPH::Vec3(-radius * 2.2f, radius * 1.6f, radius * 2.4f), kFillColor,
                            kFillIntensityPerM2 * radius * radius, radius * 0.15f, radius * 6.0f, JPH::Vec3::sZero());
    studio.rim  = MakeLight(engine, "StudioRim", ZHLN::LightType::Point, center + JPH::Vec3(radius * 2.6f, radius * 1.1f, -radius * 2.2f), kRimColor,
                            kRimIntensityPerM2 * radius * radius, radius * 0.15f, radius * 6.0f, JPH::Vec3::sZero());

    // The floor sits on the subject's lowest point, and is wide enough to catch a
    // reflection at any orbit angle without reaching the camera's far plane. Two
    // details of CreatePlane decide how it is called:
    //
    //   * basic.slang multiplies the vertex colour by the material's base colour
    //     factor, so the plane's colour argument and the override's would compound
    //     into each other's square. The tint goes in the material and the mesh
    //     gets white.
    //   * its own roughness and metallic are hardcoded (0.35 / 0.15) unless a
    //     material override arrives, which is what a finish this glossy needs.
    ZHLN::MaterialDesc floorDesc;
    floorDesc.metallic  = 0.0f;
    floorDesc.roughness = kFloorRoughness;
    floorDesc.baseColor = {kFloorColor.GetX(), kFloorColor.GetY(), kFloorColor.GetZ(), kFloorColor.GetW()};
    const ZHLN::Material floorMaterial = engine.GetRenderContext().CreateMaterial(floorDesc).value_or(ZHLN::Material {});

    studio.floor = ZHLN::PrefabFactory::CreatePlane(
        engine, radius * kFloorExtent, JPH::Vec4(1.0f, 1.0f, 1.0f, 1.0f),
        ZHLN::PrefabFactory::SpawnParams {
            .position         = JPH::RVec3(center.GetX(), subject.boundsMin.GetY(), center.GetZ()),
            .createPhysics    = false,
            .materialOverride = floorMaterial,
        }
    );
    reg.Assign<ZHLN::Components::NameComponent>(studio.floor, "StudioFloor");
    SetFloorVisible(engine, studio);
}

// ============================================================================
// RENDER SETTINGS
// ============================================================================

// The studio look, in the canonical model (include/Zahlen/GraphicsSettings.hpp).
//
// The signature fields -- AA mode and feedback, shadow resolution, GI samples,
// SSR/RTR and the ray-tracing budget -- come from a preset rather than being
// listed here, so the tier stays internally coherent and the frame loop's 1-4
// keys are the same write with a different argument.
//
// Ray tracing is off unless it is asked for, and the reason is a specific one:
// with enableRTR set, lighting.slang takes the sun shadow from a ray below 80 m of
// view depth and only blends back to the cascade maps beyond that. A turntable
// subject always lives inside 80 m, so the switch trades the cascades' analytic
// PCSS penumbra for a two-sample ray behind an A-Trous denoise. On a hero asset
// that is the worse default, which is why the screen-space reflection pass is the
// one left running.
[[nodiscard]] auto MakeStudioSettings(float subjectRadius, bool rayTraced) -> ZHLN::GraphicsSettings {
    ZHLN::GraphicsSettings gfx {};
    gfx.ApplyPreset(ZHLN::QualityLevel::Ultra);

    // --- sharpness vs noise tradeoff:
    // Ultra defaults: TAA 0.95 (95% history) + 16 GI samples + 3 atrous passes
    // = oil painting (albedo smeared). Previous fix 0.88 + 1 pass = sharp but
    // RTR half-res 1 SPP VNDF sparkles like water.
    // New: TAA 0.90 (more history than 0.88, less than 0.95) + 2 atrous passes
    // cleans RT grain without smearing albedo too much. Floor roughness 0.03
    // mirror tier (full-res warp-coherent ray) for clean reflection.
    // Use SMAA for studio look: spatial only, no temporal smear, sharpest
    // textures. TAA 0.95 is oil painting, even 0.90 still accumulates history.
    // User reported Low (FXAA) looks best — SMAA is sharper than FXAA and
    // still has edge AA. Keep TAA as option via quality tiers 2-4.
    gfx.antiAliasing.mode        = ZHLN::AAMode::SMAA;
    gfx.antiAliasing.taaFeedback = 0.90f;
    gfx.post.giSamples           = 8;

    gfx.rayTracing.denoiserPasses    = 1; // was 2 — 2 still blurred albedo, 1 is enough with mirror floor
    gfx.rayTracing.reflectionSamples = 1;
    gfx.rayTracing.shadowSamples     = 1;
    gfx.rayTracing.maxBounces        = 1;

    // Reflections: SSR always, RTR reflections for the floor. RT shadows off
    // by default to keep the cascade PCSS penumbra (analytic, not noisy).
    // ZHLN_REMOTE_GLB_RTR=1 enables full RT (shadows + reflections) via env.
    gfx.post.enableSSR               = 1;
    gfx.post.enableRTR               = 1;
    gfx.rayTracing.enableReflections = true;
    gfx.rayTracing.enableShadows     = rayTraced;

    if (!rayTraced) {
        gfx.rayTracing.enableShadows = false;
    } else {
        gfx.rayTracing.enableShadows = true;
    }

    // --- grade: blit.slang does hdr * exposure, ACES, then filter/contrast/saturation
    gfx.post.exposure      = kExposure;
    gfx.post.tonemapper    = kTonemapperACES;
    gfx.post.bloomStrength = kBloomStrength;
    gfx.post.glowIntensity = kGlowIntensity;
    gfx.post.contrast      = kContrast;
    gfx.post.saturation    = kSaturation;
    gfx.post.colorFilter   = {1.0f, 1.0f, 1.0f};

    gfx.post.vignetteIntensity = kVignette;
    gfx.post.vignettePower     = kVignettePower;

    // --- ambient occlusion: GTAO in its own half-resolution pass (giMode 3),
    // depth-weighted upsampled by the lighting pass. Its radius is the one
    // subject-relative field here: an occlusion search wider than the model's own
    // features darkens the whole silhouette instead of the crevices.
    gfx.post.mode        = 3;
    gfx.post.aoRadius    = std::clamp(subjectRadius * 0.5f, 0.02f, 4.0f);
    gfx.post.aoBias      = 0.02f;
    gfx.post.aoPower     = 1.5f;
    gfx.post.giIntensity = 1.0f;

    // --- environment: the gradient the reflection pass paints where a ray leaves
    // the scene, and the backdrop behind the subject. ambientExposure scales both
    // it and the baked SH fill.
    gfx.environment.ambientExposure = kAmbientExposure;
    gfx.environment.skyZenith       = {kSkyZenith.GetX(), kSkyZenith.GetY(), kSkyZenith.GetZ(), kSkyZenith.GetW()};
    gfx.environment.skyHorizon      = {kSkyHorizon.GetX(), kSkyHorizon.GetY(), kSkyHorizon.GetZ(), kSkyHorizon.GetW()};
    gfx.environment.skyGround       = {kSkyGround.GetX(), kSkyGround.GetY(), kSkyGround.GetZ(), kSkyGround.GetW()};
    gfx.environment.useLocalProbe   = 0;
    gfx.environment.fullBright      = 0;

    // --- shadows: four cascades fitted to slices of the camera frustum, which is
    // why the near and far planes in ApplyOrbit matter as much as the resolution
    // here. `width` is the ortho box the caster culling uses, centred on the
    // camera; UpdateShadowExtent keeps it around the subject as the viewer zooms.
    gfx.shadows.resolution         = 4096;
    gfx.shadows.sunSize            = 0.035f;
    gfx.shadows.maxPunctualShadows = 0;
    gfx.shadows.width              = std::clamp(subjectRadius * 16.0f, 4.0f, 64.0f);

    return gfx;
}

// Writes a GraphicsSettings back into the ECS editing surface the renderer
// collects from every frame: post/GI/environment on the global-settings entity,
// shadows and ray tracing beside it, AA on the main camera. Core's
// ApplyQualityPreset performs the same write-back but lives in src/engine/system,
// so the sample writes the components itself -- which is also the honest way to
// show what a preset is.
//
// Registry::Add replaces an existing instance, so the settings components are
// built whole from the model. AA is the exception: CameraSystem advances
// jitterX/Y, prevJitterX/Y and frameIndex inside that component every frame, and
// a tier change must not throw the temporal history away, so only the knobs are
// written there.
void ApplyGraphicsSettings(ZHLN::Engine& engine, const ZHLN::GraphicsSettings& gfx) {
    auto&              reg      = engine.GetRegistry();
    const ZHLN::Entity settings = reg.SingletonEntity<ZHLN::Components::GlobalSettingsTagComponent>();
    if (settings == ZHLN::Entity::Null()) {
        ZHLN::Log("[RemoteGLB] No global-settings entity; the render settings have nowhere to go.");
        return;
    }

    reg.Add(
        settings,
        ZHLN::Components::PostProcessSettingsComponent {
            .giMode            = gfx.post.mode,
            .aoRadius          = gfx.post.aoRadius,
            .aoBias            = gfx.post.aoBias,
            .aoPower           = gfx.post.aoPower,
            .giIntensity       = gfx.post.giIntensity,
            .giSamples         = gfx.post.giSamples,
            .useLocalProbe     = gfx.environment.useLocalProbe,
            .vignetteIntensity = gfx.post.vignetteIntensity,
            .vignettePower     = gfx.post.vignettePower,
            .glowIntensity     = gfx.post.glowIntensity,
            .enableSSR         = gfx.post.enableSSR,
            .enableRTR         = gfx.post.enableRTR,
            .fullBright        = gfx.environment.fullBright,
            .exposure          = gfx.post.exposure,
            .bloomStrength     = gfx.post.bloomStrength,
            .contrast          = gfx.post.contrast,
            .saturation        = gfx.post.saturation,
            .tonemapper        = gfx.post.tonemapper,
            .colorFilter       = JPH::Vec3(gfx.post.colorFilter[0], gfx.post.colorFilter[1], gfx.post.colorFilter[2]),
            .ambientExposure   = gfx.environment.ambientExposure,
            .probeMin          = JPH::Vec3(gfx.environment.probeMin[0], gfx.environment.probeMin[1], gfx.environment.probeMin[2]),
            .probeMax          = JPH::Vec3(gfx.environment.probeMax[0], gfx.environment.probeMax[1], gfx.environment.probeMax[2]),
            .probePos          = JPH::Vec3(gfx.environment.probePos[0], gfx.environment.probePos[1], gfx.environment.probePos[2]),
            .skyZenith         = JPH::Vec4(gfx.environment.skyZenith[0], gfx.environment.skyZenith[1], gfx.environment.skyZenith[2],
                                           gfx.environment.skyZenith[3]),
            .skyHorizon        = JPH::Vec4(gfx.environment.skyHorizon[0], gfx.environment.skyHorizon[1], gfx.environment.skyHorizon[2],
                                           gfx.environment.skyHorizon[3]),
            .skyGround         = JPH::Vec4(gfx.environment.skyGround[0], gfx.environment.skyGround[1], gfx.environment.skyGround[2],
                                           gfx.environment.skyGround[3]),
        }
    );
    reg.Add(
        settings,
        ZHLN::Components::ShadowSettingsComponent {
            .shadowWidth        = gfx.shadows.width,
            .shadowResolution   = static_cast<int>(gfx.shadows.resolution),
            .maxPunctualShadows = static_cast<int>(gfx.shadows.maxPunctualShadows),
            .sunSize            = gfx.shadows.sunSize,
        }
    );
    reg.Add(settings, ZHLN::Components::RayTracingSettingsComponent {.config = gfx.rayTracing});

    // AA lives on the main camera in the default scene, which is where the
    // collector looks for it first.
    const ZHLN::Entity camera = reg.SingletonEntity<ZHLN::Components::MainCameraTagComponent>();
    if (camera == ZHLN::Entity::Null()) {
        return;
    }
    if (!reg.Patch<ZHLN::Components::AASettingsComponent>(camera, [&gfx](auto& aa) -> auto {
            aa.state.mode                  = gfx.antiAliasing.mode;
            aa.state.taaFeedback           = gfx.antiAliasing.taaFeedback;
            aa.state.fxaaSubpix            = gfx.antiAliasing.fxaaSubpix;
            aa.state.fxaaEdgeThreshold     = gfx.antiAliasing.fxaaEdgeThreshold;
            aa.state.fxaaEdgeThresholdMin  = gfx.antiAliasing.fxaaEdgeThresholdMin;
            aa.state.mlaaThreshold         = gfx.antiAliasing.mlaaThreshold;
            aa.state.mlaaMaxSearchSteps    = gfx.antiAliasing.mlaaMaxSearchSteps;
        })) {
        reg.Add(camera, ZHLN::Components::AASettingsComponent {.state = gfx.antiAliasing});
    }
}

// The caster-culling ortho box is centred on the camera, so it has to grow with
// the orbit or the subject leaves it and stops casting. Written only when it moves
// by more than 2%: the collector reads the component every frame anyway, and a
// value that changed with every pixel of wheel travel would be noise.
void UpdateShadowExtent(ZHLN::Engine& engine, float orbitDistance, float subjectRadius) {
    const float        wanted   = std::clamp(2.0f * (orbitDistance + (subjectRadius * kFloorExtent)), 4.0f, 400.0f);
    auto&              reg      = engine.GetRegistry();
    const ZHLN::Entity settings = reg.SingletonEntity<ZHLN::Components::GlobalSettingsTagComponent>();
    reg.Patch<ZHLN::Components::ShadowSettingsComponent>(settings, [wanted](auto& shadow) -> auto {
        if (std::abs(shadow.shadowWidth - wanted) > (wanted * 0.02f)) {
            shadow.shadowWidth = wanted;
        }
    });
}

// QualityLevel and AAMode log as integers -- GraphicsSettingsSync names the tiers
// by hand for the same reason -- so the HUD spells them out.
[[nodiscard]] constexpr auto QualityName(ZHLN::QualityLevel level) noexcept -> std::string_view {
    switch (level) {
        case ZHLN::QualityLevel::Low: return "Low";
        case ZHLN::QualityLevel::Medium: return "Medium";
        case ZHLN::QualityLevel::High: return "High";
        case ZHLN::QualityLevel::Ultra: return "Ultra";
        case ZHLN::QualityLevel::Custom: return "Custom";
    }
    return "Unknown";
}

[[nodiscard]] constexpr auto AAName(ZHLN::AAMode mode) noexcept -> std::string_view {
    switch (mode) {
        case ZHLN::AAMode::None: return "off";
        case ZHLN::AAMode::FXAA: return "FXAA";
        case ZHLN::AAMode::MLAA: return "MLAA";
        case ZHLN::AAMode::TAA: return "TAA";
        case ZHLN::AAMode::SMAA: return "SMAA";
    }
    return "unknown";
}

// ============================================================================
// THE TURNTABLE
// ============================================================================

struct OrbitCamera {
    JPH::Vec3 target         = JPH::Vec3::sZero();
    JPH::Vec3 wantedTarget   = JPH::Vec3::sZero();
    float     distance       = 3.0f;
    float     wantedDistance = 3.0f;
    float     yaw            = -90.0f;
    float     pitch          = -10.0f;
    float     fov            = 45.0f;
    float     minDistance    = 0.05f;
    float     maxDistance    = 500.0f;
};

// The camera's own convention (Camera::GetViewMatrix): yaw and pitch build the
// view direction, and the eye sits that far back along it from the target.
[[nodiscard]] auto OrbitDirection(float yawDegrees, float pitchDegrees) noexcept -> JPH::Vec3 {
    const float yaw   = JPH::DegreesToRadians(yawDegrees);
    const float pitch = JPH::DegreesToRadians(pitchDegrees);
    return JPH::Vec3(std::cos(yaw) * std::cos(pitch), std::sin(pitch), std::sin(yaw) * std::cos(pitch));
}

// Puts the subject in frame: the bounding sphere at the vertical half-angle, with
// the zoom clamps derived from the same radius.
void FrameSubject(OrbitCamera& orbit, const Subject& subject) {
    const float distance = (subject.radius / std::tan(JPH::DegreesToRadians(orbit.fov) * 0.5f)) * kFrameMargin;
    orbit.target         = subject.center;
    orbit.wantedTarget   = subject.center;
    orbit.distance       = distance;
    orbit.wantedDistance = distance;
    orbit.minDistance    = std::max(subject.radius * 0.15f, 0.02f);
    orbit.maxDistance    = std::max(subject.radius * 60.0f, 100.0f);
}

void UpdateOrbit(OrbitCamera& orbit, const ZHLN::Components::InputStateComponent& input, float dt) {
    const bool  lmb = input.IsMouseButtonDownRaw(static_cast<uint8_t>(ZHLN::KeyCode::LButton));
    const bool  rmb = input.IsMouseButtonDownRaw(static_cast<uint8_t>(ZHLN::KeyCode::RButton));
    const bool  mmb = input.IsMouseButtonDownRaw(static_cast<uint8_t>(ZHLN::KeyCode::MButton));
    const float dx  = input.mouseDeltaX;
    const float dy  = input.mouseDeltaY;

    if (lmb || mmb) {
        orbit.yaw += dx * kOrbitSpeed;
        orbit.pitch = std::clamp(orbit.pitch - (dy * kOrbitSpeed), -89.0f, 89.0f);
    } else if (rmb) {
        // Pan in the view plane, grab-the-world way round: the subject follows the
        // cursor instead of running away from it. Right and up come from the view
        // direction the way extras/Camera's target rig derives them.
        const JPH::Vec3 forward = OrbitDirection(orbit.yaw, orbit.pitch).Normalized();
        JPH::Vec3       right   = forward.Cross(JPH::Vec3::sAxisY());
        if (right.LengthSq() > 1e-6f) {
            right = right.Normalized();
        }
        const JPH::Vec3 up    = right.Cross(forward).Normalized();
        const float     scale = orbit.distance * kPanSpeed;
        orbit.wantedTarget += ((right * -dx) + (up * dy)) * scale;
    }

    if (input.mouseWheel != 0.0f) {
        orbit.wantedDistance *= std::exp(-input.mouseWheel * kZoomSpeed);
        orbit.wantedDistance = std::clamp(orbit.wantedDistance, orbit.minDistance, orbit.maxDistance);
    }

    // Pan and zoom are discrete jumps -- one wheel notch, one frame of drag -- and
    // a turntable reads better when they land over about a tenth of a second
    // instead of in one frame. Orbit is already continuous in the mouse delta, so
    // it is deliberately not smoothed: smoothing it would put the subject behind
    // the cursor.
    const float follow = 1.0f - std::exp(-dt * kFollowRate);
    orbit.distance += (orbit.wantedDistance - orbit.distance) * follow;
    orbit.target   += (orbit.wantedTarget - orbit.target) * follow;
}

void ApplyOrbit(ZHLN::Engine& engine, const OrbitCamera& orbit, const Subject& subject) {
    ZHLN::Camera& camera = engine.GetCamera();
    camera.position      = orbit.target - (OrbitDirection(orbit.yaw, orbit.pitch) * orbit.distance);
    camera.yaw           = orbit.yaw;
    camera.pitch         = orbit.pitch;
    camera.fov           = orbit.fov;

    // The cascade splits are fractions of [near, far], so a far plane that reaches
    // the horizon puts the subject in a cascade fitted to kilometres. Keeping it
    // just past the floor is what buys the shadow its texel density, and a near
    // plane scaled to the orbit keeps the depth range narrow enough to resolve a
    // millimetre of it.
    camera.nearZ = std::clamp(orbit.distance * kNearPlaneScale, 0.01f, 0.5f);
    camera.farZ  = std::clamp((orbit.distance * 12.0f) + (subject.radius * 24.0f), 20.0f, 2000.0f);
}

// ============================================================================
// SAMPLE STATE
// ============================================================================

struct SampleState {
    RemoteAsset            asset;
    Subject                subject;
    Studio                 studio;
    OrbitCamera            orbit;
    ZHLN::GraphicsSettings settings;

    bool     rayTraced   = false;
    bool     reported    = false; // this download's outcome has been logged and imported
    uint32_t frameBudget = 0;
    double   now         = 0.0;
    float    dt          = 0.016666f;
    float    fps         = 0.0f;

    std::array<bool, 256> keyWasDown {};
};

// Edge detection for the handful of keys the sample owns: the level is in the
// input singleton, the previous frame's level is here.
[[nodiscard]] auto KeyPressed(SampleState& state, const ZHLN::Components::InputStateComponent& input, ZHLN::KeyCode key) -> bool {
    const uint8_t code = static_cast<uint8_t>(key);
    const bool    down = input.IsKeyDownRaw(code);
    const bool    was  = state.keyWasDown[code];
    state.keyWasDown[code] = down;
    return down && !was;
}

// The studio is built around the subject's bounds, so it waits for the import;
// until then the backdrop and the HUD are the whole scene.
void RebuildLook(ZHLN::Engine& engine, SampleState& state) {
    state.settings = MakeStudioSettings(state.subject.radius, state.rayTraced);
    ApplyGraphicsSettings(engine, state.settings);
    BuildStudio(engine, state.studio, state.subject);
    FrameSubject(state.orbit, state.subject);
}

// Reads the phase the worker published and, exactly once per download, imports
// what arrived. All of it runs on the frame thread: the importer uploads GPU
// resources and writes the registry.
void PollFetch(ZHLN::Engine& engine, SampleState& state) {
    const FetchPhase phase = state.asset.phase.load(std::memory_order::acquire);
    if ((phase != FetchPhase::Succeeded) && (phase != FetchPhase::Failed)) {
        return;
    }
    if (state.reported) {
        return;
    }
    state.reported = true;

    const double elapsed = state.now - state.asset.startedAt;
    if (phase == FetchPhase::Failed) {
        state.asset.bytes.clear();
        ZHLN::Log("[RemoteGLB] Fetch failed after {:.1f}s: {}", elapsed, state.asset.detail);
        ZHLN::Log("[RemoteGLB] Check the URL and the network, then press R to try again.");
        return;
    }

    if (!state.asset.detail.empty()) {
        ZHLN::Log("[RemoteGLB] {}", state.asset.detail);
    }
    ZHLN::Log(
        "[RemoteGLB] {} {} KiB in {:.2f}s: {}", state.asset.usedCache ? "Cache hit," : "Downloaded", state.asset.bytes.size() / 1024U, elapsed,
        state.asset.source
    );

    const std::string virtualPath = state.asset.cacheFile.filename().string();
    if (ImportSubject(engine, state.subject, std::span<const uint8_t>(state.asset.bytes), virtualPath)) {
        RebuildLook(engine, state);
        // The bytes are in the prefab cache and on disk now; keeping the vector
        // would be a third copy of the same file.
        state.asset.bytes.clear();
        state.asset.bytes.shrink_to_fit();
    }
}

void HandleInput(ZHLN::Engine& engine, SampleState& state) {
    auto&              reg      = engine.GetRegistry();
    const auto         entities = reg.GetEntitiesWith<ZHLN::Components::InputStateComponent>();
    if (entities.empty()) {
        return;
    }
    const auto* input = reg.Get<ZHLN::Components::InputStateComponent>(entities[0]);
    if (input == nullptr) {
        return;
    }

    if (input->needsResize) {
        engine.GetRenderContext().SetResolution(input->newSize);
        reg.Patch<ZHLN::Components::InputStateComponent>(entities[0], [](auto& st) -> auto { st.needsResize = false; });
    }

    // The panel owns input inside its strip, the way the inspector's explorer
    // does: a drag that starts on the HUD is not a drag around the subject.
    const bool overPanel = (input->mouseX >= 0.0f) && (input->mouseX <= (kPanelWidth + kPanelMargin));
    if (!overPanel) {
        UpdateOrbit(state.orbit, *input, state.dt);
    }

    if (KeyPressed(state, *input, ZHLN::KeyCode::F) && state.subject.loaded) {
        FrameSubject(state.orbit, state.subject);
        ZHLN::Log("[RemoteGLB] Re-framed: {:.3f} m from ({:.2f}, {:.2f}, {:.2f}).", state.orbit.distance, state.subject.center.GetX(),
                  state.subject.center.GetY(), state.subject.center.GetZ());
    }
    if (KeyPressed(state, *input, ZHLN::KeyCode::G) && state.studio.floor != ZHLN::Entity::Null()) {
        state.studio.floorOn = !state.studio.floorOn;
        SetFloorVisible(engine, state.studio);
    }
    if (KeyPressed(state, *input, ZHLN::KeyCode::R)) {
        ZHLN::Log("[RemoteGLB] Re-downloading '{}', ignoring the cache.", state.asset.url);
        state.reported = false;
        StartFetch(state.asset, true, EnvironmentU32("ZHLN_REMOTE_GLB_TIMEOUT", kFetchTimeout), state.now);
    }

    // 0 is the hand-tuned studio look; 1-4 are the engine's quality tiers. Both go
    // through the same write-back, and only the signature fields differ: the
    // exposure, the AO radius and the sky belong to the sample and stay put. A
    // hand-tuned look reports as Custom, which also keeps the fidelity governor --
    // it steps a late tier down and never touches Custom -- off these settings.
    struct TierKey {
        ZHLN::KeyCode      key;
        ZHLN::QualityLevel level;
    };
    for (const TierKey& tier: {TierKey {.key = ZHLN::KeyCode::Num0, .level = ZHLN::QualityLevel::Custom},
                               TierKey {.key = ZHLN::KeyCode::Num1, .level = ZHLN::QualityLevel::Low},
                               TierKey {.key = ZHLN::KeyCode::Num2, .level = ZHLN::QualityLevel::Medium},
                               TierKey {.key = ZHLN::KeyCode::Num3, .level = ZHLN::QualityLevel::High},
                               TierKey {.key = ZHLN::KeyCode::Num4, .level = ZHLN::QualityLevel::Ultra}}) {
        if (!KeyPressed(state, *input, tier.key)) {
            continue;
        }
        ZHLN::GraphicsSettings gfx = MakeStudioSettings(state.subject.radius, state.rayTraced);
        if (tier.level != ZHLN::QualityLevel::Custom) {
            gfx.ApplyPreset(tier.level);
            // Preset overwrote the sharpness tweaks — re-apply them. Ultra's
            // 3 atrous passes + 0.95 TAA feedback is oil-painting, 1 pass +
            // 0.88 feedback is sparkling water (1 SPP VNDF). Now: SMAA for
            // Custom (sharpest), FXAA for Low, TAA 0.90 + 1 denoise for others,
            // floor 0.03 mirror tier.
            if (tier.level != ZHLN::QualityLevel::Low) {
                gfx.antiAliasing.taaFeedback = 0.90f;
            }
            gfx.rayTracing.denoiserPasses    = std::min<uint32_t>(gfx.rayTracing.denoiserPasses, 1u);
            gfx.rayTracing.reflectionSamples = 1;
            gfx.rayTracing.shadowSamples     = 1;
            gfx.post.giSamples               = std::min<uint32_t>(gfx.post.giSamples, 8u);
            gfx.post.enableSSR               = 1; // floor reflection
            // Low stays FXAA/no-RTR for max sharpness (what user said looks best).
            // Medium+ get RT reflections for the floor, but RT shadows only
            // when ZHLN_REMOTE_GLB_RTR=1 to keep PCSS.
            if (tier.level == ZHLN::QualityLevel::Low) {
                gfx.post.enableRTR               = 0;
                gfx.rayTracing.enableReflections = false;
                gfx.rayTracing.enableShadows     = false;
            } else {
                gfx.post.enableRTR               = 1;
                gfx.rayTracing.enableReflections = true;
                gfx.rayTracing.enableShadows     = state.rayTraced;
            }
            // Keep studio grade even after preset — ApplyPreset doesn't touch
            // these, but state it explicitly for readability.
            gfx.post.exposure          = kExposure;
            gfx.post.bloomStrength     = kBloomStrength;
            gfx.post.glowIntensity     = kGlowIntensity;
            gfx.post.vignetteIntensity = kVignette;
            gfx.post.vignettePower     = kVignettePower;
        }
        state.settings = gfx;
        ApplyGraphicsSettings(engine, gfx);
        ZHLN::Log("[RemoteGLB] Quality: {} ({} shadow map, {} GI samples, SSR {}, RTR {}, TAA fb {:.2f}, denoise {}).", QualityName(gfx.DetectPreset()), gfx.shadows.resolution,
                  gfx.post.giSamples, gfx.post.enableSSR, gfx.post.enableRTR, gfx.antiAliasing.taaFeedback, gfx.rayTracing.denoiserPasses);
    }
}

// ============================================================================
// HUD
// ============================================================================

[[nodiscard]] auto StatusLine(const SampleState& state) -> std::string {
    switch (state.asset.phase.load(std::memory_order::acquire)) {
        case FetchPhase::Running:
            return std::format("downloading... {:.0f}s  ({})", state.now - state.asset.startedAt, state.asset.url);
        case FetchPhase::Failed:
            return std::format("fetch failed: {}  --  R retries", state.asset.detail);
        case FetchPhase::Succeeded:
            return state.reported ? std::format("{}: {}", state.asset.usedCache ? "cached" : "downloaded", state.asset.source) :
                                    std::string("importing...");
        case FetchPhase::Idle:
            break;
    }
    return "idle";
}

void DrawHUD(ZHLN::Engine& engine, const SampleState& state) {
    ZHLN::GUI::Context ui(engine);
    ui.BeginFrame(state.dt);

    const ZHLN::RenderInfo info    = engine.GetRenderContext().GetInfo();
    const std::string      subject = state.subject.loaded ?
                                     std::format("{} part(s), {} triangle(s), {} texture(s), radius {:.3f} m", state.subject.prefab->parts.size(),
                                                 state.subject.triangles, state.subject.textures, state.subject.radius) :
                                     std::string("no model yet");

    ui.Box(
        "RemoteGLBPanel",
        ZHLN::GUI::BoxConfig {
            .width        = {.fixed = kPanelWidth},
            .height       = {},
            .color        = {0.05f, 0.07f, 0.10f, 0.90f},
            .cornerRadius = {6.0f, 6.0f, 6.0f, 6.0f},
            .padding      = 14.0f,
            .gap          = 4.0f,
            .direction    = ZHLN::GUI::Direction::Column,
            .offsetX      = kPanelMargin,
            .offsetY      = kPanelMargin,
        },
        [&]() -> void {
            ui.Text("REMOTE glTF  /  DAMAGED HELMET", 16.0f, {0.30f, 0.85f, 1.00f, 1.0f});
            ui.Text(StatusLine(state), 12.0f, {0.80f, 0.86f, 0.94f, 1.0f});
            ui.Text(subject, 12.0f, {0.62f, 0.70f, 0.80f, 1.0f});
            ui.Text(
                std::format("{}  |  {}  |  quality {}  |  {}  |  AO mode {}  |  {:.0f} fps", info.gpuName, info.rayTracingSupported ? "RT capable" : "no RT",
                            QualityName(state.settings.DetectPreset()), AAName(state.settings.antiAliasing.mode), state.settings.post.mode, state.fps),
                11.0f, {0.50f, 0.57f, 0.67f, 1.0f}
            );
            ui.Text("LMB orbit   RMB pan   wheel zoom", 12.0f, {0.72f, 0.78f, 0.86f, 1.0f});
            ui.Text("F re-frame   G floor   R re-download   0 studio look   1-4 quality tiers", 11.0f, {0.45f, 0.51f, 0.60f, 1.0f});
        }
    );

    engine.SetPendingUIData(ui.EndFrame());
}

} // namespace

auto main(int argc, char* argv[]) -> int {
    auto optionsRes = ZHLN::HandleCommandLine(std::span(argv, static_cast<size_t>(argc)));
    if (!optionsRes) {
        return EXIT_FAILURE;
    }
    const auto& options = optionsRes.value();
    if (options.helpRequested || options.versionRequested) {
        return EXIT_SUCCESS;
    }

    ZHLN::SetLogLevel(options.logLevel);

    // Crash diagnostics keep their state in a caller-owned struct; static storage
    // duration is required because its address is copied into the signal handler
    // slots. See <Zahlen/Core/CrashState.hpp>.
    static ZHLN::CrashState crashState;
    ZHLN::SetupSignalHandler(crashState);
    ZHLN::TaskSystem::Init();

    auto engineRes = ZHLN::Engine::Create(
        {.physics = {.maxBodies = 1024, .maxBodyPairs = 2048, .maxContactConstraints = 2048},
         .render  = {
              .appName        = "Zahlen :: Remote glTF",
              .vsync          = options.vsync,
              .fullscreen     = options.fullscreen,
              .validationMode = options.validationMode,
              .headless       = options.headless,
          },
         .enableFallbackScene = false}
    );
    if (!engineRes) {
        ZHLN::Log("FATAL: Failed to initialize Engine: {}", engineRes.error());
        ZHLN::TaskSystem::Shutdown();
        return EXIT_FAILURE;
    }

    auto engine = std::move(engineRes.value());
    if (!options.headless) {
        engine->GetPlatformHost().Focus();
    }

#if defined(ZHLN_HAS_FONTS)
    // The composition root's default bake (see app/main.cpp): the vendored
    // JetBrains Mono NF, installed before the scene boots so the HUD's atlas is
    // that font and not core's embedded 8x8.
    if (auto fontID = ZHLN::Fonts::LoadFontAsset(*engine, ZHLN::Fonts::VendoredDefaultFontSource()); !fontID) {
        ZHLN::Log("WARNING: Font asset failed to load ({}), using embedded default.", fontID.error());
    }
#endif

    // An imported model holds GPU resources core cannot recreate, so the importer
    // subscribes its own device-lost rebuild. Once, next to the first import (see
    // GLTF::InstallDeviceLostHandler).
    ZHLN::GLTF::InstallDeviceLostHandler(*engine);

    engine->InitializeDefaultScene();

    SampleState state;
    state.rayTraced         = EnvironmentFlag("ZHLN_REMOTE_GLB_RTR");
    state.frameBudget       = EnvironmentU32("ZHLN_REMOTE_GLB_FRAMES", 0);
    state.asset.url         = EnvironmentString("ZHLN_REMOTE_GLB_URL", kDefaultAssetURL);
    state.asset.cacheFile   = CacheFileFor(state.asset.url);

    // The turntable owns the camera, so core's WASD free-cam has to come off the
    // camera entity or the two write the same transform every frame.
    {
        auto&              reg    = engine->GetRegistry();
        const ZHLN::Entity camera = reg.SingletonEntity<ZHLN::Components::MainCameraTagComponent>();
        if (camera != ZHLN::Entity::Null()) {
            reg.Remove<ZHLN::Components::FreeCamTagComponent>(camera);
        }
    }

    // A new VkDevice leaves every primitive this sample built behind with the old
    // one. The model rebuilds itself through the importer's subscription; the
    // studio and the settings are this callback's job.
    engine->AddDeviceLostCallback([&state](ZHLN::Engine& recovered) -> void {
        ZHLN::Log("[RemoteGLB] Device recovered; rebuilding the studio.");
        if (state.subject.loaded) {
            RebuildLook(recovered, state);
        } else {
            state.settings = MakeStudioSettings(state.subject.radius, state.rayTraced);
            ApplyGraphicsSettings(recovered, state.settings);
        }
    });

    // Settings first, so the backdrop and the grade are already right while the
    // download runs; the subject-relative fields are written again, with the
    // studio around them, once the bounds are known.
    state.settings = MakeStudioSettings(state.subject.radius, state.rayTraced);
    ApplyGraphicsSettings(*engine, state.settings);
    FrameSubject(state.orbit, state.subject);
    ApplyOrbit(*engine, state.orbit, state.subject);

    ZHLN::Clock clock;

    ZHLN::Log("[RemoteGLB] Asset: {}", state.asset.url);
    ZHLN::Log("[RemoteGLB] Cache: {}", state.asset.cacheFile.string());
    StartFetch(state.asset, EnvironmentFlag("ZHLN_REMOTE_GLB_REFRESH"), EnvironmentU32("ZHLN_REMOTE_GLB_TIMEOUT", kFetchTimeout), clock.GetTotalTime());

    engine->SetUICallback([&state](ZHLN::Engine& eng) -> void { DrawHUD(eng, state); });

    while (engine->IsRunning()) {
        state.dt  = std::min(clock.GetDeltaTime(), 0.05f);
        state.now = clock.GetTotalTime();
        state.fps = (state.fps * 0.90f) + ((state.dt > 0.0f ? 1.0f / state.dt : 0.0f) * 0.10f);

        engine->ProcessEvents();
        HandleInput(*engine, state);
        PollFetch(*engine, state);

        UpdateShadowExtent(*engine, state.orbit.distance, state.subject.radius);
        ApplyOrbit(*engine, state.orbit, state.subject);

        const auto status = engine->Tick(state.dt, ZHLN::GameplayDriver::Cpp);
        if (status == ZHLN::GameplayStatus::RequestQuit) {
            engine->GetPlatformHost().Close();
            break;
        }

        if (state.frameBudget != 0 && engine->GetCurrentFrame() >= state.frameBudget) {
            ZHLN::Log("[RemoteGLB] Frame budget of {} reached; exiting.", state.frameBudget);
            break;
        }
    }

    // Asks the worker to stop between candidates and joins it. A transfer already
    // inside libcurl runs to its own timeout, which is the one thing that can
    // delay shutdown here.
    state.asset.worker.request_stop();
    if (state.asset.worker.joinable()) {
        state.asset.worker.join();
    }

    ZHLN::TaskSystem::Shutdown();
    return EXIT_SUCCESS;
}
