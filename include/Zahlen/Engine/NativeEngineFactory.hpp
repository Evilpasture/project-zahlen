// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <Zahlen/COM/EngineFactory.hpp>
namespace ZHLN {
class Engine;
}
namespace ZHLN::COM {
// Native composition boundary for optional runtime libraries. No callback or
// native object crosses the consumer's COM interface. Configuration completes
// synchronously before the COM engine is returned; core knows no domain module.
using NativeEngineInitializer = void (*)(Engine&);
[[nodiscard]] ZHLN_API auto CreateConfiguredEngine(const ChainInNode*, NativeEngineInitializer) noexcept -> Result<IEngine*>;
} // namespace ZHLN::COM
