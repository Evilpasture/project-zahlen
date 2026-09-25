// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// samples/RemoteGLBSample.cpp
//
// Remote assets, end to end: fetch .glb files over HTTP, cache them on disk,
// import them, and put them on a turntable.
//
// At start-up the sample does two things in parallel, each on its own worker
// while the backdrop and the HUD come up:
//
//   * It fetches the asset the default points at -- Khronos' Damaged Helmet
//     from glTF-Sample-Assets, the model every other glTF viewer measures
//     itself against, and a hard one: a single mesh carrying the full
//     metallic-roughness set (base colour, normal, packed
//     occlusion/roughness/metallic, emissive) behind a node transform that
//     stands it upright. It is fetched at run time and never committed, which
//     is the point: the subject of this sample is the fetch, not the file. It
//     stays the CC-BY-NC material of the Khronos authors, and the cache
//     directory it lands in is outside the repository.
//   * It crawls the repository's Models/ tree into the HUD's model dropdown.
//     The crawl is a single call to the GitHub git-trees API (the recursive
//     tree listing of one branch), parsed with extras/json, and every entry
//     that is a .glb under Models/ becomes one row. The crawl is a listing,
//     not a download: a model's bytes are fetched only when its row is picked
//     -- lazily -- and each one lands in the same disk cache as the default
//     asset, so a second run touches no network for anything already on disk.
//
// Three stages, each owned by the layer that knows about it:
//
//   1. extras/RemoteAsset does the fetch. ZHLN::Remote::AsyncAssetFetcher owns
//      the download workers and their whole lifecycle -- the start, the
//      supersede, the reap, the publish, the join at destruction -- over
//      extras/HTTP (synchronous on purpose: a worker calls it, the frame keeps
//      rendering, and the HUD counts the seconds). The disk cache is
//      ZHLN::Remote::DiskCache, under the engine's own cache directory
//      (ZHLN::FS::Paths::CacheDir() -- build/cache in a dev tree, the per-user
//      cache directory elsewhere, ZHLN_CACHE_DIR over both) as
//      http/<name>-<hash>.bin. A cached file has to pass its validator --
//      here the 12-byte GLB container check -- before it is trusted, so a
//      truncated download, or an HTML error page written by an earlier run, is
//      re-fetched instead of parsed.
//   2. extras/GitHub does the crawl. ZHLN::GitHub::FetchTree is the single
//      git-trees API call and the JSON parse; this file only decides which
//      entries are rows (blobs under Models/ that are .glb) and what a row is
//      called.
//   3. extras/glTF imports the bytes straight from memory
//      (GLTF::LoadGLBPrefabFromMemory, the call the inspector's drop handler
//      makes) and PrefabFactory::InstantiatePrefab spawns the parts.
//
// Picking a model that is already downloaded is a disk read, not a transfer:
// the cache check stays on the frame thread, and only the transfer gets its
// own worker. Picking another model while one is downloading retires the old
// transfer (its worker stops at the next candidate, though the cache file it
// finishes is kept) and starts the new one; each request's result lives in its
// own slot keyed by the id Request returned, so a superseded transfer can
// never land in the new request's state.
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
//   ZHLN_REMOTE_GLB_URL=<url>     another .glb to fetch first. Self-contained
//                                 only: the in-memory importer cannot chase
//                                 external buffer or image URIs.
//   ZHLN_REMOTE_GLB_REFRESH=1     ignore the cache and fetch again
//   ZHLN_REMOTE_GLB_RTR=1         ray-traced reflections and sun shadow
//                                 (default off; MakeStudioSettings says why)
//   ZHLN_REMOTE_GLB_TIMEOUT=<s>   transfer budget, default 60
//   ZHLN_REMOTE_GLB_FRAMES=<n>    exit after n frames -- a headless smoke run
//   ZHLN_REMOTE_GLB_NO_CATALOG=1  skip the model-list crawl
//   ZHLN_REMOTE_GLB_REPO=<repo>   repository to crawl (default
//                                 KhronosGroup/glTF-Sample-Assets)
//   ZHLN_REMOTE_GLB_BRANCH=<b>    branch to crawl (default main)
//   GITHUB_TOKEN                  sent as a bearer on the crawl call; the
//                                 unauthenticated API budget is 60 calls/hour
//
// Keys: 1-4 quality tiers, 0 back to the hand-tuned studio look, F re-frame,
// G hide/show the floor, H hide/show the subject, S toggle screen-space
// reflections, R re-download, C re-crawl the model list.

#include <Zahlen/Camera.hpp>
#include <Zahlen/Clock.hpp>
#include <Zahlen/CommandLine.hpp>
#include <Zahlen/Components.hpp>
#include <Zahlen/PrefabFactory.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Entity.hpp>
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

