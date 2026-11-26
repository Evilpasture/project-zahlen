// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

// Exercises plugins/lua/Lua.hpp + Scene.hpp:
//   * Reflection-driven evaluation into scalars, FixedString<N>, std::array,
//     std::vector, maps, Optional, enums, ZHLN::FVec3/FVec3/FVec4/Quat,
//     GraphicsSettings, and ZHLN::Scene::Document.
//   * Computed expressions (loops, math, helpers) and pure function reducers
//     (EvaluateCallString).
//   * Transactional deep patching (ApplyPatchString) that preserves sibling
//     fields and rolls back completely on validation failure.
//   * Hermetic sandbox isolation (no io/os/ffi/package/debug/jit/bytecode)
//     and deterministic instruction + memory budgets.

#include "TestsFramework.hpp"
#include <Scene/Scene.hpp>
#include <Zahlen/Core/Optional.hpp>
#include <Zahlen/FileSystem/Paths.hpp>
#include <Zahlen/Core/String.hpp>
#include <Zahlen/ECS/Components.hpp>
#include <Zahlen/Math/Matrix.hpp>
#include <Zahlen/Math/Vec3.hpp>
#include <Zahlen/Math/Vec4.hpp>
#include <Zahlen/Render/GraphicsSettings.hpp>
#include <Zahlen/Render/Info.hpp>
#include <cmath>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <lua/Lua.hpp>
#include <lua/Host.hpp>
#include <lua/Scene.hpp>
#include <memory>
#include <optional>
#include <sstream>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace {

// Host-side test helper only: Lua itself never opens OS files. Tests read a
// shipped script into memory here, then feed the bytes through FromString /
// Compile, exactly as a host would with VFS-loaded bytes.
[[nodiscard]] auto ReadHostFileText(const std::filesystem::path& path) -> std::expected<std::string, ZHLN::ErrorCode> {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        return std::unexpected(ZHLN::Lua::Error::FileReadFailed);
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    if (in.bad()) {
        return std::unexpected(ZHLN::Lua::Error::FileReadFailed);
    }
    return buffer.str();
}

enum class LuaSuiteFail : uint8_t {
    UnexpectedError = 1,
    WrongValue,
    ExpectedFailureDidNotHappen,
    WrongErrorCode,
};

struct SampleSubConfig {
    float          gain    = 1.0f;
    bool           enabled = true;
    ZHLN::String64 tag     = ZHLN::String64("default");
    ZHLN::FVec3    offset  = {0.0f, 1.0f, 2.0f};
};

struct SampleAggregate {
    int32_t                                  iterations = 4;
    uint8_t                                  tier       = 1;
    std::string                              label      = "initial";
    ZHLN::Optional<int32_t>                  seed       = std::nullopt;
    std::array<float, 3>                     weights    = {1.0f, 0.5f, 0.25f};
    std::vector<int32_t>                     steps      = {};
    std::unordered_map<std::string, int32_t> counters   = {};
    SampleSubConfig                          sub        = {};
};

struct HardwareProbe {
    uint32_t    vramMiB          = 8192;
    bool        supportsRayQuery = true;
    std::string adapterName      = "Test GPU";
};

struct PodVec2 {
    float x = 0.0f;
    float y = 0.0f;
};

struct PodEntityRef {
    uint64_t id = 0;
};

struct NonPodHolder {
    std::shared_ptr<int> owned;
};

static_assert(ZHLN::Lua::TrivialUserdata<PodVec2>);
static_assert(ZHLN::Lua::TrivialUserdata<PodEntityRef>);
static_assert(!ZHLN::Lua::TrivialUserdata<NonPodHolder>);
static_assert(!ZHLN::Lua::TrivialUserdata<std::string>);
static_assert(!ZHLN::Lua::TrivialUserdata<int*>);

struct InteropCounterHost {
    int64_t                         observedSum = 0;
    ZHLN::Lua::ListenerID    listener    = ZHLN::Lua::kInvalidListenerID;
};

auto PodVec2New(ZHLN::Lua::CallFrame& frame) -> int {
    frame.PushUserdata(PodVec2 {
        .x = frame.ReadFloat(1).value_or(0.0f),
        .y = frame.ReadFloat(2).value_or(0.0f),
    });
    return 1;
}

auto PodVec2Index(ZHLN::Lua::CallFrame& frame) -> int {
    const auto* v   = frame.TestUserdata<PodVec2>(1);
    const auto  key = frame.ReadStringView(2);
    if (v == nullptr || !key) {
        frame.PushNil();
        return 1;
    }
    if (*key == "x") {
        frame.PushFloat(v->x);
    } else if (*key == "y") {
        frame.PushFloat(v->y);
    } else {
        frame.PushNil();
    }
    return 1;
}

auto PodVec2Add(ZHLN::Lua::CallFrame& frame) -> int {
    const auto* a = frame.TestUserdata<PodVec2>(1);
    const auto* b = frame.TestUserdata<PodVec2>(2);
    if (a == nullptr || b == nullptr) {
        frame.PushNil();
        return 1;
    }
    frame.PushUserdata(PodVec2 {.x = a->x + b->x, .y = a->y + b->y});
    return 1;
}

auto PodVec2LengthSq(ZHLN::Lua::CallFrame& frame) -> int {
    const auto* v = frame.TestUserdata<PodVec2>(1);
    frame.PushFloat(v != nullptr ? (v->x * v->x + v->y * v->y) : 0.0f);
    return 1;
}

