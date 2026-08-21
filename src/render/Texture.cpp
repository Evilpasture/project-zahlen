// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later
#include "Texture.hpp"

namespace ZHLN::Vk {
// NOTE: The legacy UpdateBindlessTextureSlot (descriptor-set based bindless
// texture registration) has been removed with the VK_EXT_descriptor_heap
// migration. Textures now register through
// RenderContext::Impl::WriteTextureSlotToHeap, which writes an image
// descriptor directly into the globalTextures[] region of the resource heap.
} // namespace ZHLN::Vk
