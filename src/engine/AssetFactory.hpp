#pragma once
#include "engine/Render.hpp"
#include "engine/Types.hpp"

namespace ZHLN::AssetFactory {
Mesh CreateTetrahedron(RenderContext& ctx);
Material CreateBasicMaterial(RenderContext& ctx);
} // namespace ZHLN::AssetFactory