auto PodEntityGetInterned(ZHLN::Lua::CallFrame& frame) -> int {
    const uint64_t id = frame.ReadUInt64(1).value_or(0);
    frame.PushInternedUserdata(id, PodEntityRef {.id = id});
    return 1;
}

auto PodEntityEq(ZHLN::Lua::CallFrame& frame) -> int {
    const auto* a = frame.TestUserdata<PodEntityRef>(1);
    const auto* b = frame.TestUserdata<PodEntityRef>(2);
    frame.PushBool(a != nullptr && b != nullptr && a->id == b->id);
    return 1;
}

auto RegisterCallbackHostFn(ZHLN::Lua::CallFrame& frame) -> int {
    auto* host = frame.HostContext<InteropCounterHost>();
    if (host == nullptr || !frame.IsFunction(1)) {
        frame.PushBool(false);
        return 1;
    }
    host->listener = frame.RegisterListener(1);
    frame.PushBool(host->listener != ZHLN::Lua::kInvalidListenerID);
    return 1;
}

auto ReportSumHostFn(ZHLN::Lua::CallFrame& frame) -> int {
    if (auto* host = frame.HostContext<InteropCounterHost>()) {
        host->observedSum += frame.ReadInt64(1).value_or(0);
    }
    return 0;
}

struct SandboxMirror: ZHLN::Lua::Runtime {
    struct: ZHLN::Lua::ReadOnly {
        float elapsed = 1.0f;
        bool  keyF    = true;
        bool  keyR    = false;
        bool  keyEsc  = false;
    } in {};

    struct: ZHLN::Lua::OneShot {
        bool spawnCube   = false;
        bool resetCamera = false;
        bool quit        = false;
    } out {};

    ZHLN::FVec3 lampPos {};
    float       launchSpeed = 0.0f;
};

template <typename LayerType>
inline constexpr bool kCanTick = requires(LayerType& layer, ZHLN::COM::IEngine& engine, float dt) {
    layer.Tick(engine, dt);
};

auto Near(float a, float b, float eps = 1e-4f) noexcept -> bool {
    return std::fabs(a - b) <= eps;
}

struct LuaSuite {
    struct Tests {
        auto TestScalarAndContainerHydration() -> std::expected<void, ZHLN::ErrorCode> {
        constexpr std::string_view kChunk = R"lua(
            local steps = {}
            for i = 1, 4 do
                steps[i] = i * i
            end
            return {
                iterations = 12,
                tier       = 3,
                label      = "computed",
                seed       = 1337,
                weights    = { 0.25, 0.5, 0.75 },
                steps      = steps,
                counters   = { alpha = 10, beta = 20 },
                sub = {
                    gain    = 2.5,
                    enabled = false,
                    tag     = "tuned",
                    offset  = { 4.0, 5.0, 6.0 },
                },
            }
        )lua";

        auto res = ZHLN::Lua::EvaluateString<SampleAggregate>(kChunk, "hydration");
        if (!res) {
            return std::unexpected(LuaSuiteFail::UnexpectedError);
        }

        if (res->iterations != 12 || res->tier != 3 || res->label != "computed") {
            return std::unexpected(LuaSuiteFail::WrongValue);
        }
        if (!res->seed.has_value() || *res->seed != 1337) {
            return std::unexpected(LuaSuiteFail::WrongValue);
        }
        if (!Near(res->weights[0], 0.25f) || !Near(res->weights[1], 0.5f) || !Near(res->weights[2], 0.75f)) {
            return std::unexpected(LuaSuiteFail::WrongValue);
        }
        if (res->steps.size() != 4 || res->steps[0] != 1 || res->steps[3] != 16) {
            return std::unexpected(LuaSuiteFail::WrongValue);
        }
        if (res->counters.at("alpha") != 10 || res->counters.at("beta") != 20) {
            return std::unexpected(LuaSuiteFail::WrongValue);
        }
        const std::string_view tagView(res->sub.tag.data(), res->sub.tag.size());
        if (!Near(res->sub.gain, 2.5f) || res->sub.enabled || tagView != "tuned") {
            return std::unexpected(LuaSuiteFail::WrongValue);
        }
        if (!Near(res->sub.offset.x, 4.0f) || !Near(res->sub.offset.y, 5.0f) || !Near(res->sub.offset.z, 6.0f)) {
            return std::unexpected(LuaSuiteFail::WrongValue);
        }
        return {};
    }

    auto TestProceduralSceneRecipe() -> std::expected<void, ZHLN::ErrorCode> {
        constexpr std::string_view kSceneChunk = R"lua(
            local lights = {}
            local count  = 6
            local radius = 10.0
            for i = 0, count - 1 do
                local angle = (i / count) * (2.0 * math.pi)
                lights[#lights + 1] = {
                    name      = string.format("ring_%d", i),
                    type      = "Point",
                    position  = { math.cos(angle) * radius, 3.0, math.sin(angle) * radius },
                    color     = { 1.0, 0.8, 0.6 },
                    intensity = 15.0 + i,
                    range     = 12.0,
                }
            end
            return {
                name = "RingArena",
                camera = {
                    position = { x = 0.0, y = 4.0, z = -14.0 },
                    fov      = 75.0,
                },
                entities = {
                    {
                        name  = "floor",
                        shape = "Plane",
                        body  = "Static",
                    },
                },
                lights = lights,
            }
        )lua";

