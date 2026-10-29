// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// tests/extras/TestReflectLua.cpp
//
// Exercises plugins/lua/ReflectLua.hpp + SceneLua.hpp:
//   * Reflection-driven evaluation into scalars, FixedString<N>, std::array,
//     std::vector, maps, Optional, enums, JPH::Float3/Vec3/Vec4/Quat,
//     GraphicsSettings, and ZHLN::Scene::Scene.
//   * Computed expressions (loops, math, helpers) and pure function reducers
//     (EvaluateCallString).
//   * Transactional deep patching (ApplyPatchString) that preserves sibling
//     fields and rolls back completely on validation failure.
//   * Hermetic sandbox isolation (no io/os/ffi/package/debug/jit/bytecode)
//     and deterministic instruction + memory budgets.

#include "TestsFramework.hpp"

#include <RagdollAuthoring/RagdollAuthoring.hpp>
#include <Zahlen/Components.hpp>
#include <Zahlen/Core/Optional.hpp>
#include <Zahlen/Core/String.hpp>
#include <Zahlen/GraphicsSettings.hpp>
#include <Zahlen/Physics/Jolt.hpp>
#include <Zahlen/Render/Info.hpp>
#include <Zahlen/Scene.hpp>
#include <lua/ReflectLua.hpp>
#include <lua/SceneLua.hpp>

#include <cmath>
#include <filesystem>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace {

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
    JPH::Float3    offset  = {0.0f, 1.0f, 2.0f};
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

auto Near(float a, float b, float eps = 1e-4f) noexcept -> bool {
    return std::fabs(a - b) <= eps;
}

struct ReflectLuaSuite {
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

        auto res = ZHLN::ReflectLua::EvaluateString<SampleAggregate>(kChunk, "hydration");
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

        auto scene = ZHLN::ReflectLua::EvaluateString<ZHLN::Scene::Scene>(kSceneChunk, "ring_scene");
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
                        enableRTR = rt and 1 or 0,
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
        auto                hi = ZHLN::ReflectLua::EvaluateCallString<ZHLN::GraphicsSettings>(
            kProfileFn, "graphics.lua", ZHLN::LuaBudget {}, desktopGpu
        );
        if (!hi) {
            return std::unexpected(LuaSuiteFail::UnexpectedError);
        }
        if (hi->qualityPreset != ZHLN::QualityLevel::Ultra || hi->antiAliasing.mode != ZHLN::AAMode::SMAA) {
            return std::unexpected(LuaSuiteFail::WrongValue);
        }
        if (!Near(hi->post.exposure, 1.25f) || hi->post.mode != 2 || hi->post.enableRTR != 1 || !hi->rayTracing.enableDiffuseGI) {
            return std::unexpected(LuaSuiteFail::WrongValue);
        }

