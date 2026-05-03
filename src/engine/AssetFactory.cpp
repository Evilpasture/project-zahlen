#include <Zahlen/AssetFactory.hpp>
#include <Zahlen/Resources.hpp>
#include <LLGL/LLGL.h>
#include <Jolt/Core/Core.h> // For JPH::Array
#include <cstring>

namespace ZHLN::AssetFactory {

Mesh CreatePlane(RenderContext& ctx, float extent, const JPH::Vec4& color) {
	JPH::Array<Vertex> data = {
		{{-extent, 0.0f, -extent}, color}, {{extent, 0.0f, -extent}, color},
		{{extent, 0.0f, extent}, color},   {{-extent, 0.0f, -extent}, color},
		{{extent, 0.0f, extent}, color},   {{-extent, 0.0f, extent}, color}
	};

	BufferHandle vbo = ctx.CreateVertexBuffer(data.data(), data.size() * sizeof(Vertex));
	return Mesh{.vertexBuffer = vbo, .vertexCount = static_cast<uint32_t>(data.size())};
}

Mesh CreateBox(RenderContext& ctx, JPH::Vec3Arg halfExtents, const JPH::Vec4& color) {
	const float x = halfExtents.GetX(), y = halfExtents.GetY(), z = halfExtents.GetZ();

	JPH::Array<Vertex> data = {
		{{-x, -y, z}, color},  {{x, -y, z}, color},   {{x, y, z}, color},    {{-x, -y, z}, color},
		{{x, y, z}, color},    {{-x, y, z}, color},   {{x, -y, -z}, color},  {{-x, -y, -z}, color},
		{{-x, y, -z}, color},  {{x, -y, -z}, color},  {{-x, y, -z}, color},  {{x, y, -z}, color},
		{{-x, -y, -z}, color}, {{-x, -y, z}, color},  {{-x, y, z}, color},   {{-x, -y, -z}, color},
		{{-x, y, z}, color},   {{-x, y, -z}, color},  {{x, -y, z}, color},   {{x, -y, -z}, color},
		{{x, y, -z}, color},   {{x, -y, z}, color},   {{x, y, -z}, color},   {{x, y, z}, color},
		{{-x, y, z}, color},   {{x, y, z}, color},    {{x, y, -z}, color},   {{-x, y, z}, color},
		{{x, y, -z}, color},   {{-x, y, -z}, color},  {{-x, -y, -z}, color}, {{x, -y, -z}, color},
		{{x, -y, z}, color},   {{-x, -y, -z}, color}, {{x, -y, z}, color},   {{-x, -y, z}, color}
	};

	BufferHandle vbo = ctx.CreateVertexBuffer(data.data(), data.size() * sizeof(Vertex));
	return Mesh{.vertexBuffer = vbo, .vertexCount = static_cast<uint32_t>(data.size())};
}

Material CreateBasicMaterial(RenderContext& ctx) {
	PipelineDesc desc;

	const char* rendererName = ctx.GetRendererName();
	bool isMetalRenderer = (rendererName && std::strcmp(rendererName, "Metal") == 0);
	desc.isMetal = isMetalRenderer;

	if (desc.isMetal) {
		desc.vertexShaderData = ZHLN_Resource_BasicMetal;
		desc.fragShaderData   = ZHLN_Resource_BasicMetal;
	} else {
		desc.vertexShaderData = ZHLN_Resource_BasicVertSpv;
		desc.vertexShaderSize = ZHLN_Resource_BasicVertSpv_Len;
		desc.fragShaderData   = ZHLN_Resource_BasicFragSpv;
		desc.fragShaderSize   = ZHLN_Resource_BasicFragSpv_Len;
	}

	return ctx.CreateMaterial(desc);
}

} // namespace ZHLN::AssetFactory