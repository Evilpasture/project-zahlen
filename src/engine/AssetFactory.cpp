#include "Resources.hpp"
#include "Zahlen/Components.hpp"
#include "Zahlen/Font8x8.hpp"
#include "Zahlen/Log.hpp"

#include <Zahlen/AssetFactory.hpp>
#include <Zahlen/Math3D.hpp> // For PackNormal, PackColor, etc.
#include <cgltf.h>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <stb_image.h>
#include <vector>

namespace ZHLN::AssetFactory {

Mesh CreatePlane(RenderContext& ctx, float extent, const JPH::Vec4& color) {
	Packed1010102 n = Math::PackNormal(0.0f, 1.0f, 0.0f);
	Packed1010102 t = Math::PackNormal(1.0f, 0.0f, 0.0f, 1.0f);
	PackedRGBA8 c = Math::PackColor(color.GetX(), color.GetY(), color.GetZ(), color.GetW());

	std::vector<Vertex> data = {
		{.position = {-extent, 0.0f, extent},
		 .normal = n,
		 .tangent = t,
		 .uv = Math::PackUV(0.0f, 1.0f),
		 .color = c,
		 ._padding = 0},
		{.position = {extent, 0.0f, extent},
		 .normal = n,
		 .tangent = t,
		 .uv = Math::PackUV(1.0f, 1.0f),
		 .color = c,
		 ._padding = 0},
		{.position = {extent, 0.0f, -extent},
		 .normal = n,
		 .tangent = t,
		 .uv = Math::PackUV(1.0f, 0.0f),
		 .color = c,
		 ._padding = 0},
		{.position = {extent, 0.0f, -extent},
		 .normal = n,
		 .tangent = t,
		 .uv = Math::PackUV(1.0f, 0.0f),
		 .color = c,
		 ._padding = 0},
		{.position = {-extent, 0.0f, -extent},
		 .normal = n,
		 .tangent = t,
		 .uv = Math::PackUV(0.0f, 0.0f),
		 .color = c,
		 ._padding = 0},
		{.position = {-extent, 0.0f, extent},
		 .normal = n,
		 .tangent = t,
		 .uv = Math::PackUV(0.0f, 1.0f),
		 .color = c,
		 ._padding = 0},
	};

	BufferHandle vbo = ctx.CreateVertexBuffer(data.data(), data.size() * sizeof(Vertex));
	return Mesh{.vertexBuffer = vbo, .vertexCount = static_cast<uint32_t>(data.size())};
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

	std::vector<Vertex> data = {
		// Front (+Z)
		{.position = {-x, -y, z},
		 .normal = nZ,
		 .tangent = tZ,
		 .uv = uv01,
		 .color = c,
		 ._padding = 0},
		{.position = {x, -y, z},
		 .normal = nZ,
		 .tangent = tZ,
		 .uv = uv11,
		 .color = c,
		 ._padding = 0},
		{.position = {x, y, z}, .normal = nZ, .tangent = tZ, .uv = uv10, .color = c, ._padding = 0},
		{.position = {x, y, z}, .normal = nZ, .tangent = tZ, .uv = uv10, .color = c, ._padding = 0},
		{.position = {-x, y, z},
		 .normal = nZ,
		 .tangent = tZ,
		 .uv = uv00,
		 .color = c,
		 ._padding = 0},
		{.position = {-x, -y, z},
		 .normal = nZ,
		 .tangent = tZ,
		 .uv = uv01,
		 .color = c,
		 ._padding = 0},
		// Back (-Z)
		{.position = {x, -y, -z},
		 .normal = nNZ,
		 .tangent = tNZ,
		 .uv = uv01,
		 .color = c,
		 ._padding = 0},
		{.position = {-x, -y, -z},
		 .normal = nNZ,
		 .tangent = tNZ,
		 .uv = uv11,
		 .color = c,
		 ._padding = 0},
		{.position = {-x, y, -z},
		 .normal = nNZ,
		 .tangent = tNZ,
		 .uv = uv10,
		 .color = c,
		 ._padding = 0},
		{.position = {-x, y, -z},
		 .normal = nNZ,
		 .tangent = tNZ,
		 .uv = uv10,
		 .color = c,
		 ._padding = 0},
		{.position = {x, y, -z},
		 .normal = nNZ,
		 .tangent = tNZ,
		 .uv = uv00,
		 .color = c,
		 ._padding = 0},
		{.position = {x, -y, -z},
		 .normal = nNZ,
		 .tangent = tNZ,
		 .uv = uv01,
		 .color = c,
		 ._padding = 0},
		// Top (+Y)
		{.position = {-x, y, z},
		 .normal = nY,
		 .tangent = tY,
		 .uv = uv01,
		 .color = c,
		 ._padding = 0},
		{.position = {x, y, z}, .normal = nY, .tangent = tY, .uv = uv11, .color = c, ._padding = 0},
		{.position = {x, y, -z},
		 .normal = nY,
		 .tangent = tY,
		 .uv = uv10,
		 .color = c,
		 ._padding = 0},
		{.position = {x, y, -z},
		 .normal = nY,
		 .tangent = tY,
		 .uv = uv10,
		 .color = c,
		 ._padding = 0},
		{.position = {-x, y, -z},
		 .normal = nY,
		 .tangent = tY,
		 .uv = uv00,
		 .color = c,
		 ._padding = 0},
		{.position = {-x, y, z},
		 .normal = nY,
		 .tangent = tY,
		 .uv = uv01,
		 .color = c,
		 ._padding = 0},
		// Bottom (-Y)
		{.position = {-x, -y, -z},
		 .normal = nNY,
		 .tangent = tNY,
		 .uv = uv01,
		 .color = c,
		 ._padding = 0},
		{.position = {x, -y, -z},
		 .normal = nNY,
		 .tangent = tNY,
		 .uv = uv11,
		 .color = c,
		 ._padding = 0},
		{.position = {x, -y, z},
		 .normal = nNY,
		 .tangent = tNY,
		 .uv = uv10,
		 .color = c,
		 ._padding = 0},
		{.position = {x, -y, z},
		 .normal = nNY,
		 .tangent = tNY,
		 .uv = uv10,
		 .color = c,
		 ._padding = 0},
		{.position = {-x, -y, z},
		 .normal = nNY,
		 .tangent = tNY,
		 .uv = uv00,
		 .color = c,
		 ._padding = 0},
		{.position = {-x, -y, -z},
		 .normal = nNY,
		 .tangent = tNY,
		 .uv = uv01,
		 .color = c,
		 ._padding = 0},
		// Right (+X)
		{.position = {x, -y, z},
		 .normal = nX,
		 .tangent = tX,
		 .uv = uv01,
		 .color = c,
		 ._padding = 0},
		{.position = {x, -y, -z},
		 .normal = nX,
		 .tangent = tX,
		 .uv = uv11,
		 .color = c,
		 ._padding = 0},
		{.position = {x, y, -z},
		 .normal = nX,
		 .tangent = tX,
		 .uv = uv10,
		 .color = c,
		 ._padding = 0},
		{.position = {x, y, -z},
		 .normal = nX,
		 .tangent = tX,
		 .uv = uv10,
		 .color = c,
		 ._padding = 0},
		{.position = {x, y, z}, .normal = nX, .tangent = tX, .uv = uv00, .color = c, ._padding = 0},
		{.position = {x, -y, z},
		 .normal = nX,
		 .tangent = tX,
		 .uv = uv01,
		 .color = c,
		 ._padding = 0},
		// Left (-X)
		{.position = {-x, -y, -z},
		 .normal = nNX,
		 .tangent = tNX,
		 .uv = uv01,
		 .color = c,
		 ._padding = 0},
		{.position = {-x, -y, z},
		 .normal = nNX,
		 .tangent = tNX,
		 .uv = uv11,
		 .color = c,
		 ._padding = 0},
		{.position = {-x, y, z},
		 .normal = nNX,
		 .tangent = tNX,
		 .uv = uv10,
		 .color = c,
		 ._padding = 0},
		{.position = {-x, y, z},
		 .normal = nNX,
		 .tangent = tNX,
		 .uv = uv10,
		 .color = c,
		 ._padding = 0},
		{.position = {-x, y, -z},
		 .normal = nNX,
		 .tangent = tNX,
		 .uv = uv00,
		 .color = c,
		 ._padding = 0},
		{.position = {-x, -y, -z},
		 .normal = nNX,
		 .tangent = tNX,
		 .uv = uv01,
		 .color = c,
		 ._padding = 0}};

	BufferHandle vbo = ctx.CreateVertexBuffer(data.data(), data.size() * sizeof(Vertex));
	return Mesh{.vertexBuffer = vbo, .vertexCount = static_cast<uint32_t>(data.size())};
}

auto CreateProceduralCheckerboard(RenderContext& ctx) {
	const uint32_t size = 256;
	std::vector<uint32_t> pixels(static_cast<size_t>(size * size));
	for (uint32_t y = 0; y < size; y++) {
		for (uint32_t x = 0; x < size; x++) {
			// XOR logic creates a perfect grid
			bool isWhite = ((x / 32) + (y / 32)) % 2 == 0;
			pixels[(y * size) + x] = isWhite ? 0xFFFFFFFF : 0xFF333333;
		}
	}
	// Upload this to your Bindless Registry
	return ctx.CreateTexture(pixels.data(), size, size);
}

Material CreateBasicMaterial(RenderContext& ctx) {
	PipelineDesc desc;
	desc.vertexShaderData = ZHLN_Resource_BasicVertSpv;
	desc.vertexShaderSize = ZHLN_Resource_BasicVertSpv_Len;
	desc.fragShaderData = ZHLN_Resource_BasicFragSpv;
	desc.fragShaderSize = ZHLN_Resource_BasicFragSpv_Len;

	Material mat = ctx.CreateMaterial(desc);

	// Generate the procedural texture and save its index!
	mat.albedoIndex = CreateProceduralCheckerboard(ctx);

	return mat;
}

Mesh CreateTerrain(RenderContext& ctx, int sampleCount, float worldSize, float maxHeight,
				   std::vector<float>& outHeights) {
	outHeights.resize(static_cast<size_t>(sampleCount) * sampleCount);

	// Zero-allocation pseudo-random noise hash
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

	// 4-Octave Fractional Brownian Motion for rolling hills
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

	// 1. Generate height cache for visual mesh and physics
	for (int z = 0; z < sampleCount; ++z) {
		for (int x = 0; x < sampleCount; ++x) {
			float posX = -halfSize + x * dx;
			float posZ = -halfSize + z * dz;
			outHeights[x + (z * sampleCount)] = get_height(posX, posZ);
		}
	}

	// 2. Build non-indexed triangle list
	std::vector<Vertex> data;
	data.reserve(static_cast<size_t>((sampleCount - 1)) * (sampleCount - 1) * 6);

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

			// Procedural biome coloring based on slope (Normal.Y) and absolute height
			auto get_color = [&](float y, JPH::Vec3 normal) {
				float slope = normal.GetY();
				float normY = y / maxHeight;
				if (slope < 0.65f) {
					return Math::PackColor(0.35f, 0.32f, 0.29f, 1.0f); // Cliff Slate
				}
				if (normY > 0.75f) {
					return Math::PackColor(0.95f, 0.95f, 0.98f, 1.0f); // Snow Peak
				}
				if (normY < 0.12f) {
					return Math::PackColor(0.72f, 0.64f, 0.48f, 1.0f); // Low Beach Sand
				}
				float greenVar = 0.4f + 0.12f * std::sin(y * 0.5f);
				return Math::PackColor(0.12f, greenVar, 0.08f, 1.0f); // Valley Grass
			};

			Vertex vA = {.position = {ax, ay, az},
						 .normal = Math::PackNormal(nA.GetX(), nA.GetY(), nA.GetZ()),
						 .tangent = Math::PackNormal(1.0f, 0.0f, 0.0f, 1.0f),
						 .uv = Math::PackUV((float)x / sampleCount, (float)z / sampleCount),
						 .color = get_color(ay, nA),
						 ._padding = 0};
			Vertex vB = {.position = {bx, by, bz},
						 .normal = Math::PackNormal(nB.GetX(), nB.GetY(), nB.GetZ()),
						 .tangent = Math::PackNormal(1.0f, 0.0f, 0.0f, 1.0f),
						 .uv = Math::PackUV((float)(x + 1) / sampleCount, (float)z / sampleCount),
						 .color = get_color(by, nB),
						 ._padding = 0};
			Vertex vC = {.position = {cx, cy, cz},
						 .normal = Math::PackNormal(nC.GetX(), nC.GetY(), nC.GetZ()),
						 .tangent = Math::PackNormal(1.0f, 0.0f, 0.0f, 1.0f),
						 .uv = Math::PackUV((float)x / sampleCount, (float)(z + 1) / sampleCount),
						 .color = get_color(cy, nC),
						 ._padding = 0};
			Vertex vD = {
				.position = {dx_, dy, dz_},
				.normal = Math::PackNormal(nD.GetX(), nD.GetY(), nD.GetZ()),
				.tangent = Math::PackNormal(1.0f, 0.0f, 0.0f, 1.0f),
				.uv = Math::PackUV((float)(x + 1) / sampleCount, (float)(z + 1) / sampleCount),
				.color = get_color(dy, nD),
				._padding = 0};

			// CCW Tri 1 (A -> C -> B)
			data.push_back(vA);
			data.push_back(vC);
			data.push_back(vB);
			// CCW Tri 2 (B -> C -> D)
			data.push_back(vB);
			data.push_back(vC);
			data.push_back(vD);
		}
	}

	BufferHandle vbo = ctx.CreateVertexBuffer(data.data(), data.size() * sizeof(Vertex));
	return Mesh{.vertexBuffer = vbo, .vertexCount = static_cast<uint32_t>(data.size())};
}

