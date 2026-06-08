// File: src/engine/glTFImporter.cpp
#include "Zahlen/Components.hpp"
#include "Zahlen/Engine.hpp"
#include "Zahlen/Render.hpp"
#include "ecs/ECS.hpp"
#include "physics/Physics.hpp"

#include <Zahlen/AssetFactory.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Math3D.hpp>
#include <algorithm>
#include <cgltf.h>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <stb_image.h>
#include <string>
#include <unordered_map>
#include <vector>

namespace ZHLN::AssetFactory {

// Declare shared caches (linked externally by AnimationSystem)
std::unordered_map<std::string, cgltf_data*> s_GLBCache;
std::vector<cgltf_data*> s_AnimatedGLBs;

static uint32_t LoadEmbeddedTexture(RenderContext& ctx, cgltf_image* img,
									const std::string& glbPath, bool isSRGB = true) {
	static std::unordered_map<std::string, uint32_t> textureCache;
	std::string key;

	// Scope the key with the glbPath to prevent conflicts between different models
	if (img->uri != nullptr) {
		key = glbPath + ":" + img->uri;
	} else if (img->buffer_view != nullptr) {
		key = glbPath + ":" + std::to_string(reinterpret_cast<uintptr_t>(img->buffer_view));
	} else {
		// Fallback for cases with neither URI nor buffer view
		key = glbPath + ":" + std::to_string(reinterpret_cast<uintptr_t>(img));
	}

	if (textureCache.contains(key)) {
		return textureCache[key];
	}

	int width = 0;
	int height = 0;
	int channels = 0;
	unsigned char* pixels = nullptr;

	if (img->buffer_view != nullptr) {
		const char* bufferData =
			(const char*)img->buffer_view->buffer->data + img->buffer_view->offset;
		pixels = stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(bufferData),
									   static_cast<int>(img->buffer_view->size), &width, &height,
									   &channels, 4);
	} else if (img->uri != nullptr) {
		std::filesystem::path glbFolder = std::filesystem::path(glbPath).parent_path();
		std::filesystem::path texPath = glbFolder / img->uri;
		pixels = stbi_load(texPath.string().c_str(), &width, &height, &channels, 4);
	}

	if (pixels == nullptr) {
		const char* reason = stbi_failure_reason();
		if (img->uri != nullptr) {
			std::filesystem::path glbFolder = std::filesystem::path(glbPath).parent_path();
			Log("ERROR: Failed to load texture URI: {} (Path: {}) | Reason: {}", img->uri,
				(glbFolder / img->uri).string(), (reason != nullptr) ? reason : "Unknown");
		} else {
			Log("ERROR: Failed to load embedded texture (buffer_view offset: {}) | Reason: {}",
				(img->buffer_view != nullptr) ? img->buffer_view->offset : 0,
				(reason != nullptr) ? reason : "Unknown");
		}
		return 1;
	}

	uint32_t index = ctx.CreateTexture(pixels, width, height, isSRGB);
	stbi_image_free(pixels);

	textureCache[key] = index;
	return index;
}