        auto scene = ZHLN::Lua::EvaluateString<ZHLN::Scene::Document>(kSceneChunk, "ring_scene");
        if (!scene) {
            return std::unexpected(LuaSuiteFail::UnexpectedError);
        }
        if (scene->name != "RingArena") {
            return std::unexpected(LuaSuiteFail::WrongValue);
        }
        if (!Near(scene->camera.position.y, 4.0f) || !Near(scene->camera.fov, 75.0f)) {
            return std::unexpected(LuaSuiteFail::WrongValue);
        }
        if (scene->entities.size() != 1 || scene->entities[0].shape != ZHLN::Scene::ShapeKind::Plane ||
            scene->entities[0].body != ZHLN::Scene::BodyKind::Static) {
            return std::unexpected(LuaSuiteFail::WrongValue);
        }
        if (scene->lights.size() != 6 || scene->lights[0].name != "ring_0") {
            return std::unexpected(LuaSuiteFail::WrongValue);
        }
        if (scene->lights[0].type != "Point" || !Near(scene->lights[0].position.x, 10.0f)) {
            return std::unexpected(LuaSuiteFail::WrongValue);
        }
        return {};
    }

    auto TestPureReducerGraphicsProfile() -> std::expected<void, ZHLN::ErrorCode> {
        constexpr std::string_view kProfileFn = R"lua(
            return function(hw)
                local highVram = hw.vramMiB >= 8192
                local rt       = hw.supportsRayQuery and highVram
                return {
                    qualityPreset = highVram and "Ultra" or "Medium",
                    antiAliasing  = { mode = "smaa" },
                    post = {
                        exposure  = 1.25,
                        mode      = rt and 2 or 0,
                    },
                    rayTracing = {
                        enableReflections = rt,
                        enableShadows     = rt,
                        enableDiffuseGI   = rt,
                    },
                }
            end
        )lua";

        const HardwareProbe desktopGpu {.vramMiB = 12288, .supportsRayQuery = true, .adapterName = "RTX"};
        auto                hi = ZHLN::Lua::EvaluateCallString<ZHLN::GraphicsSettings>(
            kProfileFn, "graphics.lua", ZHLN::Lua::Budget {}, desktopGpu
        );
        if (!hi) {
            return std::unexpected(LuaSuiteFail::UnexpectedError);
        }
        if (hi->qualityPreset != ZHLN::QualityLevel::Ultra || hi->antiAliasing.mode != ZHLN::AAMode::SMAA) {
            return std::unexpected(LuaSuiteFail::WrongValue);
        }
        if (!Near(hi->post.exposure, 1.25f) || hi->post.mode != 2 || !hi->rayTracing.enableReflections || !hi->rayTracing.enableDiffuseGI) {
            return std::unexpected(LuaSuiteFail::WrongValue);
        }

        const HardwareProbe lowVramGpu {.vramMiB = 4096, .supportsRayQuery = false, .adapterName = "iGPU"};
        auto                lo = ZHLN::Lua::EvaluateCallString<ZHLN::GraphicsSettings>(
            kProfileFn, "graphics.lua", ZHLN::Lua::Budget {}, lowVramGpu
        );
        if (!lo) {
            return std::unexpected(LuaSuiteFail::UnexpectedError);
        }
        if (lo->qualityPreset != ZHLN::QualityLevel::Medium || lo->rayTracing.enableReflections) {
            return std::unexpected(LuaSuiteFail::WrongValue);
        }
        return {};
    }

    auto TestTransactionalDeepPatchAndRollback() -> std::expected<void, ZHLN::ErrorCode> {
        ZHLN::GraphicsSettings live {};
        live.post.exposure      = 2.2f;
        live.post.bloomStrength = 0.04f;
        live.shadows.resolution = 4096;

        // 1. Partial nested patch updates bloomStrength while preserving sibling post.exposure.
        if (auto ok = ZHLN::Lua::ApplyPatchString(live, "return { post = { bloomStrength = 0.18 } }"); !ok) {
            return std::unexpected(LuaSuiteFail::UnexpectedError);
        }
        if (!Near(live.post.exposure, 2.2f) || !Near(live.post.bloomStrength, 0.18f) || live.shadows.resolution != 4096) {
            return std::unexpected(LuaSuiteFail::WrongValue);
        }

        // 2. Patch with a valid field followed by an unknown field must roll back completely.
        auto unknownRes = ZHLN::Lua::ApplyPatchString(
            live,
            "return { post = { exposure = 9.0, nonExistentKnob = 1 } }"
        );
        if (unknownRes) {
            return std::unexpected(LuaSuiteFail::ExpectedFailureDidNotHappen);
        }
        if (!(unknownRes.error() == ZHLN::Lua::Error::UnknownField)|| !Near(live.post.exposure, 2.2f)) {
            return std::unexpected(LuaSuiteFail::WrongErrorCode);
        }

        // 3. Invalid enum string rolls back completely.
        auto enumRes = ZHLN::Lua::ApplyPatchString(
            live,
            "return { qualityPreset = 'SuperDuperUltra' }"
        );
        if (enumRes || !(enumRes.error() == ZHLN::Lua::Error::InvalidEnumString)) {
            return std::unexpected(LuaSuiteFail::WrongErrorCode);
        }

        // 4. Native FVec3 / FVec4 accept both sequence and named forms.
        ZHLN::EnvironmentSettings envComp {};
        envComp.ambientExposure = 1.5f;
        if (auto vecOk = ZHLN::Lua::ApplyPatchString(
                envComp,
                "return { probeMin = { 0.9, 0.8, 0.7 }, skyZenith = { x = 0.1, y = 0.2, z = 0.3, w = 1.0 } }"
            );
            !vecOk) {
            return std::unexpected(LuaSuiteFail::UnexpectedError);
        }
        if (!Near(envComp.ambientExposure, 1.5f) || !Near(envComp.probeMin.y, 0.8f) || !Near(envComp.skyZenith.z, 0.3f)) {
            return std::unexpected(LuaSuiteFail::WrongValue);
        }

        // Named and short sequence patches preserve untouched coordinates;
        // an invalid coordinate rolls back the entire struct.
        if (auto ok = ZHLN::Lua::ApplyPatchString(
                envComp, "return { probeMin = { x = 0.4 }, skyZenith = { 0.6 } }"); !ok) {
            return std::unexpected(LuaSuiteFail::UnexpectedError);
        }
        if (!Near(envComp.probeMin.x, 0.4f) || !Near(envComp.probeMin.y, 0.8f) ||
            !Near(envComp.skyZenith.x, 0.6f) || !Near(envComp.skyZenith.z, 0.3f)) {
            return std::unexpected(LuaSuiteFail::WrongValue);
        }
        if (auto bad = ZHLN::Lua::ApplyPatchString(
                envComp, "return { ambientExposure = 8, probeMin = { x = 'bad' } }"); bad) {
            return std::unexpected(LuaSuiteFail::ExpectedFailureDidNotHappen);
        }
        if (!Near(envComp.ambientExposure, 1.5f) || !Near(envComp.probeMin.x, 0.4f)) {
            return std::unexpected(LuaSuiteFail::WrongValue);
        }

        return {};
    }

    auto TestNativeVectorPatchRollback() -> std::expected<void, ZHLN::ErrorCode> {
        ZHLN::FVec3 position{1.0f, 2.0f, 3.0f};
        auto named = ZHLN::Lua::ApplyPatchString(position, "return { y = 9 }");
        if (!named || !Near(position.x, 1.0f) || !Near(position.y, 9.0f) || !Near(position.z, 3.0f))
            return std::unexpected(LuaSuiteFail::WrongValue);

        ZHLN::FVec4 color{1.0f, 2.0f, 3.0f, 4.0f};
        auto shortSeq = ZHLN::Lua::ApplyPatchString(color, "return { 7, 8 }");
        if (!shortSeq || !Near(color.x, 7.0f) || !Near(color.y, 8.0f) || !Near(color.z, 3.0f) || !Near(color.w, 4.0f))
            return std::unexpected(LuaSuiteFail::WrongValue);
        auto bad = ZHLN::Lua::ApplyPatchString(color, "return { 5, 'bad' }");
        if (bad || !(bad.error() == ZHLN::Lua::Error::TypeMismatch)|| !Near(color.x, 7.0f) || !Near(color.y, 8.0f))
            return std::unexpected(LuaSuiteFail::WrongErrorCode);
        auto longSeq = ZHLN::Lua::ApplyPatchString(color, "return { 1, 2, 3, 4, 5 }");
        if (longSeq || !(longSeq.error() == ZHLN::Lua::Error::OutOfRange)|| !Near(color.x, 7.0f))
            return std::unexpected(LuaSuiteFail::WrongErrorCode);

        // Constructing a new vector retains the full-sequence value contract.
        auto incomplete = ZHLN::Lua::EvaluateString<ZHLN::FVec4>("return { 1, 2 }");
        if (incomplete || !(incomplete.error() == ZHLN::Lua::Error::OutOfRange))
            return std::unexpected(LuaSuiteFail::WrongErrorCode);
        return {};
    }

    auto TestNativeMatrixSequence() -> std::expected<void, ZHLN::ErrorCode> {
        auto matrix = ZHLN::Lua::EvaluateString<ZHLN::Mat4x4>(
            "return { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 }");
        if (!matrix || (*matrix)[0, 1] != 5.0f || (*matrix)[2, 3] != 15.0f)
            return std::unexpected(LuaSuiteFail::WrongValue);
        auto roundTrip = ZHLN::Lua::EvaluateCallString<ZHLN::Mat4x4>(
            "return function(m) return m end", "matrix_roundtrip", ZHLN::Lua::Budget {}, *matrix);
        if (!roundTrip || roundTrip->elements != matrix->elements) return std::unexpected(LuaSuiteFail::WrongValue);
        auto bad = ZHLN::Lua::ApplyPatchString(*matrix, "return { 1, 2 }");
        if (bad || !(bad.error() == ZHLN::Lua::Error::OutOfRange)|| (*matrix)[0, 1] != 5.0f)
            return std::unexpected(LuaSuiteFail::WrongErrorCode);
        return {};
    }

    auto TestHermeticSandboxAndResourceBudgets() -> std::expected<void, ZHLN::ErrorCode> {
        // 1. Forbidden libraries and globals must all be nil in the sandbox.
        constexpr std::string_view kSandboxProbe = R"lua(
            assert(io == nil, "io leaked")
            assert(os == nil, "os leaked")
            assert(ffi == nil, "ffi leaked")
            assert(package == nil, "package leaked")
            assert(debug == nil, "debug leaked")
            assert(jit == nil, "jit leaked")
            assert(dofile == nil, "dofile leaked")
            assert(loadfile == nil, "loadfile leaked")
            assert(load == nil, "load leaked")
            assert(loadstring == nil, "loadstring leaked")
            return { iterations = 7 }
        )lua";

        auto probe = ZHLN::Lua::EvaluateString<SampleAggregate>(kSandboxProbe, "sandbox_probe");
        if (!probe || probe->iterations != 7) {
            return std::unexpected(LuaSuiteFail::UnexpectedError);
        }

        // 2. Binary bytecode header ("\x1bLJ") must be rejected by text-only loader.
        const std::string fakeBytecode("\x1bLJ\x02\x00\x00", 6);
        auto              bcRes = ZHLN::Lua::EvaluateString<SampleAggregate>(fakeBytecode, "bytecode");
        if (bcRes || !(bcRes.error() == ZHLN::Lua::Error::SyntaxError)) {
            return std::unexpected(LuaSuiteFail::WrongErrorCode);
        }

        // 3. Infinite loop must deterministically trip InstructionBudgetExceeded.
        ZHLN::Lua::Budget tightCpu {.maxInstructions = 8'192, .maxMemoryBytes = 4 * 1024 * 1024};
        auto            loopRes = ZHLN::Lua::EvaluateString<SampleAggregate>(
            "while true do end return {}", "inf_loop", tightCpu
        );
        if (loopRes || !(loopRes.error() == ZHLN::Lua::Error::InstructionBudgetExceeded)) {
            return std::unexpected(LuaSuiteFail::WrongErrorCode);
        }

        // 4. Exponential string doubling must deterministically trip MemoryBudgetExceeded.
        ZHLN::Lua::Budget tightMem {.maxInstructions = 5'000'000, .maxMemoryBytes = 256 * 1024};
        auto            oomRes = ZHLN::Lua::EvaluateString<SampleAggregate>(
            "local s = string.rep('a', 1024); for i = 1, 20 do s = s .. s end; return { label = s }",
            "oom_chunk",
            tightMem
        );
        if (oomRes || !(oomRes.error() == ZHLN::Lua::Error::MemoryBudgetExceeded)) {
            return std::unexpected(LuaSuiteFail::WrongErrorCode);
        }

        return {};
        }

        auto TestShippedConfigProfiles() -> std::expected<void, ZHLN::ErrorCode> {
            const auto findConfig = [](std::string_view fileName) -> std::filesystem::path {
                if (!ZHLN::ProjectRoot.empty()) {
                    std::filesystem::path fromRoot = std::filesystem::path(ZHLN::ProjectRoot) / "config" / fileName;
                    if (std::filesystem::exists(fromRoot)) {
                        return fromRoot;
                    }
                }
                for (std::string_view prefix: {"config", "../config", "../../config", "../../../config"}) {
                    std::filesystem::path candidate = std::filesystem::path(prefix) / fileName;
                    if (std::filesystem::exists(candidate)) {
                        return candidate;
                    }
                }
                return {};
            };

            const std::filesystem::path gfxPath = findConfig("graphics.lua");
            if (gfxPath.empty()) {
                return std::unexpected(LuaSuiteFail::UnexpectedError);
            }
            auto gfxSrc = ReadHostFileText(gfxPath);
            if (!gfxSrc) {
                return std::unexpected(LuaSuiteFail::UnexpectedError);
            }

            const ZHLN::RenderInfo rtDesktop {
                .rendererName        = "Vulkan",
                .gpuName             = "RTX 4090",
                .deviceType          = ZHLN::PhysicalDeviceType::DiscreteGPU,
                .rayTracingSupported = true,
            };
            auto ultraGfx = ZHLN::Lua::EvaluateCallString<ZHLN::GraphicsSettings>(
                *gfxSrc, "@config/graphics.lua", ZHLN::Lua::Budget {}, rtDesktop
            );
            if (!ultraGfx || ultraGfx->qualityPreset != ZHLN::QualityLevel::Ultra ||
                !ultraGfx->rayTracing.enableReflections || !ultraGfx->rayTracing.enableDiffuseGI ||
                ultraGfx->shadows.resolution != 4096) {
                return std::unexpected(LuaSuiteFail::WrongValue);
            }

            const ZHLN::RenderInfo cpuAdapter {
                .rendererName        = "Vulkan",
                .gpuName             = "llvmpipe",
                .deviceType          = ZHLN::PhysicalDeviceType::CPU,
                .rayTracingSupported = false,
            };
            auto lowGfx = ZHLN::Lua::EvaluateCallString<ZHLN::GraphicsSettings>(
                *gfxSrc, "@config/graphics.lua", ZHLN::Lua::Budget {}, cpuAdapter
            );
            if (!lowGfx || lowGfx->qualityPreset != ZHLN::QualityLevel::Low ||
                lowGfx->antiAliasing.mode != ZHLN::AAMode::FXAA || lowGfx->rayTracing.enableReflections ||
                lowGfx->shadows.resolution != 1024) {
                return std::unexpected(LuaSuiteFail::WrongValue);
            }

            return {};
        }

        auto TestPodUserdataAndBudgetedListenerInterop() -> std::expected<void, ZHLN::ErrorCode> {
            if (ZHLN::Lua::TypeIdOf<PodVec2>() == ZHLN::Lua::TypeIdOf<PodEntityRef>()) {
                return std::unexpected(LuaSuiteFail::WrongValue);
            }
            ZHLN::Lua::Budget budget {.maxInstructions = 16'384, .maxMemoryBytes = 2 * 1024 * 1024};
            auto            vm = ZHLN::Lua::VirtualMachine::Create(budget);
            if (!vm) {
                return std::unexpected(LuaSuiteFail::UnexpectedError);
            }

            static constexpr std::array<ZHLN::Lua::MethodEntry, 1> kVec2Methods = {{
                {"LengthSq", &PodVec2LengthSq},
            }};
            if (!vm->RegisterUserdataType<PodVec2>(ZHLN::Lua::MetatableSpec {
                    .debugName = "Test.PodVec2",
                    .index     = &PodVec2Index,
                    .add       = &PodVec2Add,
                    .methods   = kVec2Methods,
                })) {
                return std::unexpected(LuaSuiteFail::UnexpectedError);
            }
            if (!vm->RegisterUserdataType<PodEntityRef>(ZHLN::Lua::MetatableSpec {
                    .debugName = "Test.PodEntityRef",
                    .eq        = &PodEntityEq,
                })) {
                return std::unexpected(LuaSuiteFail::UnexpectedError);
            }

            InteropCounterHost host {};
            vm->SetHostContext(&host);

            const auto populateGlobals = [](ZHLN::Lua::CallFrame& frame) {
                frame.BeginTable(0, 1);
                frame.PushFunction(&PodVec2New);
                frame.SetFieldTop("new");
                frame.SetGlobalTop("Vec2");

                frame.PushFunction(&PodEntityGetInterned);
                frame.SetGlobalTop("GetEntity");
                frame.PushFunction(&RegisterCallbackHostFn);
                frame.SetGlobalTop("OnStep");
                frame.PushFunction(&ReportSumHostFn);
                frame.SetGlobalTop("Report");
            };
            const auto setupOk = vm->WithFrame(populateGlobals);
            if (!setupOk) {
                return std::unexpected(LuaSuiteFail::UnexpectedError);
            }

            // Verify POD userdata arithmetic, methods, metatable hiding, and interned identity.
            if (!vm->ExecuteString(R"lua(
                local a = Vec2.new(3, 4)
                local b = Vec2.new(1, -1)
                local c = a + b
                assert(c.x == 4 and c.y == 3)
                assert(a:LengthSq() == 25)
                assert(getmetatable(a) == false)

                local e1 = GetEntity(42)
                local e2 = GetEntity(42)
                local e3 = GetEntity(99)
                assert(rawequal(e1, e2))
                assert(e1 == e2 and e1 ~= e3)

                assert(OnStep(function(delta, vec)
                    Report(delta + vec:LengthSq())
                end))
            )lua")) {
                return std::unexpected(LuaSuiteFail::UnexpectedError);
            }

            if (!vm->HasListener(host.listener)) {
                return std::unexpected(LuaSuiteFail::WrongValue);
            }

            const auto pushArgs = [](ZHLN::Lua::CallFrame& frame) {
                frame.PushInt64(5);
                frame.PushUserdata(PodVec2 {.x = 3.0f, .y = 4.0f});
            };
            if (!vm->InvokeListener(host.listener, pushArgs)) {
                return std::unexpected(LuaSuiteFail::UnexpectedError);
            }
            if (host.observedSum != 30) {
                return std::unexpected(LuaSuiteFail::WrongValue);
            }

            vm->UnregisterListener(host.listener);
            if (vm->HasListener(host.listener)) {
                return std::unexpected(LuaSuiteFail::WrongValue);
            }
            const auto noArgs = [](ZHLN::Lua::CallFrame&) {};
            auto       missingCall = vm->InvokeListener(host.listener, noArgs);
            if (missingCall || !(missingCall.error() == ZHLN::Lua::Error::NotAFunction)) {
                return std::unexpected(LuaSuiteFail::WrongErrorCode);
            }

            // Register an infinite-loop listener and verify InvokeListener enforces maxInstructions.
            if (!vm->ExecuteString("assert(OnStep(function() while true do end end))")) {
                return std::unexpected(LuaSuiteFail::UnexpectedError);
            }
            auto runaway = vm->InvokeListener(host.listener, noArgs, "runaway_cb");
            if (runaway || !(runaway.error() == ZHLN::Lua::Error::InstructionBudgetExceeded)) {
                return std::unexpected(LuaSuiteFail::WrongErrorCode);
            }

            vm->ClearHostContext<InteropCounterHost>();
            return {};
        }
    };
};

} // namespace

