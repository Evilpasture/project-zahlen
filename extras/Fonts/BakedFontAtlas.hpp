// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// extras/Fonts/BakedFontAtlas.hpp
//
// The baked side of the font pipeline. tools/fontbm.sh bakes a TTF into a
// .fnt metrics file, a .png texture page and a LICENSE.txt under
// resources/fonts/<Family>/. When such a bake is committed, the engine
// consumes it verbatim: no TTF parsing at runtime, no dependence on which
// system fonts happen to be installed.
//
// Core stays ignorant of the bake: CreativeWorksFactory exposes a
// BakedFontLoader hook and this module implements it. A host built without
// extras (the minimal configuration) never installs the hook, and there the
// runtime TTF parse over system fonts is the font.
#pragma once
#include <Zahlen/Render/Handles.hpp> // TextureHandle

namespace ZHLN {
class RenderContext;
namespace ECS {
class Registry;
}
} // namespace ZHLN

namespace ZHLN::Fonts {

// Installs LoadBakedFontAtlas into the core's font-atlas hook. Call once per
// process, before any scene seeds its atlas; the composition roots
// (app/, the UI editor) are the only callers.
auto InstallBakedFontLoader() -> void;

// The hook body: locate the committed bake, parse the .fnt, decode the
// texture page and store the result on the UISettingsComponent singleton.
// Returns TextureHandle::Invalid when no bake exists or is readable, which
// defers to the core's runtime TTF path.
auto LoadBakedFontAtlas(RenderContext& ctx, ECS::Registry& registry) -> TextureHandle;

} // namespace ZHLN::Fonts
