// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// extras/glTF/glTF.cppm
export module ZHLN.glTF;

extern "C++" {
namespace ZHLN {
class Engine;
}
}

export namespace ZHLN::glTF {

void Initialize(ZHLN::Engine& engine);

} // namespace ZHLN::glTF
