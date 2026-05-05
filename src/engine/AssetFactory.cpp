#include <Jolt/Core/Core.h>
#include <Zahlen/AssetFactory.hpp>
#include <Zahlen/Resources.hpp>
#include <cstring>

namespace ZHLN::AssetFactory {

Mesh CreatePlane(RenderContext& ctx, float extent, const JPH::Vec4& color) {
	// Strict CCW winding for a plane facing UP (+Y)
	JPH::Array<Vertex> data = {// Triangle 1: Bottom-Left -> Bottom-Right -> Top-Right
							   {{-extent, 0.0f, extent}, color},
							   {{extent, 0.0f, extent}, color},
							   {{extent, 0.0f, -extent}, color},
							   // Triangle 2: Top-Right -> Top-Left -> Bottom-Left
							   {{extent, 0.0f, -extent}, color},
							   {{-extent, 0.0f, -extent}, color},
							   {{-extent, 0.0f, extent}, color}};

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

	const char* rendererName = ctx.GetRendererName();
	bool isMetalRenderer = (rendererName && std::strstr(rendererName, "Metal") != nullptr);
	desc.isMetal = isMetalRenderer;

	if (desc.isMetal) {
		desc.vertexShaderData = ZHLN_Resource_BasicMetal;
		desc.vertexShaderSize = ZHLN_Resource_BasicMetal_Len;
		desc.fragShaderData = ZHLN_Resource_BasicMetal;
		desc.fragShaderSize = ZHLN_Resource_BasicMetal_Len;
	} else {
		desc.vertexShaderData = ZHLN_Resource_BasicVertSpv;
		desc.vertexShaderSize = ZHLN_Resource_BasicVertSpv_Len;
		desc.fragShaderData = ZHLN_Resource_BasicFragSpv;
		desc.fragShaderSize = ZHLN_Resource_BasicFragSpv_Len;
	}

	return ctx.CreateMaterial(desc);
}

} // namespace ZHLN::AssetFactory