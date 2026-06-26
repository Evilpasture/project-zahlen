// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// File: src/engine/MeshBuilder.cpp
#include "Resources.hpp"
#include "Zahlen/Components.hpp"
#include "Zahlen/Render.hpp"

#include <Zahlen/AssetFactory.hpp>
#include <Zahlen/Math3D.hpp>
#include <cmath>
#include <cstring>
#include <vector>

namespace ZHLN::AssetFactory {

// --- HELPER: Generates a procedural 2D grid texture ---
static uint32_t CreateProceduralCheckerboard(RenderContext& ctx) {
	const uint32_t size = 256;
	std::vector<uint32_t> pixels(static_cast<size_t>(size * size));
	for (uint32_t y = 0; y < size; y++) {
		for (uint32_t x = 0; x < size; x++) {
			// XOR logic creates a perfect grid
			bool isWhite = ((x / 32) + (y / 32)) % 2 == 0;
			pixels[(y * size) + x] = isWhite ? 0xFFFFFFFF : 0xFF333333;
		}
	}
	return ctx.CreateTexture(pixels.data(), size, size);
}

Material CreateBasicMaterial(RenderContext& ctx, bool doubleSided, bool alphaBlend) {
	PipelineDesc desc;
	desc.vertexShaderData = ZHLN_Resource_BasicVertSpv;
	desc.vertexShaderSize = ZHLN_Resource_BasicVertSpv_Len;

	if (alphaBlend) {
		desc.fragShaderData = ZHLN_Resource_ForwardFragSpv;
		desc.fragShaderSize = ZHLN_Resource_ForwardFragSpv_Len;
	} else {
		desc.fragShaderData = ZHLN_Resource_BasicFragSpv;
		desc.fragShaderSize = ZHLN_Resource_BasicFragSpv_Len;
	}

	desc.doubleSided = doubleSided;
	desc.alphaBlend = alphaBlend;
	Material mat = ctx.CreateMaterial(desc);
	mat.albedoIndex = 1;
	return mat;
}

Mesh CreatePlane(RenderContext& ctx, float extent, const JPH::Vec4& color) {
	Packed1010102 n = Math::PackNormal(0.0f, 1.0f, 0.0f);
	Packed1010102 t = Math::PackNormal(1.0f, 0.0f, 0.0f, 1.0f);
	PackedRGBA8 c = Math::PackColor(color.GetX(), color.GetY(), color.GetZ(), color.GetW());

	std::vector<VertexPosition> positions = {{{-extent, 0.0f, extent}},	 {{extent, 0.0f, extent}},
											 {{extent, 0.0f, -extent}},	 {{extent, 0.0f, -extent}},
											 {{-extent, 0.0f, -extent}}, {{-extent, 0.0f, extent}}};

	std::vector<VertexAttributes> attributes = {
		{.normal = n, .tangent = t, .uv = Math::PackUV(0.0f, 1.0f), .color = c},
		{n, t, Math::PackUV(1.0f, 1.0f), c},
		{.normal = n, .tangent = t, .uv = Math::PackUV(1.0f, 0.0f), .color = c},
		{n, t, Math::PackUV(1.0f, 0.0f), c},
		{.normal = n, .tangent = t, .uv = Math::PackUV(0.0f, 0.0f), .color = c},
		{n, t, Math::PackUV(0.0f, 1.0f), c}};

	BufferHandle posVbo =
		ctx.CreateVertexBuffer(positions.data(), positions.size() * sizeof(VertexPosition));
	BufferHandle attrVbo =
		ctx.CreateVertexBuffer(attributes.data(), attributes.size() * sizeof(VertexAttributes));

	auto finalMesh = Mesh{.posBuffer = posVbo,
						  .attrBuffer = attrVbo,
						  .skinBuffer = BufferHandle::Invalid,
						  .indexBuffer = BufferHandle::Invalid,
						  .vertexCount = static_cast<uint32_t>(positions.size()),
						  .indexCount = 0};
	ctx.BuildMeshBLAS(finalMesh);
	return finalMesh;
}

Mesh CreateBox(RenderContext& ctx, JPH::Vec3Arg halfExtents, const JPH::Vec4& color) {
	const float x = halfExtents.GetX();
	const float y = halfExtents.GetY();
	const float z = halfExtents.GetZ();
	PackedRGBA8 c = Math::PackColor(color.GetX(), color.GetY(), color.GetZ(), color.GetW());

	// Front/Back/Top/Bottom/Right/Left normals
	Packed1010102 nZ = Math::PackNormal(0, 0, 1);
	Packed1010102 tZ = Math::PackNormal(1, 0, 0, 1);
	Packed1010102 nNZ = Math::PackNormal(0, 0, -1);
	Packed1010102 tNZ = Math::PackNormal(-1, 0, 0, 1);
	Packed1010102 nY = Math::PackNormal(0, 1, 0);
	Packed1010102 tY = Math::PackNormal(1, 0, 0, 1);
	Packed1010102 nNY = Math::PackNormal(0, -1, 0);
	Packed1010102 tNY = Math::PackNormal(1, 0, 0, 1);
	Packed1010102 nX = Math::PackNormal(1, 0, 0);
	Packed1010102 tX = Math::PackNormal(0, 0, -1, 1);
	Packed1010102 nNX = Math::PackNormal(-1, 0, 0);
	Packed1010102 tNX = Math::PackNormal(0, 0, 1, 1);

	auto uv00 = Math::PackUV(0.0f, 0.0f);
	auto uv10 = Math::PackUV(1.0f, 0.0f);
	auto uv01 = Math::PackUV(0.0f, 1.0f);
	auto uv11 = Math::PackUV(1.0f, 1.0f);

	std::vector<VertexPosition> positions = {// Front (+Z)
											 {{-x, -y, z}},
											 {{x, -y, z}},
											 {{x, y, z}},
											 {{x, y, z}},
											 {{-x, y, z}},
											 {{-x, -y, z}},
											 // Back (-Z)
											 {{x, -y, -z}},
											 {{-x, -y, -z}},
											 {{-x, y, -z}},
											 {{-x, y, -z}},
											 {{x, y, -z}},
											 {{x, -y, -z}},
											 // Top (+Y)
											 {{-x, y, z}},
											 {{x, y, z}},
											 {{x, y, -z}},
											 {{x, y, -z}},
											 {{-x, y, -z}},
											 {{-x, y, z}},
											 // Bottom (-Y)
											 {{-x, -y, -z}},
											 {{x, -y, -z}},
											 {{x, -y, z}},
											 {{x, -y, z}},
											 {{-x, -y, z}},
											 {{-x, -y, -z}},
											 // Right (+X)
											 {{x, -y, z}},
											 {{x, -y, -z}},
											 {{x, y, -z}},
											 {{x, y, -z}},
											 {{x, y, z}},
											 {{x, -y, z}},
											 // Left (-X)
											 {{-x, -y, -z}},
											 {{-x, -y, z}},
											 {{-x, y, z}},
											 {{-x, y, z}},
											 {{-x, y, -z}},
											 {{-x, -y, -z}}};

	std::vector<VertexAttributes> attributes = {// Front (+Z)
												{nZ, tZ, uv01, c},
												{nZ, tZ, uv11, c},
												{nZ, tZ, uv10, c},
												{nZ, tZ, uv10, c},
												{nZ, tZ, uv00, c},
												{nZ, tZ, uv01, c},
												// Back (-Z)
												{nNZ, tNZ, uv01, c},
												{nNZ, tNZ, uv11, c},
												{nNZ, tNZ, uv10, c},
												{nNZ, tNZ, uv10, c},
												{nNZ, tNZ, uv00, c},
												{nNZ, tNZ, uv01, c},
												// Top (+Y)
												{nY, tY, uv01, c},
												{nY, tY, uv11, c},
												{nY, tY, uv10, c},
												{nY, tY, uv10, c},
												{nY, tY, uv00, c},
												{nY, tY, uv01, c},
												// Bottom (-Y)
												{nNY, tNY, uv01, c},
												{nNY, tNY, uv11, c},
												{nNY, tNY, uv10, c},
												{nNY, tNY, uv10, c},
												{nNY, tNY, uv00, c},
												{nNY, tNY, uv01, c},
												// Right (+X)
												{nX, tX, uv01, c},
												{nX, tX, uv11, c},
												{nX, tX, uv10, c},
												{nX, tX, uv10, c},
												{nX, tX, uv00, c},
												{nX, tX, uv01, c},
												// Left (-X)
												{nNX, tNX, uv01, c},
												{nNX, tNX, uv11, c},
												{nNX, tNX, uv10, c},
												{nNX, tNX, uv10, c},
												{nNX, tNX, uv00, c},
												{nNX, tNX, uv01, c}};

	BufferHandle posVbo =
		ctx.CreateVertexBuffer(positions.data(), positions.size() * sizeof(VertexPosition));
	BufferHandle attrVbo =
		ctx.CreateVertexBuffer(attributes.data(), attributes.size() * sizeof(VertexAttributes));

	auto finalMesh = Mesh{.posBuffer = posVbo,
						  .attrBuffer = attrVbo,
						  .skinBuffer = BufferHandle::Invalid,
						  .indexBuffer = BufferHandle::Invalid,
						  .vertexCount = static_cast<uint32_t>(positions.size()),
						  .indexCount = 0};
	ctx.BuildMeshBLAS(finalMesh);
	return finalMesh;
}

Mesh CreateTerrain(RenderContext& ctx, int sampleCount, float worldSize, float maxHeight,
				   float* outHeights) {
	// outHeights allocation size is assumed to be sampleCount * sampleCount
	auto hash = [](float x, float y) -> float {
		uint32_t ix = 0;
		std::memcpy(&ix, &x, sizeof(float));
		uint32_t iy = 0;
		std::memcpy(&iy, &y, sizeof(float));
		ix *= 1597u;
		iy *= 5147u;
		uint32_t hashVal = (ix ^ iy) * 0x9E3779B9u;
		return static_cast<float>(hashVal & 0xFFFFFFu) / 16777215.0f;
	};

	auto lerp = [](float a, float b, float t) { return a + t * (b - a); };

	auto noise = [&](float x, float y) {
		float ix = std::floor(x);
		float iy = std::floor(y);
		float fx = x - ix;
		float fy = y - iy;
		float ux = fx * fx * fx * (fx * (fx * 6.0f - 15.0f) + 10.0f);
		float uy = fy * fy * fy * (fy * (fy * 6.0f - 15.0f) + 10.0f);
		return lerp(lerp(hash(ix, iy), hash(ix + 1.0f, iy), ux),
					lerp(hash(ix, iy + 1.0f), hash(ix + 1.0f, iy + 1.0f), ux), uy);
	};

	auto get_height = [&](float x, float z) {
		float val = 0.0f;
		float amp = 0.5f;
		float freq = 0.015f;
		float tx = x * freq;
		float tz = z * freq;
		for (int i = 0; i < 4; i++) {
			val += amp * noise(tx, tz);
			tx *= 2.1f;
			tz *= 2.15f;
			amp *= 0.45f;
		}
		return std::pow(val, 1.4f) * maxHeight;
	};

	float halfSize = worldSize / 2.0f;
	float dx = worldSize / (sampleCount - 1);
	float dz = worldSize / (sampleCount - 1);

	for (int z = 0; z < sampleCount; ++z) {
		for (int x = 0; x < sampleCount; ++x) {
			float posX = -halfSize + x * dx;
			float posZ = -halfSize + z * dz;
			outHeights[x + (z * sampleCount)] = get_height(posX, posZ);
		}
	}

	std::vector<VertexPosition> positions;
	std::vector<VertexAttributes> attributes;
	positions.reserve(static_cast<size_t>((sampleCount - 1)) * (sampleCount - 1) * 6);
	attributes.reserve(static_cast<size_t>((sampleCount - 1)) * (sampleCount - 1) * 6);

	auto get_normal = [&](int x, int z) -> JPH::Vec3 {
		float posX = -halfSize + x * dx;
		float posZ = -halfSize + z * dz;
		float hL = (x > 0) ? outHeights[(x - 1) + z * sampleCount] : get_height(posX - dx, posZ);
		float hR = (x < sampleCount - 1) ? outHeights[(x + 1) + z * sampleCount]
										 : get_height(posX + dx, posZ);
		float hD = (z > 0) ? outHeights[x + (z - 1) * sampleCount] : get_height(posX, posZ - dz);
		float hU = (z < sampleCount - 1) ? outHeights[x + (z + 1) * sampleCount]
										 : get_height(posX, posZ + dz);
		JPH::Vec3 normal(hL - hR, 2.0f * dx, hD - hU);
		return normal.Normalized();
	};

	for (int z = 0; z < sampleCount - 1; ++z) {
		for (int x = 0; x < sampleCount - 1; ++x) {
			int idxA = x + z * sampleCount;
			int idxB = (x + 1) + z * sampleCount;
			int idxC = x + (z + 1) * sampleCount;
			int idxD = (x + 1) + (z + 1) * sampleCount;

			float ax = -halfSize + x * dx;
			float az = -halfSize + z * dz;
			float bx = -halfSize + (x + 1) * dx;
			float bz = -halfSize + z * dz;
			float cx = -halfSize + x * dx;
			float cz = -halfSize + (z + 1) * dz;
			float dx_ = -halfSize + (x + 1) * dx;
			float dz_ = -halfSize + (z + 1) * dz;

			float ay = outHeights[idxA];
			float by = outHeights[idxB];
			float cy = outHeights[idxC];
			float dy = outHeights[idxD];

			JPH::Vec3 nA = get_normal(x, z);
			JPH::Vec3 nB = get_normal(x + 1, z);
			JPH::Vec3 nC = get_normal(x, z + 1);
			JPH::Vec3 nD = get_normal(x + 1, z + 1);

			auto get_color = [&](float y, JPH::Vec3 normal) {
				float slope = normal.GetY();
				float normY = y / maxHeight;
				if (slope < 0.65f) {
					return Math::PackColor(0.35f, 0.32f, 0.29f, 1.0f);
				}
				if (normY > 0.75f) {
					return Math::PackColor(0.95f, 0.95f, 0.98f, 1.0f);
				}
				if (normY < 0.12f) {
					return Math::PackColor(0.72f, 0.64f, 0.48f, 1.0f);
				}
				float greenVar = 0.4f + 0.12f * std::sin(y * 0.5f);
				return Math::PackColor(0.12f, greenVar, 0.08f, 1.0f);
			};

			VertexPosition posA = {{ax, ay, az}};
			VertexAttributes attrA = {
				.normal = Math::PackNormal(nA.GetX(), nA.GetY(), nA.GetZ()),
				.tangent = Math::PackNormal(1.0f, 0.0f, 0.0f, 1.0f),
				.uv = Math::PackUV((float)x / sampleCount, (float)z / sampleCount),
				.color = get_color(ay, nA)};

			VertexPosition vB = {{bx, by, bz}};
			VertexAttributes attrB = {
				.normal = Math::PackNormal(nB.GetX(), nB.GetY(), nB.GetZ()),
				.tangent = Math::PackNormal(1.0f, 0.0f, 0.0f, 1.0f),
				.uv = Math::PackUV((float)(x + 1) / sampleCount, (float)z / sampleCount),
				.color = get_color(by, nB)};

			VertexPosition vC = {{cx, cy, cz}};
			VertexAttributes attrC = {
				.normal = Math::PackNormal(nC.GetX(), nC.GetY(), nC.GetZ()),
				.tangent = Math::PackNormal(1.0f, 0.0f, 0.0f, 1.0f),
				.uv = Math::PackUV((float)x / sampleCount, (float)(z + 1) * sampleCount),
				.color = get_color(cy, nC)};

			VertexPosition vD = {{dx_, dy, dz_}};
			VertexAttributes attrD = {
				.normal = Math::PackNormal(nD.GetX(), nD.GetY(), nD.GetZ()),
				.tangent = Math::PackNormal(1.0f, 0.0f, 0.0f, 1.0f),
				.uv = Math::PackUV((float)(x + 1) / sampleCount, (float)(z + 1) / sampleCount),
				.color = get_color(dy, nD)};

			positions.push_back(posA);
			attributes.push_back(attrA);

			positions.push_back(vC);
			attributes.push_back(attrC);

			positions.push_back(vB);
			attributes.push_back(attrB);

			positions.push_back(vB);
			attributes.push_back(attrB);

			positions.push_back(vC);
			attributes.push_back(attrC);

			positions.push_back(vD);
			attributes.push_back(attrD);
		}
	}

	BufferHandle posVbo =
		ctx.CreateVertexBuffer(positions.data(), positions.size() * sizeof(VertexPosition));
	BufferHandle attrVbo =
		ctx.CreateVertexBuffer(attributes.data(), attributes.size() * sizeof(VertexAttributes));

	auto finalMesh = Mesh{.posBuffer = posVbo,
						  .attrBuffer = attrVbo,
						  .skinBuffer = BufferHandle::Invalid,
						  .indexBuffer = BufferHandle::Invalid,
						  .vertexCount = static_cast<uint32_t>(positions.size()),
						  .indexCount = 0};
	ctx.BuildMeshBLAS(finalMesh);
	return finalMesh;
}

} // namespace ZHLN::AssetFactory
