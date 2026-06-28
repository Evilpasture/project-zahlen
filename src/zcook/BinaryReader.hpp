// src/zcook/BinaryReader.hpp
// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "IR.hpp"

#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace Compiler {

class BinaryReader {
  public:
	explicit BinaryReader(const std::string& filepath) : m_stream(filepath, std::ios::binary) {
		if (!m_stream.is_open()) {
			throw std::runtime_error("Failed to open binary metadata file: " + filepath);
		}
	}

	std::string ReadString() {
		uint32_t len = 0;
		m_stream.read(reinterpret_cast<char*>(&len), sizeof(len));
		if (len == 0)
			return "";
		std::string s(len, '\0');
		m_stream.read(s.data(), len);
		return s;
	}

	template <typename T> T Read() {
		T value;
		m_stream.read(reinterpret_cast<char*>(&value), sizeof(T));
		return value;
	}

	void ReadFloats(float* dest, size_t count) {
		if (count > 0) {
			m_stream.read(reinterpret_cast<char*>(dest), count * sizeof(float));
		}
	}

	std::vector<float> ReadFloatVector() {
		uint32_t count = Read<uint32_t>();
		std::vector<float> vec(count);
		if (count > 0) {
			ReadFloats(vec.data(), count);
		}
		return vec;
	}