Mesh LoadGLB(RenderContext& ctx, std::string_view path) {
	cgltf_options opts{};
	cgltf_data* data = nullptr;

	std::string pathStr(path);
	if (cgltf_parse_file(&opts, pathStr.c_str(), &data) != cgltf_result_success) {
		Log("ERROR: Failed to parse GLB file: {}", path);
		return {};
	}

	if (cgltf_load_buffers(&opts, data, pathStr.c_str()) != cgltf_result_success) {
		Log("ERROR: Failed to load GLB buffers: {}", path);
		cgltf_free(data);
		return {};
	}

	std::vector<Vertex> vertexBuffer;

	for (cgltf_size i = 0; i < data->nodes_count; ++i) {
		const cgltf_node* node = &data->nodes[i];
		if (node->mesh == nullptr) {
			continue;
		}

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

				float rawPos[3] = {0.0f, 0.0f, 0.0f};
				cgltf_accessor_read_float(posAcc, vIdx, rawPos, 3);

				v.position[0] = matrix[0] * rawPos[0] + matrix[4] * rawPos[1] +
								matrix[8] * rawPos[2] + matrix[12];
				v.position[1] = matrix[1] * rawPos[0] + matrix[5] * rawPos[1] +
								matrix[9] * rawPos[2] + matrix[13];
				v.position[2] = matrix[2] * rawPos[0] + matrix[6] * rawPos[1] +
								matrix[10] * rawPos[2] + matrix[14];

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

				float uv[2] = {0.0f, 0.0f};
				if (uvAcc != nullptr) {
					cgltf_accessor_read_float(uvAcc, vIdx, uv, 2);
				}
				v.uv = Math::PackUV(uv[0], uv[1]);

				v.color = Math::PackColor(1.0f, 1.0f, 1.0f, 1.0f);

				v.joints[0] = 0;
				v.joints[1] = 0;
				v.joints[2] = 0;
				v.joints[3] = 0;
				v.weights[0] = 0.0f;
				v.weights[1] = 0.0f;
				v.weights[2] = 0.0f;
				v.weights[3] = 0.0f;
			}

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

template <bool CreatePhysics, bool Animated>
uint32_t SpawnGLB(RenderContext& ctx, ECS::Registry& reg, std::string_view path, Entity* outBuffer,
				  uint32_t maxCount) {
	cgltf_options opts{};
	cgltf_data* data = nullptr;

	std::string pathStr(path);
	std::string rawPath = "resources/assets/" + pathStr;

	if (s_GLBCache.contains(pathStr)) {
		data = s_GLBCache[pathStr];
	} else {
		if (cgltf_parse_file(&opts, rawPath.c_str(), &data) != cgltf_result_success) {
			Log("ERROR: Failed to parse GLB: {}", rawPath);
			return 0;
		}

		if (cgltf_load_buffers(&opts, data, rawPath.c_str()) != cgltf_result_success) {
			Log("ERROR: Failed to load GLB buffers: {}", rawPath);
			cgltf_free(data);
			return 0;
		}
		s_GLBCache[pathStr] = data;

		// Decompose all static matrices into standard TRS paths
		for (cgltf_size idx = 0; idx < data->nodes_count; ++idx) {
			cgltf_node* node = &data->nodes[idx];
			if (node->has_matrix) {
				node->has_translation = 1;
				node->translation[0] = node->matrix[12];
				node->translation[1] = node->matrix[13];
				node->translation[2] = node->matrix[14];

				JPH::Vec3 col0(node->matrix[0], node->matrix[1], node->matrix[2]);
				JPH::Vec3 col1(node->matrix[4], node->matrix[5], node->matrix[6]);
				JPH::Vec3 col2(node->matrix[8], node->matrix[9], node->matrix[10]);

				node->has_scale = 1;
				node->scale[0] = col0.Length();
				node->scale[1] = col1.Length();
				node->scale[2] = col2.Length();

				if (node->scale[0] > 1e-6f) {
					col0 /= node->scale[0];
				}
				if (node->scale[1] > 1e-6f) {
					col1 /= node->scale[1];
				}
				if (node->scale[2] > 1e-6f) {
					col2 /= node->scale[2];
				}

				JPH::Mat44 rotMat(JPH::Vec4(col0, 0.0f), JPH::Vec4(col1, 0.0f),
								  JPH::Vec4(col2, 0.0f), JPH::Vec4(0, 0, 0, 1));
				JPH::Quat rot = rotMat.GetQuaternion();

				node->has_rotation = 1;
				node->rotation[0] = rot.GetX();
				node->rotation[1] = rot.GetY();
				node->rotation[2] = rot.GetZ();
				node->rotation[3] = rot.GetW();

				node->has_matrix = 0;
			}
		}
	}

	if constexpr (Animated) {
		if (std::find(s_AnimatedGLBs.begin(), s_AnimatedGLBs.end(), data) == s_AnimatedGLBs.end()) {
			s_AnimatedGLBs.push_back(data);
		}
	}

	uint32_t spawnedCount = 0;

	for (cgltf_size i = 0; i < data->nodes_count; ++i) {
		const cgltf_node* node = &data->nodes[i];
		if (node->mesh == nullptr) {
			continue;
		}

		float matrix[16];
		cgltf_node_transform_world(node, matrix);

		JPH::Mat44 nodeTransform(JPH::Vec4(matrix[0], matrix[1], matrix[2], matrix[3]),
								 JPH::Vec4(matrix[4], matrix[5], matrix[6], matrix[7]),
								 JPH::Vec4(matrix[8], matrix[9], matrix[10], matrix[11]),
								 JPH::Vec4(matrix[12], matrix[13], matrix[14], matrix[15]));

		const auto* mesh = node->mesh;
		for (cgltf_size p = 0; p < mesh->primitives_count; ++p) {
			const auto& prim = mesh->primitives[p];

			cgltf_accessor* posAcc = nullptr;
			cgltf_accessor* normAcc = nullptr;
			cgltf_accessor* tangentAcc = nullptr;
			cgltf_accessor* uvAcc = nullptr;
			cgltf_accessor* colorAcc = nullptr;
			cgltf_accessor* jointsAcc = nullptr;
			cgltf_accessor* weightsAcc = nullptr;

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
				} else if (attr.type == cgltf_attribute_type_color && attr.index == 0) {
					colorAcc = attr.data; // Matches COLOR_0
				} else if (attr.type == cgltf_attribute_type_joints && attr.index == 0) {
					jointsAcc = attr.data; // Matches JOINTS_0
				} else if (attr.type == cgltf_attribute_type_weights && attr.index == 0) {
					weightsAcc = attr.data; // Matches WEIGHTS_0
				}
			}

			if (posAcc == nullptr) {
				continue;
			}

			// ----------------------------------------------------------------
			// Extract local bounds for Jolt collision mapping [6]
			// ----------------------------------------------------------------
			float localMin[3] = {0.0f, 0.0f, 0.0f};
			float localMax[3] = {0.0f, 0.0f, 0.0f};
			if (posAcc->has_min) {
				std::copy(posAcc->min, posAcc->min + 3, localMin);
			}
			if (posAcc->has_max) {
				std::copy(posAcc->max, posAcc->max + 3, localMax);
			}

			bool doubleSided = false;
			bool alphaBlend = false;
			float baseColorFactor[4] = {1.0f, 1.0f, 1.0f, 1.0f};
			float metallicFactor = 1.0f;
			float roughnessFactor = 1.0f;
			float alphaCutoff = 0.5f;
			uint32_t alphaMode = 0;

			if (prim.material != nullptr) {
				doubleSided = (prim.material->double_sided != 0);
				if (prim.material->alpha_mode == cgltf_alpha_mode_mask) {
					alphaMode = 1;
					alphaCutoff = prim.material->alpha_cutoff;
				} else if (prim.material->alpha_mode == cgltf_alpha_mode_blend) {
					alphaMode = 2;
					alphaBlend = true;
				}

				if (prim.material->has_pbr_metallic_roughness) {
					const float* c = prim.material->pbr_metallic_roughness.base_color_factor;
					baseColorFactor[0] = c[0];
					baseColorFactor[1] = c[1];
					baseColorFactor[2] = c[2];
					baseColorFactor[3] = c[3];
					metallicFactor = prim.material->pbr_metallic_roughness.metallic_factor;
					roughnessFactor = prim.material->pbr_metallic_roughness.roughness_factor;

					if (prim.material->pbr_metallic_roughness.base_color_texture.texture !=
						nullptr) {
						/* stub for now */
					}
				}
			}

			size_t vertexCount = posAcc->count;
			std::vector<Vertex> primVertices(vertexCount);

			for (size_t vIdx = 0; vIdx < vertexCount; ++vIdx) {
				Vertex& v = primVertices[vIdx];
				std::memset(&v, 0, sizeof(Vertex));

				float rawPos[3] = {0.0f, 0.0f, 0.0f};
				cgltf_accessor_read_float(posAcc, vIdx, rawPos, 3);
				v.position[0] = rawPos[0];
				v.position[1] = rawPos[1];
				v.position[2] = rawPos[2];

				float rawNorm[3] = {0.0f, 1.0f, 0.0f};
				if (normAcc != nullptr) {
					cgltf_accessor_read_float(normAcc, vIdx, rawNorm, 3);
				}
				float nLen = std::sqrt(rawNorm[0] * rawNorm[0] + rawNorm[1] * rawNorm[1] +
									   rawNorm[2] * rawNorm[2]);
				if (nLen > 1e-6f) {
					rawNorm[0] /= nLen;
					rawNorm[1] /= nLen;
					rawNorm[2] /= nLen;
				}
				v.normal = Math::PackNormal(rawNorm[0], rawNorm[1], rawNorm[2]);

				float rawTangent[4] = {1.0f, 0.0f, 0.0f, 1.0f};
				if (tangentAcc != nullptr) {
					cgltf_accessor_read_float(tangentAcc, vIdx, rawTangent, 4);
				}
				float tLen =
					std::sqrt(rawTangent[0] * rawTangent[0] + rawTangent[1] * rawTangent[1] +
							  rawTangent[2] * rawTangent[2]);
				if (tLen > 1e-6f) {
					rawTangent[0] /= tLen;
					rawTangent[1] /= tLen;
					rawTangent[2] /= tLen;
				}
				v.tangent =
					Math::PackNormal(rawTangent[0], rawTangent[1], rawTangent[2], rawTangent[3]);

				float uv[2] = {0.0f, 0.0f};
				if (uvAcc != nullptr) {
					cgltf_accessor_read_float(uvAcc, vIdx, uv, 2);
				}
				v.uv = Math::PackUV(uv[0], uv[1]);

				float rawColor[4] = {1.0f, 1.0f, 1.0f, 1.0f};
				if (colorAcc !=
					nullptr) { // Removed !hasAlbedoTexture for now to achieve specification
							   // compliance with models that use vertex colors alongside textures
					cgltf_accessor_read_float(colorAcc, vIdx, rawColor, 4);
				}
				v.color = Math::PackColor(rawColor[0], rawColor[1], rawColor[2], rawColor[3]);

				uint32_t joints[4] = {0, 0, 0, 0};
				if (jointsAcc != nullptr) {
					cgltf_accessor_read_uint(jointsAcc, vIdx, joints, 4);
				}
				v.joints[0] = static_cast<uint16_t>(joints[0]);
				v.joints[1] = static_cast<uint16_t>(joints[1]);
				v.joints[2] = static_cast<uint16_t>(joints[2]);
				v.joints[3] = static_cast<uint16_t>(joints[3]);

				float weights[4] = {0.0f, 0.0f, 0.0f, 0.0f};
				if (weightsAcc != nullptr) {
					cgltf_accessor_read_float(weightsAcc, vIdx, weights, 4);
				}
				v.weights[0] = weights[0];
				v.weights[1] = weights[1];
				v.weights[2] = weights[2];
				v.weights[3] = weights[3];
			}

			// FIX: Do not unroll vertices if we are providing an IBO. Use the original compact VBO.
			BufferHandle vbo =
				ctx.CreateVertexBuffer(primVertices.data(), primVertices.size() * sizeof(Vertex));

			uint32_t indexCount = 0;
			std::vector<uint32_t> indices32;

			if (prim.indices != nullptr) {
				indexCount = static_cast<uint32_t>(prim.indices->count);
				indices32.resize(indexCount);
				for (size_t idx = 0; idx < indexCount; ++idx) {
					indices32[idx] =
						static_cast<uint32_t>(cgltf_accessor_read_index(prim.indices, idx));
				}
			} else {
				// FIX: Force non-indexed meshes to be indexed to prevent GPU Culling indirect
				// command stride corruption
				indexCount = static_cast<uint32_t>(primVertices.size());
				indices32.resize(indexCount);
				for (uint32_t idx = 0; idx < indexCount; ++idx) {
					indices32[idx] = idx;
				}
			}

			BufferHandle ibo =
				ctx.CreateIndexBuffer(indices32.data(), indexCount * sizeof(uint32_t));

			Mesh subMesh = {.vertexBuffer = vbo,
							.indexBuffer = ibo,
							.vertexCount = static_cast<uint32_t>(primVertices.size()),
							.indexCount = indexCount}; // Check if we are loading character parts to
													   // assign the toon material
			// bool isCharacter = (path.contains("POMNI") || path.contains("tadc_models"));

			Material subMaterial = CreateBasicMaterial(ctx, doubleSided, alphaBlend);

			subMaterial.alphaMode = alphaMode;
			subMaterial.alphaCutoff = alphaCutoff;
			subMaterial.metallicFactor = metallicFactor;
			subMaterial.roughnessFactor = roughnessFactor;
			subMaterial.baseColorFactor[0] = baseColorFactor[0];
			subMaterial.baseColorFactor[1] = baseColorFactor[1];
			subMaterial.baseColorFactor[2] = baseColorFactor[2];
			subMaterial.baseColorFactor[3] = baseColorFactor[3];
			subMaterial.albedoIndex = 1;

			if (prim.material != nullptr) {
				if (prim.material->has_pbr_metallic_roughness) {
					auto& pbr = prim.material->pbr_metallic_roughness;
					if ((pbr.base_color_texture.texture != nullptr) &&
						(pbr.base_color_texture.texture->image != nullptr)) {
						subMaterial.albedoIndex = LoadEmbeddedTexture(
							ctx, pbr.base_color_texture.texture->image, rawPath, true);
					}
				}
				if ((prim.material->normal_texture.texture != nullptr) &&
					(prim.material->normal_texture.texture->image != nullptr)) {
					subMaterial.normalIndex = LoadEmbeddedTexture(
						ctx, prim.material->normal_texture.texture->image, rawPath, false);
				}
				if (prim.material->has_pbr_metallic_roughness) {
					auto& pbr = prim.material->pbr_metallic_roughness;
					if ((pbr.metallic_roughness_texture.texture != nullptr) &&
						(pbr.metallic_roughness_texture.texture->image != nullptr)) {
						subMaterial.pbrIndex = LoadEmbeddedTexture(
							ctx, pbr.metallic_roughness_texture.texture->image, rawPath, false);
					}
				}
			}

			uint32_t morphOffset = 0;
			uint32_t activeMorphCount = 0;
			float defaultWeights[4] = {0.0f, 0.0f, 0.0f, 0.0f}; // Default initializer

			if (prim.targets_count > 0) {
				ZHLN::Log("[Diagnostics] Mesh Part '{}' has {} total morph targets. Engine is "
						  "keeping {}.",
						  (node->name != nullptr) ? node->name : "Unnamed Node", prim.targets_count,
						  std::min((uint32_t)prim.targets_count, 4u));
				activeMorphCount = std::min((uint32_t)prim.targets_count, 4u);

				// Extract default weights from glTF if they are authored
				if (mesh->weights != nullptr) {
					for (uint32_t w = 0; w < activeMorphCount; ++w) {
						defaultWeights[w] = mesh->weights[w];
					}
				}

				// FIX: Do not use unrolled size. Use the original compact vertex count.
				auto currentVertexCount = static_cast<uint32_t>(primVertices.size());

				std::vector<float> tempDeltas;
				tempDeltas.resize(static_cast<size_t>(currentVertexCount) * activeMorphCount * 4,
								  0.0f);

				uint32_t deltaPtr = 0;
				for (uint32_t t = 0; t < activeMorphCount; ++t) {
					cgltf_accessor* targetPosAcc = nullptr;
					for (cgltf_size a = 0; a < prim.targets[t].attributes_count; ++a) {
						if (prim.targets[t].attributes[a].type == cgltf_attribute_type_position) {
							targetPosAcc = prim.targets[t].attributes[a].data;
							break;
						}
					}

					if (targetPosAcc != nullptr) {
						for (size_t vIdx = 0; vIdx < currentVertexCount; ++vIdx) {
							float delta[3] = {0.0f, 0.0f, 0.0f};
							cgltf_accessor_read_float(targetPosAcc, vIdx, delta, 3);

							tempDeltas[deltaPtr++] = delta[0];
							tempDeltas[deltaPtr++] = delta[1];
							tempDeltas[deltaPtr++] = delta[2];
							tempDeltas[deltaPtr++] = 0.0f; // std430 float4 padding
						}
					} else {
						// Skip zeroes if no position target was found
						deltaPtr += currentVertexCount * 4;
					}
				}

				// Expose currentVertexCount to the shader
				morphOffset = ctx.AllocateMorphDeltas(currentVertexCount * activeMorphCount,
													  tempDeltas.data());
			}

			// Calculate the furthest distance from the local pivot (0,0,0) to any actual vertex
			// This completely bypasses broken/missing glTF min/max metadata.
			float maxD2 = 0.0f;
			for (size_t vIdx = 0; vIdx < vertexCount; ++vIdx) {
				float d2 = primVertices[vIdx].position[0] * primVertices[vIdx].position[0] +
						   primVertices[vIdx].position[1] * primVertices[vIdx].position[1] +
						   primVertices[vIdx].position[2] * primVertices[vIdx].position[2];
				maxD2 = std::max(d2, maxD2);
			}

			JPH::Vec3 nodeScale(nodeTransform.GetColumn4(0).Length(),
								nodeTransform.GetColumn4(1).Length(),
								nodeTransform.GetColumn4(2).Length());
			float maxScale = std::max({nodeScale.GetX(), nodeScale.GetY(), nodeScale.GetZ()});

			// Add 20% padding + 1.0m constant to ensure meshes at the absolute edge of the screen
			// are drawn for TAA history accumulation, preventing edge-pop artifacts.
			float boundingRadius = std::sqrt(maxD2) * maxScale * 1.2f + 1.0f;

			Entity part = reg.Create();
			reg.Add(part, MeshComponent{
							  .mesh = subMesh,
							  .material = subMaterial,
							  .cullRadius = boundingRadius,
							  .localTransform = nodeTransform,
							  .prevTransform = nodeTransform,
							  .jointOffset = 0,
							  // Only flag skinning and bind nodes if animation is requested [2]
							  .isSkinned = (node->skin != nullptr) && Animated,
							  .morphOffset = morphOffset,
							  .activeMorphCount = activeMorphCount,
							  .morphWeights = {defaultWeights[0], defaultWeights[1],
											   defaultWeights[2], defaultWeights[3]},
							  .gltfNode = Animated ? (void*)node : nullptr,
							  .gltfSkin = (Animated && node->skin != nullptr) ? (void*)node->skin
																			  : nullptr});

			// ----------------------------------------------------------------
			// 3. Compile-time Evaluated Physical Collider Generation
			// ----------------------------------------------------------------
			if constexpr (CreatePhysics) { // <-- Evaluated entirely at compile-time!
				auto& pc = GetEngineContext()->GetPhysicsContext();

				// Create a precise, concave static Jolt MeshShape directly from the GLB vertices
				// and indices
				JPH::ShapeRefC meshShape = Physics::CreateMeshShape(
					primVertices.data(), static_cast<uint32_t>(primVertices.size()),
					indices32.data(), indexCount);

				if (meshShape) {
					// We translate and rotate the static body based on the glTF node matrix
					JPH::Vec3 translation = nodeTransform.GetTranslation();
					JPH::Vec3 col0 = nodeTransform.GetColumn3(0);
					JPH::Vec3 col1 = nodeTransform.GetColumn3(1);
					JPH::Vec3 col2 = nodeTransform.GetColumn3(2);

					if (maxScale > 1e-6f) {
						col0 /= nodeScale.GetX();
						col1 /= nodeScale.GetY();
						col2 /= nodeScale.GetZ();
					}

					JPH::Mat44 rotationMatrix(JPH::Vec4(col0, 0.0f), JPH::Vec4(col1, 0.0f),
											  JPH::Vec4(col2, 0.0f),
											  JPH::Vec4(0.0f, 0.0f, 0.0f, 1.0f));
					JPH::Quat rotation = rotationMatrix.GetQuaternion();

					// Register the static physical body in Jolt
					reg.Add(part, PhysicsComponent{Physics::CreateRigidBody(
									  pc, meshShape, JPH::RVec3(translation), rotation,
									  JPH::EMotionType::Static, 0)});
				}
			}
			if (outBuffer != nullptr && spawnedCount < maxCount) {
				outBuffer[spawnedCount] = part;
			}
			spawnedCount++;
		}
	}

	Log("Spawned GLB Model: {} ({} submesh parts/materials loaded dynamically)", path,
		spawnedCount);
	return spawnedCount;
}