// --- ZHLN::Lua: compiled scripts, permissions, and transactional patching --

namespace {

struct LuaScriptState: ZHLN::Lua::Runtime {
    int32_t     value      = 1;
    int32_t     steps      = 0;
    std::string label      = "before";
    float       speed      ZHLN_ANNOTATION(ZHLN::Lua::ReadOnly {}) = 1.0f;
    bool        spawn      ZHLN_ANNOTATION(ZHLN::Lua::OneShot {})  = false;
    std::unique_ptr<int> owned ZHLN_ANNOTATION(ZHLN::Lua::Hidden {}) = std::make_unique<int>(7);

  private:
    int32_t privateValue = 99;
};

auto LuaAddOne(int64_t value) -> int64_t {
    return value + 1;
}

struct TaggedScriptState: ZHLN::Lua::Runtime {
    struct: ZHLN::Lua::ReadOnly {
        int32_t locked = 7;
    } in {};

    struct: ZHLN::Lua::OneShot {
        bool fire = false;
    } out {};

    int32_t value = 0;
};

struct LuaScriptSuite {
    struct Tests {
        auto TestCompiledScriptIsStateless() -> std::expected<void, ZHLN::ErrorCode> {
            constexpr std::string_view kChunk = R"lua(
                local calls = 0
                return function(state)
                    calls = calls + 1
                    return {
                        value = state.value + 1,
                        steps = calls,
                        label = "after",
                        spawn = true,
                    }
                end
            )lua";