	IRManifest Parse() {
		// Validate Header Magic
		char magic[4];
		m_stream.read(magic, 4);
		if (std::string(magic, 4) != "ZMET") {
			throw std::runtime_error("Invalid file magic. Expected 'ZMET'");
		}

		uint32_t version = Read<uint32_t>();
		if (version != 1) {
			throw std::runtime_error("Unsupported metadata version!");
		}

		IRManifest manifest;
		manifest.levelName = ReadString();

		// 1. Materials
		uint32_t matCount = Read<uint32_t>();
		manifest.materials.reserve(matCount);
		for (uint32_t i = 0; i < matCount; ++i) {
			IRMaterial mat;
			mat.id = ReadString();
			ReadFloats(mat.baseColor, 4);
			mat.metallic = Read<float>();
			mat.roughness = Read<float>();
			ReadFloats(mat.emissiveFactor, 3);
			mat.emissiveStrength = Read<float>();
			mat.doubleSided = (Read<uint8_t>() != 0);
			mat.albedoMap = ReadString();
			mat.normalMap = ReadString();
			mat.metallicRoughnessMap = ReadString();
			mat.emissiveMap = ReadString();
			uint8_t hasProcedural = Read<uint8_t>();
			if (hasProcedural != 0) {
				mat.procedural.active = true;
				mat.procedural.type = ReadString();

				uint32_t paramCount = Read<uint32_t>();
				mat.procedural.parameters.resize(paramCount);
				for (uint32_t p = 0; p < paramCount; ++p) {
					mat.procedural.parameters[p].name = ReadString();
					uint8_t floatCount = Read<uint8_t>();
					mat.procedural.parameters[p].values.resize(floatCount);
					for (uint8_t f = 0; f < floatCount; ++f) {
						mat.procedural.parameters[p].values[f] = Read<float>();
					}
				}
			}
			manifest.materials.push_back(mat);
		}

		// 2. Meshes
		uint32_t meshCount = Read<uint32_t>();
		manifest.meshes.reserve(meshCount);
		for (uint32_t i = 0; i < meshCount; ++i) {
			IRMesh mesh;
			mesh.id = ReadString();
			mesh.layout = ReadString();
			mesh.binFile = ReadString();
			mesh.vertexBuffer.byteOffset = Read<uint32_t>();
			mesh.vertexBuffer.byteLength = Read<uint32_t>();

			// Primitives
			uint32_t primCount = Read<uint32_t>();
			mesh.primitives.reserve(primCount);
			for (uint32_t p = 0; p < primCount; ++p) {
				IRPrimitive prim;
				prim.materialId = ReadString();
				prim.vertexOffset = Read<uint32_t>();
				prim.vertexCount = Read<uint32_t>();
				mesh.primitives.push_back(prim);
			}

			// Morph Targets
			uint32_t morphCount = Read<uint32_t>();
			mesh.morphTargets.reserve(morphCount);
			for (uint32_t m = 0; m < morphCount; ++m) {
				IRMorphTarget target;
				target.name = ReadString();
				target.binFile = ReadString();
				target.byteOffset = Read<uint32_t>();
				target.byteLength = Read<uint32_t>();
				mesh.morphTargets.push_back(target);
			}
			manifest.meshes.push_back(mesh);
		}

		// 3. Nodes
		uint32_t nodeCount = Read<uint32_t>();
		manifest.nodes.reserve(nodeCount);
		for (uint32_t i = 0; i < nodeCount; ++i) {
			IRNode node;
			node.id = ReadString();
			node.parentId = ReadString();
			node.visible = (Read<uint8_t>() != 0);
			ReadFloats(node.localMatrix, 16);
			ReadFloats(node.worldMatrix, 16);
			node.meshId = ReadString();
			node.skinId = ReadString();
			node.lightId = ReadString();
			manifest.nodes.push_back(node);
		}

		// 4. Lights
		uint32_t lightCount = Read<uint32_t>();
		manifest.lights.reserve(lightCount);
		for (uint32_t i = 0; i < lightCount; ++i) {
			IRLight light;
			light.id = ReadString();
			light.type = ReadString();
			ReadFloats(light.color, 3);
			light.intensity = Read<float>();
			manifest.lights.push_back(light);
		}

		// 5. Skins
		uint32_t skinCount = Read<uint32_t>();
		manifest.skins.reserve(skinCount);
		for (uint32_t i = 0; i < skinCount; ++i) {
			IRSkin skin;
			skin.id = ReadString();
			skin.name = ReadString();

			// Joints
			uint32_t jointsCount = Read<uint32_t>();
			skin.joints.reserve(jointsCount);
			for (uint32_t j = 0; j < jointsCount; ++j) {
				skin.joints.push_back(ReadString());
			}

			// Parents
			uint32_t parentsCount = Read<uint32_t>();
			skin.parents.reserve(parentsCount);
			for (uint32_t p = 0; p < parentsCount; ++p) {
				skin.parents.push_back(ReadString());
			}

			skin.inverseBindMatrices = ReadFloatVector();
			skin.restPose = ReadFloatVector();
			manifest.skins.push_back(skin);
		}

		// 6. Animations
		uint32_t animCount = Read<uint32_t>();
		manifest.animations.reserve(animCount);
		for (uint32_t i = 0; i < animCount; ++i) {
			IRAnimation anim;
			anim.id = ReadString();
			anim.name = ReadString();
			anim.duration = Read<float>();
			anim.loop = (Read<uint8_t>() != 0);

			// Channels
			uint32_t chanCount = Read<uint32_t>();
			anim.channels.reserve(chanCount);
			for (uint32_t c = 0; c < chanCount; ++c) {
				IRAnimationChannel chan;
				chan.targetNodeId = ReadString();
				chan.targetPath = ReadString();
				chan.samplerId = Read<uint32_t>();
				anim.channels.push_back(chan);
			}

			// Samplers
			uint32_t sampCount = Read<uint32_t>();
			anim.samplers.reserve(sampCount);
			for (uint32_t s = 0; s < sampCount; ++s) {
				IRAnimationSampler samp;
				samp.interpolation = ReadString();
				samp.inputOffset = Read<uint32_t>();
				samp.inputLength = Read<uint32_t>();
				samp.outputOffset = Read<uint32_t>();
				samp.outputLength = Read<uint32_t>();
				samp.binFile = ReadString();
				anim.samplers.push_back(samp);
			}
			manifest.animations.push_back(anim);
		}

		return manifest;
	}

  private:
	std::ifstream m_stream;
};

} // namespace Compiler