Mesh LoadGLB(RenderContext& ctx, const std::string& path) {
	cgltf_options opts{};
	cgltf_data* data = nullptr;

	if (cgltf_parse_file(&opts, path.c_str(), &data) != cgltf_result_success) {
		Log("ERROR: Failed to parse GLB file: {}", path);
		return {};
	}

	if (cgltf_load_buffers(&opts, data, path.c_str()) != cgltf_result_success) {
		Log("ERROR: Failed to load GLB buffers: {}", path);
		cgltf_free(data);
		return {};
	}

	std::vector<Vertex> vertexBuffer;

	// Traverse the glTF node hierarchy to respect scaling, rotation, and translation
	for (cgltf_size i = 0; i < data->nodes_count; ++i) {
		const cgltf_node* node = &data->nodes[i];
		if (node->mesh == nullptr) {
			continue; // Skip nodes that don't contain geometric meshes
		}

		// Fetch the node's consolidated world transform matrix (Column-Major 4x4)
		float matrix[16];
		cgltf_node_transform_world(node, matrix);

		const auto* mesh = node->mesh;
		for (cgltf_size p = 0; p < mesh->primitives_count; ++p) {
			const auto& prim = mesh->primitives[p];

			cgltf_accessor* posAcc = nullptr;
			cgltf_accessor* normAcc = nullptr;
			cgltf_accessor* tangentAcc = nullptr;
			cgltf_accessor* uvAcc = nullptr;

			for (cgltf_size a = 0; a < prim.attributes_count; ++a) {
				const auto& attr = prim.attributes[a];
				if (attr.type == cgltf_attribute_type_position) {
					posAcc = attr.data;
				} else if (attr.type == cgltf_attribute_type_normal) {
					normAcc = attr.data;
				} else if (attr.type == cgltf_attribute_type_tangent) {
					tangentAcc = attr.data;
				} else if (attr.type == cgltf_attribute_type_texcoord && attr.index == 0) {
					uvAcc = attr.data;
				}
			}

			if (posAcc == nullptr) {
				continue;
			}

			size_t vertexCount = posAcc->count;
			std::vector<Vertex> primVertices(vertexCount);

			for (size_t vIdx = 0; vIdx < vertexCount; ++vIdx) {
				Vertex& v = primVertices[vIdx];
				std::memset(&v, 0, sizeof(Vertex));

				// 1. Read position and transform by the 4x4 world matrix
				float rawPos[3] = {0.0f, 0.0f, 0.0f};
				cgltf_accessor_read_float(posAcc, vIdx, rawPos, 3);

				v.position[0] = matrix[0] * rawPos[0] + matrix[4] * rawPos[1] +
								matrix[8] * rawPos[2] + matrix[12];
				v.position[1] = matrix[1] * rawPos[0] + matrix[5] * rawPos[1] +
								matrix[9] * rawPos[2] + matrix[13];
				v.position[2] = matrix[2] * rawPos[0] + matrix[6] * rawPos[1] +
								matrix[10] * rawPos[2] + matrix[14];

				// 2. Read normal and transform by the 3x3 normal rotation matrix
				float rawNorm[3] = {0.0f, 1.0f, 0.0f};
				if (normAcc != nullptr) {
					cgltf_accessor_read_float(normAcc, vIdx, rawNorm, 3);
				}
				float nx = matrix[0] * rawNorm[0] + matrix[4] * rawNorm[1] + matrix[8] * rawNorm[2];
				float ny = matrix[1] * rawNorm[0] + matrix[5] * rawNorm[1] + matrix[9] * rawNorm[2];
				float nz =
					matrix[2] * rawNorm[0] + matrix[6] * rawNorm[1] + matrix[10] * rawNorm[2];
				float nLen = std::sqrt(nx * nx + ny * ny + nz * nz);
				if (nLen > 1e-6f) {
					nx /= nLen;
					ny /= nLen;
					nz /= nLen;
				}
				v.normal = Math::PackNormal(nx, ny, nz);

				// 3. Read tangent and transform by 3x3 normal rotation matrix
				float rawTangent[4] = {1.0f, 0.0f, 0.0f, 1.0f};
				if (tangentAcc != nullptr) {
					cgltf_accessor_read_float(tangentAcc, vIdx, rawTangent, 4);
				}
				float tx = matrix[0] * rawTangent[0] + matrix[4] * rawTangent[1] +
						   matrix[8] * rawTangent[2];
				float ty = matrix[1] * rawTangent[0] + matrix[5] * rawTangent[1] +
						   matrix[9] * rawTangent[2];
				float tz = matrix[2] * rawTangent[0] + matrix[6] * rawTangent[1] +
						   matrix[10] * rawTangent[2];
				float tLen = std::sqrt(tx * tx + ty * ty + tz * tz);
				if (tLen > 1e-6f) {
					tx /= tLen;
					ty /= tLen;
					tz /= tLen;
				}
				v.tangent = Math::PackNormal(tx, ty, tz, rawTangent[3]); // Preserve bitangent sign

				// 4. Read UV
				float uv[2] = {0.0f, 0.0f};
				if (uvAcc != nullptr) {
					cgltf_accessor_read_float(uvAcc, vIdx, uv, 2);
				}
				v.uv = Math::PackUV(uv[0], uv[1]);

				v.color = Math::PackColor(1.0f, 1.0f, 1.0f, 1.0f);
			}

			// Unroll indices
			if (prim.indices != nullptr) {
				size_t indexCount = prim.indices->count;
				for (size_t idx = 0; idx < indexCount; ++idx) {
					size_t originalIdx = cgltf_accessor_read_index(prim.indices, idx);
					vertexBuffer.push_back(primVertices[originalIdx]);
				}
			} else {
				vertexBuffer.insert(vertexBuffer.end(), primVertices.begin(), primVertices.end());
			}
		}
	}

	cgltf_free(data);

	if (vertexBuffer.empty()) {
		Log("WARNING: Loaded GLB has no geometry: {}", path);
		return {};
	}

	BufferHandle vbo =
		ctx.CreateVertexBuffer(vertexBuffer.data(), vertexBuffer.size() * sizeof(Vertex));
	Log("Loaded GLB: {} ({} vertices uploaded, world-transforms baked)", path, vertexBuffer.size());
	return Mesh{.vertexBuffer = vbo, .vertexCount = static_cast<uint32_t>(vertexBuffer.size())};
}

