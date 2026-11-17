// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <Zahlen/COM/EngineFactory.hpp>
namespace ZHLN::Animation {
// Explicit optional-runtime composition. Plain COM::CreateEngine does not
// link, install, create state for, or execute this animation extension.
[[nodiscard]] auto        CreateEngine(const COM::ChainInNode* config = nullptr) noexcept -> COM::Result<COM::IEngine*>;
[[nodiscard]] inline auto CreateEngineRef(const COM::ChainInNode* config = nullptr) noexcept -> std::expected<COM::Ref<COM::IEngine>, ErrorCode> {
    const auto result = Animation::CreateEngine(config);
    if (!result.HasValue())
        return std::unexpected(result.error);
    return COM::Ref<COM::IEngine>::Adopt(result.value);
}
} // namespace ZHLN::Animation
