// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TestsFramework.hpp"
#include <ProjectLightClient/ClientConfig.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <string_view>

struct TestProjectLightConfigSuite {
    struct Tests {
        std::expected<void, ZHLN::ErrorCode> dotenv_settings_have_no_compiled_connection_defaults() {
            auto empty = ZHLN::ProjectLight::ParseClientSettings("");
            ZHLN::Test::ExpectTrue(empty.has_value());
            if (empty) {
                ZHLN::Test::ExpectTrue(empty->launchScheme.empty());
                ZHLN::Test::ExpectFalse(empty->launch.enabled);
                ZHLN::Test::ExpectTrue(empty->launch.host.empty());
                ZHLN::Test::ExpectEq(empty->launch.port, uint16_t {0});
                ZHLN::Test::ExpectEq(empty->launch.userId, uint64_t {0});
                ZHLN::Test::ExpectTrue(empty->launch.username.empty());
                ZHLN::Test::ExpectTrue(empty->launch.token.empty());
            }
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> dotenv_loads_all_settings_and_quoted_tokens() {
            auto parsed = ZHLN::ProjectLight::ParseClientSettings(
                "# local configuration\n"
                "PROJECT_LIGHT_SCHEME=Test+Launch://\n"
                "PROJECT_LIGHT_SERVER=127.0.0.1\n"
                "PROJECT_LIGHT_PORT=6012\n"
                "PROJECT_LIGHT_USER_ID=184\n"
                "PROJECT_LIGHT_USERNAME=LocalPlayer\n"
                "PROJECT_LIGHT_TOKEN=\"token with # and spaces\" # trailing comment\n"
                "PROJECT_LIGHT_PLACE=sample-world\n"
                "UNRELATED_SETTING=ignored\n"
            );
            ZHLN::Test::ExpectTrue(parsed.has_value());
            if (parsed) {
                ZHLN::Test::ExpectEq(parsed->launchScheme, std::string_view("test+launch"));
                ZHLN::Test::ExpectTrue(parsed->launch.enabled);
                ZHLN::Test::ExpectEq(parsed->launch.host, std::string_view("127.0.0.1"));
                ZHLN::Test::ExpectEq(parsed->launch.port, uint16_t {6012});
                ZHLN::Test::ExpectEq(parsed->launch.userId, uint64_t {184});
                ZHLN::Test::ExpectEq(parsed->launch.username, std::string_view("LocalPlayer"));
                ZHLN::Test::ExpectEq(parsed->launch.token, std::string_view("token with # and spaces"));
                ZHLN::Test::ExpectEq(parsed->launch.place, std::string_view("sample-world"));
            }
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> launch_uri_overrides_only_supplied_values() {
            auto parsed = ZHLN::ProjectLight::ParseClientSettings(
                "PROJECT_LIGHT_SCHEME=custom+game\n"
                "PROJECT_LIGHT_SERVER=server.example\n"
                "PROJECT_LIGHT_PORT=5555\n"
                "PROJECT_LIGHT_USER_ID=7\n"
                "PROJECT_LIGHT_USERNAME=BasePlayer\n"
                "PROJECT_LIGHT_TOKEN=base-secret\n"
            );
            ZHLN::Test::ExpectTrue(parsed.has_value());
            if (!parsed) return {};

            constexpr std::array<std::string_view, 3> args = {
                "project-light-client",
                "CUSTOM+GAME://?server=127.0.0.1&userId=12",
                "--headless",
            };
            auto filtered = ZHLN::ProjectLight::ExtractLaunchUrlArgs(args, parsed->launch, parsed->launchScheme);
            ZHLN::Test::ExpectTrue(filtered.has_value());
            if (filtered) {
                ZHLN::Test::ExpectEq(filtered->size(), size_t {2});
                ZHLN::Test::ExpectEq((*filtered)[0], std::string_view("project-light-client"));
                ZHLN::Test::ExpectEq((*filtered)[1], std::string_view("--headless"));
            }
            ZHLN::Test::ExpectEq(parsed->launch.host, std::string_view("127.0.0.1"));
            ZHLN::Test::ExpectEq(parsed->launch.port, uint16_t {5555});
            ZHLN::Test::ExpectEq(parsed->launch.userId, uint64_t {12});
            ZHLN::Test::ExpectEq(parsed->launch.username, std::string_view("BasePlayer"));
            ZHLN::Test::ExpectEq(parsed->launch.token, std::string_view("base-secret"));
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> malformed_configuration_and_scheme_mismatch_are_rejected() {
            ZHLN::Test::ExpectFalse(ZHLN::ProjectLight::ParseClientSettings("PROJECT_LIGHT_SCHEME=1bad\n").has_value());
            ZHLN::Test::ExpectFalse(ZHLN::ProjectLight::ParseClientSettings("PROJECT_LIGHT_PORT=70000\n").has_value());

            ZHLN::ProjectLight::LaunchConfig config;
            auto wrongScheme = ZHLN::ProjectLight::ParseLaunchUrl("other://?server=127.0.0.1", "configured");
            ZHLN::Test::ExpectFalse(wrongScheme.has_value());

            constexpr std::array<std::string_view, 2> args = {"client", "another://?server=localhost"};
            auto extracted = ZHLN::ProjectLight::ExtractLaunchUrlArgs(args, config, "configured");
            ZHLN::Test::ExpectFalse(extracted.has_value());
            return {};
        }
    };
};

int main() {
    return ZHLN::Test::Runner::Run<TestProjectLightConfigSuite>();
}