// The extras this sample is about. All are optional targets -- no libcurl, no
// zahlen_http, zahlen_remote_asset or zahlen_github; no extras, no
// zahlen_gltf -- and samples/CMakeLists.txt skips a sample whose extras were
// not built, so none of the includes needs a guard here. The fetch machinery
// itself (URL rewrites, the disk cache, the worker lifecycle) is
// extras/RemoteAsset, and the git trees crawl is extras/GitHub; this file
// keeps only the showroom policy around them -- which rows are interesting,
// what a row is called, and the turntable the model spins on.
#include <GitHub/GitHub.hpp>
#include <RemoteAsset/AsyncAssetFetcher.hpp>
#include <RemoteAsset/DiskCache.hpp>
#include <RemoteAsset/URLResolver.hpp>
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
#include <format>
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
// it. ZHLN::Remote::ResolveURL rewrites one into the other, so the URL below is
// the URL to paste rather than a second spelling to maintain.
inline constexpr std::string_view kDefaultAssetURL =
    "https://github.com/KhronosGroup/glTF-Sample-Assets/blob/main/Models/DamagedHelmet/glTF-Binary/DamagedHelmet.glb";

// The catalog: which repository the dropdown crawls. The tree listing is one
// call to the git trees API with recursive=1 -- the unauthenticated API budget
// is 60 calls/hour, and the crawl is exactly one call -- while the bytes
// themselves come from raw.githubusercontent.com, which is not API-metered.
// Both halves live in extras/GitHub; the prefix below is this sample's filter:
// the rows it wants are the .glb files under Models/.
inline constexpr std::string_view kDefaultRepo    = "KhronosGroup/glTF-Sample-Assets";
inline constexpr std::string_view kDefaultBranch  = "main";
inline constexpr std::string_view kModelsPrefix   = "Models/";

inline constexpr uint32_t kFetchTimeout   = 60;
inline constexpr size_t   kMaxDetailChars = 400;

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
inline constexpr float kPanelWidth   = 460.0f;
inline constexpr float kPanelMargin  = 16.0f;
inline constexpr float kPanelPadding = 14.0f;
// The dropdown's row is [label][field], and the orbit gate above is an x-only
// test, so the field must finish before the panel's right edge or a drag that
// starts on the field (or on the list floating under it, which shares its x)
// would orbit the subject. The 60 px is the label's advance at 15 px plus the
// row's gap, with room to spare.
inline constexpr float kDropdownLabelSpace = 60.0f;
inline constexpr float kFieldWidth         = kPanelWidth - (2.0f * kPanelPadding) - kDropdownLabelSpace;

// ============================================================================
// ENVIRONMENT
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

// ============================================================================
// THE CATALOG
// ============================================================================

// One row of the model dropdown: where the model's bytes live and what to call
// it. `path` is repository-relative and "" for the synthetic row a URL that is
// not in the crawled repository gets; `url` is what gets fetched when the row
// is picked; `label` is what the dropdown draws, size included.
struct GLBEntry {
    std::string path;
    std::string url;
    std::string label;
    uint64_t    sizeBytes = 0;
};

enum class CatalogPhase : uint8_t { Idle, Loading, Ready, Failed };

// The crawl's own state. The worker fills `entries`, `count` and `detail` and
// stores the phase with release; the frame thread reads them only after an
// acquire load reports Ready or Failed, and the frame never starts a crawl
// while one is running (phase is Loading), so one release/acquire pair is the
// whole synchronisation. The frame's display copy of the list is SampleState's,
// not here: DrawHUD reads only that, so a re-crawl can replace `entries`
// underneath without the worker and the frame ever sharing a vector.
struct Catalog {
    std::string               repo;
    std::string               branch;
    std::vector<GLBEntry>     entries;
    std::string               detail;
    uint32_t                  count   = 0;
    std::atomic<CatalogPhase> phase {CatalogPhase::Idle};
    std::jthread              worker;
};