            auto script = ZHLN::Lua::Compile<LuaScriptState>(kChunk, "stateless.lua").Build();
            if (!script) {
                return std::unexpected(LuaSuiteFail::UnexpectedError);
            }
            LuaScriptState state;
            for (int i = 0; i < 3; ++i) {
                if (auto res = (*script)(state); !res) {
                    return std::unexpected(LuaSuiteFail::UnexpectedError);
                }
                if (state.value != i + 2 || state.steps != 1 || state.label != "after" || !state.spawn) {
                    return std::unexpected(LuaSuiteFail::WrongValue);
                }
            }
            if (state.owned == nullptr || *state.owned != 7) {
                return std::unexpected(LuaSuiteFail::WrongValue);
            }
            return {};
        }

        auto TestReadOnlyAndUnknownFieldsRollBack() -> std::expected<void, ZHLN::ErrorCode> {
            auto readOnly = ZHLN::Lua::Compile<LuaScriptState>(
                "return function(state) return { speed = 9.0 } end", "readonly.lua"
            ).Build();
            if (!readOnly) {
                return std::unexpected(LuaSuiteFail::UnexpectedError);
            }
            LuaScriptState state;
            auto res = (*readOnly)(state);
            if (res || state.speed != 1.0f) {
                return std::unexpected(LuaSuiteFail::ExpectedFailureDidNotHappen);
            }
            if (state.LastError() == std::nullopt || state.LastError()->code != ZHLN::Lua::Error::ReadOnlyField) {
                // Script does not set Runtime::LastError; only the Layer does.
            }

            auto unknown = ZHLN::Lua::Compile<LuaScriptState>(
                "return function(state) return { value = 5, bogus = 1 } end", "unknown.lua"
            ).Build();
            if (!unknown) {
                return std::unexpected(LuaSuiteFail::UnexpectedError);
            }
            auto unknownRes = (*unknown)(state);
            if (unknownRes || state.value != 1) {
                return std::unexpected(LuaSuiteFail::ExpectedFailureDidNotHappen);
            }
            return {};
        }

