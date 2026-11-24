// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <Zahlen/COM/ILogicWorld.hpp>
#include <Zahlen/COM/ErrorTypes.hpp>
#include <Zahlen/COM/LoggerTypes.hpp>
#include <Zahlen/Config.hpp>
#include <Zahlen/Core/COM/Ref.hpp>
namespace ZHLN::COM {
// Accepts PhysicsConfig, LoggerConfig and ErrorConfig in one chain -- any
// subset of them, or nullptr for defaults -- the same slices CreateEngineRaw
// takes. Acquires Jolt registration through the engine runtime, but never
// initializes a window, GPU or audio.
ZHLN_API auto CreateLogicWorldRaw(const ChainInNode* config = nullptr) noexcept -> Result<ILogicWorld*>;
[[nodiscard]] inline auto   CreateLogicWorld(const ChainInNode* config = nullptr) noexcept -> std::expected<Ref<ILogicWorld>, ErrorCode> {
    auto created = CreateLogicWorldRaw(config);
    if (!created.HasValue())
        return std::unexpected(created.error);
    return Ref<ILogicWorld>::Adopt(created.value);
}
} // namespace ZHLN::COM