uint32_t CreateFontAtlasTexture(RenderContext& ctx) {
	const uint32_t atlasSize = 128;
	std::vector<uint32_t> pixels(static_cast<size_t>(atlasSize * atlasSize),
								 0x00000000); // Transparent RGBA8

	for (uint32_t c = 0; c < 128; ++c) {
		uint32_t gridX = c % 16;
		uint32_t gridY = c / 16;
		uint32_t startX = gridX * 8;
		uint32_t startY = gridY * 8;

		for (uint32_t row = 0; row < 8; ++row) {
			uint8_t byteVal = Font8x8_Basic[c][row];
			for (uint32_t col = 0; col < 8; ++col) {
				// Read bits from Left (MSB) to Right (LSB)
				bool bit = (byteVal & (0x80 >> col)) != 0;
				uint32_t pixelX = startX + col;
				uint32_t pixelY = startY + row;

				// White for letters (0xFFFFFFFF), transparent for empty slots
				pixels[pixelY * atlasSize + pixelX] = bit ? 0xFFFFFFFF : 0x00000000;
			}
		}
	}

	// Upload using your existing bindless texture engine
	return ctx.CreateTexture(pixels.data(), atlasSize, atlasSize);
}

Mesh LoadCookedMesh(RenderContext& ctx, AssetManager& assetMgr, const std::string& virtualPath) {
#ifdef ZHLN_DEV_MODE
	// --- DEVELOPMENT PATH: Load baked GLB directly from disk as loose files ---
	std::string rawPath = "resources/assets/" + virtualPath;
	return LoadGLB(ctx, rawPath);
#else
	// --- RELEASE PATH: Load zero-copy baked mesh from PAK ---
	AssetLoadRequest req;
	req.assetID = HashAssetPath(virtualPath);

	if (!assetMgr.LoadSync(req)) {
		Log("ERROR: Failed to load cooked mesh from PAK: {}", virtualPath);
		return {};
	}

	const auto* header = static_cast<const CookedMeshHeader*>(req.outData);
	if (header->magic != 0x3048534D) { // 'MSH0'
		Log("ERROR: Invalid CookedMeshHeader magic for: {}", virtualPath);
		return {};
	}

	const auto* vertices = reinterpret_cast<const Vertex*>(header + 1);

	BufferHandle vbo = ctx.CreateVertexBuffer(vertices, header->vertexCount * sizeof(Vertex));
	assetMgr.FreeAssetMemory(req);

	Log("Loaded Cooked Mesh: {} ({} vertices)", virtualPath, header->vertexCount);
	return Mesh{.vertexBuffer = vbo, .vertexCount = header->vertexCount};
#endif
}
uint32_t LoadCookedTexture(RenderContext& ctx, AssetManager& assetMgr,
						   const std::string& virtualPath) {
#ifdef ZHLN_DEV_MODE
	// --- DEVELOPMENT PATH: Load baked PNG directly from disk as loose files ---
	std::string rawPath = "resources/assets/" + virtualPath;

	int width = 0, height = 0, channels = 0;
	unsigned char* pixels =
		stbi_load(rawPath.c_str(), &width, &height, &channels, 4); // Force RGBA8
	if (!pixels) {
		Log("WARNING: Failed to load raw texture in dev mode: {}", rawPath);
		return 0;
	}

	uint32_t textureIndex = ctx.CreateTexture(pixels, width, height);
	stbi_image_free(pixels);
	return textureIndex;
#else
	// --- RELEASE PATH: Load cooked texture from PAK ---
	AssetLoadRequest req;
	req.assetID = HashAssetPath(virtualPath);

	if (!assetMgr.LoadSync(req)) {
		Log("ERROR: Failed to load texture from PAK: {}", virtualPath);
		return 0;
	}

	int width = 0;
	int height = 0;
	int channels = 0;
	unsigned char* pixels =
		stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(req.outData),
							  static_cast<int>(req.outSize), &width, &height, &channels, 4);

	if (pixels == nullptr) {
		Log("ERROR: STB failed to decode image: {}", virtualPath);
		assetMgr.FreeAssetMemory(req);
		return 0;
	}

	uint32_t textureIndex = ctx.CreateTexture(pixels, width, height);

	stbi_image_free(pixels);
	assetMgr.FreeAssetMemory(req);

	Log("Loaded Cooked Texture: {} (Bindless Index: {})", virtualPath, textureIndex);
	return textureIndex;