[[nodiscard]] auto FormatSize(uint64_t bytes) -> std::string {
    if (bytes >= (1024ULL * 1024ULL)) {
        return std::format("{:.1f} MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    }
    if (bytes >= 1024ULL) {
        return std::format("{:.0f} KB", static_cast<double>(bytes) / 1024.0);
    }
    return std::format("{} B", bytes);
}

// The model folder a path under Models/ belongs to: the first segment after
// the prefix.
[[nodiscard]] auto ModelFolder(std::string_view path) -> std::string {
    const size_t start = kModelsPrefix.size();
    const size_t slash = path.find('/', start);
    return (slash == std::string_view::npos) ? std::string(path.substr(start)) : std::string(path.substr(start, slash - start));
}

// The label a row gets: the model folder, unless the folder ships more than
// one .glb (ABeautifulGame has a Draco/KTX sibling, CarConcept a second cut),
// in which case the path under Models/ with only the .glb dropped is what
// separates the rows -- subfolder names alone could collide across models. The
// size rides on the label: the point of the list is that a pick happens before
// a download, so the cost of the pick is visible on the pick.
void BuildCatalogLabels(std::vector<GLBEntry>& entries) {
    std::vector<std::string> folders(entries.size());
    for (size_t i = 0; i < entries.size(); ++i) {
        folders[i] = ModelFolder(entries[i].path);
    }
    for (size_t i = 0; i < entries.size(); ++i) {
        uint32_t siblings = 0;
        for (size_t j = 0; j < entries.size(); ++j) {
            if (folders[j] == folders[i]) {
                ++siblings;
            }
        }
        std::string name;
        if (siblings == 1) {
            name = folders[i];
        } else {
            // The path under Models/ without the .glb:
            // "ABeautifulGame/glTF-Binary-KTX-ETC1S-Draco/ABeautifulGame". Built
            // as a string, not a view: string::substr hands back a temporary a
            // view would dangle off.
            name = entries[i].path.substr(kModelsPrefix.size(), entries[i].path.size() - kModelsPrefix.size() - 4U);
        }
        entries[i].label = entries[i].sizeBytes > 0 ? std::format("{} ({})", name, FormatSize(entries[i].sizeBytes)) : name;
    }
}

// The head of a body for a failure line: the first @p n characters, with
// anything that is not printable ASCII -- a newline, a NUL, high-bit UTF-8 --
// replaced by a space, so the line stays one line and one message.
// The crawl: one API call, one JSON parse, a filter. The call and the parse
// live in extras/GitHub -- ZHLN::GitHub::FetchTree is the whole mechanism,
// one transfer, one listing, one failure line -- and this worker is the
// sample's policy on top of it: which entries are rows, and what the rows
// are called. It runs on the catalog's own worker, in parallel with the
// first asset's download, because the frame should not wait for either.
void StartCatalogCrawl(Catalog& catalog, uint32_t timeoutSeconds) {
    catalog.detail.clear();
    catalog.phase.store(CatalogPhase::Loading, std::memory_order::release);

    // Unauthenticated the API budget is 60 calls/hour per address; a token is
    // 5000. One crawl is one call either way, but a flaky retry loop is not.
    const std::string token = EnvironmentString("GITHUB_TOKEN", "");

    catalog.worker = std::jthread([catalog = &catalog, token, timeoutSeconds](std::stop_token) -> void {
        const auto note = [catalog](std::string text) -> void {
            if (catalog->detail.size() >= kMaxDetailChars) {
                return;
            }
            if (!catalog->detail.empty()) {
                catalog->detail += "; ";
            }
            catalog->detail += std::move(text);
        };
        const auto fail = [catalog, &note](std::string text) -> void {
            note(std::move(text));
            catalog->phase.store(CatalogPhase::Failed, std::memory_order::release);
        };

        const ZHLN::GitHub::TreeResponse listing = ZHLN::GitHub::FetchTree(catalog->repo, catalog->branch, timeoutSeconds, token);
        if (!listing.ok) {
            fail(std::move(listing.detail));
            return;
        }
        if (listing.truncated) {
            note(listing.detail);
        }

        // The rows this sample wants: the blobs under Models/ that are .glb.
        // Each row's bytes come from the raw host, which is not API-metered --
        // the extra's RawURL is the spelling, and the path is the listing's
        // own, so no encoding is needed.
        std::vector<GLBEntry> found;
        for (const ZHLN::GitHub::TreeEntry& entry: listing.entries) {
            if ((entry.type != "blob") || !entry.path.starts_with(kModelsPrefix) || !entry.path.ends_with(".glb")) {
                continue;
            }
            GLBEntry row;
            row.path      = entry.path;
            row.url       = ZHLN::GitHub::RawURL(catalog->repo, catalog->branch, entry.path);
            row.sizeBytes = entry.sizeBytes;
            found.push_back(std::move(row));
        }

        // The listing already comes in repository order; sort anyway, because
        // a dropdown is for choosing and choosing wants a stable order.
        std::stable_sort(found.begin(), found.end(), [](const GLBEntry& a, const GLBEntry& b) -> bool { return a.path < b.path; });
        BuildCatalogLabels(found);
        if (found.empty() && catalog->detail.empty()) {
            note("no .glb under Models/");
        }

        // Read the size before the move: a moved-from vector is empty, which
        // would file the listing as failed with no detail.
        const uint32_t count = static_cast<uint32_t>(found.size());
        catalog->entries     = std::move(found);
        catalog->count       = count;
        catalog->phase.store(count > 0 ? CatalogPhase::Ready : CatalogPhase::Failed, std::memory_order::release);
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
//
// The load happens before the clear: a pick that does not import -- a
// Draco-compressed asset this importer cannot decode, a binary it cannot parse --
// leaves the previous subject on screen instead of an empty turntable.
auto ImportSubject(ZHLN::Engine& engine, Subject& subject, std::span<const uint8_t> bytes, std::string_view virtualPath) -> bool {
    ZHLN::ModelPrefab* prefab = ZHLN::GLTF::LoadGLBPrefabFromMemory(engine.GetRenderContext(), engine.GetAssetManager(), bytes, virtualPath);
    if (prefab == nullptr) {
        ZHLN::Log("[RemoteGLB] '{}' is not a glTF this importer can read; the previous subject stays on screen.", virtualPath);
        return false;
    }
    ClearSubject(engine, subject);

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
    bool         floorOn  = true;
    bool         ssrOn    = true;
    bool         subjectOn = true;
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

// Hides the subject's mesh instances (shadow casters and reflections with it).
// The floor-shimmer diagnostic: if the flicker follows the subject into
// invisibility, it is the subject's shadow or reflection, not the floor.
void SetSubjectVisible(ZHLN::Engine& engine, const Subject& subject, const Studio& studio) {
    for (const ZHLN::Entity entity: subject.instances) {
        engine.GetRegistry().Patch<ZHLN::Components::MeshComponent>(entity, [&studio](auto& mesh) -> auto {
            if (studio.subjectOn) {
                mesh.flags &= ~ZHLN::DrawFlags::Hidden;
            } else {
                mesh.flags |= ZHLN::DrawFlags::Hidden;
            }
        });
    }
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

    // Reflections: SSR on by default (S toggles it -- the floor-shimmer A/B
    // switch), RTR reflections for the floor. RT shadows off
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
    // 2048 is the size the engine allocates the cascade pair at (TargetManager):
    // requesting the Ultra preset's 4096 makes ApplySettings reallocate a
    // ~512 MB shadow map pair at load, and if the GPU declines, the frame still
    // carries 4096 into the lighting pass -- where the PCSS blocker search,
    // bias and penumbra filter are all expressed in units of the nominal
    // resolution, so they would run at half the width of the real 2048 texels
    // and the floor shimmers around the subject's shadow.
    gfx.shadows.resolution         = 2048;
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

// The stage (floor, framing, zoom range) scales with the subject's radius, but
// these two frustum-level quantities used to be clamped to flat metre-scale
// ceilings. A subject over ~2.7 m in radius outran them: the far plane stopped
// 2000 m out while the zoom range (60 r) and the 5 r floor ran past it, so the
// floor's far edge -- and eventually the subject itself -- clipped at far zoom
// (this is what Fox hit: it is authored in centimetres, so its 87.775 m "radius"
// is really 0.88 m of geometry). The ceilings are now each formula's own value
// at the far end of the zoom range, which is exactly 2000/400 for every model
// the old caps were enough for.
[[nodiscard]] constexpr auto FrameFarPlane(float orbitDistance, float maxDistance, float subjectRadius) noexcept -> float {
    const float ceiling = std::max(2000.0f, (maxDistance * 12.0f) + (subjectRadius * 24.0f));
    return std::clamp((orbitDistance * 12.0f) + (subjectRadius * 24.0f), 20.0f, ceiling);
}

[[nodiscard]] constexpr auto ShadowBoxExtent(float orbitDistance, float maxDistance, float subjectRadius) noexcept -> float {
    const float ceiling = std::max(400.0f, 2.0f * (maxDistance + (subjectRadius * kFloorExtent)));
    return std::clamp(2.0f * (orbitDistance + (subjectRadius * kFloorExtent)), 4.0f, ceiling);
}

// The caster-culling ortho box is centred on the camera, so it has to grow with
// the orbit or the subject leaves it and stops casting. Written only when it moves
// by more than 2%: the collector reads the component every frame anyway, and a
// value that changed with every pixel of wheel travel would be noise.
void UpdateShadowExtent(ZHLN::Engine& engine, float orbitDistance, float maxOrbitDistance, float subjectRadius) {
    const float        wanted = ShadowBoxExtent(orbitDistance, maxOrbitDistance, subjectRadius);
    auto&              reg    = engine.GetRegistry();
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
    camera.farZ  = FrameFarPlane(orbit.distance, orbit.maxDistance, subject.radius);
}

// ============================================================================
// SAMPLE STATE
// ============================================================================

struct SampleState {
    // The fetcher is a local in main (it is not copyable, and its destructor
    // is the join): the pointer keeps the state a plain struct, and main
    // declares it before this one, so it is destroyed after.
    ZHLN::Remote::AsyncAssetFetcher* fetcher = nullptr;
    uint32_t                         activeRequest = 0;
    std::string                      assetUrl;
    double                           fetchStartedAt = 0.0;

    // What the last Take said, for the HUD: the source that served the bytes,
    // whether it was the disk cache, and the one-line report (a corrupt cache
    // file removed on the way, or every candidate's failure).
    std::string lastSource;
    bool        lastFromCache = false;
    std::string fetchDetail;

    Catalog                catalog;
    Subject                subject;
    Studio                 studio;
    OrbitCamera            orbit;
    ZHLN::GraphicsSettings settings;

    // The frame's copy of the catalog list. DrawHUD and the pick handler read
    // only this; the crawl's worker writes only catalog.entries; and the
    // hand-over happens on this thread (UpdateCatalogDisplay). A URL that is
    // not in the crawled repository gets a synthetic row for itself, so
    // whatever is on screen is always a row in the list.
    std::vector<GLBEntry> displayEntries {};
    int                   dropdownSelected = 0;
    bool                  catalogSeeded    = false;

    bool     rayTraced   = false;
    bool     reported    = false; // this download's outcome has been logged and imported
    uint32_t frameBudget = 0;
    double   now         = 0.0;
    float    dt          = 0.016666f;
    float    fps         = 0.0f;

    std::array<bool, 256> keyWasDown {};
};

// The synthetic dropdown row for a URL the crawl does not cover: the name the
// file has on its own URL, no size (the tree's size column is what a crawled
// row gets).
[[nodiscard]] auto MakeSyntheticEntry(std::string_view url) -> GLBEntry {
    GLBEntry entry;
    entry.url   = std::string(url);
    entry.label = ZHLN::Remote::UrlStem(url);
    return entry;
}

// Moves the crawl's list into the frame's display copy, once, when it arrives --
// and again after a C re-crawl. The row the dropdown selects is the one the
// current URL points at: the repository-relative path it resolves to, or, when
// it resolves nowhere, the synthetic row this function puts in front.
void UpdateCatalogDisplay(SampleState& state) {
    if (state.catalogSeeded) {
        return;
    }
    if (state.catalog.phase.load(std::memory_order::acquire) != CatalogPhase::Ready) {
        return;
    }

    state.displayEntries = std::move(state.catalog.entries);
    const std::string    repoPath = ZHLN::GitHub::RepoPathForUrl(state.assetUrl, state.catalog.repo, state.catalog.branch);
    int                  selected = 0;
    bool                 found    = false;
    if (!repoPath.empty()) {
        for (size_t i = 0; i < state.displayEntries.size(); ++i) {
            if (state.displayEntries[i].path == repoPath) {
                selected = static_cast<int>(i);
                found    = true;
                break;
            }
        }
    }
    if (!found) {
        state.displayEntries.insert(state.displayEntries.begin(), MakeSyntheticEntry(state.assetUrl));
        selected = 0;
    }
    state.dropdownSelected = selected;
    state.catalogSeeded    = true;
}

// A dropdown pick: the row's URL is what gets fetched, and only then -- the
// list is the lazy part. A pick of what is already on screen, or already in
// flight, is a no-op; a pick of a model whose last download failed is the
// retry.
void SelectModel(SampleState& state, int index) {
    if ((index < 0) || (index >= static_cast<int>(state.displayEntries.size()))) {
        return;
    }
    const GLBEntry& entry = state.displayEntries[static_cast<size_t>(index)];
    const auto      status = state.fetcher->Status(state.activeRequest);
    if ((entry.url == state.assetUrl) && (state.subject.loaded || (status == ZHLN::Remote::FetchStatus::Pending) || (status == ZHLN::Remote::FetchStatus::Succeeded))) {
        ZHLN::Log("[RemoteGLB] '{}' is already on screen.", entry.label);
        return;
    }
    ZHLN::Log("[RemoteGLB] Selecting '{}' ({}).", entry.label, entry.url);
    state.reported = false;
    state.fetchDetail.clear();
    state.assetUrl = entry.url;
    state.activeRequest = state.fetcher->Request(entry.url, ZHLN::Remote::Validators::IsGLB, false);
    state.fetchStartedAt = state.now;
}

// The one status line the model list earns in the HUD.
[[nodiscard]] auto CatalogLine(const SampleState& state) -> std::string {
    const auto phase = state.catalog.phase.load(std::memory_order::acquire);
    if (phase == CatalogPhase::Idle) {
        return {};
    }
    if (phase == CatalogPhase::Loading) {
        return std::format("catalog: fetching the model list from {}", state.catalog.repo);
    }
    if (phase == CatalogPhase::Failed) {
        return std::format("catalog failed: {}  --  C retries", state.catalog.detail);
    }
    std::string line = std::format("catalog: {} model(s) from {}", state.catalog.count, state.catalog.repo);
    if (!state.catalog.detail.empty()) {
        line += "; " + state.catalog.detail;
    }
    return line;
}

// What the status line calls the model currently on screen or in flight: the
// display row that has its URL, or the URL's own stem when it has no row.
[[nodiscard]] auto CurrentLabel(const SampleState& state) -> std::string {
    for (const GLBEntry& entry: state.displayEntries) {
        if (entry.url == state.assetUrl) {
            return entry.label;
        }
    }
    return ZHLN::Remote::UrlStem(state.assetUrl);
}

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
    // A fresh import brings fresh instances: carry the H-toggle's choice over
    // them the way BuildStudio does for the floor.
    SetSubjectVisible(engine, state.subject, state.studio);
    FrameSubject(state.orbit, state.subject);
}

// Reaps the workers that are done and, exactly once per download, imports
// what arrived. All of it runs on the frame thread: the importer uploads GPU
// resources and writes the registry.
void PollFetch(ZHLN::Engine& engine, SampleState& state) {
    state.fetcher->Poll();

    const auto status = state.fetcher->Status(state.activeRequest);
    if ((status != ZHLN::Remote::FetchStatus::Succeeded) && (status != ZHLN::Remote::FetchStatus::Failed)) {
        return;
    }
    if (state.reported) {
        return;
    }
    auto result = state.fetcher->Take(state.activeRequest);
    if (!result) {
        return;
    }
    state.reported = true;

    const double elapsed = state.now - state.fetchStartedAt;
    state.lastSource    = std::move(result->sourceUrl);
    state.lastFromCache = result->fromCache;
    state.fetchDetail   = std::move(result->errorMessage);

    if (status == ZHLN::Remote::FetchStatus::Failed) {
        ZHLN::Log("[RemoteGLB] Fetch failed after {:.1f}s: {}", elapsed, state.fetchDetail);
        ZHLN::Log("[RemoteGLB] Check the URL and the network, then press R to try again.");
        return;
    }

    if (!state.fetchDetail.empty()) {
        ZHLN::Log("[RemoteGLB] {}", state.fetchDetail);
    }
    ZHLN::Log(
        "[RemoteGLB] {} {} KiB in {:.2f}s: {}", state.lastFromCache ? "Cache hit," : "Downloaded", result->data.size() / 1024U, elapsed,
        state.lastSource
    );

    // The cache file's name is the model's identity in the prefab cache, so a
    // second run and a device-lost rebuild import the same name and hit it.
    const std::string virtualPath = ZHLN::Remote::ResolveURL(state.assetUrl).cacheFileName;
    if (ImportSubject(engine, state.subject, std::span<const uint8_t>(result->data), virtualPath)) {
        RebuildLook(engine, state);
        // The bytes are in the prefab cache and on disk now; `result` goes out
        // of scope, which is the third copy dying.
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
    // Screen-space reflections off and on. The floor's mirror layer is the
    // sample's only reflection that is not temporally filtered, so when the
    // floor shimmers around a moving (animated) subject, S is the A/B switch
    // that says whether the shimmer is the reflection or the sun shadow.
    if (KeyPressed(state, *input, ZHLN::KeyCode::S)) {
        state.studio.ssrOn = !state.studio.ssrOn;
        const ZHLN::Entity settings = reg.SingletonEntity<ZHLN::Components::GlobalSettingsTagComponent>();
        if (settings != ZHLN::Entity::Null()) {
            reg.Patch<ZHLN::Components::PostProcessSettingsComponent>(settings, [&](auto& p) -> auto { p.enableSSR = state.studio.ssrOn ? 1 : 0; });
        }
        ZHLN::Log("[RemoteGLB] Screen-space reflections {}.", state.studio.ssrOn ? "on" : "off");
    }
    // H — hide the subject itself, shadow casters and reflections with it: the
    // other half of the floor-shimmer A/B test. If the shimmer follows the
    // subject into invisibility, the floor is only showing it; if it stays,
    // the floor's own shading is at fault.
    if (KeyPressed(state, *input, ZHLN::KeyCode::H) && state.subject.loaded) {
        state.studio.subjectOn = !state.studio.subjectOn;
        SetSubjectVisible(engine, state.subject, state.studio);
    }
    if (KeyPressed(state, *input, ZHLN::KeyCode::R)) {
        ZHLN::Log("[RemoteGLB] Re-downloading '{}', ignoring the cache.", state.assetUrl);
        state.reported = false;
        state.fetchDetail.clear();
        state.activeRequest = state.fetcher->Request(state.assetUrl, ZHLN::Remote::Validators::IsGLB, true);
        state.fetchStartedAt = state.now;
    }
    // Re-crawl the model list. A crawl that is already running is left alone:
    // it is one small request, and its result will be Ready or Failed either
    // way -- and the frame never starts a second crawl underneath one.
    if (KeyPressed(state, *input, ZHLN::KeyCode::C) && (state.catalog.phase.load(std::memory_order::acquire) != CatalogPhase::Loading)) {
        ZHLN::Log("[RemoteGLB] Re-crawling the model list: {}", ZHLN::GitHub::TreeURL(state.catalog.repo, state.catalog.branch));
        state.catalogSeeded = false; // seed again once the new listing arrives
        StartCatalogCrawl(state.catalog, EnvironmentU32("ZHLN_REMOTE_GLB_TIMEOUT", kFetchTimeout));
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
        // A tier rewrite rebuilds the post-process component from the preset,
        // so carry the S-toggle's SSR choice over it.
        gfx.post.enableSSR = state.studio.ssrOn ? 1 : 0;
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
    // The outcome fields (lastSource, fetchDetail) are written by PollFetch,
    // which runs before this on every frame, so a terminal status here is
    // always paired with the report for it.
    switch (state.fetcher->Status(state.activeRequest)) {
        case ZHLN::Remote::FetchStatus::Pending:
            return std::format("downloading '{}'... {:.0f}s", CurrentLabel(state), state.now - state.fetchStartedAt);
        case ZHLN::Remote::FetchStatus::Failed:
            return std::format("fetch failed: {}  --  R retries", state.fetchDetail);
        case ZHLN::Remote::FetchStatus::Succeeded:
            return state.reported ? std::format("{}: {}", state.lastFromCache ? "cached" : "downloaded", state.lastSource) :
                                    std::string("importing...");
        case ZHLN::Remote::FetchStatus::Idle:
            break;
    }
    return "idle";
}

void DrawHUD(ZHLN::Engine& engine, SampleState& state) {
    ZHLN::GUI::Context ui(engine);
    ui.BeginFrame(state.dt);

    const ZHLN::RenderInfo info    = engine.GetRenderContext().GetInfo();
    const std::string      subject = state.subject.loaded ?
                                     std::format("{} part(s), {} triangle(s), {} texture(s), radius {:.3f} m", state.subject.prefab->parts.size(),
                                                 state.subject.triangles, state.subject.textures, state.subject.radius) :
                                     std::string("no model yet");
    const std::string      catalog = CatalogLine(state);

    // The dropdown's options: views into the display copy's strings, which are
    // stable -- the list is rebuilt only by UpdateCatalogDisplay, on this
    // thread, and the span only has to live for this call.
    std::vector<std::string_view> options;
    options.reserve(state.displayEntries.size());
    for (const GLBEntry& entry: state.displayEntries) {
        options.push_back(entry.label);
    }

    ui.Box(
        "RemoteGLBPanel",
        ZHLN::GUI::BoxConfig {
            .width        = {.fixed = kPanelWidth},
            .height       = {},
            .color        = {0.05f, 0.07f, 0.10f, 0.90f},
            .cornerRadius = {6.0f, 6.0f, 6.0f, 6.0f},
            .padding      = kPanelPadding,
            .gap          = 4.0f,
            .direction    = ZHLN::GUI::Direction::Column,
            .offsetX      = kPanelMargin,
            .offsetY      = kPanelMargin,
        },
        [&]() -> void {
            ui.Text("REMOTE glTF  /  GLTF-SAMPLE-ASSETS", 16.0f, {0.30f, 0.85f, 1.00f, 1.0f});
            const bool changed = ui.Dropdown("Model", options, state.dropdownSelected, ZHLN::GUI::Sizing {.fixed = kFieldWidth});
            ui.Text(StatusLine(state), 12.0f, {0.80f, 0.86f, 0.94f, 1.0f});
            ui.Text(subject, 12.0f, {0.62f, 0.70f, 0.80f, 1.0f});
            if (!catalog.empty()) {
                ui.Text(catalog, 11.0f, {0.50f, 0.57f, 0.67f, 1.0f});
            }
            ui.Text(
                std::format("{}  |  {}  |  quality {}  |  {}  |  AO mode {}  |  {:.0f} fps", info.gpuName, info.rayTracingSupported ? "RT capable" : "no RT",
                            QualityName(state.settings.DetectPreset()), AAName(state.settings.antiAliasing.mode), state.settings.post.mode, state.fps),
                11.0f, {0.50f, 0.57f, 0.67f, 1.0f}
            );
            ui.Text("LMB orbit   RMB pan   wheel zoom", 12.0f, {0.72f, 0.78f, 0.86f, 1.0f});
            ui.Text("F re-frame   G floor   H subject   S ssr", 11.0f, {0.45f, 0.51f, 0.60f, 1.0f});
            ui.Text("R re-download   C re-crawl   0 studio look   1-4 quality tiers", 11.0f, {0.45f, 0.51f, 0.60f, 1.0f});

            // The pick is handled here, in the frame the widget reported it.
            // SelectModel only touches this state and starts a fetch -- no
            // engine calls, so the Clay layout it sits inside stays intact.
            if (changed) {
                SelectModel(state, state.dropdownSelected);
            }
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
    state.rayTraced   = EnvironmentFlag("ZHLN_REMOTE_GLB_RTR");
    state.frameBudget = EnvironmentU32("ZHLN_REMOTE_GLB_FRAMES", 0);
    state.assetUrl    = EnvironmentString("ZHLN_REMOTE_GLB_URL", kDefaultAssetURL);

    state.catalog.repo   = EnvironmentString("ZHLN_REMOTE_GLB_REPO", kDefaultRepo);
    state.catalog.branch = EnvironmentString("ZHLN_REMOTE_GLB_BRANCH", kDefaultBranch);

    // Whatever the URL points at is a row in the dropdown from the first frame
    // -- the only one, until the crawl answers (or forever, when the crawl is
    // off or fails).
    state.displayEntries = {MakeSyntheticEntry(state.assetUrl)};

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
    const uint32_t timeoutSeconds = EnvironmentU32("ZHLN_REMOTE_GLB_TIMEOUT", kFetchTimeout);

    // The download side of the sample, in two locals: the cache directory
    // (build/cache in a dev tree, the per-user cache otherwise,
    // ZHLN_CACHE_DIR over both) and the fetcher that owns the download
    // workers and their whole lifecycle. A local rather than a member of
    // SampleState because it is not copyable, and its destructor is the join.
    // It is declared after `state`, so at the end of main the fetcher dies
    // first -- asking its workers to retire and joining them while nothing
    // can still call back into the engine -- and `state` outlives it with a
    // dangling pointer that no code between the two destructions dereferences.
    ZHLN::Remote::DiskCache         diskCache;
    ZHLN::Remote::AsyncAssetFetcher fetcher(diskCache, timeoutSeconds);
    state.fetcher                   = &fetcher;

    // The crawl and the first download run side by side, on their own workers:
    // the listing is one small API call, and neither should hold up the other.
    if (EnvironmentFlag("ZHLN_REMOTE_GLB_NO_CATALOG")) {
        ZHLN::Log("[RemoteGLB] Catalog: disabled (ZHLN_REMOTE_GLB_NO_CATALOG=1)");
    } else {
        ZHLN::Log("[RemoteGLB] Catalog: {}", ZHLN::GitHub::TreeURL(state.catalog.repo, state.catalog.branch));
        StartCatalogCrawl(state.catalog, timeoutSeconds);
    }

    const ZHLN::Remote::ResolvedURL resolvedAsset = ZHLN::Remote::ResolveURL(state.assetUrl);
    ZHLN::Log("[RemoteGLB] Asset: {}", state.assetUrl);
    ZHLN::Log("[RemoteGLB] Cache: {}", (diskCache.Root() / resolvedAsset.cacheFileName).string());
    state.activeRequest = fetcher.Request(state.assetUrl, ZHLN::Remote::Validators::IsGLB, EnvironmentFlag("ZHLN_REMOTE_GLB_REFRESH"));
    state.fetchStartedAt = clock.GetTotalTime();

    engine->SetUICallback([&state](ZHLN::Engine& eng) -> void { DrawHUD(eng, state); });

    while (engine->IsRunning()) {
        state.dt  = std::min(clock.GetDeltaTime(), 0.05f);
        state.now = clock.GetTotalTime();
        state.fps = (state.fps * 0.90f) + ((state.dt > 0.0f ? 1.0f / state.dt : 0.0f) * 0.10f);

        engine->ProcessEvents();
        HandleInput(*engine, state);
        PollFetch(*engine, state);
        UpdateCatalogDisplay(state);

        UpdateShadowExtent(*engine, state.orbit.distance, state.orbit.maxDistance, state.subject.radius);
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

    // Asks the crawl worker to retire and joins it -- a single small request.
    // The download workers need no loop here: `fetcher`'s destructor, just
    // below, asks them to retire and joins them. A transfer already inside
    // libcurl runs to its own timeout, which is the one thing that can delay
    // shutdown either way.
    state.catalog.worker.request_stop();
    if (state.catalog.worker.joinable()) {
        state.catalog.worker.join();
    }

    ZHLN::TaskSystem::Shutdown();
    return EXIT_SUCCESS;
}
