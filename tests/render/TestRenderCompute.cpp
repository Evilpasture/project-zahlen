// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TestsFramework.hpp"
#include <Zahlen/Engine.hpp>
#include <Zahlen/Render.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <Zahlen/Threading/Thread.hpp>
#include <expected>

struct RenderComputeTestSuite {
    RenderComputeTestSuite() {
        ZHLN::Fiber::InitMainThread();
        ZHLN::TaskSystem::Init(2, 32, 131072);
    }

    ~RenderComputeTestSuite() {
        ZHLN::TaskSystem::Shutdown();
    }

    struct Tests {
        std::expected<void, ZHLN::Error> procedural_bake_compute_execution() {
            const ZHLN::EngineConfig cfg {
                .render = {.appName = "LocalGPUComputeTest", .width = 640, .height = 480, .vsync = false, .validationMode = ZHLN::ValidationMode::On}
            };

            auto       engineRes   = ZHLN::Engine::Create(cfg);
            const auto checkEngine = ZHLN::Test::AssertTrue(engineRes.has_value());
            if (!checkEngine) {
                return checkEngine;
            }

            const auto engine = std::move(engineRes.value());
            auto&      rc     = engine->GetRenderContext();

            const auto bakeRes   = rc.BakeProceduralTexture(128, 128, 0, 4.0f, 1.0f);
            const auto checkBake = ZHLN::Test::AssertTrue(bakeRes.has_value());
            if (!checkBake) {
                return checkBake;
            }

            const uint32_t bindlessIndex = *bakeRes;
            ZHLN::Test::ExpectTrue(bindlessIndex > 0);

            return {};
        }
    };
};

int main() {
    return ZHLN::Test::Runner::Run<RenderComputeTestSuite>();
}
