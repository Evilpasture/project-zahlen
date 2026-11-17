// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <Zahlen/COM/ILogicWorld.hpp>
#include <Zahlen/Core/COM/Ref.hpp>
namespace ZHLN::COM {
// Accepts PhysicsConfig, or nullptr for defaults. Acquires Jolt registration
// through the engine runtime, but never initializes a window, GPU or audio.
[[nodiscard]] ZHLN_API auto CreateLogicWorld(const ChainInNode* config = nullptr) noexcept -> Result<ILogicWorld*>;
[[nodiscard]] inline auto   CreateLogicWorldRef(const ChainInNode* config = nullptr) noexcept -> std::expected<Ref<ILogicWorld>, ErrorCode> {
    auto created = CreateLogicWorld(config);
    if (!created.HasValue())
        return std::unexpected(created.error);
    return Ref<ILogicWorld>::Adopt(created.value);
}
} // namespace ZHLN::COM