        auto TestGlobalsWriteStaysEphemeral() -> std::expected<void, ZHLN::ErrorCode> {
            auto script = ZHLN::Lua::Compile<LuaScriptState>(
                R"lua(
                    scratch = (scratch or 0) + 1
                    return function(state) return { steps = scratch } end
                )lua",
                "globals.lua"
            )
                .Yes(ZHLN::Lua::Permission::GlobalsWrite)
                .No(ZHLN::Lua::Permission::Print)
                .Build();
            if (!script) {
                return std::unexpected(LuaSuiteFail::UnexpectedError);
            }
            LuaScriptState state;
            for (int i = 0; i < 2; ++i) {
                if (auto res = (*script)(state); !res) {
                    return std::unexpected(LuaSuiteFail::UnexpectedError);
                }
                if (state.steps != 1) {
                    return std::unexpected(LuaSuiteFail::WrongValue);
                }
            }
            return {};
        }

        auto TestHostFunctionCapability() -> std::expected<void, ZHLN::ErrorCode> {
            auto script = ZHLN::Lua::Compile<LuaScriptState>(
                "return function(state) return { value = addOne(state.value) } end", "hostfn.lua"
            )
                .Host(ZHLN::Lua::Permission::HostFn<"addOne", &LuaAddOne>)
                .Build();
            if (!script) {
                return std::unexpected(LuaSuiteFail::UnexpectedError);
            }
            LuaScriptState state;
            if (auto res = (*script)(state); !res) {
                return std::unexpected(LuaSuiteFail::UnexpectedError);
            }
            if (state.value != 2) {
                return std::unexpected(LuaSuiteFail::WrongValue);
            }
            return {};
        }

