// src/zcook/GLB.cpp
// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "GLB.hpp"

#include "Transform.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <format>
#include <print>
#include <unordered_map>
#include <vector>

namespace GLB {

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
		if (relativeUri.empty())
			return -1;

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

		if (albedoTex != -1)
			pbrStr += std::format(R"(,
      "baseColorTexture": {{"index": {}}})",
								  albedoTex);

		std::string matStr = std::format(R"(    {{
      "name": "{}",
      "pbrMetallicRoughness": {{
  {}
      }})",
										 mat.id, pbrStr);

		if (mat.doubleSided)
			matStr += R"(,
      "doubleSided": true)";

		if (normalTex != -1)
			matStr += std::format(R"(,
      "normalTexture": {{"index": {}}})",
								  normalTex);
		if (mrTex != -1)
			matStr += std::format(R"(,
      "metallicRoughnessTexture": {{"index": {}}})",
								  mrTex);

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
			if (emissiveTex != -1)
				matStr += std::format(R"(,
      "emissiveTexture": {{"index": {}}})",
									  emissiveTex);
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
		if (compiled.glbVertices.empty())
			continue;

		meshIdToGlbIndex[mesh.id] = static_cast<int>(meshesJson.size());

		auto vboOffset = static_cast<uint32_t>(binBuffer.size());
		size_t vboBytes = compiled.glbVertices.size() * sizeof(float);
		if (vboBytes > 0)
			binBuffer.insert(binBuffer.end(),
							 reinterpret_cast<uint8_t*>(compiled.glbVertices.data()),
							 reinterpret_cast<uint8_t*>(compiled.glbVertices.data()) + vboBytes);
		while (binBuffer.size() % 4 != 0)
			binBuffer.push_back(0);

		bufferViews.push_back(std::format(R"(    {{
      "buffer": 0,
      "byteOffset": {},
      "byteLength": {},
      "byteStride": 64,
      "target": 34962
    }})",
										  vboOffset, vboBytes));
		uint32_t vboBViewIdx = bViewIndex++;

		auto iboOffset = static_cast<uint32_t>(binBuffer.size());
		size_t iboBytes = compiled.indices.size() * sizeof(uint32_t);
		if (iboBytes > 0)
			binBuffer.insert(binBuffer.end(), reinterpret_cast<uint8_t*>(compiled.indices.data()),
							 reinterpret_cast<uint8_t*>(compiled.indices.data()) + iboBytes);
		while (binBuffer.size() % 4 != 0)
			binBuffer.push_back(0);

		bufferViews.push_back(std::format(R"(    {{
      "buffer": 0,
      "byteOffset": {},
      "byteLength": {},
      "target": 34963
    }})",
										  iboOffset, iboBytes));
		uint32_t iboBViewIdx = bViewIndex++;

		auto vertexCount = static_cast<uint32_t>(compiled.glbVertices.size() / 16);
		uint32_t posAcc = accIndex++;
		uint32_t normAcc = accIndex++;
		uint32_t tangAcc = accIndex++;
		uint32_t uvAcc = accIndex++;
		uint32_t colorAcc = accIndex++;

		accessors.push_back(std::format(
			R"(    {{"bufferView": {}, "componentType": 5126, "count": {}, "type": "VEC3", "min": [{}, {}, {}], "max": [{}, {}, {}]}})",
			vboBViewIdx, vertexCount, compiled.minB[0], compiled.minB[1], compiled.minB[2],
			compiled.maxB[0], compiled.maxB[1], compiled.maxB[2]));
		accessors.push_back(std::format(
			R"(    {{"bufferView": {}, "byteOffset": 12, "componentType": 5126, "count": {}, "type": "VEC3"}})",
			vboBViewIdx, vertexCount));
		accessors.push_back(std::format(
			R"(    {{"bufferView": {}, "byteOffset": 24, "componentType": 5126, "count": {}, "type": "VEC4"}})",
			vboBViewIdx, vertexCount));
		accessors.push_back(std::format(
			R"(    {{"bufferView": {}, "byteOffset": 40, "componentType": 5126, "count": {}, "type": "VEC2"}})",
			vboBViewIdx, vertexCount));
		accessors.push_back(std::format(
			R"(    {{"bufferView": {}, "byteOffset": 48, "componentType": 5126, "count": {}, "type": "VEC4"}})",
			vboBViewIdx, vertexCount));

		uint32_t jointsAcc = 0;
		uint32_t weightsAcc = 0;

		if (compiled.isSkinned) {
			while (binBuffer.size() % 4 != 0)
				binBuffer.push_back(0);
			auto jboOffset = static_cast<uint32_t>(binBuffer.size());
			size_t jboBytes = compiled.joints.size() * sizeof(uint16_t);
			if (jboBytes > 0)
				binBuffer.insert(binBuffer.end(),
								 reinterpret_cast<uint8_t*>(compiled.joints.data()),
								 reinterpret_cast<uint8_t*>(compiled.joints.data()) + jboBytes);
			while (binBuffer.size() % 4 != 0)
				binBuffer.push_back(0);

			bufferViews.push_back(std::format(
				R"(    {{ "buffer": 0, "byteOffset": {}, "byteLength": {}, "target": 34962 }})",
				jboOffset, jboBytes));
			uint32_t jboBViewIdx = bViewIndex++;

			jointsAcc = accIndex++;
			accessors.push_back(std::format(
				R"(    {{"bufferView": {}, "componentType": 5123, "count": {}, "type": "VEC4"}})",
				jboBViewIdx, vertexCount));

			while (binBuffer.size() % 4 != 0)
				binBuffer.push_back(0);
			auto wboOffset = static_cast<uint32_t>(binBuffer.size());
			size_t wboBytes = compiled.weights.size() * sizeof(float);
			if (wboBytes > 0)
				binBuffer.insert(binBuffer.end(),
								 reinterpret_cast<uint8_t*>(compiled.weights.data()),
								 reinterpret_cast<uint8_t*>(compiled.weights.data()) + wboBytes);
			while (binBuffer.size() % 4 != 0)
				binBuffer.push_back(0);

			bufferViews.push_back(std::format(
				R"(    {{ "buffer": 0, "byteOffset": {}, "byteLength": {}, "target": 34962 }})",
				wboOffset, wboBytes));
			uint32_t wboBViewIdx = bViewIndex++;

			weightsAcc = accIndex++;
			accessors.push_back(std::format(
				R"(    {{"bufferView": {}, "componentType": 5126, "count": {}, "type": "VEC4"}})",
				wboBViewIdx, vertexCount));
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
			if (it != matIdToGlbIndex.end())
				matGlbIdx = it->second;

			std::string matStr;
			if (matGlbIdx != -1)
				matStr = std::format(R"(, "material": {})", matGlbIdx);

			std::string skinAttribs;
			if (compiled.isSkinned)
				skinAttribs =
					std::format(R"(, "JOINTS_0": {}, "WEIGHTS_0": {})", jointsAcc, weightsAcc);

			primsStr += std::format(R"(        {{
          "attributes": {{ "POSITION": {}, "NORMAL": {}, "TANGENT": {}, "TEXCOORD_0": {}, "COLOR_0": {} {} }},
          "indices": {} {}
        }})",
									posAcc, normAcc, tangAcc, uvAcc, colorAcc, skinAttribs,
									indexAcc, matStr);
			if (p < compiled.primitives.size() - 1)
				primsStr += ",\n";
		}

		meshesJson.push_back(
			std::format(R"(    {{ "name": "{}", "primitives": [ {} ] }})", mesh.id, primsStr));
	}

	for (size_t i = 0; i < manifest.lights.size(); ++i)
		lightIdToGlbIndex[manifest.lights[i].id] = static_cast<int>(i);

	std::unordered_map<std::string, std::vector<std::string>> nodeChildren;
	for (const auto& node : manifest.nodes) {
		if (!node.parentId.empty())
			nodeChildren[node.parentId].push_back(node.id);
	}
	for (const auto& skin : manifest.skins) {
		for (size_t i = 0; i < skin.joints.size(); ++i) {
			if (i < skin.parents.size() && !skin.parents[i].empty())
				nodeChildren[skin.parents[i]].push_back(skin.joints[i]);
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
			if (isTargetedByAnim)
				break;
		}
		if (!node.visible && node.meshId.empty() && node.lightId.empty() && !isTargetedByAnim)
			continue;
		nodeIdToGlbIndex[node.id] = glbNodeIdx++;
		nodesToEmit.push_back(&node);
	}

	std::vector<std::pair<std::string, std::string>> jointsToEmit;
	for (const auto& skin : manifest.skins) {
		for (size_t i = 0; i < skin.joints.size(); ++i) {
			const auto& jointId = skin.joints[i];
			if (nodeIdToGlbIndex.contains(jointId))
				continue;

			std::string matrixStr = "[";
			for (int m = 0; m < 16; ++m) {
				float val = (i * 16 + m < skin.restPose.size())
								? skin.restPose[i * 16 + m]
								: (m == 0 || m == 5 || m == 10 || m == 15 ? 1.0f : 0.0f);
				matrixStr += std::to_string(val);
				if (m < 15)
					matrixStr += ", ";
			}
			matrixStr += "]";
			nodeIdToGlbIndex[jointId] = glbNodeIdx++;
			jointsToEmit.push_back({jointId, matrixStr});
		}
	}

	nodesJson.resize(glbNodeIdx);

	for (const auto& skin : manifest.skins) {
		while (binBuffer.size() % 4 != 0)
			binBuffer.push_back(0);
		auto ibmOffset = static_cast<uint32_t>(binBuffer.size());
		size_t ibmBytes = skin.inverseBindMatrices.size() * sizeof(float);
		if (ibmBytes > 0)
			binBuffer.insert(
				binBuffer.end(), reinterpret_cast<const uint8_t*>(skin.inverseBindMatrices.data()),
				reinterpret_cast<const uint8_t*>(skin.inverseBindMatrices.data()) + ibmBytes);

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

		std::string meshStr, skinStr, extStr;
		if (!node.meshId.empty()) {
			auto it = meshIdToGlbIndex.find(node.meshId);
			if (it != meshIdToGlbIndex.end())
				meshStr = std::format(",\n      \"mesh\": {}", it->second);
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
		nodesJson[nIdx] = std::format(R"(    {{ "name": "{}", "matrix": {}{}{}{}{} }})", node.id,
									  matrixStr, meshStr, skinStr, extStr, childrenStr);
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
