// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Network/LightProtocol.hpp>
#include <expected>
#include <filesystem>
#include <string>
#include <string_view>

namespace ZHLN::ProjectLight {

// Runtime-only application settings. The URI scheme and all connection values
// come from a dotenv file / process environment; none are compiled in.
struct ClientSettings {
    std::string          launchScheme;
    LaunchConfig         launch;
    std::filesystem::path loadedFrom;
};

// Parses the supported PROJECT_LIGHT_* keys from dotenv text. Unknown keys are
// ignored so the file can also serve other local tooling.
[[nodiscard]] auto ParseClientSettings(std::string_view dotenv, std::string source = {})
    -> std::expected<ClientSettings, std::string>;

// Loads PROJECT_LIGHT_ENV_FILE when set; otherwise searches the working
// directory and its parents for project-light.env or .env, then the user's
// ~/.config/project-light/client.env. Process environment variables override
// values loaded from a file. A missing implicit file is not an error.
[[nodiscard]] auto LoadClientSettings(const std::filesystem::path& workingDirectory = {})
    -> std::expected<ClientSettings, std::string>;

} // namespace ZHLN::ProjectLight