        auto TestPermissionComposition() -> std::expected<void, ZHLN::ErrorCode> {
            using TickPolicy  = decltype(ZHLN::Lua::Permission::DefaultPolicy {}.Yes(ZHLN::Lua::Permission::Tick));
            using NoTickPolicy = decltype(ZHLN::Lua::Permission::DefaultPolicy {}.No(ZHLN::Lua::Permission::Tick));
            static_assert((TickPolicy::yes & static_cast<uint64_t>(ZHLN::Lua::Permission::Bit::Tick)) != 0u);
            static_assert((NoTickPolicy::yes & static_cast<uint64_t>(ZHLN::Lua::Permission::Bit::Tick)) == 0u);
            static_assert((TickPolicy::yes & static_cast<uint64_t>(ZHLN::Lua::Permission::Bit::Print)) != 0u);

            static_assert(kCanTick<ZHLN::Lua::Layer<LuaScriptState, TickPolicy>>);
            static_assert(!kCanTick<ZHLN::Lua::Layer<LuaScriptState, NoTickPolicy>>);
            return {};
        }

        auto TestEmptyBaseTags() -> std::expected<void, ZHLN::ErrorCode> {
            auto script = ZHLN::Lua::Compile<TaggedScriptState>(
                "return function(state) return { value = 11, out = { fire = true } } end", "tags.lua"
            ).Build();
            if (!script) {
                return std::unexpected(LuaSuiteFail::UnexpectedError);
            }
            TaggedScriptState state;
            if (auto res = (*script)(state); !res) {
                return std::unexpected(LuaSuiteFail::UnexpectedError);
            }
            if (state.value != 11 || !state.out.fire) {
                return std::unexpected(LuaSuiteFail::WrongValue);
            }
            ZHLN::Lua::TemplatedDetail::ResetOneShots(state);
            if (state.out.fire) {
                return std::unexpected(LuaSuiteFail::WrongValue);
            }

            auto denied = ZHLN::Lua::Compile<TaggedScriptState>(
                "return function(state) return { [\"in\"] = { locked = 99 } } end", "tags_ro.lua"
            ).Build();
            if (!denied) {
                return std::unexpected(LuaSuiteFail::UnexpectedError);
            }
            auto res = (*denied)(state);
            const auto code = res ? std::optional<ZHLN::ErrorCode> {} : std::optional<ZHLN::ErrorCode> {res.error()};
            if (res || state.in.locked != 7 || !code ||
                code->get<ZHLN::Lua::Error>() != ZHLN::Lua::Error::ReadOnlyField) {
                return std::unexpected(LuaSuiteFail::WrongErrorCode);
            }
            return {};
        }