        const HardwareProbe lowVramGpu {.vramMiB = 4096, .supportsRayQuery = false, .adapterName = "iGPU"};
        auto                lo = ZHLN::ReflectLua::EvaluateCallString<ZHLN::GraphicsSettings>(
            kProfileFn, "graphics.lua", ZHLN::LuaBudget {}, lowVramGpu
        );
        if (!lo) {
            return std::unexpected(LuaSuiteFail::UnexpectedError);
        }
        if (lo->qualityPreset != ZHLN::QualityLevel::Medium || lo->post.enableRTR != 0 || lo->rayTracing.enableReflections) {
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
        if (auto ok = ZHLN::ReflectLua::ApplyPatchString(live, "return { post = { bloomStrength = 0.18 } }"); !ok) {
            return std::unexpected(LuaSuiteFail::UnexpectedError);
        }
        if (!Near(live.post.exposure, 2.2f) || !Near(live.post.bloomStrength, 0.18f) || live.shadows.resolution != 4096) {
            return std::unexpected(LuaSuiteFail::WrongValue);
        }

        // 2. Patch with a valid field followed by an unknown field must roll back completely.
        auto unknownRes = ZHLN::ReflectLua::ApplyPatchString(
            live,
            "return { post = { exposure = 9.0, nonExistentKnob = 1 } }"
        );
        if (unknownRes) {
            return std::unexpected(LuaSuiteFail::ExpectedFailureDidNotHappen);
        }
        if (!unknownRes.error().Is(ZHLN::LuaError::UnknownField) || !Near(live.post.exposure, 2.2f)) {
            return std::unexpected(LuaSuiteFail::WrongErrorCode);
        }

        // 3. Invalid enum string rolls back completely.
        auto enumRes = ZHLN::ReflectLua::ApplyPatchString(
            live,
            "return { qualityPreset = 'SuperDuperUltra' }"
        );
        if (enumRes || !enumRes.error().Is(ZHLN::LuaError::InvalidEnumString)) {
            return std::unexpected(LuaSuiteFail::WrongErrorCode);
        }

        // 4. Component SIMD JPH::Vec3 / Vec4 patch works directly on trivially copyable ECS component.
        ZHLN::Components::PostProcessSettingsComponent postComp {};
        postComp.exposure = 1.5f;
        if (auto vecOk = ZHLN::ReflectLua::ApplyPatchString(
                postComp,
                "return { colorFilter = { 0.9, 0.8, 0.7 }, skyZenith = { x = 0.1, y = 0.2, z = 0.3, w = 1.0 } }"
            );
            !vecOk) {
            return std::unexpected(LuaSuiteFail::UnexpectedError);
        }
        if (!Near(postComp.exposure, 1.5f) || !Near(postComp.colorFilter.GetY(), 0.8f) || !Near(postComp.skyZenith.GetZ(), 0.3f)) {
            return std::unexpected(LuaSuiteFail::WrongValue);
        }

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

        auto probe = ZHLN::ReflectLua::EvaluateString<SampleAggregate>(kSandboxProbe, "sandbox_probe");
        if (!probe || probe->iterations != 7) {
            return std::unexpected(LuaSuiteFail::UnexpectedError);
        }

        // 2. Binary bytecode header ("\x1bLJ") must be rejected by text-only loader.
        const std::string fakeBytecode("\x1bLJ\x02\x00\x00", 6);
        auto              bcRes = ZHLN::ReflectLua::EvaluateString<SampleAggregate>(fakeBytecode, "bytecode");
        if (bcRes || !bcRes.error().Is(ZHLN::LuaError::SyntaxError)) {
            return std::unexpected(LuaSuiteFail::WrongErrorCode);
        }

        // 3. Infinite loop must deterministically trip InstructionBudgetExceeded.
        ZHLN::LuaBudget tightCpu {.maxInstructions = 8'192, .maxMemoryBytes = 4 * 1024 * 1024};
        auto            loopRes = ZHLN::ReflectLua::EvaluateString<SampleAggregate>(
            "while true do end return {}", "inf_loop", tightCpu
        );
        if (loopRes || !loopRes.error().Is(ZHLN::LuaError::InstructionBudgetExceeded)) {
            return std::unexpected(LuaSuiteFail::WrongErrorCode);
        }

        // 4. Exponential string doubling must deterministically trip MemoryBudgetExceeded.
        ZHLN::LuaBudget tightMem {.maxInstructions = 5'000'000, .maxMemoryBytes = 256 * 1024};
        auto            oomRes = ZHLN::ReflectLua::EvaluateString<SampleAggregate>(
            "local s = string.rep('a', 1024); for i = 1, 20 do s = s .. s end; return { label = s }",
            "oom_chunk",
            tightMem
        );
        if (oomRes || !oomRes.error().Is(ZHLN::LuaError::MemoryBudgetExceeded)) {
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
            auto gfxSrc = ZHLN::ReflectLua::ReadLuaFileText(gfxPath);
            if (!gfxSrc) {
                return std::unexpected(LuaSuiteFail::UnexpectedError);
            }

            const ZHLN::RenderInfo rtDesktop {
                .rendererName        = "Vulkan",
                .gpuName             = "RTX 4090",
                .deviceType          = ZHLN::PhysicalDeviceType::DiscreteGPU,
                .rayTracingSupported = true,
            };
            auto ultraGfx = ZHLN::ReflectLua::EvaluateCallString<ZHLN::GraphicsSettings>(
                *gfxSrc, "@config/graphics.lua", ZHLN::LuaBudget {}, rtDesktop
            );
            if (!ultraGfx || ultraGfx->qualityPreset != ZHLN::QualityLevel::Ultra ||
                ultraGfx->post.enableRTR != 1 || !ultraGfx->rayTracing.enableDiffuseGI ||
                ultraGfx->shadows.resolution != 4096) {
                return std::unexpected(LuaSuiteFail::WrongValue);
            }

            const ZHLN::RenderInfo cpuAdapter {
                .rendererName        = "Vulkan",
                .gpuName             = "llvmpipe",
                .deviceType          = ZHLN::PhysicalDeviceType::CPU,
                .rayTracingSupported = false,
            };
            auto lowGfx = ZHLN::ReflectLua::EvaluateCallString<ZHLN::GraphicsSettings>(
                *gfxSrc, "@config/graphics.lua", ZHLN::LuaBudget {}, cpuAdapter
            );
            if (!lowGfx || lowGfx->qualityPreset != ZHLN::QualityLevel::Low ||
                lowGfx->antiAliasing.mode != ZHLN::AAMode::FXAA || lowGfx->rayTracing.enableReflections ||
                lowGfx->shadows.resolution != 1024) {
                return std::unexpected(LuaSuiteFail::WrongValue);
            }

            const std::filesystem::path ragdollPath = findConfig("ragdoll_humanoid.lua");
            if (ragdollPath.empty()) {
                return std::unexpected(LuaSuiteFail::UnexpectedError);
            }
            auto recipe = ZHLN::RagdollAuthoring::LoadRecipeFromLuaFile(ragdollPath);
            if (!recipe || recipe->rules.size() != 4 || recipe->rules[0].spec.shape != ZHLN::RagdollAuthoring::RagdollShapeKind::Capsule ||
                !Near(recipe->rules[1].spec.maxMotorForce, 250.0f) ||
                recipe->fallback.shape != ZHLN::RagdollAuthoring::RagdollShapeKind::Sphere) {
                return std::unexpected(LuaSuiteFail::WrongValue);
            }

            return {};
        }
    };
};

} // namespace
