// src/zcook/GLB.cpp
// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "GLB.hpp"

#include "Transform.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <format>
#include <print>
#include <unordered_map>
#include <vector>
namespace ZHLN {
namespace GLB {

namespace {

// Safe scalar helper to expand half-precision floats back to standard 32-bit floats
inline float HalfToFloat(uint16_t h) noexcept {
	uint32_t sign = (h >> 15) & 0x00000001;
	uint32_t exponent = (h >> 10) & 0x0000001f;
	uint32_t mantissa = h & 0x000003ff;

	if (exponent == 0) {
		if (mantissa == 0) {
			return sign ? -0.0f : 0.0f;
		}
		return (sign ? -1.0f : 1.0f) * std::ldexp(static_cast<float>(mantissa), -24);
	} else if (exponent == 31) {
		return sign ? -INFINITY : INFINITY;
	}

	return (sign ? -1.0f : 1.0f) *
		   std::ldexp(static_cast<float>(mantissa | 0x0400), static_cast<int>(exponent) - 15 - 10);
}

// Unpacks 10-bit per-axis packed normal/tangent formats back into standard floats
inline std::array<float, 4> UnpackNormal(uint32_t packed) noexcept {
	float x = (float(packed & 0x3FF) / 1023.0f) * 2.0f - 1.0f;
	float y = (float((packed >> 10) & 0x3FF) / 1023.0f) * 2.0f - 1.0f;
	float z = (float((packed >> 20) & 0x3FF) / 1023.0f) * 2.0f - 1.0f;
	float w = (packed >> 30) > 0 ? 1.0f : -1.0f;
	return {x, y, z, w};
}

} // namespace

bool EmitGLB(const Compiler::IRManifest& manifest, const std::string& levelFolder,
			 const std::string& outputPath) {
	std::vector<uint8_t> binBuffer;
	binBuffer.reserve(static_cast<size_t>(16 * 1024 * 1024));

	std::vector<std::string> bufferViews;
	std::vector<std::string> accessors;
	std::vector<std::string> meshesJson;
	std::vector<std::string> nodesJson;
	std::vector<std::string> skinsJson;
	std::vector<std::string> glbAnimsJson;

	std::unordered_map<std::string, int> meshIdToGlbIndex;
	std::unordered_map<std::string, int> lightIdToGlbIndex;
	std::unordered_map<std::string, int> nodeIdToGlbIndex;
	std::unordered_map<std::string, int> skinIdToGlbIndex;

	uint32_t accIndex = 0;
	uint32_t bViewIndex = 0;

	std::vector<std::string> images;
	std::vector<std::string> textures;
	std::vector<std::string> materialsJson;
	std::unordered_map<std::string, int> matIdToGlbIndex;

	struct PackedImage {
		std::string relativeUri;
		uint32_t bufferViewIndex;
	};
	std::vector<PackedImage> packedImages;

	auto getTextureIndex = [&](const std::string& relativeUri) -> int {
		if (relativeUri.empty()) {
			return -1;
		}

		for (size_t i = 0; i < packedImages.size(); ++i) {
			if (packedImages[i].relativeUri == relativeUri)
				return static_cast<int>(i);
		}

		std::string fullPath = levelFolder + "/" + relativeUri;
		FILE* f = std::fopen(fullPath.c_str(), "rb");
		if (f == nullptr) {
			std::println(stderr,
						 "[zcook] WARNING: Failed to open texture source file '{}': {}. Skipping.",
						 fullPath, std::strerror(errno));
			return -1;
		}

		std::fseek(f, 0, SEEK_END);
		long size = std::ftell(f);
		std::fseek(f, 0, SEEK_SET);

		std::vector<uint8_t> imgBytes(size);
		std::fread(imgBytes.data(), 1, size, f);
		std::fclose(f);

		while (binBuffer.size() % 4 != 0)
			binBuffer.push_back(0);

		auto imgOffset = static_cast<uint32_t>(binBuffer.size());
		binBuffer.insert(binBuffer.end(), imgBytes.begin(), imgBytes.end());
		while (binBuffer.size() % 4 != 0)
			binBuffer.push_back(0);

		std::string mimeType = "image/png";
		if (relativeUri.ends_with(".jpg") || relativeUri.ends_with(".jpeg"))
			mimeType = "image/jpeg";

		bufferViews.push_back(std::format(R"(    {{
      "buffer": 0,
      "byteOffset": {},
      "byteLength": {}
    }})",
										  imgOffset, size));
		uint32_t imgBViewIdx = bViewIndex++;

		int idx = static_cast<int>(packedImages.size());
		packedImages.push_back({.relativeUri = relativeUri, .bufferViewIndex = imgBViewIdx});
		textures.push_back(std::format(R"(    {{"sampler": 0, "source": {}}})", idx));
		images.push_back(
			std::format(R"(    {{"bufferView": {}, "mimeType": "{}"}})", imgBViewIdx, mimeType));