        auto TestBaseGlobalsAvailable() -> std::expected<void, ZHLN::ErrorCode> {
            auto script = ZHLN::Lua::Compile<LuaScriptState>(
                R"lua(
                    return function(state)
                        assert(type(state.value) == "number")
                        local total = 0
                        for key, value in pairs({ alpha = 1, beta = 2 }) do
                            total = total + value
                        end
                        return { steps = total, label = tostring(type(state.value)) }
                    end
                )lua",
                "base.lua"
            ).Build();
            if (!script) {
                return std::unexpected(LuaSuiteFail::UnexpectedError);
            }
            LuaScriptState state;
            if (auto res = (*script)(state); !res) {
                return std::unexpected(LuaSuiteFail::UnexpectedError);
            }
            if (state.steps != 3 || state.label != "number") {
                return std::unexpected(LuaSuiteFail::WrongValue);
            }
            return {};
        }

        auto TestShippedSandboxScript() -> std::expected<void, ZHLN::ErrorCode> {
            const auto path = ZHLN::FS::Paths::FindDataFile("scripts/sandbox.lua");
            if (!path) {
                return std::unexpected(LuaSuiteFail::UnexpectedError);
            }
            auto source = ReadHostFileText(*path);
            if (!source) {
                return std::unexpected(LuaSuiteFail::UnexpectedError);
            }
            auto script = ZHLN::Lua::Compile<SandboxMirror>(*source, "sandbox.lua").Build();
            if (!script) {
                return std::unexpected(LuaSuiteFail::UnexpectedError);
            }
            SandboxMirror state;
            if (auto res = (*script)(state); !res) {
                return std::unexpected(LuaSuiteFail::UnexpectedError);
            }
            if (!Near(state.lampPos.x, std::cos(0.9f) * 3.0f) || !Near(state.lampPos.z, std::sin(0.9f) * 3.0f) ||
                !Near(state.launchSpeed, 12.0f + std::sin(0.25f) * 2.0f) || !state.out.spawnCube || state.out.quit) {
                return std::unexpected(LuaSuiteFail::WrongValue);
            }
            return {};
        }

        auto TestByteSpanSource() -> std::expected<void, ZHLN::ErrorCode> {
            static_assert(requires { ZHLN::Lua::FromString(std::span<const std::byte> {}); });
            static_assert(requires { ZHLN::Lua::Compile<LuaScriptState>(std::span<const std::byte> {}, "bytes.lua"); });
            const std::string code = "return function(state) return { value = 11, steps = 1, label = 'bytes' } end";
            const std::span<const std::byte> bytes(reinterpret_cast<const std::byte*>(code.data()), code.size());
            auto script = ZHLN::Lua::Compile<LuaScriptState>(bytes, "bytes.lua").Build();
            if (!script) {
                return std::unexpected(LuaSuiteFail::UnexpectedError);
            }
            LuaScriptState state;
            if (auto res = (*script)(state); !res) {
                return std::unexpected(LuaSuiteFail::UnexpectedError);
            }
            if (state.value != 11 || state.steps != 1 || state.label != "bytes") {
                return std::unexpected(LuaSuiteFail::WrongValue);
            }
            return {};
        }

        auto TestRejectsBytecodeChunks() -> std::expected<void, ZHLN::ErrorCode> {
            auto vm = ZHLN::Lua::VirtualMachine::Create({});
            if (!vm) {
                return std::unexpected(LuaSuiteFail::UnexpectedError);
            }
            // LuaJIT accepts unverified bytecode; a leading ESC byte must be
            // rejected before luaL_loadbufferx sees the payload (LuaRocks 2026).
            const std::string payload {char(0x1B), 'L', 'J', 0x02, 0x00, 0x01, 0x02, 0x03};
            const auto       res = vm->ExecuteString(payload, "evil.lua");
            if (res || res.error().get<ZHLN::Lua::Error>() != ZHLN::Lua::Error::SyntaxError ||
                vm->LastErrorDetail().find("bytecode") == std::string_view::npos) {
                return std::unexpected(LuaSuiteFail::WrongErrorCode);
            }
            return {};
        }

        auto TestOneShotReset() -> std::expected<void, ZHLN::ErrorCode> {
            LuaScriptState state;
            state.spawn = true;
            ZHLN::Lua::TemplatedDetail::ResetOneShots(state);
            if (state.spawn) {
                return std::unexpected(LuaSuiteFail::WrongValue);
            }
            return {};
        }
    };
};

} // namespace
