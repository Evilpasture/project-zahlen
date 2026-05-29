// src/cooker_main.cpp
#include <Zahlen/AssetManager.hpp>
#include <Zahlen/Math3D.hpp>
#include <Zahlen/Types.hpp>
#include <algorithm>
#include <cgltf.h>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <print> // Replaced <iostream>
#include <string>
#include <vector>

using namespace ZHLN;

int main(int argc, char** argv) {
	if (argc < 3) {
		std::println(stderr, "Usage: zahlen_cooker <input_dir> <output.pak>");
		return 1;
	}

	std::string inputDir = argv[1];
	std::string outputFile = argv[2];

	std::vector<PakEntry> entries;
	std::vector<char> payloadData;

	for (const auto& entry : std::filesystem::recursive_directory_iterator(inputDir)) {
		std::string ext = entry.path().extension().string();
		std::string filepath = entry.path().string();

		// Virtual path format (e.g. "assets/Man.glb" -> "Man.glb")
		std::string virtualPath = filepath.substr(inputDir.length());
		if (virtualPath[0] == '/' || virtualPath[0] == '\\') {
			virtualPath = virtualPath.substr(1);
		}
		std::replace(virtualPath.begin(), virtualPath.end(), '\\', '/');
		
		if (ext == ".glb") {
			cgltf_options opts{};
			cgltf_data* data = nullptr;

			if (cgltf_parse_file(&opts, filepath.c_str(), &data) != cgltf_result_success ||
				cgltf_load_buffers(&opts, data, filepath.c_str()) != cgltf_result_success) {
				std::println(stderr, "Failed to parse: {}", filepath);
				continue;
			}

			std::vector<Vertex> bakedVertices;

			// Extraction loop
			for (cgltf_size i = 0; i < data->nodes_count; ++i) {
				const cgltf_node* node = &data->nodes[i];
				if (!node->mesh)
					continue;

				float matrix[16];
				cgltf_node_transform_world(node, matrix);

				const auto* mesh = node->mesh;
				for (cgltf_size p = 0; p < mesh->primitives_count; ++p) {
					const auto& prim = mesh->primitives[p];

					cgltf_accessor* posAcc = nullptr;
					cgltf_accessor* normAcc = nullptr;
					cgltf_accessor* tangentAcc = nullptr;
					cgltf_accessor* uvAcc = nullptr;
					cgltf_accessor* colorAcc = nullptr; // Track the color attribute

					for (cgltf_size a = 0; a < prim.attributes_count; ++a) {
						const auto& attr = prim.attributes[a];
						if (attr.type == cgltf_attribute_type_position)
							posAcc = attr.data;
						else if (attr.type == cgltf_attribute_type_normal)
							normAcc = attr.data;
						else if (attr.type == cgltf_attribute_type_tangent)
							tangentAcc = attr.data;
						else if (attr.type == cgltf_attribute_type_texcoord && attr.index == 0)
							uvAcc = attr.data;
						else if (attr.type == cgltf_attribute_type_color && attr.index == 0)
							colorAcc = attr.data;
					}

					if (!posAcc)
						continue;

					size_t vertexCount = posAcc->count;
					std::vector<Vertex> primVertices(vertexCount);

					for (size_t vIdx = 0; vIdx < vertexCount; ++vIdx) {
						Vertex& v = primVertices[vIdx];
						std::memset(&v, 0, sizeof(Vertex));

						// Read and Transform Position
						float rawPos[3] = {0.0f, 0.0f, 0.0f};
						cgltf_accessor_read_float(posAcc, vIdx, rawPos, 3);
						v.position[0] = matrix[0] * rawPos[0] + matrix[4] * rawPos[1] +
										matrix[8] * rawPos[2] + matrix[12];
						v.position[1] = matrix[1] * rawPos[0] + matrix[5] * rawPos[1] +
										matrix[9] * rawPos[2] + matrix[13];
						v.position[2] = matrix[2] * rawPos[0] + matrix[6] * rawPos[1] +
										matrix[10] * rawPos[2] + matrix[14];

						// Read and Transform Normal
						float rawNorm[3] = {0.0f, 1.0f, 0.0f};
						if (normAcc)
							cgltf_accessor_read_float(normAcc, vIdx, rawNorm, 3);
						float nx = matrix[0] * rawNorm[0] + matrix[4] * rawNorm[1] +
								   matrix[8] * rawNorm[2];
						float ny = matrix[1] * rawNorm[0] + matrix[5] * rawNorm[1] +
								   matrix[9] * rawNorm[2];
						float nz = matrix[2] * rawNorm[0] + matrix[6] * rawNorm[1] +
								   matrix[10] * rawNorm[2];
						float nLen = std::sqrt(nx * nx + ny * ny + nz * nz);
						if (nLen > 1e-6f) {
							nx /= nLen;
							ny /= nLen;
							nz /= nLen;
						}
						v.normal = Math::PackNormal(nx, ny, nz);

						// Read and Transform Tangent
						float rawTangent[4] = {1.0f, 0.0f, 0.0f, 1.0f};
						if (tangentAcc)
							cgltf_accessor_read_float(tangentAcc, vIdx, rawTangent, 4);
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
						if (uvAcc)
							cgltf_accessor_read_float(uvAcc, vIdx, uv, 2);
						v.uv = Math::PackUV(uv[0], uv[1]);

						// Read and pack the color attribute
						float rawColor[4] = {1.0f, 1.0f, 1.0f, 1.0f};
						if (colorAcc) {
							cgltf_accessor_read_float(colorAcc, vIdx, rawColor, 4);
						}
						v.color = Math::PackColor(rawColor[0], rawColor[1], rawColor[2], rawColor[3]);
					}

					// Resolve Indices
					if (prim.indices) {
						size_t indexCount = prim.indices->count;
						for (size_t idx = 0; idx < indexCount; ++idx) {
							size_t originalIdx = cgltf_accessor_read_index(prim.indices, idx);
							bakedVertices.push_back(primVertices[originalIdx]);
						}
					} else {
						bakedVertices.insert(bakedVertices.end(), primVertices.begin(),
											 primVertices.end());
					}
				}
			}
			cgltf_free(data);

			// Pack the Blob
			CookedMeshHeader meshHeader{};
			meshHeader.magic = 0x3048534D; // 'MSH0'
			meshHeader.version = 1;
			meshHeader.vertexCount = bakedVertices.size();
			meshHeader.indexCount = 0;

			size_t blobSize = sizeof(CookedMeshHeader) + (bakedVertices.size() * sizeof(Vertex));

			// Align payload offset to 16 bytes for safe Zero-Copy GPU uploads
			size_t padding = (16 - (payloadData.size() % 16)) % 16;
			payloadData.insert(payloadData.end(), padding, 0);

			PakEntry pakEntry{};
			pakEntry.pathHash = HashAssetPath(virtualPath);
			pakEntry.offset = sizeof(PakHeader) + payloadData.size(); // TOC comes later
			pakEntry.compressedSize = blobSize;
			pakEntry.uncompressedSize = blobSize;
			pakEntry.compression = 0; // ZERO-COPY

			entries.push_back(pakEntry);

			// Append Header then Vertices
			const char* headerBytes = reinterpret_cast<const char*>(&meshHeader);
			payloadData.insert(payloadData.end(), headerBytes,
							   headerBytes + sizeof(CookedMeshHeader));

			const char* vertexBytes = reinterpret_cast<const char*>(bakedVertices.data());
			payloadData.insert(payloadData.end(), vertexBytes,
							   vertexBytes + (bakedVertices.size() * sizeof(Vertex)));

			std::println("Cooked: {} ({} vertices)", virtualPath, bakedVertices.size());
		} else if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga") {
			// Raw image packing
			std::ifstream file(filepath, std::ios::binary | std::ios::ate);
			if (!file.is_open()) {
				std::println(stderr, "Failed to read texture file: {}", filepath);
				continue;
			}
			size_t size = file.tellg();
			std::vector<char> fileData(size);
			file.seekg(0);
			file.read(fileData.data(), size);
			file.close();

			// Align payload offset to 16 bytes
			size_t padding = (16 - (payloadData.size() % 16)) % 16;
			payloadData.insert(payloadData.end(), padding, 0);

			PakEntry pakEntry{};
			pakEntry.pathHash = HashAssetPath(virtualPath);
			pakEntry.offset = sizeof(PakHeader) + payloadData.size();
			pakEntry.compressedSize = size;
			pakEntry.uncompressedSize = size;
			pakEntry.compression = 0;

			entries.push_back(pakEntry);
			payloadData.insert(payloadData.end(), fileData.begin(), fileData.end());

			std::println("Cooked Raw Texture: {} ({} bytes)", virtualPath, size);
		}
	}

	// Write PAK File
	PakHeader header{};
	std::memcpy(header.magic, "ZPAK", 4);
	header.version = 1;
	header.entryCount = entries.size();
	header.tocOffset = sizeof(PakHeader) + payloadData.size(); // TOC is stored after all blobs

	std::ofstream out(outputFile, std::ios::binary);
	out.write(reinterpret_cast<const char*>(&header), sizeof(PakHeader));
	out.write(payloadData.data(), payloadData.size());
	out.write(reinterpret_cast<const char*>(entries.data()), entries.size() * sizeof(PakEntry));
	out.close();

	std::println("Successfully cooked {} assets to {}", entries.size(), outputFile);
	return 0;
}