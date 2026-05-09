#include <Jolt/Core/Core.h>
#include <Zahlen/AssetFactory.hpp>

namespace ZHLN::AssetFactory {

Mesh CreatePlane(RenderContext& ctx, float extent, const JPH::Vec4& color) {
	JPH::Array<Vertex> data = {
		Vertex{.position={-extent, 0.0f, extent},  .normal={0, 1, 0}, .tangent={1, 0, 0, 1}, .uv0={0, 0}, .uv1={0, 0}},
		Vertex{.position={extent, 0.0f, extent},   .normal={0, 1, 0}, .tangent={1, 0, 0, 1}, .uv0={1, 0}, .uv1={0, 0}},
		Vertex{.position={extent, 0.0f, -extent},  .normal={0, 1, 0}, .tangent={1, 0, 0, 1}, .uv0={1, 1}, .uv1={0, 0}},
		Vertex{.position={-extent, 0.0f, -extent}, .normal={0, 1, 0}, .tangent={1, 0, 0, 1}, .uv0={0, 1}, .uv1={0, 0}}
	};
	JPH::Array<uint32_t> indices = { 0, 1, 2, 2, 3, 0 };
	return ctx.CreateMesh(data.data(), data.size(), indices.data(), indices.size());
}

Mesh CreateBox(RenderContext& ctx, JPH::Vec3Arg half, const JPH::Vec4& color) {
	float x = half.GetX(), y = half.GetY(), z = half.GetZ();
	JPH::Array<Vertex> data = {
		// Front
		Vertex{.position={-x, -y, z}, .normal={0, 0, 1}, .tangent={1, 0, 0, 1}, .uv0={0, 1}, .uv1={0,0}}, Vertex{.position={x, -y, z}, .normal={0, 0, 1}, .tangent={1, 0, 0, 1}, .uv0={1, 1}, .uv1={0,0}},
		Vertex{.position={x, y, z}, .normal={0, 0, 1}, .tangent={1, 0, 0, 1}, .uv0={1, 0}, .uv1={0,0}}, Vertex{.position={-x, y, z}, .normal={0, 0, 1}, .tangent={1, 0, 0, 1}, .uv0={0, 0}, .uv1={0,0}},
		// Back
		Vertex{.position={x, -y, -z}, .normal={0, 0, -1}, .tangent={-1, 0, 0, 1}, .uv0={0, 1}, .uv1={0,0}}, Vertex{.position={-x, -y, -z}, .normal={0, 0, -1}, .tangent={-1, 0, 0, 1}, .uv0={1, 1}, .uv1={0,0}},
		Vertex{.position={-x, y, -z}, .normal={0, 0, -1}, .tangent={-1, 0, 0, 1}, .uv0={1, 0}, .uv1={0,0}}, Vertex{.position={x, y, -z}, .normal={0, 0, -1}, .tangent={-1, 0, 0, 1}, .uv0={0, 0}, .uv1={0,0}},
		// Top
		Vertex{.position={-x, y, z}, .normal={0, 1, 0}, .tangent={1, 0, 0, 1}, .uv0={0, 1}, .uv1={0,0}}, Vertex{.position={x, y, z}, .normal={0, 1, 0}, .tangent={1, 0, 0, 1}, .uv0={1, 1}, .uv1={0,0}},
		Vertex{.position={x, y, -z}, .normal={0, 1, 0}, .tangent={1, 0, 0, 1}, .uv0={1, 0}, .uv1={0,0}}, Vertex{.position={-x, y, -z}, .normal={0, 1, 0}, .tangent={1, 0, 0, 1}, .uv0={0, 0}, .uv1={0,0}},
		// Bottom
		Vertex{.position={-x, -y, -z}, .normal={0, -1, 0}, .tangent={1, 0, 0, 1}, .uv0={0, 1}, .uv1={0,0}}, Vertex{.position={x, -y, -z}, .normal={0, -1, 0}, .tangent={1, 0, 0, 1}, .uv0={1, 1}, .uv1={0,0}},
		Vertex{.position={x, -y, z}, .normal={0, -1, 0}, .tangent={1, 0, 0, 1}, .uv0={1, 0}, .uv1={0,0}}, Vertex{.position={-x, -y, z}, .normal={0, -1, 0}, .tangent={1, 0, 0, 1}, .uv0={0, 0}, .uv1={0,0}},
		// Right
		Vertex{.position={x, -y, z}, .normal={1, 0, 0}, .tangent={0, 0, -1, 1}, .uv0={0, 1}, .uv1={0,0}}, Vertex{.position={x, -y, -z}, .normal={1, 0, 0}, .tangent={0, 0, -1, 1}, .uv0={1, 1}, .uv1={0,0}},
		Vertex{.position={x, y, -z}, .normal={1, 0, 0}, .tangent={0, 0, -1, 1}, .uv0={1, 0}, .uv1={0,0}}, Vertex{.position={x, y, z}, .normal={1, 0, 0}, .tangent={0, 0, -1, 1}, .uv0={0, 0}, .uv1={0,0}},
		// Left
		Vertex{.position={-x, -y, -z}, .normal={-1, 0, 0}, .tangent={0, 0, 1, 1}, .uv0={0, 1}, .uv1={0,0}}, Vertex{.position={-x, -y, z}, .normal={-1, 0, 0}, .tangent={0, 0, 1, 1}, .uv0={1, 1}, .uv1={0,0}},
		Vertex{.position={-x, y, z}, .normal={-1, 0, 0}, .tangent={0, 0, 1, 1}, .uv0={1, 0}, .uv1={0,0}}, Vertex{.position={-x, y, -z}, .normal={-1, 0, 0}, .tangent={0, 0, 1, 1}, .uv0={0, 0}, .uv1={0,0}}
	};

	JPH::Array<uint32_t> indices = {
		0, 1, 2, 2, 3, 0,       4, 5, 6, 6, 7, 4,
		8, 9, 10, 10, 11, 8,    12, 13, 14, 14, 15, 12,
		16, 17, 18, 18, 19, 16, 20, 21, 22, 22, 23, 20
	};
	return ctx.CreateMesh(data.data(), data.size(), indices.data(), indices.size());
}

Material CreateBasicMaterial(RenderContext& ctx) {
	return ctx.CreateMaterial();
}

} // namespace ZHLN::AssetFactory