		return idx;
	};

	for (const auto& mat : manifest.materials) {
		int albedoTex = getTextureIndex(mat.albedoMap);
		int normalTex = getTextureIndex(mat.normalMap);
		int mrTex = getTextureIndex(mat.metallicRoughnessMap);
		int emissiveTex = getTextureIndex(mat.emissiveMap);

		std::string pbrStr = std::format(R"(      "baseColorFactor": [{}, {}, {}, {}],
      "metallicFactor": {},
      "roughnessFactor": {})",
										 mat.baseColor[0], mat.baseColor[1], mat.baseColor[2],
										 mat.baseColor[3], mat.metallic, mat.roughness);

		if (albedoTex != -1) {
			pbrStr += std::format(R"(,
      "baseColorTexture": {{"index": {}}})",
								  albedoTex);
		}

		std::string matStr = std::format(R"(    {{
      "name": "{}",
      "pbrMetallicRoughness": {{
  {}
      }})",
										 mat.id, pbrStr);

		// If procedural data is present, write it as a custom extension
		if (mat.procedural.active) {
			std::string paramsJson = "{\n";
			for (size_t p = 0; p < mat.procedural.parameters.size(); ++p) {
				const auto& param = mat.procedural.parameters[p];
				paramsJson += std::format(R"(          "{}": )", param.name);
				if (param.values.size() == 1) {
					paramsJson += std::to_string(param.values[0]);
				} else {
					paramsJson += "[";
					for (size_t v = 0; v < param.values.size(); ++v) {
						paramsJson += std::to_string(param.values[v]) +
									  (v < param.values.size() - 1 ? ", " : "");
					}
					paramsJson += "]";
				}
				paramsJson += (p < mat.procedural.parameters.size() - 1 ? ",\n" : "\n");
			}
			paramsJson += "        }";

			matStr += std::format(R"(,
      "extensions": {{
        "ZHLN_procedural_shader": {{
          "type": "{}",
          "parameters": {}
        }}
      }})",
								  mat.procedural.type, paramsJson);
		}

		if (mat.baseColor[3] < 0.999f) {
			matStr += R"(,
      "alphaMode": "BLEND")";
		}

		if (mat.doubleSided) {
			matStr += R"(,
      "doubleSided": true)";
		}

		if (normalTex != -1) {
			matStr += std::format(R"(,
      "normalTexture": {{"index": {}}})",
								  normalTex);
		}
		if (mrTex != -1) {
			matStr += std::format(R"(,
      "metallicRoughnessTexture": {{"index": {}}})",
								  mrTex);
		}

		bool hasEmissive =
			(mat.emissiveStrength > 0.f) &&
			((emissiveTex != -1) || (mat.emissiveFactor[0] > 0.f || mat.emissiveFactor[1] > 0.f ||
									 mat.emissiveFactor[2] > 0.f));
		if (hasEmissive) {
			float ef[3] = {mat.emissiveFactor[0], mat.emissiveFactor[1], mat.emissiveFactor[2]};
			if (emissiveTex != -1 && ef[0] == 0.f && ef[1] == 0.f && ef[2] == 0.f) {
				ef[0] = 1.f;
				ef[1] = 1.f;
				ef[2] = 1.f;
			}
			float strength = mat.emissiveStrength;
			if (strength < 1.f) {
				ef[0] *= strength;
				ef[1] *= strength;
				ef[2] *= strength;
				strength = 1.f;
			}

			matStr += std::format(R"(,
      "emissiveFactor": [{}, {}, {}])",
								  ef[0], ef[1], ef[2]);
			if (emissiveTex != -1) {
				matStr += std::format(R"(,
      "emissiveTexture": {{"index": {}}})",
									  emissiveTex);
			}
			if (strength > 1.f) {
				matStr += std::format(R"(,
      "extensions": {{
        "KHR_materials_emissive_strength": {{
          "emissiveStrength": {}
        }}
      }})",
									  strength);
			}
		}

		matStr += "\n    }";
		matIdToGlbIndex[mat.id] = static_cast<int>(materialsJson.size());
		materialsJson.push_back(matStr);
	}

	for (const auto& mesh : manifest.meshes) {
		std::string binPath = levelFolder + "/" + mesh.binFile;
		CompiledMesh compiled = CompileRawMesh(mesh, binPath);
		if (compiled.positions.empty()) {
			continue;
		}

		meshIdToGlbIndex[mesh.id] = static_cast<int>(meshesJson.size());

		auto vertexCount = static_cast<uint32_t>(compiled.positions.size());

		// 1. Pack Positions (directly from compiled.positions)
		auto posOffset = static_cast<uint32_t>(binBuffer.size());
		size_t posBytes = compiled.positions.size() * sizeof(VertexPosition);
		binBuffer.insert(binBuffer.end(),
						 reinterpret_cast<const uint8_t*>(compiled.positions.data()),
						 reinterpret_cast<const uint8_t*>(compiled.positions.data()) + posBytes);
		while (binBuffer.size() % 4 != 0)
			binBuffer.push_back(0);

		bufferViews.push_back(std::format(R"(    {{
      "buffer": 0,
      "byteOffset": {},
      "byteLength": {},
      "target": 34962
    }})",
										  posOffset, posBytes));
		uint32_t posBViewIdx = bViewIndex++;

		// 2. Unpack and Pack Normals (FLOAT3)
		auto normOffset = static_cast<uint32_t>(binBuffer.size());
		for (const auto& attr : compiled.attributes) {
			auto n = UnpackNormal(attr.normal.data);
			float nFlt[3] = {n[0], n[1], n[2]};
			binBuffer.insert(binBuffer.end(), reinterpret_cast<const uint8_t*>(nFlt),
							 reinterpret_cast<const uint8_t*>(nFlt) + 12);
		}
		while (binBuffer.size() % 4 != 0)
			binBuffer.push_back(0);

		bufferViews.push_back(std::format(R"(    {{
      "buffer": 0,
      "byteOffset": {},
      "byteLength": {},
      "target": 34962
    }})",
										  normOffset, vertexCount * 12));
		uint32_t normBViewIdx = bViewIndex++;

		// 3. Unpack and Pack Tangents (FLOAT4)
		auto tangOffset = static_cast<uint32_t>(binBuffer.size());
		for (const auto& attr : compiled.attributes) {
			auto t = UnpackNormal(attr.tangent.data);
			float tFlt[4] = {t[0], t[1], t[2], t[3]};
			binBuffer.insert(binBuffer.end(), reinterpret_cast<const uint8_t*>(tFlt),
							 reinterpret_cast<const uint8_t*>(tFlt) + 16);
		}
		while (binBuffer.size() % 4 != 0)
			binBuffer.push_back(0);

		bufferViews.push_back(std::format(R"(    {{
      "buffer": 0,
      "byteOffset": {},
      "byteLength": {},
      "target": 34962
    }})",
										  tangOffset, vertexCount * 16));
		uint32_t tangBViewIdx = bViewIndex++;

		// 4. Unpack and Pack UVs (FLOAT2)
		auto uvOffset = static_cast<uint32_t>(binBuffer.size());
		for (const auto& attr : compiled.attributes) {
			float u = HalfToFloat(attr.uv.data & 0xFFFF);
			float v = HalfToFloat(attr.uv.data >> 16);
			float uvFlt[2] = {u, v};
			binBuffer.insert(binBuffer.end(), reinterpret_cast<const uint8_t*>(uvFlt),
							 reinterpret_cast<const uint8_t*>(uvFlt) + 8);
		}
		while (binBuffer.size() % 4 != 0)
			binBuffer.push_back(0);

		bufferViews.push_back(std::format(R"(    {{
      "buffer": 0,
      "byteOffset": {},
      "byteLength": {},
      "target": 34962
    }})",
										  uvOffset, vertexCount * 8));
		uint32_t uvBViewIdx = bViewIndex++;

		// 5. Pack Colors (directly as UNORM8)
		auto colorOffset = static_cast<uint32_t>(binBuffer.size());
		for (const auto& attr : compiled.attributes) {
			uint32_t col = attr.color.data;
			binBuffer.insert(binBuffer.end(), reinterpret_cast<const uint8_t*>(&col),
							 reinterpret_cast<const uint8_t*>(&col) + 4);
		}
		while (binBuffer.size() % 4 != 0)
			binBuffer.push_back(0);

		bufferViews.push_back(std::format(R"(    {{
      "buffer": 0,
      "byteOffset": {},
      "byteLength": {},
      "target": 34962
    }})",
										  colorOffset, vertexCount * 4));
		uint32_t colorBViewIdx = bViewIndex++;

		// 6. Indices (IBO)
		auto iboOffset = static_cast<uint32_t>(binBuffer.size());
		size_t iboBytes = compiled.indices.size() * sizeof(uint32_t);
		if (iboBytes > 0) {
			binBuffer.insert(binBuffer.end(), reinterpret_cast<uint8_t*>(compiled.indices.data()),
							 reinterpret_cast<uint8_t*>(compiled.indices.data()) + iboBytes);
		}
		while (binBuffer.size() % 4 != 0) {
			binBuffer.push_back(0);
		}

		bufferViews.push_back(std::format(R"(    {{
      "buffer": 0,
      "byteOffset": {},
      "byteLength": {},
      "target": 34963
    }})",
										  iboOffset, iboBytes));
		uint32_t iboBViewIdx = bViewIndex++;

		uint32_t posAcc = accIndex++;
		uint32_t normAcc = accIndex++;
		uint32_t tangAcc = accIndex++;
		uint32_t uvAcc = accIndex++;
		uint32_t colorAcc = accIndex++;

		accessors.push_back(std::format(
			R"(    {{"bufferView": {}, "componentType": 5126, "count": {}, "type": "VEC3", "min": [{}, {}, {}], "max": [{}, {}, {}]}})",
			posBViewIdx, vertexCount, compiled.minB[0], compiled.minB[1], compiled.minB[2],
			compiled.maxB[0], compiled.maxB[1], compiled.maxB[2]));
		accessors.push_back(std::format(
			R"(    {{"bufferView": {}, "componentType": 5126, "count": {}, "type": "VEC3"}})",
			normBViewIdx, vertexCount));
		accessors.push_back(std::format(
			R"(    {{"bufferView": {}, "componentType": 5126, "count": {}, "type": "VEC4"}})",
			tangBViewIdx, vertexCount));
		accessors.push_back(std::format(
			R"(    {{"bufferView": {}, "componentType": 5126, "count": {}, "type": "VEC2"}})",
			uvBViewIdx, vertexCount));

		// Use 5121 (UNSIGNED_BYTE) normalized=true for vertex colors
		accessors.push_back(std::format(
			R"(    {{"bufferView": {}, "componentType": 5121, "count": {}, "type": "VEC4", "normalized": true}})",
			colorBViewIdx, vertexCount));

		uint32_t jointsAcc = 0;
		uint32_t weightsAcc = 0;

		if (compiled.isSkinned) {
			// joints: uint16_t[4] -> 5123 (UNSIGNED_SHORT)
			while (binBuffer.size() % 4 != 0) {
				binBuffer.push_back(0);
			}
			auto jboOffset = static_cast<uint32_t>(binBuffer.size());
			size_t jboBytes = compiled.skins.size() * 8;
			for (const auto& s : compiled.skins) {
				binBuffer.insert(binBuffer.end(), reinterpret_cast<const uint8_t*>(s.joints),
								 reinterpret_cast<const uint8_t*>(s.joints) + 8);
			}
			while (binBuffer.size() % 4 != 0) {
				binBuffer.push_back(0);
			}

			bufferViews.push_back(std::format(
				R"(    {{ "buffer": 0, "byteOffset": {}, "byteLength": {}, "target": 34962 }})",
				jboOffset, jboBytes));
			uint32_t jboBViewIdx = bViewIndex++;

			jointsAcc = accIndex++;
			accessors.push_back(std::format(
				R"(    {{"bufferView": {}, "componentType": 5123, "count": {}, "type": "VEC4"}})",
				jboBViewIdx, vertexCount));

			// weights: PackedRGBA8 -> 5121 (UNSIGNED_BYTE) normalized=true
			while (binBuffer.size() % 4 != 0) {
				binBuffer.push_back(0);
			}
			auto wboOffset = static_cast<uint32_t>(binBuffer.size());
			for (const auto& s : compiled.skins) {
				uint32_t w = s.weights.data;
				binBuffer.insert(binBuffer.end(), reinterpret_cast<const uint8_t*>(&w),
								 reinterpret_cast<const uint8_t*>(&w) + 4);
			}
			while (binBuffer.size() % 4 != 0) {
				binBuffer.push_back(0);
			}

			bufferViews.push_back(std::format(
				R"(    {{ "buffer": 0, "byteOffset": {}, "byteLength": {}, "target": 34962 }})",
				wboOffset, vertexCount * 4));
			uint32_t wboBViewIdx = bViewIndex++;

			weightsAcc = accIndex++;
			accessors.push_back(std::format(
				R"(    {{"bufferView": {}, "componentType": 5121, "count": {}, "type": "VEC4", "normalized": true}})",
				wboBViewIdx, vertexCount));
		}

		// Compile morph targets (shape keys)
		std::vector<std::string> targetsJson;
		for (const auto& target : mesh.morphTargets) {
			std::string targetBinPath = levelFolder + "/" + target.binFile;
			FILE* tbf = std::fopen(targetBinPath.c_str(), "rb");
			if (tbf == nullptr) {
				continue;
			}

			std::fseek(tbf, 0, SEEK_END);
			long tSize = std::ftell(tbf);
			std::fseek(tbf, 0, SEEK_SET);
			std::vector<float> rawOffsets(tSize / sizeof(float));
			if (!rawOffsets.empty()) {
				std::fread(rawOffsets.data(), sizeof(float), rawOffsets.size(), tbf);
			}
			std::fclose(tbf);

			// Remap original raw offsets using original Blender vertex indices
			std::vector<float> compiledOffsets(static_cast<size_t>(vertexCount * 3), 0.0f);
			for (size_t i = 0; i < vertexCount; ++i) {
				uint32_t origIdx = compiled.originalVertexIndices[i];
				if (origIdx * 3 + 2 < rawOffsets.size()) {
					compiledOffsets[i * 3 + 0] = rawOffsets[origIdx * 3 + 0];
					compiledOffsets[i * 3 + 1] = rawOffsets[origIdx * 3 + 1];
					compiledOffsets[i * 3 + 2] = rawOffsets[origIdx * 3 + 2];
				}
			}

			while (binBuffer.size() % 4 != 0) {
				binBuffer.push_back(0);
			}
			auto targetOffset = static_cast<uint32_t>(binBuffer.size());
			size_t targetBytes = compiledOffsets.size() * sizeof(float);
			binBuffer.insert(binBuffer.end(), reinterpret_cast<uint8_t*>(compiledOffsets.data()),
							 reinterpret_cast<uint8_t*>(compiledOffsets.data()) + targetBytes);

			bufferViews.push_back(
				std::format(R"(    {{ "buffer": 0, "byteOffset": {}, "byteLength": {} }})",
							targetOffset, targetBytes));
			uint32_t targetBViewIdx = bViewIndex++;

			uint32_t targetAccIdx = accIndex++;

			float minO[3] = {1e30f, 1e30f, 1e30f};
			float maxO[3] = {-1e30f, -1e30f, -1e30f};
			for (size_t i = 0; i < vertexCount; ++i) {
				minO[0] = std::min(minO[0], compiledOffsets[i * 3 + 0]);
				minO[1] = std::min(minO[1], compiledOffsets[i * 3 + 1]);
				minO[2] = std::min(minO[2], compiledOffsets[i * 3 + 2]);
				maxO[0] = std::max(maxO[0], compiledOffsets[i * 3 + 0]);
				maxO[1] = std::max(maxO[1], compiledOffsets[i * 3 + 1]);
				maxO[2] = std::max(maxO[2], compiledOffsets[i * 3 + 2]);
			}

			accessors.push_back(std::format(
				R"(    {{"bufferView": {}, "componentType": 5126, "count": {}, "type": "VEC3", "min": [{}, {}, {}], "max": [{}, {}, {}]}})",
				targetBViewIdx, vertexCount, minO[0], minO[1], minO[2], maxO[0], maxO[1], maxO[2]));

			targetsJson.push_back(std::format(R"(        {{ "POSITION": {} }})", targetAccIdx));
		}

		std::string targetsArrayStr;
		if (!targetsJson.empty()) {
			targetsArrayStr = R"(, "targets": [ )";
			for (size_t t = 0; t < targetsJson.size(); ++t) {
				targetsArrayStr += targetsJson[t] + (t < targetsJson.size() - 1 ? ", " : "");
			}
			targetsArrayStr += " ]";
		}

		std::string primsStr;
		for (size_t p = 0; p < compiled.primitives.size(); ++p) {
			const auto& prim = compiled.primitives[p];
			uint32_t indexAcc = accIndex++;

			accessors.push_back(std::format(
				R"(    {{ "bufferView": {}, "byteOffset": {}, "componentType": 5125, "count": {}, "type": "SCALAR" }})",
				iboBViewIdx, prim.vertexOffset, prim.vertexCount));

			int matGlbIdx = -1;
			auto it = matIdToGlbIndex.find(prim.materialId);
			if (it != matIdToGlbIndex.end()) {
				matGlbIdx = it->second;
			}

			std::string matStr;
			if (matGlbIdx != -1) {
				matStr = std::format(R"(, "material": {})", matGlbIdx);
			}

			std::string skinAttribs;
			if (compiled.isSkinned) {
				skinAttribs =
					std::format(R"(, "JOINTS_0": {}, "WEIGHTS_0": {})", jointsAcc, weightsAcc);
			}

			primsStr += std::format(R"(        {{
          "attributes": {{ "POSITION": {}, "NORMAL": {}, "TANGENT": {}, "TEXCOORD_0": {}, "COLOR_0": {} {} }},
          "indices": {} {}{}
        }})",
									posAcc, normAcc, tangAcc, uvAcc, colorAcc, skinAttribs,
									indexAcc, matStr, targetsArrayStr);
			if (p < compiled.primitives.size() - 1) {
				primsStr += ",\n";
			}
		}

		meshesJson.push_back(
			std::format(R"(    {{ "name": "{}", "primitives": [ {} ] }})", mesh.id, primsStr));
	}

	for (size_t i = 0; i < manifest.lights.size(); ++i) {
		lightIdToGlbIndex[manifest.lights[i].id] = static_cast<int>(i);
	}

	std::unordered_map<std::string, std::vector<std::string>> nodeChildren;
	for (const auto& node : manifest.nodes) {
		if (!node.parentId.empty()) {
			nodeChildren[node.parentId].push_back(node.id);
		}
	}
	for (const auto& skin : manifest.skins) {
		for (size_t i = 0; i < skin.joints.size(); ++i) {
			if (i < skin.parents.size() && !skin.parents[i].empty()) {
				nodeChildren[skin.parents[i]].push_back(skin.joints[i]);
			}
		}
	}

	int glbNodeIdx = 0;
	std::vector<const Compiler::IRNode*> nodesToEmit;
	for (const auto& node : manifest.nodes) {
		bool isTargetedByAnim = false;
		for (const auto& anim : manifest.animations) {
			for (const auto& chan : anim.channels) {
				if (chan.targetNodeId == node.id) {
					isTargetedByAnim = true;
					break;
				}
			}
			if (isTargetedByAnim) {
				break;
			}
		}
		if (!node.visible && node.meshId.empty() && node.lightId.empty() && !isTargetedByAnim) {
			continue;
		}
		nodeIdToGlbIndex[node.id] = glbNodeIdx++;
		nodesToEmit.push_back(&node);
	}

	std::vector<std::pair<std::string, std::string>> jointsToEmit;
	for (const auto& skin : manifest.skins) {
		for (size_t i = 0; i < skin.joints.size(); ++i) {
			const auto& jointId = skin.joints[i];
			if (nodeIdToGlbIndex.contains(jointId)) {
				continue;
			}

			std::string matrixStr = "[";
			for (int m = 0; m < 16; ++m) {
				float val = (i * 16 + m < skin.restPose.size())
								? skin.restPose[i * 16 + m]
								: (m == 0 || m == 5 || m == 10 || m == 15 ? 1.0f : 0.0f);
				matrixStr += std::to_string(val);
				if (m < 15) {
					matrixStr += ", ";
				}
			}
			matrixStr += "]";
			nodeIdToGlbIndex[jointId] = glbNodeIdx++;
			jointsToEmit.emplace_back(jointId, matrixStr);
		}
	}

	nodesJson.resize(glbNodeIdx);

	for (const auto& skin : manifest.skins) {
		while (binBuffer.size() % 4 != 0) {
			binBuffer.push_back(0);
		}
		auto ibmOffset = static_cast<uint32_t>(binBuffer.size());
		size_t ibmBytes = skin.inverseBindMatrices.size() * sizeof(float);
		if (ibmBytes > 0) {
			binBuffer.insert(
				binBuffer.end(), reinterpret_cast<const uint8_t*>(skin.inverseBindMatrices.data()),
				reinterpret_cast<const uint8_t*>(skin.inverseBindMatrices.data()) + ibmBytes);
		}

		bufferViews.push_back(std::format(
			R"(    {{ "buffer": 0, "byteOffset": {}, "byteLength": {} }})", ibmOffset, ibmBytes));
		uint32_t ibmBViewIdx = bViewIndex++;

		uint32_t ibmAccIdx = accIndex++;
		accessors.push_back(std::format(
			R"(    {{"bufferView": {}, "componentType": 5126, "count": {}, "type": "MAT4"}})",
			ibmBViewIdx, skin.joints.size()));

		std::string jointsStr;
		for (size_t j = 0; j < skin.joints.size(); ++j) {
			jointsStr += std::to_string(nodeIdToGlbIndex[skin.joints[j]]);
			if (j < skin.joints.size() - 1)
				jointsStr += ", ";
		}

		skinIdToGlbIndex[skin.id] = static_cast<int>(skinsJson.size());
		skinsJson.push_back(
			std::format(R"(    {{ "name": "{}", "inverseBindMatrices": {}, "joints": [{}] }})",
						skin.name, ibmAccIdx, jointsStr));
	}

	for (const auto& [jointId, matrixStr] : jointsToEmit) {
		int nIdx = nodeIdToGlbIndex[jointId];
		std::string childrenStr;
		auto cIt = nodeChildren.find(jointId);
		if (cIt != nodeChildren.end() && !cIt->second.empty()) {
			std::vector<int> activeChildren;
			for (const auto& childId : cIt->second) {
				auto childIt = nodeIdToGlbIndex.find(childId);
				if (childIt != nodeIdToGlbIndex.end())
					activeChildren.push_back(childIt->second);
			}
			if (!activeChildren.empty()) {
				childrenStr = ",\n      \"children\": [";
				for (size_t c = 0; c < activeChildren.size(); ++c) {
					childrenStr += std::to_string(activeChildren[c]);
					if (c < activeChildren.size() - 1)
						childrenStr += ", ";
				}
				childrenStr += "]";
			}
		}
		nodesJson[nIdx] = std::format(R"(    {{ "name": "{}", "matrix": {}{} }})", jointId,
									  matrixStr, childrenStr);
	}

	for (const auto* nodePtr : nodesToEmit) {
		const auto& node = *nodePtr;
		int nIdx = nodeIdToGlbIndex[node.id];

		const float* matrixData = node.worldMatrix;
		if (!node.parentId.empty()) {
			auto pIt = nodeIdToGlbIndex.find(node.parentId);
			if (pIt != nodeIdToGlbIndex.end())
				matrixData = node.localMatrix;
		}

		std::string matrixStr = "[";
		for (int i = 0; i < 16; ++i) {
			matrixStr += std::to_string(matrixData[i]);
			if (i < 15)
				matrixStr += ", ";
		}
		matrixStr += "]";

		std::string meshStr, skinStr, extStr, weightsStr;
		if (!node.meshId.empty() && node.visible) {
			auto it = meshIdToGlbIndex.find(node.meshId);
			if (it != meshIdToGlbIndex.end()) {
				meshStr = std::format(",\n      \"mesh\": {}", it->second);

				auto mIt = std::ranges::find_if(manifest.meshes,
												[&](const auto& m) { return m.id == node.meshId; });
				if (mIt != manifest.meshes.end() && !mIt->morphTargets.empty()) {
					weightsStr = ",\n      \"weights\": [";
					for (size_t w = 0; w < mIt->morphTargets.size(); ++w) {
						weightsStr +=
							"0.0" + std::string(w < mIt->morphTargets.size() - 1 ? ", " : "");
					}
					weightsStr += "]";
				}
			}
		}
		if (!node.skinId.empty()) {
			auto sit = skinIdToGlbIndex.find(node.skinId);
			if (sit != skinIdToGlbIndex.end())
				skinStr = std::format(",\n      \"skin\": {}", sit->second);
		}
		if (!node.lightId.empty()) {
			auto lit = lightIdToGlbIndex.find(node.lightId);
			if (lit != lightIdToGlbIndex.end())
				extStr =
					std::format(R"(, "extensions": {{ "KHR_lights_punctual": {{ "light": {} }} }})",
								lit->second);
		}

		std::string childrenStr;
		auto cIt = nodeChildren.find(node.id);
		if (cIt != nodeChildren.end() && !cIt->second.empty()) {
			std::vector<int> activeChildren;
			for (const auto& childId : cIt->second) {
				auto childIt = nodeIdToGlbIndex.find(childId);
				if (childIt != nodeIdToGlbIndex.end())
					activeChildren.push_back(childIt->second);
			}
			if (!activeChildren.empty()) {
				childrenStr = ",\n      \"children\": [";
				for (size_t c = 0; c < activeChildren.size(); ++c) {
					childrenStr += std::to_string(activeChildren[c]);
					if (c < activeChildren.size() - 1)
						childrenStr += ", ";
				}
				childrenStr += "]";
			}
		}
		nodesJson[nIdx] = std::format(R"(    {{ "name": "{}", "matrix": {}{}{}{}{}{} }})", node.id,
									  matrixStr, meshStr, skinStr, extStr, childrenStr, weightsStr);
	}

	for (const auto& anim : manifest.animations) {
		std::vector<std::string> channelsJson;
		std::vector<std::string> samplersJson;

		for (size_t sIdx = 0; sIdx < anim.samplers.size(); ++sIdx) {
			const auto& s = anim.samplers[sIdx];
			std::string fullBinPath = levelFolder + "/" + s.binFile;

			FILE* abf = std::fopen(fullBinPath.c_str(), "rb");
			if (!abf)
				continue;

			std::vector<uint8_t> inputBytes(s.inputLength);
			std::fseek(abf, s.inputOffset, SEEK_SET);
			std::fread(inputBytes.data(), 1, s.inputLength, abf);

			std::vector<uint8_t> outputBytes(s.outputLength);
			std::fseek(abf, s.outputOffset, SEEK_SET);
			std::fread(outputBytes.data(), 1, s.outputLength, abf);
			std::fclose(abf);

			while (binBuffer.size() % 4 != 0)
				binBuffer.push_back(0);
			uint32_t glbInOffset = static_cast<uint32_t>(binBuffer.size());
			binBuffer.insert(binBuffer.end(), inputBytes.begin(), inputBytes.end());

			while (binBuffer.size() % 4 != 0)
				binBuffer.push_back(0);
			uint32_t glbOutOffset = static_cast<uint32_t>(binBuffer.size());
			binBuffer.insert(binBuffer.end(), outputBytes.begin(), outputBytes.end());

			bufferViews.push_back(
				std::format(R"(    {{ "buffer": 0, "byteOffset": {}, "byteLength": {} }})",
							glbInOffset, s.inputLength));
			uint32_t inBViewIdx = bViewIndex++;

			bufferViews.push_back(
				std::format(R"(    {{ "buffer": 0, "byteOffset": {}, "byteLength": {} }})",
							glbOutOffset, s.outputLength));
			uint32_t outBViewIdx = bViewIndex++;

			uint32_t keyCount = s.inputLength / sizeof(float);
			std::string outputType = "VEC3";
			for (const auto& chan : anim.channels) {
				if (chan.samplerId == sIdx) {
					if (chan.targetPath == "rotation")
						outputType = "VEC4";
					else if (chan.targetPath == "weights")
						outputType = "SCALAR";
					break;
				}
			}

			float minTime = 0.0f;
			float maxTime = anim.duration;
			if (keyCount > 0 && inputBytes.size() >= sizeof(float)) {
				std::memcpy(&minTime, inputBytes.data(), sizeof(float));
				if (inputBytes.size() >= keyCount * sizeof(float)) {
					std::memcpy(&maxTime, inputBytes.data() + (keyCount - 1) * sizeof(float),
								sizeof(float));
				}
			}

			uint32_t inAccIdx = accIndex++;
			accessors.push_back(std::format(
				R"(    {{"bufferView": {}, "componentType": 5126, "count": {}, "type": "SCALAR", "min": [{}], "max": [{}]}})",
				inBViewIdx, keyCount, minTime, maxTime));

			uint32_t outAccIdx = accIndex++;
			accessors.push_back(std::format(
				R"(    {{"bufferView": {}, "componentType": 5126, "count": {}, "type": "{}"}})",
				outBViewIdx, keyCount, outputType));

			samplersJson.push_back(std::format(
				R"(        {{ "input": {}, "interpolation": "{}", "output": {} }})", inAccIdx,
				s.interpolation.empty() ? "LINEAR" : s.interpolation, outAccIdx));
		}

		for (const auto& chan : anim.channels) {
			auto nIt = nodeIdToGlbIndex.find(chan.targetNodeId);
			if (nIt == nodeIdToGlbIndex.end())
				continue;
			channelsJson.push_back(std::format(
				R"(        {{ "sampler": {}, "target": {{ "node": {}, "path": "{}" }} }})",
				chan.samplerId, nIt->second, chan.targetPath));
		}

		if (!channelsJson.empty() && !samplersJson.empty()) {
			std::string chansArr;
			for (size_t i = 0; i < channelsJson.size(); ++i)
				chansArr += channelsJson[i] + (i < channelsJson.size() - 1 ? ",\n" : "");
			std::string sampsArr;
			for (size_t i = 0; i < samplersJson.size(); ++i)
				sampsArr += samplersJson[i] + (i < samplersJson.size() - 1 ? ",\n" : "");

			glbAnimsJson.push_back(
				std::format(R"(    {{ "name": "{}", "channels": [ {} ], "samplers": [ {} ] }})",
							anim.name, chansArr, sampsArr));
		}
	}

	std::vector<int> rootNodeIndices;
	for (const auto& node : manifest.nodes) {
		if (node.parentId.empty() || !nodeIdToGlbIndex.contains(node.parentId)) {
			auto it = nodeIdToGlbIndex.find(node.id);
			if (it != nodeIdToGlbIndex.end())
				rootNodeIndices.push_back(it->second);
		}
	}
	for (const auto& skin : manifest.skins) {
		for (size_t i = 0; i < skin.joints.size(); ++i) {
			if (i < skin.parents.size()) {
				std::string parentId = skin.parents[i];
				if (parentId.empty() || !nodeIdToGlbIndex.contains(parentId)) {
					auto it = nodeIdToGlbIndex.find(skin.joints[i]);
					if (it != nodeIdToGlbIndex.end()) {
						int idx = it->second;
						if (std::ranges::find(rootNodeIndices, idx) == rootNodeIndices.end()) {
							bool isChild = false;
							for (const auto& pair : nodeChildren) {
								if (nodeIdToGlbIndex.contains(pair.first)) {
									if (std::ranges::find(pair.second, skin.joints[i]) !=
										pair.second.end()) {
										isChild = true;
										break;
									}
								}
							}
							if (!isChild)
								rootNodeIndices.push_back(idx);
						}
					}
				}
			}
		}
	}

	std::vector<std::string> usedExts;
	if (!manifest.lights.empty())
		usedExts.emplace_back("\"KHR_lights_punctual\"");
	bool usesEmissiveStrength = false;
	for (const auto& mat : manifest.materials) {
		bool hasEmissive = (mat.emissiveStrength > 0.f) &&
						   ((!mat.emissiveMap.empty()) ||
							(mat.emissiveFactor[0] > 0.f || mat.emissiveFactor[1] > 0.f ||
							 mat.emissiveFactor[2] > 0.f));
		if (hasEmissive && mat.emissiveStrength > 1.f) {
			usesEmissiveStrength = true;
			break;
		}
	}
	if (usesEmissiveStrength)
		usedExts.emplace_back("\"KHR_materials_emissive_strength\"");

	usedExts.emplace_back("\"ZHLN_procedural_shader\"");

	std::string extensionsUsed;
	for (size_t i = 0; i < usedExts.size(); ++i)
		extensionsUsed += usedExts[i] + (i < usedExts.size() - 1 ? ", " : "");
	std::string rootExtensions;
	if (!manifest.lights.empty()) {
		std::string lightsArr;
		for (size_t i = 0; i < manifest.lights.size(); ++i) {
			const auto& l = manifest.lights[i];
			lightsArr += std::format(
				R"(        {{ "name": "{}", "type": "{}", "color": [{}, {}, {}], "intensity": {} }})",
				l.id, l.type, l.color[0], l.color[1], l.color[2], l.intensity);
			if (i < manifest.lights.size() - 1)
				lightsArr += ",\n";
		}
		rootExtensions += std::format(
			R"(  "extensions": {{ "KHR_lights_punctual": {{ "lights": [
{}
      ] }} }},
)",
			lightsArr);
	}

	std::string json;
	json.reserve(static_cast<size_t>(128 * 1024));
	json.append(R"({
  "asset": {"version": "2.0", "generator": "Zahlen GLB Emitter"},
)");

	if (!extensionsUsed.empty()) {
		json.append(std::format(R"(  "extensionsUsed": [{}],
)",
								extensionsUsed));
	}
	if (!rootExtensions.empty()) {
		json.append(rootExtensions);
	}

	json.append(R"(  "bufferViews": [
)");
	for (size_t i = 0; i < bufferViews.size(); ++i) {
		json.append(bufferViews[i]);
		json.append(i < bufferViews.size() - 1 ? ",\n" : "\n");
	}
	json.append(R"(  ],
  "accessors": [
)");
	for (size_t i = 0; i < accessors.size(); ++i) {
		json.append(accessors[i]);
		json.append(i < accessors.size() - 1 ? ",\n" : "\n");
	}
	json.append(R"(  ],
)");

	if (!materialsJson.empty()) {
		json.append(R"(  "materials": [
)");
		for (size_t i = 0; i < materialsJson.size(); ++i) {
			json.append(materialsJson[i]);
			json.append(i < materialsJson.size() - 1 ? ",\n" : "\n");
		}
		json.append(R"(  ],
)");
	}

	if (!textures.empty()) {
		json.append(R"(  "textures": [
)");
		for (size_t i = 0; i < textures.size(); ++i) {
			json.append(textures[i]);
			json.append(i < textures.size() - 1 ? ",\n" : "\n");
		}
		json.append(R"(  ],
)");
	}

	if (!images.empty()) {
		json.append(R"(  "images": [
)");
		for (size_t i = 0; i < images.size(); ++i) {
			json.append(images[i]);
			json.append(i < images.size() - 1 ? ",\n" : "\n");
		}
		json.append(R"(  ],
  "samplers": [
    {"magFilter": 9729, "minFilter": 9729, "wrapS": 10497, "wrapT": 10497}
  ],
)");
	}

	if (!meshesJson.empty()) {
		json.append(R"(  "meshes": [
)");
		for (size_t i = 0; i < meshesJson.size(); ++i) {
			json.append(meshesJson[i]);
			json.append(i < meshesJson.size() - 1 ? ",\n" : "\n");
		}
		json.append(R"(  ],
)");
	}

	if (!skinsJson.empty()) {
		json.append(R"(  "skins": [
)");
		for (size_t i = 0; i < skinsJson.size(); ++i) {
			json.append(skinsJson[i]);
			json.append(i < skinsJson.size() - 1 ? ",\n" : "\n");
		}
		json.append(R"(  ],
)");
	}

	if (!glbAnimsJson.empty()) {
		json.append(R"(  "animations": [
)");
		for (size_t i = 0; i < glbAnimsJson.size(); ++i) {
			json.append(glbAnimsJson[i]);
			json.append(i < glbAnimsJson.size() - 1 ? ",\n" : "\n");
		}
		json.append(R"(  ],
)");
	}

	json.append(R"(  "nodes": [
)");
	for (size_t i = 0; i < nodesJson.size(); ++i) {
		json.append(nodesJson[i]);
		json.append(i < nodesJson.size() - 1 ? ",\n" : "\n");
	}
	json.append(R"(  ],
  "scenes": [
    {
      "nodes": [)");
	for (size_t i = 0; i < rootNodeIndices.size(); ++i) {
		json.append(std::to_string(rootNodeIndices[i]));
		if (i < rootNodeIndices.size() - 1) {
			json.append(",");
		}
	}
	json.append(R"(]
    }
  ],
  "scene": 0,
)");

	json.append(std::format(R"(  "buffers": [
    {{
      "byteLength": {}
    }}
  ]
}})",
							binBuffer.size()));

	while (json.length() % 4 != 0)
		json += ' ';

	auto jsonChunkLength = static_cast<uint32_t>(json.length());
	auto binChunkLength = static_cast<uint32_t>(binBuffer.size());
	uint32_t totalFileLength = 12 + 8 + jsonChunkLength + 8 + binChunkLength;

	FILE* out = std::fopen(outputPath.c_str(), "wb");
	if (out == nullptr)
		return false;

	uint32_t magic = 0x46546C67, version = 2;
	std::fwrite(&magic, 1, 4, out);
	std::fwrite(&version, 1, 4, out);
	std::fwrite(&totalFileLength, 1, 4, out);

	uint32_t chunkTypeJson = 0x4E4F534A;
	std::fwrite(&jsonChunkLength, 1, 4, out);
	std::fwrite(&chunkTypeJson, 1, 4, out);
	std::fwrite(json.data(), 1, jsonChunkLength, out);

	uint32_t chunkTypeBin = 0x004E4942;
	std::fwrite(&binChunkLength, 1, 4, out);
	std::fwrite(&chunkTypeBin, 1, 4, out);
	std::fwrite(binBuffer.data(), 1, binChunkLength, out);

	std::fclose(out);
	return true;
}

} // namespace GLB
} // namespace ZHLN
