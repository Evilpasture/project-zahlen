// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <filesystem>

namespace ZHLN {
class Engine;
}

namespace ZHLN::Tools {
// Shared document import for the dedicated scene host and world editor.
// Core and the ordinary game executable never depend on document plugins.
[[nodiscard]] auto LoadSceneDocument(Engine& engine, const std::filesystem::path& path) -> bool;
}
