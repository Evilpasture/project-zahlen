// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TestsFramework.hpp"
#include <Zahlen/COM/AssetTypes.hpp>
#include <Zahlen/COM/EngineFactory.hpp>
#include <Zahlen/COM/IAssetLibrary.hpp>
#include <Zahlen/COM/IEngine.hpp>
#include <Zahlen/Core/Description.hpp>
#include <Zahlen/Render/EnvironmentImage.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <cstdint>
#include <expected>
#include <string_view>
#include <utility>

enum class EnvironmentImageTestError : uint8_t {
    InvalidRegistration ZHLN_ANNOTATION(ZHLN::Description<"invalid environment pixels changed the cache"> {}) = 1,
    CacheFailed         ZHLN_ANNOTATION(ZHLN::Description<"prepared environment pixels were not cached by key"> {}),
};

namespace {

[[nodiscard]] auto Pixels(float red) -> ZHLN::EnvironmentImage {
    return {.width = 2, .height = 1, .rgba = {red, 0.0f, 0.0f, 1.0f, red, 0.0f, 0.0f, 1.0f}};
}

[[nodiscard]] auto CacheEnv(ZHLN::COM::IAssetLibrary& library, std::string_view key, const ZHLN::EnvironmentImage& img) -> bool {
    const ZHLN::COM::EnvironmentImageCacheInfo info {
        .key          = {key.data(), key.size()},
        .extent       = {img.width, img.height},
        .rgba         = img.rgba,
        .lightingRgba = img.lightingRgba,
        .sun          = img.sun ? ZHLN::COM::Option<ZHLN::EnvironmentSun>(*img.sun) : ZHLN::COM::Option<ZHLN::EnvironmentSun>{},
        .contentHash  = img.contentHash,
    };
    return library.CacheEnvironmentImage(ZHLN::COM::InChain(info)).HasValue();
}

[[nodiscard]] auto MakeHeadlessEngine() -> ZHLN::COM::Ref<ZHLN::COM::IEngine> {
    auto res = ZHLN::COM::CreateEngineRef(ZHLN::EngineConfig {
        .appName        = "EnvironmentImageTest",
        .width          = 64,
        .height         = 64,
        .vsync          = false,
        .validationMode = ZHLN::ValidationMode::Off,
        .headless       = true,
        .logLevel       = ZHLN::LogLevel::Quiet,
    });
    return res ? std::move(*res) : ZHLN::COM::Ref<ZHLN::COM::IEngine>{};
}

struct EnvironmentImageTestSuite {
    ZHLN::TaskSystem::Scope taskScope {1, 16};

    struct Tests {
        auto missing_environment_is_not_loaded_implicitly() -> std::expected<void, ZHLN::ErrorCode> {
            auto engine = MakeHeadlessEngine();
            if (!engine) return {};
            auto& library = *engine->Assets();
            if (!ZHLN::Test::ExpectFalse(library.FindEnvironmentImage("environment.hdr").HasValue())) {
                return std::unexpected(EnvironmentImageTestError::CacheFailed);
            }
            return {};
        }

        auto rejects_malformed_pixels_without_changing_cache() -> std::expected<void, ZHLN::ErrorCode> {
            auto engine = MakeHeadlessEngine();
            if (!engine) return {};
            auto& library = *engine->Assets();
            if (!CacheEnv(library, "lighting", Pixels(1.0f))) {
                return std::unexpected(EnvironmentImageTestError::CacheFailed);
            }
            const auto firstInfo   = library.FindEnvironmentImage("lighting");
            const auto firstPixels = library.EnvironmentPixels("lighting");
            if (!firstInfo.HasValue() || firstPixels.size != 8u ||
                !ZHLN::Test::ExpectEq(firstPixels.data[0], 1.0f) ||
                !ZHLN::Test::ExpectEq(firstInfo.value.width, 2u) ||
                !ZHLN::Test::ExpectEq(firstInfo.value.height, 1u)) {
                return std::unexpected(EnvironmentImageTestError::CacheFailed);
            }

            auto bad = Pixels(2.0f);
            bad.rgba.pop_back();
            if (!ZHLN::Test::ExpectFalse(CacheEnv(library, "lighting", bad)) ||
                !ZHLN::Test::ExpectFalse(CacheEnv(library, "", Pixels(2.0f))) ||
                !ZHLN::Test::ExpectFalse(CacheEnv(library, "lighting", {}))) {
                return std::unexpected(EnvironmentImageTestError::InvalidRegistration);
            }
            const auto stillFirst = library.EnvironmentPixels("lighting");
            if (!ZHLN::Test::ExpectEq(stillFirst.data, firstPixels.data)) {
                return std::unexpected(EnvironmentImageTestError::InvalidRegistration);
            }
            return {};
        }

        auto validates_prepared_sun_and_replacement() -> std::expected<void, ZHLN::ErrorCode> {
            auto engine = MakeHeadlessEngine();
            if (!engine) return {};
            auto& library = *engine->Assets();
            auto prepared = Pixels(2.0f);
            prepared.lightingRgba = Pixels(0.5f).rgba;
            prepared.sun.emplace();
            prepared.sun->direction  = {1.0f, 0.0f, 0.0f};
            prepared.sun->irradiance = {2.0f, 1.0f, 0.5f};
            prepared.contentHash     = 42;
            if (!CacheEnv(library, "cooked", prepared)) {
                return std::unexpected(EnvironmentImageTestError::CacheFailed);
            }
            auto invalid = Pixels(3.0f);
            invalid.sun.emplace();
            invalid.sun->direction = {1.0f, 0.0f, 0.0f};
            if (CacheEnv(library, "cooked", invalid)) {
                return std::unexpected(EnvironmentImageTestError::InvalidRegistration);
            }
            invalid = Pixels(3.0f);
            invalid.lightingRgba = {0.0f};
            if (CacheEnv(library, "cooked", invalid)) {
                return std::unexpected(EnvironmentImageTestError::InvalidRegistration);
            }
            if (!CacheEnv(library, "cooked", Pixels(4.0f))) {
                return std::unexpected(EnvironmentImageTestError::CacheFailed);
            }
            const auto next = library.EnvironmentPixels("cooked");
            if (next.size != 8u || !ZHLN::Test::ExpectEq(next.data[0], 4.0f)) {
                return std::unexpected(EnvironmentImageTestError::CacheFailed);
            }
            library.ClearCache();
            if (library.FindEnvironmentImage("cooked").HasValue()) {
                return std::unexpected(EnvironmentImageTestError::InvalidRegistration);
            }
            return {};
        }
    };
};

} // namespace