template uint32_t SpawnGLB<true, true>(RenderContext& ctx, ECS::Registry& reg,
									   std::string_view path, Entity* outBuffer, uint32_t maxCount);
template uint32_t SpawnGLB<true, false>(RenderContext& ctx, ECS::Registry& reg,
										std::string_view path, Entity* outBuffer,
										uint32_t maxCount);
template uint32_t SpawnGLB<false, true>(RenderContext& ctx, ECS::Registry& reg,
										std::string_view path, Entity* outBuffer,
										uint32_t maxCount);
template uint32_t SpawnGLB<false, false>(RenderContext& ctx, ECS::Registry& reg,
										 std::string_view path, Entity* outBuffer,
										 uint32_t maxCount);

// Helper to compile Jolt Skeleton on-the-fly from glTF joint hierarchies
static JPH::Ref<JPH::Skeleton> BuildJoltSkeletonFromCgltf(const cgltf_skin* skin) {
	auto* skeleton = new JPH::Skeleton();
	for (size_t i = 0; i < skin->joints_count; ++i) {
		cgltf_node* jointNode = skin->joints[i];
		std::string name =
			(jointNode->name != nullptr) ? jointNode->name : "joint_" + std::to_string(i);
		std::string parentName =
			((jointNode->parent != nullptr) && (jointNode->parent->name != nullptr))
				? jointNode->parent->name
				: "";
		skeleton->AddJoint(name, parentName);
	}
	skeleton->CalculateParentJointIndices();
	return skeleton;
}

