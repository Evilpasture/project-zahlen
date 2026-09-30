// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TestsFramework.hpp"
#include <Zahlen/AssetManager.hpp>
#include <Zahlen/Core/Description.hpp>
#include <Zahlen/Render/EnvironmentImage.hpp>
#include <cstdint>
#include <expected>
#include <utility>

enum class EnvironmentImageTestError : uint8_t {
    InvalidRegistration ZHLN_ANNOTATION(ZHLN::Description<"invalid environment pixels changed the cache"> {}) = 1,
    CacheFailed ZHLN_ANNOTATION(ZHLN::Description<"prepared environment pixels were not cached by key"> {}),
};

namespace {

[[nodiscard]] auto Pixels(float red) -> ZHLN::EnvironmentImage {
    return {.width = 2, .height = 1, .rgba = {red, 0.0f, 0.0f, 1.0f, red, 0.0f, 0.0f, 1.0f}};
}

} // namespace

struct EnvironmentImageTestSuite {
    struct Tests {
        auto missing_environment_is_not_loaded_implicitly() -> std::expected<void, ZHLN::ErrorCode> {
            ZHLN::AssetManager assets;
            if (!ZHLN::Test::ExpectFalse(assets.FindEnvironmentImage("environment.hdr").has_value())) {
                return std::unexpected(EnvironmentImageTestError::CacheFailed);
            }
            return {};
        }

        auto rejects_malformed_pixels_without_changing_cache() -> std::expected<void, ZHLN::ErrorCode> {
            ZHLN::AssetManager assets;
            if (!assets.CacheEnvironmentImage("lighting", Pixels(1.0f))) {
                return std::unexpected(EnvironmentImageTestError::CacheFailed);
            }
            const auto first = assets.FindEnvironmentImage("lighting");
            if (!first || !ZHLN::Test::ExpectEq(first->rgba[0], 1.0f) ||
                !ZHLN::Test::ExpectEq(first->width, 2u) || !ZHLN::Test::ExpectEq(first->height, 1u)) {
                return std::unexpected(EnvironmentImageTestError::CacheFailed);
            }

            auto bad = Pixels(2.0f);
            bad.rgba.pop_back();
            if (!ZHLN::Test::ExpectFalse(assets.CacheEnvironmentImage("lighting", std::move(bad))) ||
                !ZHLN::Test::ExpectFalse(assets.CacheEnvironmentImage("", Pixels(2.0f))) ||
                !ZHLN::Test::ExpectFalse(assets.CacheEnvironmentImage("lighting", {}))) {
                return std::unexpected(EnvironmentImageTestError::InvalidRegistration);
            }
            const auto stillFirst = assets.FindEnvironmentImage("lighting");
            if (!stillFirst || !ZHLN::Test::ExpectEq(stillFirst->rgba.data(), first->rgba.data())) {
                return std::unexpected(EnvironmentImageTestError::InvalidRegistration);
            }
            return {};
        }

        auto validates_prepared_sun_and_both_panorama_lifetimes() -> std::expected<void, ZHLN::ErrorCode> {
            ZHLN::AssetManager assets;
            auto prepared = Pixels(2.0f);
            prepared.lightingRgba = Pixels(0.5f).rgba;
            prepared.sun.emplace();
            prepared.sun->direction = {1.0f, 0.0f, 0.0f};
            prepared.sun->irradiance = {2.0f, 1.0f, 0.5f};
            prepared.contentHash = 42;
            if (!assets.CacheEnvironmentImage("cooked", std::move(prepared)))
                return std::unexpected(EnvironmentImageTestError::CacheFailed);
            const auto first = assets.FindEnvironmentImage("cooked");
            if (!first || !first->sun || first->lightingRgba.size() != 8u ||
                !ZHLN::Test::ExpectEq(first->rgba[0], 2.0f) ||
                !ZHLN::Test::ExpectEq(first->lightingRgba[0], 0.5f) ||
                !ZHLN::Test::ExpectEq(first->sun->irradiance[0], 2.0f) ||
                !ZHLN::Test::ExpectEq(first->contentHash, 42ull)) {
                return std::unexpected(EnvironmentImageTestError::CacheFailed);
            }
            auto invalid = Pixels(3.0f);
            invalid.sun.emplace();
            invalid.sun->direction = {1.0f, 0.0f, 0.0f};
            if (assets.CacheEnvironmentImage("cooked", std::move(invalid)))
                return std::unexpected(EnvironmentImageTestError::InvalidRegistration);
            invalid = Pixels(3.0f);
            invalid.lightingRgba = {0.0f};
            if (assets.CacheEnvironmentImage("cooked", std::move(invalid)))
                return std::unexpected(EnvironmentImageTestError::InvalidRegistration);
            invalid = Pixels(3.0f);
            invalid.lightingRgba = invalid.rgba;
            invalid.sun.emplace(); // zero is not a valid direction
            if (assets.CacheEnvironmentImage("cooked", std::move(invalid)))
                return std::unexpected(EnvironmentImageTestError::InvalidRegistration);
            const auto retained = assets.FindEnvironmentImage("cooked");
            if (!retained || !ZHLN::Test::ExpectEq(retained->rgba.data(), first->rgba.data()) ||
                !ZHLN::Test::ExpectEq(retained->lightingRgba.data(), first->lightingRgba.data())) {
                return std::unexpected(EnvironmentImageTestError::InvalidRegistration);
            }
            assets.ClearCache();
            if (assets.FindEnvironmentImage("cooked")) return std::unexpected(EnvironmentImageTestError::InvalidRegistration);
            return {};
        }

        auto replacement_and_clear_manage_borrowed_pixels() -> std::expected<void, ZHLN::ErrorCode> {
            ZHLN::AssetManager assets;
            if (!assets.CacheEnvironmentImage("lighting", Pixels(1.0f))) {
                return std::unexpected(EnvironmentImageTestError::CacheFailed);
            }
            const auto first = assets.FindEnvironmentImage("lighting");
            if (!assets.CacheEnvironmentImage("lighting", Pixels(2.0f))) {
                return std::unexpected(EnvironmentImageTestError::CacheFailed);
            }
            const auto next = assets.FindEnvironmentImage("lighting");
            if (!first || !next || !ZHLN::Test::ExpectEq(first->rgba[0], 1.0f) ||
                !ZHLN::Test::ExpectEq(next->rgba[0], 2.0f) ||
                !ZHLN::Test::ExpectNe(first->rgba.data(), next->rgba.data())) {
                return std::unexpected(EnvironmentImageTestError::CacheFailed);
            }
            assets.ClearCache(); // invalidates both borrowed spans
            if (!ZHLN::Test::ExpectFalse(assets.FindEnvironmentImage("lighting").has_value())) {
                return std::unexpected(EnvironmentImageTestError::CacheFailed);
            }
            return {};
        }
    };
};

auto RunEnvironmentImageSuite() -> ZHLN::Test::TestStats {
    return ZHLN::Test::RunSuite<EnvironmentImageTestSuite>();
}
