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
#include <Zahlen/COM/IActionContext.hpp>
#include <Zahlen/COM/IAudioDevice.hpp>
#include <Zahlen/COM/IEntities.hpp>
#include <Zahlen/COM/IGraphicsDevice.hpp>
#include <Zahlen/COM/ISimulation.hpp>
#include <Zahlen/COM/IState.hpp>
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

struct SandboxMirror {
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
        if (!ZHLN::Test::ExpectFalse(!res)) {
            return std::unexpected(res.error());
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
        if (!ZHLN::Test::ExpectFalse(!scene)) {
            return std::unexpected(scene.error());
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
        if (!ZHLN::Test::ExpectFalse(!hi)) {
            return std::unexpected(hi.error());
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
        if (!ZHLN::Test::ExpectFalse(!lo)) {
            return std::unexpected(lo.error());
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
        if (auto ok = ZHLN::Lua::ApplyPatchString(live, "return { post = { bloomStrength = 0.18 } }"); !ZHLN::Test::ExpectFalse(!ok)) {
            return std::unexpected(ok.error());
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
        if (!(unknownRes.error() == ZHLN::Lua::Error::UnknownField) || !Near(live.post.exposure, 2.2f)) {
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
            ); !ZHLN::Test::ExpectFalse(!vecOk)) {
            return std::unexpected(vecOk.error());
        }
        if (!Near(envComp.ambientExposure, 1.5f) || !Near(envComp.probeMin.y, 0.8f) || !Near(envComp.skyZenith.z, 0.3f)) {
            return std::unexpected(LuaSuiteFail::WrongValue);
        }

        // Named and short sequence patches preserve untouched coordinates;
        // an invalid coordinate rolls back the entire struct.
        if (auto ok = ZHLN::Lua::ApplyPatchString(
                envComp, "return { probeMin = { x = 0.4 }, skyZenith = { 0.6 } }"); !ZHLN::Test::ExpectFalse(!ok)) {
            return std::unexpected(ok.error());
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
        if (bad || !(bad.error() == ZHLN::Lua::Error::TypeMismatch) || !Near(color.x, 7.0f) || !Near(color.y, 8.0f))
            return std::unexpected(LuaSuiteFail::WrongErrorCode);
        auto longSeq = ZHLN::Lua::ApplyPatchString(color, "return { 1, 2, 3, 4, 5 }");
        if (longSeq || !(longSeq.error() == ZHLN::Lua::Error::OutOfRange) || !Near(color.x, 7.0f))
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
        if (bad || !(bad.error() == ZHLN::Lua::Error::OutOfRange) || (*matrix)[0, 1] != 5.0f)
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
        if (!ZHLN::Test::ExpectFalse(!probe)) {
            return std::unexpected(probe.error());
        }
        if (!ZHLN::Test::ExpectFalse(probe->iterations != 7)) {
            return std::unexpected(LuaSuiteFail::WrongValue);
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
            // Deliberately goes through the runtime's own search path: if the
            // build did not stage config/ next to the binary, this is where that
            // shows up, and FileReadFailed names it instead of hiding it.
            const auto gfxFound = ZHLN::FS::Paths::FindDataFile("config/graphics.lua");
            if (!ZHLN::Test::ExpectFalse(!gfxFound)) {
                return std::unexpected(ZHLN::Lua::Error::FileReadFailed);
            }
            const std::filesystem::path gfxPath = *gfxFound;
            auto gfxSrc = ReadHostFileText(gfxPath);
            if (!ZHLN::Test::ExpectFalse(!gfxSrc)) {
                return std::unexpected(gfxSrc.error());
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
            if (!ZHLN::Test::ExpectFalse(!vm)) {
                return std::unexpected(vm.error());
            }

            static constexpr std::array<ZHLN::Lua::MethodEntry, 1> kVec2Methods = {{
                {"LengthSq", &PodVec2LengthSq},
            }};
            if (auto vec2Reg = vm->RegisterUserdataType<PodVec2>(ZHLN::Lua::MetatableSpec {
                    .debugName = "Test.PodVec2",
                    .index     = &PodVec2Index,
                    .add       = &PodVec2Add,
                    .methods   = kVec2Methods,
                }); !ZHLN::Test::ExpectFalse(!vec2Reg)) {
                return std::unexpected(vec2Reg.error());
            }
            if (auto entityReg = vm->RegisterUserdataType<PodEntityRef>(ZHLN::Lua::MetatableSpec {
                    .debugName = "Test.PodEntityRef",
                    .eq        = &PodEntityEq,
                }); !ZHLN::Test::ExpectFalse(!entityReg)) {
                return std::unexpected(entityReg.error());
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
            if (!ZHLN::Test::ExpectFalse(!setupOk)) {
                return std::unexpected(setupOk.error());
            }

            // Verify POD userdata arithmetic, methods, metatable hiding, and interned identity.
            if (auto execRes = vm->ExecuteString(R"lua(
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
            )lua"); !ZHLN::Test::ExpectFalse(!execRes)) {
                return std::unexpected(execRes.error());
            }

            if (!vm->HasListener(host.listener)) {
                return std::unexpected(LuaSuiteFail::WrongValue);
            }

            const auto pushArgs = [](ZHLN::Lua::CallFrame& frame) {
                frame.PushInt64(5);
                frame.PushUserdata(PodVec2 {.x = 3.0f, .y = 4.0f});
            };
            if (auto invokeRes = vm->InvokeListener(host.listener, pushArgs); !ZHLN::Test::ExpectFalse(!invokeRes)) {
                return std::unexpected(invokeRes.error());
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
            if (auto execRes2 = vm->ExecuteString("assert(OnStep(function() while true do end end))"); !ZHLN::Test::ExpectFalse(!execRes2)) {
                return std::unexpected(execRes2.error());
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

// A plain aggregate, on purpose: the script boundary is data, and the host
// half (if any) is a separate Runtime<State>. Each annotation member exercises
// one branch of PushState/StageObject.
struct LuaScriptState {
    int32_t                    value                                   = 1;
    int32_t                    steps                                   = 0;
    std::string                label                                   = "before";
    float speed                ZHLN_ANNOTATION(ZHLN::Lua::ReadOnly {}) = 1.0f;
    bool spawn                 ZHLN_ANNOTATION(ZHLN::Lua::OneShot {})  = false;
    std::unique_ptr<int> owned ZHLN_ANNOTATION(ZHLN::Lua::Hidden {}) = std::make_unique<int>(7);

  private:
    int32_t privateValue = 99;
};

auto LuaAddOne(int64_t value) -> int64_t {
    return value + 1;
}

struct TaggedScriptState {
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
            if (!ZHLN::Test::ExpectFalse(!script)) {
                return std::unexpected(script.error());
            }
            LuaScriptState state;
            for (int i = 0; i < 3; ++i) {
                if (auto res = (*script)(state); !ZHLN::Test::ExpectFalse(!res)) {
                    return std::unexpected(res.error());
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
            if (!ZHLN::Test::ExpectFalse(!readOnly)) {
                return std::unexpected(readOnly.error());
            }
            LuaScriptState state;
            ZHLN::Lua::ScriptDiagnostics diagnostics;
            auto                        res = (*readOnly)(state, diagnostics);
            if (res || state.speed != 1.0f) {
                return std::unexpected(LuaSuiteFail::ExpectedFailureDidNotHappen);
            }
            // The failure carries everything: the code AND the field that was
            // refused. Nothing is cached on the Script, so this is the only
            // place the reason exists.
            if (res.error().get<ZHLN::Lua::Error>() != ZHLN::Lua::Error::ReadOnlyField) {
                return std::unexpected(LuaSuiteFail::WrongErrorCode);
            }
            if (diagnostics.detail.find("speed") == std::string::npos) {
                return std::unexpected(LuaSuiteFail::WrongValue);
            }

            auto unknown = ZHLN::Lua::Compile<LuaScriptState>(
                "return function(state) return { value = 5, bogus = 1 } end", "unknown.lua"
            ).Build();
            if (!ZHLN::Test::ExpectFalse(!unknown)) {
                return std::unexpected(unknown.error());
            }
            ZHLN::Lua::ScriptDiagnostics unknownDiagnostics;
            auto                        unknownRes = (*unknown)(state, unknownDiagnostics);
            if (unknownRes || state.value != 1) {
                return std::unexpected(LuaSuiteFail::ExpectedFailureDidNotHappen);
            }
            // A sibling field staged in the same table must not have landed:
            // the whole patch is transactional.
            if (unknownRes.error().get<ZHLN::Lua::Error>() != ZHLN::Lua::Error::UnknownField ||
                unknownDiagnostics.detail.find("bogus") == std::string::npos) {
                return std::unexpected(LuaSuiteFail::WrongErrorCode);
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
            if (!ZHLN::Test::ExpectFalse(!script)) {
                return std::unexpected(script.error());
            }
            LuaScriptState state;
            for (int i = 0; i < 2; ++i) {
                if (auto res = (*script)(state); !ZHLN::Test::ExpectFalse(!res)) {
                    return std::unexpected(res.error());
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
            if (!ZHLN::Test::ExpectFalse(!script)) {
                return std::unexpected(script.error());
            }
            LuaScriptState state;
            if (auto res = (*script)(state); !ZHLN::Test::ExpectFalse(!res)) {
                return std::unexpected(res.error());
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

            static_assert(kCanTick<ZHLN::Lua::Layer<LuaScriptState, void, TickPolicy>>);
            static_assert(!kCanTick<ZHLN::Lua::Layer<LuaScriptState, void, NoTickPolicy>>);
            return {};
        }

        auto TestEmptyBaseTags() -> std::expected<void, ZHLN::ErrorCode> {
            auto script = ZHLN::Lua::Compile<TaggedScriptState>(
                "return function(state) return { value = 11, out = { fire = true } } end", "tags.lua"
            ).Build();
            if (!ZHLN::Test::ExpectFalse(!script)) {
                return std::unexpected(script.error());
            }
            TaggedScriptState state;
            if (auto res = (*script)(state); !ZHLN::Test::ExpectFalse(!res)) {
                return std::unexpected(res.error());
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
            if (!ZHLN::Test::ExpectFalse(!denied)) {
                return std::unexpected(denied.error());
            }
            ZHLN::Lua::ScriptDiagnostics deniedDiagnostics;
            auto                        res = (*denied)(state, deniedDiagnostics);
            if (res || state.in.locked != 7) {
                return std::unexpected(LuaSuiteFail::WrongErrorCode);
            }
            // The refused field is the read-only *block* (`in`), not the leaf
            // it tried to reach: assignability is decided per member of the
            // state, and a ReadOnly-derived sub-struct is refused as a whole.
            if (res.error().get<ZHLN::Lua::Error>() != ZHLN::Lua::Error::ReadOnlyField ||
                deniedDiagnostics.detail.find("'in'") == std::string::npos) {
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
            if (!ZHLN::Test::ExpectFalse(!script)) {
                return std::unexpected(script.error());
            }
            LuaScriptState state;
            if (auto res = (*script)(state); !ZHLN::Test::ExpectFalse(!res)) {
                return std::unexpected(res.error());
            }
            if (state.steps != 3 || state.label != "number") {
                return std::unexpected(LuaSuiteFail::WrongValue);
            }
            return {};
        }

        auto TestShippedSandboxScript() -> std::expected<void, ZHLN::ErrorCode> {
            const auto path = ZHLN::FS::Paths::FindDataFile("scripts/sandbox.lua");
            if (!ZHLN::Test::ExpectFalse(!path)) {
                // FindDataFile returns std::optional: absence IS the diagnosis --
                // the shipped script is not where the runtime looks for it.
                return std::unexpected(ZHLN::Lua::Error::FileReadFailed);
            }
            auto source = ReadHostFileText(*path);
            if (!ZHLN::Test::ExpectFalse(!source)) {
                return std::unexpected(source.error());
            }
            auto script = ZHLN::Lua::Compile<SandboxMirror>(*source, "sandbox.lua").Build();
            if (!ZHLN::Test::ExpectFalse(!script)) {
                return std::unexpected(script.error());
            }
            SandboxMirror state;
            if (auto res = (*script)(state); !ZHLN::Test::ExpectFalse(!res)) {
                return std::unexpected(res.error());
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
            if (!ZHLN::Test::ExpectFalse(!script)) {
                return std::unexpected(script.error());
            }
            LuaScriptState state;
            if (auto res = (*script)(state); !ZHLN::Test::ExpectFalse(!res)) {
                return std::unexpected(res.error());
            }
            if (state.value != 11 || state.steps != 1 || state.label != "bytes") {
                return std::unexpected(LuaSuiteFail::WrongValue);
            }
            return {};
        }

        auto TestRejectsBytecodeChunks() -> std::expected<void, ZHLN::ErrorCode> {
            auto vm = ZHLN::Lua::VirtualMachine::Create({});
            if (!ZHLN::Test::ExpectFalse(!vm)) {
                return std::unexpected(vm.error());
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

// --- ZHLN::Lua: hosts, phases, and error policy ---------------------------
//
// These cases drive the layer through a CPU-only stub engine. The layer is a
// thin coordinator over four things that are easy to get wrong and invisible
// from the outside: which phase failed, whether a reload re-enters the host,
// whether a swallowed error still reaches the registry, and whether a botched
// hot reload destroys the last good chunk. Each case below names one.

// A minimal IEngine: IObject's three virtuals come from COM::Implements, and
// everything the Lua layer touches (Errors, Logger, Assets) is a null-safe
// stub. Nothing here needs a GPU, a window, or a physics world.
struct StubEngine final: ZHLN::COM::Implements<StubEngine, ZHLN::COM::IEngine> {
    ZHLN::COM::Ref<ZHLN::COM::IErrorRegistry> errors;

    StubEngine(): errors(*ZHLN::COM::CreateErrorRegistry()) {
    }

    bool IsRunning() const noexcept override {
        return true;
    }
    void ProcessEvents() noexcept override {
    }
    void RequestClose() noexcept override {
    }
    ZHLN::COM::Result<void> Tick(float) noexcept override {
        return {};
    }
    void ClearScene() noexcept override {
    }
    ZHLN::COM::ILogger* Logger() noexcept override {
        return nullptr;
    }
    ZHLN::COM::IErrorRegistry* Errors() noexcept override {
        return errors.Get();
    }
    ZHLN::COM::IGraphicsDevice* Graphics() noexcept override {
        return nullptr;
    }
    ZHLN::COM::IEntities* Entities() noexcept override {
        return nullptr;
    }
    ZHLN::COM::ISimulation* Simulation() noexcept override {
        return nullptr;
    }
    ZHLN::COM::IAudioDevice* Audio() noexcept override {
        return nullptr;
    }
    ZHLN::COM::IAssetLibrary* Assets() noexcept override {
        return nullptr;
    }
    ZHLN::COM::IActionContext* Actions() noexcept override {
        return nullptr;
    }
    ZHLN::COM::IState* State() noexcept override {
        return nullptr;
    }
    ZHLN::COM::IDebugDraw* Debug() noexcept override {
        return nullptr;
    }
    void SetMaxFramesPerSecond(uint32_t) noexcept override {
    }
    uint32_t MaxFramesPerSecond() const noexcept override {
        return 0;
    }
    uint64_t CurrentFrame() const noexcept override {
        return 0;
    }
    uint64_t DeviceGeneration() const noexcept override {
        return 0;
    }
    ZHLN::COM::Result<void> RecreateGraphicsDevice() noexcept override {
        return {};
    }
    ZHLN::COM::Result<uint64_t> AddDeviceLostCallback(void (*)(void*), void*) noexcept override {
        return uint64_t {0};
    }
    void RemoveDeviceLostCallback(uint64_t) noexcept override {
    }
    ZHLN::WindowID PrimaryWindow() const noexcept override {
        return {};
    }
    uint32_t WindowsRaw(ZHLN::WindowID*, uint32_t) const noexcept override {
        return 0;
    }
    ZHLN::Extent2D WindowExtent(ZHLN::WindowID) const noexcept override {
        return {};
    }
    bool IsWindowFocused(ZHLN::WindowID) const noexcept override {
        return false;
    }
    ZHLN::COM::Result<ZHLN::WindowID> AddWindow(const ZHLN::COM::ChainInNode*) noexcept override {
        return ZHLN::WindowID {};
    }
    void RemoveWindow(ZHLN::WindowID) noexcept override {
    }
    bool IsWindowRunning(ZHLN::WindowID) const noexcept override {
        return false;
    }
    void CloseWindow(ZHLN::WindowID) noexcept override {
    }
    void FocusWindow(ZHLN::WindowID) noexcept override {
    }
    void SetCursorMode(ZHLN::WindowID, ZHLN::CursorMode) noexcept override {
    }
    ZHLN::CursorMode GetCursorMode(ZHLN::WindowID) const noexcept override {
        return ZHLN::CursorMode::System;
    }
    void CaptureMouse(ZHLN::WindowID, bool) noexcept override {
    }
    bool IsMouseCaptured(ZHLN::WindowID) const noexcept override {
        return false;
    }
    ZHLN::COM::Result<void> SetClipboardText(ZHLN::COM::StringView) noexcept override {
        return {};
    }
    ZHLN::COM::Result<uint32_t> ClipboardTextRaw(char*, uint32_t) const noexcept override {
        return uint32_t {0};
    }
    ZHLN::COM::Result<void> SetFileDropCallback(ZHLN::WindowID, ZHLN::COM::FileDropCallback, void*) noexcept override {
        return {};
    }
    ZHLN::COM::Result<void> ReadHostInput(ZHLN::WindowID, ZHLN::COM::ChainOutNode*, bool) noexcept override {
        return {};
    }
    ZHLN::COM::Result<void> SubmitUI(ZHLN::WindowID, const ZHLN::COM::ChainInNode*) noexcept override {
        return {};
    }
};

// The detail the layer attached to the last recorded error code.
[[nodiscard]] auto RecordedDetail(ZHLN::COM::IErrorRegistry& registry) -> std::string {
    ZHLN::COM::ErrorRecord record {};
    if (!registry.LastError(ZHLN::COM::OutChain(record).Head()).HasValue()) {
        return {};
    }
    return std::string(std::string_view(record.detail));
}

struct HostState {
    struct: ZHLN::Lua::ReadOnly {
        float elapsed = 0.0f;
    } in {};

    struct: ZHLN::Lua::OneShot {
        bool spawn = false;
    } out {};

    int32_t ticks = 0;
};

// Every hook the layer can call, recording what it saw. `final` is part of the
// contract: the layer only binds a final host, and only ever calls the hooks
// through the qualified `host.HostT::On...` form.
//
// OnPrepare is deliberately three-argument: the layer forwards the frame delta
// so a host can accumulate elapsed time, which is exactly what the sandbox does.
struct RecordingHost final: ZHLN::Lua::Runtime<HostState> {
    struct Journal {
        int              starts          = 0;
        int              prepares        = 0;
        int              commits         = 0;
        int              stops           = 0;
        int              errors          = 0;
        float            lastDt          = -1.0f;
        float            elapsedAtCommit = -1.0f;
        ZHLN::Lua::Phase lastPhase       = ZHLN::Lua::Phase::Start;
        ZHLN::ErrorCode  lastCode {};
        std::string      lastDetail;
        bool             preparedWithMutableState = false;
        bool             startedWithSpawn         = false;
    };

    explicit RecordingHost(Journal& journal): _journal(&journal) {
    }

    // Two-argument: the layer hands over the state the startup patch produced,
    // so a host can read what the script authored before the first frame --
    // here the `out.spawn` one-shot the chunk set, which is the state a real
    // host acts on.
    void OnStart(ZHLN::COM::IEngine& engine, HostState& state) override {
        (void) engine;
        ++_journal->starts;
        _journal->startedWithSpawn = state.out.spawn;
    }
    void OnPrepare(ZHLN::COM::IEngine& engine, HostState& state, float dt) override {
        (void) engine;
        ++_journal->prepares;
        _journal->lastDt                   = dt;
        _journal->preparedWithMutableState = true;
        state.in.elapsed += dt;
    }
    void OnCommit(ZHLN::COM::IEngine& engine, const HostState& state) override {
        (void) engine;
        ++_journal->commits;
        _journal->elapsedAtCommit = state.in.elapsed;
    }
    void OnStop(ZHLN::COM::IEngine& engine) override {
        (void) engine;
        ++_journal->stops;
    }
    void OnError(ZHLN::COM::IEngine& engine, const ZHLN::Lua::Failure& failure) override {
        (void) engine;
        ++_journal->errors;
        _journal->lastPhase   = failure.phase;
        _journal->lastCode    = failure.code;
        _journal->lastDetail  = std::string(failure.detail);
    }

  private:
    Journal* _journal = nullptr;
};

struct LuaLayerSuite {
    struct Tests {
        auto TestPhaseEnumAndNames() -> std::expected<void, ZHLN::ErrorCode> {
            // Phase is the wire vocabulary between the layer and a host; the
            // names are what an error message and the registry carry.
            static_assert(std::is_same_v<std::underlying_type_t<ZHLN::Lua::Phase>, uint8_t>);
            constexpr std::array<ZHLN::Lua::Phase, 5> kAll {
                ZHLN::Lua::Phase::Start, ZHLN::Lua::Phase::Tick, ZHLN::Lua::Phase::BeforeRender, ZHLN::Lua::Phase::AfterTick, ZHLN::Lua::Phase::HotReload,
            };
            for (const auto phase: kAll) {
                if (ZHLN::Lua::PhaseName(phase) == "Unknown") {
                    return std::unexpected(LuaSuiteFail::WrongValue);
                }
            }
            return {};
        }

        auto TestScriptErrorCarriesCodeAndDetail() -> std::expected<void, ZHLN::ErrorCode> {
            // The plain form reports the code alone: this is the whole answer
            // for a caller that branches on it, and it costs no allocation.
            auto script = ZHLN::Lua::Compile<LuaScriptState>("return function(state) return { speed = 4.0 } end", "detail.lua").Build();
            if (!ZHLN::Test::ExpectFalse(!script)) {
                return std::unexpected(script.error());
            }
            LuaScriptState state;
            auto           bare = (*script)(state);
            if (bare || bare.error() != ZHLN::Lua::Error::ReadOnlyField) {
                return std::unexpected(LuaSuiteFail::WrongErrorCode);
            }

            // The diagnostics form adds why, with nothing cached on the Script
            // for a later call to read.
            ZHLN::Lua::ScriptDiagnostics diagnostics;
            auto                        res = (*script)(state, diagnostics);
            if (res) {
                return std::unexpected(LuaSuiteFail::ExpectedFailureDidNotHappen);
            }
            if (res.error().get<ZHLN::Lua::Error>() != ZHLN::Lua::Error::ReadOnlyField) {
                return std::unexpected(LuaSuiteFail::WrongErrorCode);
            }
            if (diagnostics.detail.find("speed") == std::string::npos) {
                return std::unexpected(LuaSuiteFail::WrongValue);
            }

            // A syntax error has no patch detail, so the VM's own message --
            // which names the chunk -- is what surfaces.
            auto broken = ZHLN::Lua::Script<LuaScriptState>::CompileWithDiagnostics("return function(state) return {", "broken.lua");
            if (broken || broken.error().detail.find("broken.lua") == std::string::npos) {
                return std::unexpected(LuaSuiteFail::WrongValue);
            }
            return {};
        }

        auto TestDiagnosticsAreOptIn() -> std::expected<void, ZHLN::ErrorCode> {
            // The default path returns a bare ErrorCode: no std::string in the
            // value, so a caller that only branches on the code pays nothing.
            // The *WithDiagnostics form is where the reason lives.
            struct State {
                float speed = 1.0f;
            } state;

            static_assert(std::is_same_v<decltype(ZHLN::Lua::ApplyPatchString(state, "return {}")), std::expected<void, ZHLN::ErrorCode>>);
            static_assert(std::is_same_v<decltype(ZHLN::Lua::EvaluateString<int>("return 1")), std::expected<int, ZHLN::ErrorCode>>);
            static_assert(std::is_same_v<decltype(ZHLN::Lua::Compile<State>("return function() end", "x").Build()), std::expected<ZHLN::Lua::Script<State>, ZHLN::ErrorCode>>);

            // ScriptError substitutes for ErrorCode, so a diagnostics caller
            // can still compare without reaching into `code`.
            auto patched = ZHLN::Lua::ApplyPatchStringWithDiagnostics(state, "return { bogus = 1 }");
            if (patched || !(patched.error() == ZHLN::Lua::Error::UnknownField) || !(patched.error().get<ZHLN::Lua::Error>() == ZHLN::Lua::Error::UnknownField)) {
                return std::unexpected(LuaSuiteFail::WrongErrorCode);
            }
            if (patched.error().detail != "unknown field 'bogus'") {
                return std::unexpected(LuaSuiteFail::WrongValue);
            }

            // Describe() renders the enum description once, not twice: the
            // duplication that a separate `message` member used to cause.
            const auto described = ZHLN::Lua::Failure {
                .phase = ZHLN::Lua::Phase::Start, .code = ZHLN::Lua::Error::UnknownField, .detail = "unknown field 'bogus'",
            }.Describe();
            constexpr std::string_view kSentence = "key not present on the reflected C++ struct";
            if (described.find(kSentence) != described.rfind(kSentence)) {
                return std::unexpected(LuaSuiteFail::WrongValue);
            }
            return {};
        }

        auto TestNestedFailureNamesItsField() -> std::expected<void, ZHLN::ErrorCode> {
            // A failure inside a sub-struct must name the member that led
            // there. Both patching paths are checked because they compose the
            // path in different places: ApplyPatchString walks PatchObject,
            // while Script<T> walks the staged StageObject.
            struct Inner {
                float speed = 1.0f;
            };
            struct Nested {
                Inner   out {};
                int32_t value = 0;
            } state;

            auto patched = ZHLN::Lua::ApplyPatchStringWithDiagnostics(state, "return { out = { speed = 'not a number' } }", "nested.lua");
            if (patched) {
                return std::unexpected(LuaSuiteFail::ExpectedFailureDidNotHappen);
            }
            if (!(patched.error() == ZHLN::Lua::Error::TypeMismatch)) {
                return std::unexpected(LuaSuiteFail::WrongErrorCode);
            }
            // No trailing separator either: the inner failure carries no detail
            // of its own, so the message must stop at the field name.
            if (patched.error().detail != "field 'out': field 'speed'") {
                return std::unexpected(LuaSuiteFail::WrongValue);
            }

            auto script = ZHLN::Lua::Compile<Nested>("return function(s) return { out = { speed = 'not a number' } } end", "nested.lua").Build();
            if (!ZHLN::Test::ExpectFalse(!script)) {
                return std::unexpected(script.error());
            }
            ZHLN::Lua::ScriptDiagnostics evaluatedDiagnostics;
            auto                        evaluated = (*script)(state, evaluatedDiagnostics);
            if (evaluated) {
                return std::unexpected(LuaSuiteFail::ExpectedFailureDidNotHappen);
            }
            if (!(evaluated.error() == ZHLN::Lua::Error::TypeMismatch)) {
                return std::unexpected(LuaSuiteFail::WrongErrorCode);
            }
            if (evaluatedDiagnostics.detail != "field 'out': field 'speed'") {
                return std::unexpected(LuaSuiteFail::WrongValue);
            }

            // Neither path may have committed a half-applied patch.
            if (!Near(state.out.speed, 1.0f) || state.value != 0) {
                return std::unexpected(LuaSuiteFail::WrongValue);
            }
            return {};
        }

        auto TestPlainStateLayerLifecycle() -> std::expected<void, ZHLN::ErrorCode> {
            StubEngine engine;
            struct PlainState {
                int32_t value = 1;
            } state;

            auto layer = ZHLN::Lua::FromString("return function(s) return { value = 7 } end", "plain.lua").Yes(ZHLN::Lua::Permission::Tick).State(state);
            // Frame phases before Start must fail, not run against no chunk.
            auto early = layer.Tick(engine, 0.016f);
            if (early || early.error().get<ZHLN::Lua::Error>() != ZHLN::Lua::Error::ScriptLoadFailed) {
                return std::unexpected(LuaSuiteFail::WrongErrorCode);
            }
            if (!ZHLN::Test::ExpectFalse(!layer.Start(engine))) {
                return std::unexpected(LuaSuiteFail::UnexpectedError);
            }
            // The startup patch lands before Start returns.
            if (state.value != 7) {
                return std::unexpected(LuaSuiteFail::WrongValue);
            }
            if (!ZHLN::Test::ExpectFalse(!layer.Tick(engine, 0.016f))) {
                return std::unexpected(LuaSuiteFail::UnexpectedError);
            }
            layer.Stop(engine);
            return {};
        }

        auto TestHostLifecycleAndQualifiedHooks() -> std::expected<void, ZHLN::ErrorCode> {
            StubEngine             engine;
            RecordingHost::Journal journal;

            auto layer = ZHLN::Lua::FromString("return function(s) return { ticks = s.ticks + 1, out = { spawn = true } } end", "hosted.lua")
                             .Yes(ZHLN::Lua::Permission::Tick)
                             .Bind<RecordingHost>(journal);

            // A hosted layer owns its state; HostPtr/StatePtr are how a caller
            // reaches it.
            if (layer.HostPtr() == nullptr || layer.StatePtr() == nullptr) {
                return std::unexpected(LuaSuiteFail::WrongValue);
            }
            if (journal.starts != 0) {
                return std::unexpected(LuaSuiteFail::WrongValue);
            }

            if (!ZHLN::Test::ExpectFalse(!layer.Start(engine))) {
                return std::unexpected(LuaSuiteFail::UnexpectedError);
            }
            if (journal.starts != 1) {
                return std::unexpected(LuaSuiteFail::WrongValue);
            }
            // The startup chunk has already run, so OnStart saw the one-shot it
            // authored rather than an untouched default: this is the sandbox's
            // lamp position in miniature.
            if (!journal.startedWithSpawn) {
                return std::unexpected(LuaSuiteFail::WrongValue);
            }
            // Start is idempotent: App may hand the same layer a second frame.
            if (!ZHLN::Test::ExpectFalse(!layer.Start(engine)) || journal.starts != 1) {
                return std::unexpected(LuaSuiteFail::WrongValue);
            }

            // One frame: OnPrepare (with dt) -> chunk -> OnCommit.
            if (!ZHLN::Test::ExpectFalse(!layer.Tick(engine, 0.5f))) {
                return std::unexpected(LuaSuiteFail::UnexpectedError);
            }
            if (journal.prepares != 1 || journal.commits != 1 || !journal.preparedWithMutableState) {
                return std::unexpected(LuaSuiteFail::WrongValue);
            }
            if (!Near(journal.lastDt, 0.5f) || !Near(journal.elapsedAtCommit, 0.5f)) {
                return std::unexpected(LuaSuiteFail::WrongValue);
            }
            // Permission::Start is in the default mask, so the chunk also ran
            // once at Start: the first Tick is the second evaluation.
            if (layer.StatePtr()->ticks != 2 || !layer.StatePtr()->out.spawn) {
                return std::unexpected(LuaSuiteFail::WrongValue);
            }

            layer.Stop(engine);
            if (journal.stops != 1) {
                return std::unexpected(LuaSuiteFail::WrongValue);
            }
            // Stop is idempotent too, and must not re-enter OnStop.
            layer.Stop(engine);
            if (journal.stops != 1) {
                return std::unexpected(LuaSuiteFail::WrongValue);
            }
            return {};
        }

        auto TestAfterTickForwardsDt() -> std::expected<void, ZHLN::ErrorCode> {
            // AfterTick used to pass 0.0f; a host accumulating elapsed time in
            // OnPrepare measured nothing. The dt the caller passed must arrive.
            StubEngine             engine;
            RecordingHost::Journal journal;
            auto                   layer = ZHLN::Lua::FromString("return function(s) return { ticks = 1 } end", "after.lua")
                                               .No(ZHLN::Lua::Permission::Tick)
                                               .Yes(ZHLN::Lua::Permission::AfterTick)
                                               .Bind<RecordingHost>(journal);
            if (!ZHLN::Test::ExpectFalse(!layer.Start(engine))) {
                return std::unexpected(LuaSuiteFail::UnexpectedError);
            }
            if (!ZHLN::Test::ExpectFalse(!layer.AfterTick(engine, 0.25f))) {
                return std::unexpected(LuaSuiteFail::UnexpectedError);
            }
            if (!Near(journal.lastDt, 0.25f)) {
                return std::unexpected(LuaSuiteFail::WrongValue);
            }
            layer.Stop(engine);
            return {};
        }

        auto TestFailPolicyPropagatesAndRecords() -> std::expected<void, ZHLN::ErrorCode> {
            StubEngine             engine;
            RecordingHost::Journal journal;
            auto                   layer = ZHLN::Lua::FromString("return function(s) return { bogus = 1 } end", "fail.lua")
                                               .Yes(ZHLN::Lua::Permission::Tick)
                                               .OnError(ZHLN::Lua::ErrorPolicy::Fail)
                                               .Bind<RecordingHost>(journal);

            // The startup patch runs inside Start, so the bad field fails there.
            auto res = layer.Start(engine);
            if (res) {
                return std::unexpected(LuaSuiteFail::ExpectedFailureDidNotHappen);
            }
            if (res.error().get<ZHLN::Lua::Error>() != ZHLN::Lua::Error::UnknownField) {
                return std::unexpected(LuaSuiteFail::WrongErrorCode);
            }
            if (journal.errors != 1 || journal.lastPhase != ZHLN::Lua::Phase::Start) {
                return std::unexpected(LuaSuiteFail::WrongValue);
            }
            if (journal.lastDetail.find("bogus") == std::string::npos) {
                return std::unexpected(LuaSuiteFail::WrongValue);
            }
            // The whole reason Fail exists: the registry owns the detail after
            // the caller only keeps the code.
            if (RecordedDetail(*engine.Errors()).find("bogus") == std::string::npos) {
                return std::unexpected(LuaSuiteFail::WrongValue);
            }
            // OnStart never ran, so Stop must not run OnStop either -- App calls
            // Stop on entered layers even when Start failed.
            layer.Stop(engine);
            if (journal.starts != 0 || journal.stops != 0) {
                return std::unexpected(LuaSuiteFail::WrongValue);
            }
            return {};
        }

        auto TestLogAndSkipStillRecordsAndKeepsRunning() -> std::expected<void, ZHLN::ErrorCode> {
            StubEngine             engine;
            RecordingHost::Journal journal;
            auto                   layer = ZHLN::Lua::FromString("return function(s) return { bogus = 1 } end", "skip.lua")
                                               .Yes(ZHLN::Lua::Permission::Tick)
                                               .OnError(ZHLN::Lua::ErrorPolicy::LogAndSkip)
                                               .Bind<RecordingHost>(journal);

            if (!ZHLN::Test::ExpectFalse(!layer.Start(engine))) {
                return std::unexpected(LuaSuiteFail::UnexpectedError);
            }
            if (journal.errors < 1) {
                return std::unexpected(LuaSuiteFail::WrongValue);
            }
            // Swallowed, but still recorded: an engine-side log must be able to
            // name the script that failed.
            if (RecordedDetail(*engine.Errors()).find("bogus") == std::string::npos) {
                return std::unexpected(LuaSuiteFail::WrongValue);
            }
            if (!ZHLN::Test::ExpectFalse(!layer.Tick(engine, 0.016f))) {
                return std::unexpected(LuaSuiteFail::UnexpectedError);
            }
            layer.Stop(engine);
            return {};
        }

        auto TestCompileFailureSkipsStartAndStop() -> std::expected<void, ZHLN::ErrorCode> {
            StubEngine             engine;
            RecordingHost::Journal journal;
            auto layer = ZHLN::Lua::FromString("return function(s) return {", "syntax.lua").Yes(ZHLN::Lua::Permission::Tick).Bind<RecordingHost>(journal);
            auto res   = layer.Start(engine);
            if (res || res.error().get<ZHLN::Lua::Error>() != ZHLN::Lua::Error::SyntaxError) {
                return std::unexpected(LuaSuiteFail::WrongErrorCode);
            }
            if (journal.starts != 0 || journal.errors != 1) {
                return std::unexpected(LuaSuiteFail::WrongValue);
            }
            // The syntax message, not a generic "compile failed", is what the
            // registry must hold: it names the file and the line.
            if (RecordedDetail(*engine.Errors()).find("syntax.lua") == std::string::npos) {
                return std::unexpected(LuaSuiteFail::WrongValue);
            }
            layer.Stop(engine);
            if (journal.stops != 0) {
                return std::unexpected(LuaSuiteFail::WrongValue);
            }
            return {};
        }

        auto TestReloadDoesNotReenterOnStart() -> std::expected<void, ZHLN::ErrorCode> {
            StubEngine             engine;
            RecordingHost::Journal journal;
            auto                   layer = ZHLN::Lua::FromString("return function(s) return { ticks = 1 } end", "reload.lua")
                                               .Yes(ZHLN::Lua::Permission::Tick)
                                               .Yes(ZHLN::Lua::Permission::HotReload)
                                               .Bind<RecordingHost>(journal);
            if (!ZHLN::Test::ExpectFalse(!layer.Start(engine)) || journal.starts != 1) {
                return std::unexpected(LuaSuiteFail::WrongValue);
            }
            if (!ZHLN::Test::ExpectFalse(!layer.Reload(engine))) {
                return std::unexpected(LuaSuiteFail::UnexpectedError);
            }
            // A reload swaps the chunk; it must not rebuild the world.
            if (journal.starts != 1) {
                return std::unexpected(LuaSuiteFail::WrongValue);
            }
            if (!ZHLN::Test::ExpectFalse(!layer.HotReloadSource(engine, "return function(s) return { ticks = 5 } end"))) {
                return std::unexpected(LuaSuiteFail::UnexpectedError);
            }
            if (!ZHLN::Test::ExpectFalse(!layer.Tick(engine, 0.016f))) {
                return std::unexpected(LuaSuiteFail::UnexpectedError);
            }
            if (layer.StatePtr()->ticks != 5) {
                return std::unexpected(LuaSuiteFail::WrongValue);
            }
            layer.Stop(engine);
            return {};
        }

        auto TestBrokenHotReloadKeepsLastGoodChunk() -> std::expected<void, ZHLN::ErrorCode> {
            StubEngine             engine;
            RecordingHost::Journal journal;
            auto                   layer = ZHLN::Lua::FromString("return function(s) return { ticks = 9 } end", "keep.lua")
                                               .Yes(ZHLN::Lua::Permission::Tick)
                                               .OnError(ZHLN::Lua::ErrorPolicy::LogAndSkip)
                                               .Bind<RecordingHost>(journal);
            if (!ZHLN::Test::ExpectFalse(!layer.Start(engine))) {
                return std::unexpected(LuaSuiteFail::UnexpectedError);
            }
            // Under LogAndSkip a botched reload is swallowed -- but the previous
            // chunk has to keep running, or one typo blanks the feature.
            if (!ZHLN::Test::ExpectFalse(!layer.HotReloadSource(engine, "return function(s) return {{"))) {
                return std::unexpected(LuaSuiteFail::UnexpectedError);
            }
            if (!ZHLN::Test::ExpectFalse(!layer.Tick(engine, 0.016f))) {
                return std::unexpected(LuaSuiteFail::UnexpectedError);
            }
            if (layer.StatePtr()->ticks != 9) {
                return std::unexpected(LuaSuiteFail::WrongValue);
            }
            layer.Stop(engine);
            return {};
        }

        auto TestOneShotResetAtFrameBoundary() -> std::expected<void, ZHLN::ErrorCode> {
            // The chunk never writes `out`, so what survives is purely the
            // layer's ResetOneShots call at the frame boundary.
            StubEngine             engine;
            RecordingHost::Journal journal;
            auto                   layer = ZHLN::Lua::FromString("return function(s) return { ticks = s.ticks + 1 } end", "oneshot.lua")
                                               .Yes(ZHLN::Lua::Permission::Tick)
                                               .Bind<RecordingHost>(journal);
            if (!ZHLN::Test::ExpectFalse(!layer.Start(engine))) {
                return std::unexpected(LuaSuiteFail::UnexpectedError);
            }
            layer.StatePtr()->out.spawn = true;
            if (!ZHLN::Test::ExpectFalse(!layer.Tick(engine, 0.016f))) {
                return std::unexpected(LuaSuiteFail::UnexpectedError);
            }
            if (layer.StatePtr()->out.spawn) {
                return std::unexpected(LuaSuiteFail::WrongValue);
            }
            layer.Stop(engine);
            return {};
        }

        auto TestReferenceAndHiddenMembersAreInvisible() -> std::expected<void, ZHLN::ErrorCode> {
            // A reference member (an ECS handle, a borrowed object) has no
            // value to marshal out and nothing to patch in, so it must be
            // skipped on the way out and refused, with a detail, on the way in.
            struct Backing {
                int32_t value = 0;
            };
            struct RefState {
                int32_t  value = 0;
                Backing& ref;
            };
            Backing  backing {};
            RefState state {.value = 3, .ref = backing};

            auto read = ZHLN::Lua::Compile<RefState>("return function(s) return { value = 7 } end", "ref.lua").Build();
            if (!ZHLN::Test::ExpectFalse(!read)) {
                return std::unexpected(read.error());
            }
            auto readRes = (*read)(state);
            if (!readRes || state.value != 7) {
                return std::unexpected(LuaSuiteFail::WrongValue);
            }
            // The reference stayed exactly as it was: nothing rebuilt it.
            if (&state.ref != &backing || backing.value != 0) {
                return std::unexpected(LuaSuiteFail::WrongValue);
            }

            auto write = ZHLN::Lua::Compile<RefState>("return function(s) return { ref = { value = 1 } } end", "refw.lua").Build();
            if (!ZHLN::Test::ExpectFalse(!write)) {
                return std::unexpected(write.error());
            }
            ZHLN::Lua::ScriptDiagnostics writeDiagnostics;
            auto                        writeRes = (*write)(state, writeDiagnostics);
            if (writeRes || writeRes.error().get<ZHLN::Lua::Error>() != ZHLN::Lua::Error::ReadOnlyField) {
                return std::unexpected(LuaSuiteFail::WrongErrorCode);
            }
            if (writeDiagnostics.detail.find("ref") == std::string::npos) {
                return std::unexpected(LuaSuiteFail::WrongValue);
            }
            return {};
        }

        auto TestHiddenMemberNeverReachesLua() -> std::expected<void, ZHLN::ErrorCode> {
            // LuaScriptState owns `owned` behind Hidden; a script that echoes
            // every key it can see must not find it, and one that writes it must
            // be told the field is unknown.
            auto echo = ZHLN::Lua::Compile<LuaScriptState>(
                            R"lua(
                    return function(state)
                        local seen = false
                        for key, _ in pairs(state) do
                            if key == "owned" then seen = true end
                        end
                        return { steps = seen and 1 or 0 }
                    end
                )lua",
                            "hidden.lua"
            )
                            .Build();
            if (!ZHLN::Test::ExpectFalse(!echo)) {
                return std::unexpected(echo.error());
            }
            LuaScriptState state;
            if (auto res = (*echo)(state); !ZHLN::Test::ExpectFalse(!res)) {
                return std::unexpected(res.error());
            }
            if (state.steps != 0 || state.owned == nullptr) {
                return std::unexpected(LuaSuiteFail::WrongValue);
            }

            auto write = ZHLN::Lua::Compile<LuaScriptState>("return function(state) return { owned = 5 } end", "hiddenw.lua").Build();
            if (!ZHLN::Test::ExpectFalse(!write)) {
                return std::unexpected(write.error());
            }
            ZHLN::Lua::ScriptDiagnostics hiddenDiagnostics;
            auto                        res = (*write)(state, hiddenDiagnostics);
            if (res || res.error().get<ZHLN::Lua::Error>() != ZHLN::Lua::Error::UnknownField || hiddenDiagnostics.detail.find("owned") == std::string::npos) {
                return std::unexpected(LuaSuiteFail::WrongErrorCode);
            }
            return {};
        }

        auto TestHostedLayerMovesWithItsState() -> std::expected<void, ZHLN::ErrorCode> {
            // App stores the layer in a shared_ptr and the tests move it around;
            // the state the host and the layer point at has to survive the move,
            // and so does a policy change.
            StubEngine             engine;
            RecordingHost::Journal journal;
            auto                   layer =
                ZHLN::Lua::FromString("return function(s) return { ticks = 1 } end", "move.lua").Yes(ZHLN::Lua::Permission::Tick).Bind<RecordingHost>(journal);
            RecordingHost* host  = layer.HostPtr();
            HostState*     state = layer.StatePtr();

            auto moved = std::move(layer);
            if (moved.HostPtr() != host || moved.StatePtr() != state) {
                return std::unexpected(LuaSuiteFail::WrongValue);
            }
            // A moved-from layer is inert, not a dangling borrow of storage the
            // new layer now owns: Tick reports the load failure instead of
            // running a chunk against someone else's state.
            if (layer.StatePtr() != nullptr || layer.HostPtr() != nullptr) {
                return std::unexpected(LuaSuiteFail::WrongValue);
            }
            if (auto stale = layer.Tick(engine, 0.016f); stale || stale.error().get<ZHLN::Lua::Error>() != ZHLN::Lua::Error::ScriptLoadFailed) {
                return std::unexpected(LuaSuiteFail::WrongErrorCode);
            }
            layer.Stop(engine);
            if (journal.starts != 0 || journal.stops != 0) {
                return std::unexpected(LuaSuiteFail::WrongValue);
            }

            auto repolicied = std::move(moved).OnError(ZHLN::Lua::ErrorPolicy::LogAndSkip);
            if (repolicied.HostPtr() != host || repolicied.StatePtr() != state) {
                return std::unexpected(LuaSuiteFail::WrongValue);
            }
            if (!ZHLN::Test::ExpectFalse(!repolicied.Start(engine))) {
                return std::unexpected(LuaSuiteFail::UnexpectedError);
            }
            repolicied.Stop(engine);
            return {};
        }
    };
};

} // namespace