void SetupPlayerRagdoll(RenderContext& rc, PhysicsContext& pc, ECS::Registry& reg,
						Entity playerEntity, std::span<const Entity> visualParts) {
	const cgltf_skin* pomniSkin = nullptr;
	for (Entity part : visualParts) {
		if (auto* meshComp = reg.Get<MeshComponent>(part)) {
			if (meshComp->gltfSkin != nullptr) {
				pomniSkin = static_cast<const cgltf_skin*>(meshComp->gltfSkin);
				break;
			}
		}
	}

	if (pomniSkin != nullptr) {
		auto skeleton = BuildJoltSkeletonFromCgltf(pomniSkin);

		// Map key joints (hips, spine, head) by name
		uint32_t hipIdx = 0;
		uint32_t spineIdx = 0;
		uint32_t headIdx = 0;

		for (size_t i = 0; i < pomniSkin->joints_count; ++i) {
			std::string name =
				(pomniSkin->joints[i]->name != nullptr) ? pomniSkin->joints[i]->name : "";
			std::transform(name.begin(), name.end(), name.begin(), ::tolower);
			if (name.contains("hip") || name.contains("pelvis") || name.contains("root")) {
				hipIdx = (uint32_t)i;
			} else if (name.contains("spine") || name.contains("chest") || name.contains("torso")) {
				spineIdx = (uint32_t)i;
			} else if (name.contains("head") || name.contains("neck")) {
				headIdx = (uint32_t)i;
			}
		}

		// Fallbacks
		if (spineIdx == 0 && pomniSkin->joints_count > 1) {
			spineIdx = (uint32_t)(pomniSkin->joints_count / 2);
		}
		if (headIdx == 0 && pomniSkin->joints_count > 2) {
			headIdx = (uint32_t)(pomniSkin->joints_count - 1);
		}

		auto GetNodeWorldTRS = [](const cgltf_node* node, JPH::RVec3& outPos, JPH::Quat& outRot) {
			float m[16];
			cgltf_node_transform_world(node, m);
			outPos = JPH::RVec3(m[12], m[13], m[14]);

			JPH::Vec3 col0(m[0], m[1], m[2]);
			JPH::Vec3 col1(m[4], m[5], m[6]);
			JPH::Vec3 col2(m[8], m[9], m[10]);

			JPH::Mat44 rotMat(JPH::Vec4(col0, 0), JPH::Vec4(col1, 0), JPH::Vec4(col2, 0),
							  JPH::Vec4(0, 0, 0, 1));
			outRot = rotMat.GetQuaternion();
		};

		// 1. Pre-populate all 1,121 joints with lightweight default parameters
		// and their actual visual world-space bone coordinates
		std::vector<Physics::RagdollPartParams> parts;
		parts.resize(pomniSkin->joints_count);

		JPH::ShapeRefC dummyShape =
			Physics::GetOrCreateShape(pc, Physics::ShapeType::Sphere, 0.05f);

		for (size_t i = 0; i < pomniSkin->joints_count; ++i) {
			auto& part = parts[i];
			part.jointIndex = (uint32_t)i;
			part.parentJointIndex = skeleton->GetJoint(i).mParentJointIndex;
			part.shape = dummyShape;
			part.mass = 0.001f;		   // 1 gram
			part.enableMotors = false; // No motor overhead on dummy joints

			GetNodeWorldTRS(pomniSkin->joints[i], part.position, part.rotation);
		}

		// 2. Overwrite the Pelvis, Spine, and Head joints with their heavy physical attributes
		// Hips (Root)
		auto& hipPart = parts[hipIdx];
		hipPart.shape = Physics::GetOrCreateShape(pc, Physics::ShapeType::Capsule, 0.4f, 0.2f);
		hipPart.mass = 15.0f;

		// Spine
		auto& spinePart = parts[spineIdx];
		spinePart.shape = Physics::GetOrCreateShape(pc, Physics::ShapeType::Capsule, 0.5f, 0.25f);
		spinePart.mass = 20.0f;
		spinePart.enableMotors = true;
		spinePart.maxMotorForce = 250.0f;

		// Head
		auto& headPart = parts[headIdx];
		headPart.shape = Physics::GetOrCreateShape(pc, Physics::ShapeType::Sphere, 0.3f);
		headPart.mass = 8.0f;
		headPart.enableMotors = true;
		headPart.maxMotorForce = 250.0f;

		// Invoke the physics module's low-level constructor [3]
		auto ragdollInstance = Physics::CreateSkeletalRagdoll(pc, skeleton.GetPtr(), parts);

		uint32_t jointOffset = 0;
		if (!visualParts.empty()) {
			if (auto* meshComp = reg.Get<MeshComponent>(visualParts[0])) {
				jointOffset = meshComp->jointOffset;
			}
		}

		// Attach the component to the player controller entity
		reg.Add(playerEntity, RagdollComponent{.ragdollInstance = ragdollInstance,
											   .state = RagdollState::Inactive,
											   .prevState = RagdollState::Inactive,
											   .isAddedToPhysics = 0,
											   .jointOffset = jointOffset,
											   .jointCount = (uint32_t)pomniSkin->joints_count,
											   .gltfSkin = const_cast<cgltf_skin*>(pomniSkin)});

		Log("Skeletal Ragdoll successfully generated and bound to player controller.");
	} else {
		Log("WARNING: SetupPlayerRagdoll failed because no skeletal skin was found in visual "
			"parts.");
	}
}
} // namespace ZHLN::AssetFactory
