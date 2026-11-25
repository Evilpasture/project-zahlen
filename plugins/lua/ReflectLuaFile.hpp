// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Opt-in host-side filesystem helpers for ZHLN::ReflectLua.
//
// Core <lua/ReflectLua.hpp> is strictly in-memory / string-only and never
// touches the host OS filesystem. Include this header only from host CLI tools,
// config loaders, or tests that explicitly read .lua scripts from OS paths.

#include "ReflectLua.hpp"
#include <expected>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>

namespace ZHLN::ReflectLua {

[[nodiscard]] inline auto ReadFileText(const std::filesystem::path& path) noexcept -> std::expected<std::string, ErrorCode> {
    std::error_code ec;
    if (path.empty() || !std::filesystem::is_regular_file(path, ec) || ec) {
        return std::unexpected(LuaError::FileReadFailed);
    }
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        return std::unexpected(LuaError::FileReadFailed);
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    if (in.bad()) {
        return std::unexpected(LuaError::FileReadFailed);
    }
    return buffer.str();
}

[[nodiscard]] inline auto ExecuteFile(Vm& vm, const std::filesystem::path& path) noexcept -> std::expected<void, ErrorCode> {
    auto source = ReadFileText(path);
    if (!source) {
        return std::unexpected(source.error());
    }
    const std::string chunkName = "@" + path.generic_string();
    return vm.ExecuteString(*source, chunkName);
}

template <typename T>
[[nodiscard]] auto ApplyPatchFile(T& target, const std::filesystem::path& path, LuaBudget budget = {}, std::string* outDetail = nullptr)
    -> std::expected<void, ErrorCode> {
    auto source = ReadFileText(path);
    if (!source) {
        return std::unexpected(source.error());
    }
    const std::string chunkName = "@" + path.generic_string();
    return ApplyPatchString(target, *source, chunkName, budget, outDetail);
}

template <typename T>
[[nodiscard]] auto EvaluateFile(const std::filesystem::path& path, LuaBudget budget = {}, std::string* outDetail = nullptr)
    -> std::expected<T, ErrorCode> {
    auto source = ReadFileText(path);
    if (!source) {
        return std::unexpected(source.error());
    }
    const std::string chunkName = "@" + path.generic_string();
    return EvaluateString<T>(*source, chunkName, budget, outDetail);
}

template <typename OutT, typename... InArgs>
[[nodiscard]] auto EvaluateCallFile(const std::filesystem::path& path, LuaBudget budget, const InArgs&... inputs)
    -> std::expected<OutT, ErrorCode> {
    auto source = ReadFileText(path);
    if (!source) {
        return std::unexpected(source.error());
    }
    const std::string chunkName = "@" + path.generic_string();
    return EvaluateCallString<OutT>(*source, chunkName, budget, inputs...);
}

} // namespace ZHLN::ReflectLua
