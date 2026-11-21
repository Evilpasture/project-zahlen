// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#include "render/RenderInternal.hpp"

namespace ZHLN {

auto RenderContext::Impl::SetupUI() -> std::expected<void, ErrorCode> {
    return uiRenderer.Init(*this);
}

}
