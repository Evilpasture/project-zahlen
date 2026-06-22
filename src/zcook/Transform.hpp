// src/zcook/Transform.hpp
// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "IR.hpp"

#include <cstdint>
#include <string>
#include <vector>

struct CompiledMesh {
	std::vector<float> glbVertices;
	std::vector<uint32_t> indices;
	std::vector<uint16_t> joints;
	std::vector<float> weights;
	std::vector<Compiler::IRPrimitive> primitives;
	std::vector<uint32_t> originalVertexIndices;
	float minB[3] = {1e30f, 1e30f, 1e30f};
	float maxB[3] = {-1e30f, -1e30f, -1e30f};
	bool isSkinned = false;
};

CompiledMesh CompileRawMesh(const Compiler::IRMesh& mesh, const std::string& binPath);
