// File: src/engine/glTFImporter.cpp
#include "Zahlen/Components.hpp"

#include <Zahlen/AssetFactory.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Math3D.hpp>
#include <cgltf.h>
#include <cmath>
#include <filesystem>
#include <stb_image.h>
#include <string>
#include <unordered_map>
#include <vector>

namespace ZHLN::AssetFactory {

// Declare shared caches (linked externally by AnimationSystem)
std::unordered_map<std::string, cgltf_data*> s_GLBCache;

static uint32_t LoadEmbeddedTexture(RenderContext& ctx, cgltf_image* img,
									const std::string& glbPath, bool isSRGB = true) {
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
				(glbFolder / img->uri).string(), reason ? reason : "Unknown");
		} else {
			Log("ERROR: Failed to load embedded texture (buffer_view offset: {}) | Reason: {}",
				img->buffer_view ? img->buffer_view->offset : 0, reason ? reason : "Unknown");
		}
		return 1;
	}

	uint32_t index = ctx.CreateTexture(pixels, width, height, isSRGB);
	stbi_image_free(pixels);

	textureCache[key] = index;
	return index;
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

std::vector<Entity> SpawnGLB(RenderContext& ctx, ECS::Registry& reg, const std::string& path) {
	cgltf_options opts{};
	cgltf_data* data = nullptr;

	std::string rawPath = "resources/assets/" + path;

	if (s_GLBCache.contains(path)) {
		data = s_GLBCache[path];
	} else {
		if (cgltf_parse_file(&opts, rawPath.c_str(), &data) != cgltf_result_success) {
			Log("ERROR: Failed to parse GLB: {}", rawPath);
			return {};
		}

		if (cgltf_load_buffers(&opts, data, rawPath.c_str()) != cgltf_result_success) {
			Log("ERROR: Failed to load GLB buffers: {}", rawPath);
			cgltf_free(data);
			return {};
		}
		s_GLBCache[path] = data;

		// Decompose all static matrices into standard TRS paths
		for (cgltf_size idx = 0; idx < data->nodes_count; ++idx) {
			cgltf_node* node = &data->nodes[idx];
			if (node->has_matrix) {
				node->has_translation = true;
				node->translation[0] = node->matrix[12];
				node->translation[1] = node->matrix[13];
				node->translation[2] = node->matrix[14];

				JPH::Vec3 col0(node->matrix[0], node->matrix[1], node->matrix[2]);
				JPH::Vec3 col1(node->matrix[4], node->matrix[5], node->matrix[6]);
				JPH::Vec3 col2(node->matrix[8], node->matrix[9], node->matrix[10]);

				node->has_scale = true;
				node->scale[0] = col0.Length();
				node->scale[1] = col1.Length();
				node->scale[2] = col2.Length();

				if (node->scale[0] > 1e-6f)
					col0 /= node->scale[0];
				if (node->scale[1] > 1e-6f)
					col1 /= node->scale[1];
				if (node->scale[2] > 1e-6f)
					col2 /= node->scale[2];

				JPH::Mat44 rotMat(JPH::Vec4(col0, 0.0f), JPH::Vec4(col1, 0.0f),
								  JPH::Vec4(col2, 0.0f), JPH::Vec4(0, 0, 0, 1));
				JPH::Quat rot = rotMat.GetQuaternion();

				node->has_rotation = true;
				node->rotation[0] = rot.GetX();
				node->rotation[1] = rot.GetY();
				node->rotation[2] = rot.GetZ();
				node->rotation[3] = rot.GetW();

				node->has_matrix = false;
			}
		}
	}

	std::vector<Entity> spawnedEntities;

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

			bool doubleSided = false;
			bool alphaBlend = false;
			float baseColorFactor[4] = {1.0f, 1.0f, 1.0f, 1.0f};
			float metallicFactor = 1.0f;
			float roughnessFactor = 1.0f;
			float alphaCutoff = 0.5f;
			uint32_t alphaMode = 0;
			bool hasAlbedoTexture = false;

			if (prim.material != nullptr) {
				doubleSided = prim.material->double_sided;
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
						hasAlbedoTexture = true;
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
				if (colorAcc != nullptr && !hasAlbedoTexture) {
					cgltf_accessor_read_float(colorAcc, vIdx, rawColor, 4);
				}
				v.color = Math::PackColor(rawColor[0], rawColor[1], rawColor[2], rawColor[3]);

				uint32_t joints[4] = {0, 0, 0, 0};
				if (jointsAcc) {
					cgltf_accessor_read_uint(jointsAcc, vIdx, joints, 4);
				}
				v.joints[0] = static_cast<uint16_t>(joints[0]);
				v.joints[1] = static_cast<uint16_t>(joints[1]);
				v.joints[2] = static_cast<uint16_t>(joints[2]);
				v.joints[3] = static_cast<uint16_t>(joints[3]);

				float weights[4] = {0.0f, 0.0f, 0.0f, 0.0f};
				if (weightsAcc) {
					cgltf_accessor_read_float(weightsAcc, vIdx, weights, 4);
				}
				v.weights[0] = weights[0];
				v.weights[1] = weights[1];
				v.weights[2] = weights[2];
				v.weights[3] = weights[3];
			}

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

			BufferHandle vbo = ctx.CreateVertexBuffer(unrolledVertices.data(),
													  unrolledVertices.size() * sizeof(Vertex));
			Mesh subMesh = {.vertexBuffer = vbo,
							.vertexCount = static_cast<uint32_t>(unrolledVertices.size())};

			// Check if we are loading character parts to assign the toon material
			bool isCharacter = (path.contains("POMNI") || path.contains("tadc_models"));

			Material subMaterial = isCharacter ? CreateToonMaterial(ctx, doubleSided, alphaBlend)
											   : CreateBasicMaterial(ctx, doubleSided, alphaBlend);

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

			Entity part = reg.Create();
			reg.Add(part, MeshComponent{.mesh = subMesh,
										.material = subMaterial,
										.cullRadius = 100.0f,
										.localTransform = nodeTransform,
										.prevTransform = nodeTransform,
										.gltfNode = (void*)node,
										.gltfSkin = (void*)node->skin});
			spawnedEntities.push_back(part);
		}
	}

	Log("Spawned GLB Model: {} ({} submesh parts/materials loaded dynamically)", path,
		spawnedEntities.size());
	return spawnedEntities;
}
} // namespace ZHLN::AssetFactory