#endif
}

// --- HELPER: Extracts and uploads embedded glTF textures dynamically ---
static uint32_t LoadEmbeddedTexture(RenderContext& ctx, cgltf_image* img,
									const std::string& glbPath) {
	static std::unordered_map<std::string, uint32_t> textureCache;
	std::string key;

	if (img->uri != nullptr) {
		key = img->uri;
	} else if (img->buffer_view != nullptr) {
		key = std::to_string(reinterpret_cast<uintptr_t>(img->buffer_view));
	}

	if (textureCache.contains(key)) {
		return textureCache[key];
	}

	int width = 0;
	int height = 0;
	int channels = 0;
	unsigned char* pixels = nullptr;

	if (img->buffer_view != nullptr) {
		// Decode PNG/JPG bytes directly out of the GLB binary buffer view
		const char* bufferData =
			(const char*)img->buffer_view->buffer->data + img->buffer_view->offset;
		pixels = stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(bufferData),
									   static_cast<int>(img->buffer_view->size), &width, &height,
									   &channels, 4);
	} else if (img->uri != nullptr) {
		// Resolve external relative texture paths
		std::filesystem::path glbFolder = std::filesystem::path(glbPath).parent_path();
		std::filesystem::path texPath = glbFolder / img->uri;
		pixels = stbi_load(texPath.string().c_str(), &width, &height, &channels, 4);
	}

	if (pixels == nullptr) {
		return 0; // Return flat fallback if load fails
	}

	uint32_t index = ctx.CreateTexture(pixels, width, height);
	stbi_image_free(pixels);

	textureCache[key] = index;
	return index;
}

