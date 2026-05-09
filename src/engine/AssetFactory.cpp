#include <Jolt/Core/Core.h>
#include <Zahlen/AssetFactory.hpp>
#include <Zahlen/Resources.hpp>
#include <cstring>

namespace ZHLN::AssetFactory {

Mesh CreatePlane(RenderContext& ctx, float extent, const JPH::Vec4& color) {
	// Strict CCW winding for a plane facing UP (+Y)
	JPH::Array<Vertex> data = {{{-extent, 0.0f, extent}, color},  {{extent, 0.0f, extent}, color},
							   {{extent, 0.0f, -extent}, color},  {{extent, 0.0f, -extent}, color},
							   {{-extent, 0.0f, -extent}, color}, {{-extent, 0.0f, extent}, color}};

	BufferHandle vbo = ctx.CreateVertexBuffer(data.data(), data.size() * sizeof(Vertex));
	return Mesh{.vertexBuffer = vbo, .vertexCount = static_cast<uint32_t>(data.size())};
}

Mesh CreateBox(RenderContext& ctx, JPH::Vec3Arg halfExtents, const JPH::Vec4& color) {
	const float x = halfExtents.GetX(), y = halfExtents.GetY(), z = halfExtents.GetZ();

	// Strict CCW Outward-facing Box
	JPH::Array<Vertex> data = {// Front (+Z)
							   {{-x, -y, z}, color},
							   {{x, -y, z}, color},
							   {{x, y, z}, color},
							   {{x, y, z}, color},
							   {{-x, y, z}, color},
							   {{-x, -y, z}, color},
							   // Back (-Z)
							   {{x, -y, -z}, color},
							   {{-x, -y, -z}, color},
							   {{-x, y, -z}, color},
							   {{-x, y, -z}, color},
							   {{x, y, -z}, color},
							   {{x, -y, -z}, color},
							   // Top (+Y)
							   {{-x, y, z}, color},
							   {{x, y, z}, color},
							   {{x, y, -z}, color},
							   {{x, y, -z}, color},
							   {{-x, y, -z}, color},
							   {{-x, y, z}, color},
							   // Bottom (-Y)
							   {{-x, -y, -z}, color},
							   {{x, -y, -z}, color},
							   {{x, -y, z}, color},
							   {{x, -y, z}, color},
							   {{-x, -y, z}, color},
							   {{-x, -y, -z}, color},
							   // Right (+X)
							   {{x, -y, z}, color},
							   {{x, -y, -z}, color},
							   {{x, y, -z}, color},
							   {{x, y, -z}, color},
							   {{x, y, z}, color},
							   {{x, -y, z}, color},
							   // Left (-X)
							   {{-x, -y, -z}, color},
							   {{-x, -y, z}, color},
							   {{-x, y, z}, color},
							   {{-x, y, z}, color},
							   {{-x, y, -z}, color},
							   {{-x, -y, -z}, color}};

	BufferHandle vbo = ctx.CreateVertexBuffer(data.data(), data.size() * sizeof(Vertex));
	return Mesh{.vertexBuffer = vbo, .vertexCount = static_cast<uint32_t>(data.size())};
}

Material CreateBasicMaterial(RenderContext& ctx) {
	PipelineDesc desc;

	// ZHLN now auto-reflects the entry points from these blobs.
	// Whether it's "main", "VSMain", or "SkyboxVertexShader", it just works.
	desc.vertexShaderData = ZHLN_Resource_BasicVertSpv;
	desc.vertexShaderSize = ZHLN_Resource_BasicVertSpv_Len;
	desc.fragShaderData = ZHLN_Resource_BasicFragSpv;
	desc.fragShaderSize = ZHLN_Resource_BasicFragSpv_Len;

	return ctx.CreateMaterial(desc);
}

} // namespace ZHLN::AssetFactory