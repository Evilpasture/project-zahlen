// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// tests/render/LightingRTCommon.hpp
//
// Shared by TestClusteredLighting.cpp, TestRayTracedShadows.cpp and
// TestReflections.cpp, which were split out of TestLightingRayTraced.cpp.
//
// LightingRTTestError is a global-scope enum, so it must be defined once and
// included: three translation units each defining it with a different subset of
// enumerators would be an ODR violation, and only one of them would win.
//
// The include set is the union the single file used. Splitting it per case
// would risk a translation unit missing a header it reaches only through
// another case's code path, which is a compile error that shows up far from
// its cause.

#pragma once

#include "TestsFramework.hpp"
#include "helpers/HeadlessEngineFixture.hpp"
// stb_image_write's implementation comes from tests/helpers/ImageWriteImpl.cpp,
// compiled once into the group binary. Do not define ZHLN_TEST_IMAGE_WRITE_IMPL
// here -- a second definition in the same link is a duplicate-symbol error.
#include "helpers/ImageTesting.hpp"
#include <Zahlen/Camera.hpp>
#include <Zahlen/Components.hpp>
#include <Zahlen/CreativeWorksFactory.hpp>
#include <Zahlen/DefaultPreset.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Math3D.hpp>
#include <Zahlen/Profiler.hpp>
#include <Zahlen/Render.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <Zahlen/Threading/Thread.hpp>
#include <Zahlen/Types.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <expected>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// ============================================================================
// Test Error Types
// ============================================================================

enum class LightingRTTestError : uint8_t {
    EngineInitFailed ZHLN_ANNOTATION(ZHLN::Description<"Failed to initialize headless Engine context for the lighting/raytracing test."> {}) = 1,
    RenderOutputBlank ZHLN_ANNOTATION(ZHLN::Description<"Rendered frame is blank or could not be captured."> {}),
    TemporalFlickerDetected ZHLN_ANNOTATION(ZHLN::Description<"A static fully-lit scene changed more frame-to-frame than the engine's own noise floor."> {}),
    LightCullingPopDetected ZHLN_ANNOTATION(ZHLN::Description<"A point light inside the frustum/range lost its lighting contribution for a frame (cluster culling)."> {}),
    RayTracedShadowFailed ZHLN_ANNOTATION(ZHLN::Description<"The ray-traced sun shadow did not appear, disappeared, or took out the whole frame."> {}),
    ReflectionMissing ZHLN_ANNOTATION(ZHLN::Description<"The polished surface shows no reflection of the emissive object (RTR/SSR fell back to IBL)."> {}),
    ReflectionArtifacts ZHLN_ANNOTATION(ZHLN::Description<"The reflected region contains blowout, ray-debris speckles, or flicker."> {}),
    MultiLightClusteringFailed ZHLN_ANNOTATION(ZHLN::Description<"Multi-light clustered accumulation or chromatic blending mismatch detected."> {}),
    MultiEmissiveReflectionFailed ZHLN_ANNOTATION(ZHLN::Description<"Multi-emissive source reflection analysis failed: missing spatial mirror correspondence or color "
                                                       "fidelity."> {}),
    DenseCrossInteractionFailed ZHLN_ANNOTATION(ZHLN::Description<"Dense multi-light & emissive interaction produced blowout, NaN/Inf, or lighting failure."> {}),
    DeviceLostDuringTest ZHLN_ANNOTATION(ZHLN::Description<"The Vulkan device was lost repeatedly during the scenario; the engine hot-rebuild recovered, but the GPU was "
                                              "not stable."> {}),
    ValidationErrorsRaised ZHLN_ANNOTATION(ZHLN::Description<"The validation layer reported errors while rendering the lighting/raytracing frames."> {}),
};

// ============================================================================
// Shared Helpers
// ============================================================================
//
// Frame I/O, pixel statistics and the headless engine lifecycle live in
// tests/helpers/. The using declarations below keep the call sites reading as
// they did when every one of these was defined locally in this file.

namespace {

using ZHLN::Test::Image::ChangedRegion;
using ZHLN::Test::Image::CoefficientOfVariation;
using ZHLN::Test::Image::CompareFrames;
using ZHLN::Test::Image::DiffRegion;
using ZHLN::Test::Image::FrameDiff;
using ZHLN::Test::Image::FrameMetrics;
using ZHLN::Test::Image::LoadPPM;
using ZHLN::Test::Image::Luma;
using ZHLN::Test::Image::Mean;
using ZHLN::Test::Image::MeasureImage;
using ZHLN::Test::Image::MeasureSubRegion;
using ZHLN::Test::Image::PngPathOf;
using ZHLN::Test::Image::RgbImage;
using ZHLN::Test::Image::SavePNG;
using ZHLN::Test::Image::WriteAmplifiedDiff;
using ZHLN::Test::Image::WriteRegionCrop;

using ZHLN::Test::Headless::Capture;
using ZHLN::Test::Headless::DisableTAA;
using ZHLN::Test::Headless::RunStableScene;
using ZHLN::Test::Headless::StableRunResult;
using ZHLN::Test::Headless::TickFrames;

/// This suite needs a larger physics slab than the fixture default, and the
/// window title identifies it in a capture directory shared with other suites.
///
/// Pooled: one engine per resolution for the whole binary, with the scene
/// reset between tests. See the engine-reuse notes in HeadlessEngineFixture.
[[nodiscard]] inline auto CreateTestEngine(uint32_t width = 640, uint32_t height = 480) -> ZHLN::Test::Headless::EngineHandle {
    return ZHLN::Test::Headless::AcquireEngine(ZHLN::Test::Headless::EngineOptions {
        .appName               = "Headless Lighting RT Test",
        .width                 = width,
        .height                = height,
        .maxBodies             = 512,
        .maxBodyPairs          = 1024,
        .maxContactConstraints = 1024,
    });
}

} // namespace