// --- MAIN RUNTIME LOADER: Spawns multi-material GLBs natively ---
std::vector<Entity> SpawnGLB(RenderContext& ctx, ECS::Registry& reg, const std::string& path) {
	cgltf_options opts{};
	cgltf_data* data = nullptr;

	std::string rawPath = "resources/assets/" + path;

	if (cgltf_parse_file(&opts, rawPath.c_str(), &data) != cgltf_result_success) {
		Log("ERROR: Failed to parse GLB: {}", rawPath);
		return {};
	}

	if (cgltf_load_buffers(&opts, data, rawPath.c_str()) != cgltf_result_success) {
		Log("ERROR: Failed to load GLB buffers: {}", rawPath);
		cgltf_free(data);
		return {};
	}

	std::vector<Entity> spawnedEntities;

	// Traverse the glTF node hierarchy
	for (cgltf_size i = 0; i < data->nodes_count; ++i) {
		const cgltf_node* node = &data->nodes[i];
		if (node->mesh == nullptr) {
			continue;
		}

		// Fetch the node's world transform matrix
		float matrix[16];
		cgltf_node_transform_world(node, matrix);

		const auto* mesh = node->mesh;
		for (cgltf_size p = 0; p < mesh->primitives_count; ++p) {
			const auto& prim = mesh->primitives[p];

			cgltf_accessor* posAcc = nullptr;
			cgltf_accessor* normAcc = nullptr;
			cgltf_accessor* tangentAcc = nullptr;
			cgltf_accessor* uvAcc = nullptr;
			cgltf_accessor* colorAcc = nullptr; // <--- 1. ADD COLOR ACCESSOR

			for (cgltf_size a = 0; a < prim.attributes_count; ++a) {
				const auto& attr = prim.attributes[a];
				if (attr.type == cgltf_attribute_type_position) {
					posAcc = attr.data;
				} else if (attr.type == cgltf_attribute_type_normal) {
					normAcc = attr.data;
				} else if (attr.type == cgltf_attribute_type_tangent) {
					tangentAcc = attr.data;
				} else if (attr.type == cgltf_attribute_type_texcoord && attr.index == 0) {
					uvAcc = attr.data;
				} else if (attr.type == cgltf_attribute_type_color) {
					colorAcc = attr.data; // <--- 2. EXTRACT COLOR ATTR
				}
			}

			if (posAcc == nullptr) {
				continue;
			}

			size_t vertexCount = posAcc->count;
			std::vector<Vertex> primVertices(vertexCount);

			for (size_t vIdx = 0; vIdx < vertexCount; ++vIdx) {
				Vertex& v = primVertices[vIdx];
				std::memset(&v, 0, sizeof(Vertex));

				// Transform position
				float rawPos[3] = {0.0f, 0.0f, 0.0f};
				cgltf_accessor_read_float(posAcc, vIdx, rawPos, 3);
				v.position[0] = matrix[0] * rawPos[0] + matrix[4] * rawPos[1] +
								matrix[8] * rawPos[2] + matrix[12];
				v.position[1] = matrix[1] * rawPos[0] + matrix[5] * rawPos[1] +
								matrix[9] * rawPos[2] + matrix[13];
				v.position[2] = matrix[2] * rawPos[0] + matrix[6] * rawPos[1] +
								matrix[10] * rawPos[2] + matrix[14];

				// --- NEW: Read, Rotate, and Pack Normal ---
				float rawNorm[3] = {0.0f, 1.0f, 0.0f};
				if (normAcc != nullptr) {
					cgltf_accessor_read_float(normAcc, vIdx, rawNorm, 3);
				}
				// Rotate normal direction (ignoring translation)
				float nx = matrix[0] * rawNorm[0] + matrix[4] * rawNorm[1] + matrix[8] * rawNorm[2];
				float ny = matrix[1] * rawNorm[0] + matrix[5] * rawNorm[1] + matrix[9] * rawNorm[2];
				float nz =
					matrix[2] * rawNorm[0] + matrix[6] * rawNorm[1] + matrix[10] * rawNorm[2];
				float nLen = std::sqrt(nx * nx + ny * ny + nz * nz);
				if (nLen > 1e-6f) {
					nx /= nLen;
					ny /= nLen;
					nz /= nLen;
				}
				v.normal = Math::PackNormal(nx, ny, nz);

				// Read, Transform, and Pack Tangent
				float rawTangent[4] = {1.0f, 0.0f, 0.0f, 1.0f};
				if (tangentAcc != nullptr) {
					cgltf_accessor_read_float(tangentAcc, vIdx, rawTangent, 4);
				}
				float tx = matrix[0] * rawTangent[0] + matrix[4] * rawTangent[1] +
						   matrix[8] * rawTangent[2];
				float ty = matrix[1] * rawTangent[0] + matrix[5] * rawTangent[1] +
						   matrix[9] * rawTangent[2];
				float tz = matrix[2] * rawTangent[0] + matrix[6] * rawTangent[1] +
						   matrix[10] * rawTangent[2];
				float tLen = std::sqrt(tx * tx + ty * ty + tz * tz);
				if (tLen > 1e-6f) {
					tx /= tLen;
					ty /= tLen;
					tz /= tLen;
				}
				v.tangent = Math::PackNormal(tx, ty, tz, rawTangent[3]);

				// Read UV
				float uv[2] = {0.0f, 0.0f};
				if (uvAcc != nullptr) {
					cgltf_accessor_read_float(uvAcc, vIdx, uv, 2);
				}
				v.uv = Math::PackUV(uv[0], uv[1]);

				// Read Vertex Color
				float rawColor[4] = {1.0f, 1.0f, 1.0f, 1.0f};
				if (colorAcc != nullptr) {
					cgltf_accessor_read_float(colorAcc, vIdx, rawColor, 4);
				}
				v.color = Math::PackColor(rawColor[0], rawColor[1], rawColor[2], rawColor[3]);
			}
			// Resolve Indices (Unroll)
			std::vector<Vertex> unrolledVertices;
			if (prim.indices != nullptr) {
				size_t indexCount = prim.indices->count;
				unrolledVertices.reserve(indexCount);
				for (size_t idx = 0; idx < indexCount; ++idx) {
					size_t originalIdx = cgltf_accessor_read_index(prim.indices, idx);
					unrolledVertices.push_back(primVertices[originalIdx]);
				}
			} else {
				unrolledVertices = std::move(primVertices);
			}

			// Create vertex buffer
			BufferHandle vbo = ctx.CreateVertexBuffer(unrolledVertices.data(),
													  unrolledVertices.size() * sizeof(Vertex));
			Mesh subMesh = {.vertexBuffer = vbo,
							.vertexCount = static_cast<uint32_t>(unrolledVertices.size())};

			// Setup material
			Material subMaterial = CreateBasicMaterial(ctx);
			subMaterial.albedoIndex =
				1; // <--- 4. DEFAULT TO SOLID WHITE FALLBACK (prevent checkerboard multiplication)

			if (prim.material != nullptr) {
				// Extract PBR Base Color Texture
				if (prim.material->has_pbr_metallic_roughness) {
					auto& pbr = prim.material->pbr_metallic_roughness;
					if ((pbr.base_color_texture.texture != nullptr) &&
						(pbr.base_color_texture.texture->image != nullptr)) {
						subMaterial.albedoIndex = LoadEmbeddedTexture(
							ctx, pbr.base_color_texture.texture->image, rawPath);
					}
				}
				// Extract Normal Map
				if ((prim.material->normal_texture.texture != nullptr) &&
					(prim.material->normal_texture.texture->image != nullptr)) {
					subMaterial.normalIndex = LoadEmbeddedTexture(
						ctx, prim.material->normal_texture.texture->image, rawPath);
				}
			}

			// Spawn separate entity for this submesh
			Entity part = reg.Create();
			reg.Add(part,
					MeshComponent{.mesh = subMesh, .material = subMaterial, .cullRadius = 100.0f});
			spawnedEntities.push_back(part);
		}
	}

	cgltf_free(data);
	Log("Spawned GLB Model: {} ({} submesh parts/materials loaded dynamically)", path,
		spawnedEntities.size());
	return spawnedEntities;
}

} // namespace ZHLN::AssetFactory
