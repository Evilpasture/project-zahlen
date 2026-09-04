// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TestsFramework.hpp"
#include "helpers/HeadlessEngineFixture.hpp"
#include <Zahlen/Engine.hpp>
#include <Zahlen/Render.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <Zahlen/Threading/Thread.hpp>
#include <expected>

struct RenderComputeTestSuite {
    RenderComputeTestSuite() {
        // Nested in the group binary's session: the task system and the pooled
        // engine outlive this suite (see HeadlessEngineFixture.hpp).
        ZHLN::Test::Headless::BeginSession();
    }

    ~RenderComputeTestSuite() {
        ZHLN::Test::Headless::EndSession();
    }
    enum class RenderComputeTestError : uint8_t {
        EngineInitFailed     ZHLN_ANNOTATION(ZHLN::Description<"Failed to initialize headless Engine context for the compute bake test."> {}) = 1,
        ProceduralBakeFailed ZHLN_ANNOTATION(ZHLN::Description<"BakeProceduralTexture did not return a bindless index."> {}),
    };

    struct Tests {
        std::expected<void, ZHLN::Error> procedural_bake_compute_execution() {
            // Pooled: the compute bake does not care what earlier tests
            // uploaded, and a device of its own costs a Vulkan instance.
            const auto engine = ZHLN::Test::Headless::AcquireEngine("LocalGPUComputeTest");
            if (!ZHLN::Test::ExpectTrue(engine != nullptr)) {
                return std::unexpected(RenderComputeTestError::EngineInitFailed);
            }

            auto& rc = engine->GetRenderContext();

            const auto bakeRes = rc.BakeProceduralTexture(128, 128, 0, 4.0f, 1.0f);
            if (!ZHLN::Test::ExpectTrue(bakeRes.has_value())) {
                return std::unexpected(RenderComputeTestError::ProceduralBakeFailed);
            }

            const uint32_t bindlessIndex = *bakeRes;
            ZHLN::Test::ExpectTrue(bindlessIndex > 0);

            return {};
        }
    };
};

// Exported for the GPU_Pipeline group binary, which aggregates every suite in
// this domain through Runner::RunDeferred.
auto RunRenderComputeSuite() -> ZHLN::Test::TestStats {
    return ZHLN::Test::RunSuite<RenderComputeTestSuite>();
}
