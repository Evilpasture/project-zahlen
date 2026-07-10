// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// File: src/zcook/Transform.cpp
#include "Transform.hpp"
#include <Jolt/Jolt.h>
#include <Jolt/Math/Vec3.h>
#include <Zahlen/Math3D.hpp>
#include <Zahlen/Types.hpp>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <print>
#include <unordered_map>

namespace {

struct RawLoopVertex {
    uint32_t v_idx;
    float    px, py, pz;
    float    nx, ny, nz;
    float    u, v;
    float    r, g, b, a;
    uint16_t joints[4];
    float    weights[4];

    bool operator==(const RawLoopVertex& o) const {
        if (v_idx != o.v_idx)
            return false;
        if (u != o.u || v != o.v)
            return false;
        if (nx != o.nx || ny != o.ny || nz != o.nz)
            return false;
        if (r != o.r || g != o.g || b != o.b || a != o.a)
            return false;
        return true;
    }
};

struct RawLoopVertexHash {
    size_t operator()(const RawLoopVertex& v) const {
        size_t h       = std::hash<uint32_t> {}(v.v_idx);
        auto   combine = [&](float val) {
            uint32_t bits = 0;
            std::memcpy(&bits, &val, 4);
            h ^= bits + 0x9e3779b9 + (h << 6) + (h >> 2);
        };
        combine(v.u);
        combine(v.v);
        combine(v.nx);
        combine(v.ny);
        combine(v.nz);
        return h;
    }
};

} // namespace

namespace ZHLN {

CompiledMesh CompileRawMesh(const Compiler::IRMesh& mesh, const std::string& binPath) {
    CompiledMesh result;

    FILE* bf = std::fopen(binPath.c_str(), "rb");
    if (bf == nullptr) {
        std::println(stderr, "[zcook] ERROR: Failed to open intermediate bin file '{}': {}", binPath, std::strerror(errno));
        return result;
    }

    std::fseek(bf, 0, SEEK_END);
    long binSize = std::ftell(bf);
    std::fseek(bf, 0, SEEK_SET);
    std::vector<float> rawFloats(binSize / sizeof(float));
    if (!rawFloats.empty()) {
        std::fread(rawFloats.data(), sizeof(float), rawFloats.size(), bf);
    }
    std::fclose(bf);

    bool hasSkin     = mesh.layout.contains("_J4W4");
    result.isSkinned = hasSkin;
    size_t stride    = hasSkin ? 21 : 13;

    std::unordered_map<RawLoopVertex, uint32_t, RawLoopVertexHash> uniqueMap;
    std::vector<RawLoopVertex>                                     rawVerts;

    for (const auto& prim: mesh.primitives) {
        uint32_t startLoop = prim.vertexOffset;
        uint32_t loopCount = prim.vertexCount;

        Compiler::IRPrimitive outPrim;
        outPrim.materialId   = prim.materialId;
        outPrim.vertexOffset = static_cast<uint32_t>(result.indices.size() * sizeof(uint32_t));
        outPrim.vertexCount  = loopCount;

        for (uint32_t i = 0; i < loopCount; ++i) {
            size_t offset = (startLoop + i) * stride;
            if (offset + stride > rawFloats.size())
                break;

            RawLoopVertex rv {};
            rv.v_idx = static_cast<uint32_t>(rawFloats[offset + 0]);
            rv.px    = rawFloats[offset + 1];
            rv.py    = rawFloats[offset + 2];
            rv.pz    = rawFloats[offset + 3];
            rv.nx    = rawFloats[offset + 4];
            rv.ny    = rawFloats[offset + 5];
            rv.nz    = rawFloats[offset + 6];
            rv.u     = rawFloats[offset + 7];
            rv.v     = rawFloats[offset + 8];
            rv.r     = rawFloats[offset + 9];
            rv.g     = rawFloats[offset + 10];
            rv.b     = rawFloats[offset + 11];
            rv.a     = rawFloats[offset + 12];

            if (hasSkin) {
                rv.joints[0]  = static_cast<uint16_t>(rawFloats[offset + 13]);
                rv.joints[1]  = static_cast<uint16_t>(rawFloats[offset + 14]);
                rv.joints[2]  = static_cast<uint16_t>(rawFloats[offset + 15]);
                rv.joints[3]  = static_cast<uint16_t>(rawFloats[offset + 16]);
                rv.weights[0] = rawFloats[offset + 17];
                rv.weights[1] = rawFloats[offset + 18];
                rv.weights[2] = rawFloats[offset + 19];
                rv.weights[3] = rawFloats[offset + 20];
            } else {
                rv.joints[0] = rv.joints[1] = rv.joints[2] = rv.joints[3] = 0;
                rv.weights[0] = rv.weights[1] = rv.weights[2] = rv.weights[3] = 0.0f;
            }

            auto it = uniqueMap.find(rv);
            if (it != uniqueMap.end()) {
                result.indices.push_back(it->second);
            } else {
                auto newIdx   = static_cast<uint32_t>(rawVerts.size());
                uniqueMap[rv] = newIdx;
                result.indices.push_back(newIdx);
                rawVerts.push_back(rv);
                result.originalVertexIndices.push_back(rv.v_idx);
            }
        }
        result.primitives.push_back(outPrim);
    }

    std::vector<JPH::Vec3> tangents(rawVerts.size(), JPH::Vec3::sZero());
    std::vector<JPH::Vec3> bitangents(rawVerts.size(), JPH::Vec3::sZero());

    for (size_t i = 0; i + 2 < result.indices.size(); i += 3) {
        uint32_t i0 = result.indices[i + 0];
        uint32_t i1 = result.indices[i + 1];
        uint32_t i2 = result.indices[i + 2];

        if (i0 >= rawVerts.size() || i1 >= rawVerts.size() || i2 >= rawVerts.size())
            continue;

        auto& v0 = rawVerts[i0];
        auto& v1 = rawVerts[i1];
        auto& v2 = rawVerts[i2];

        JPH::Vec3 p0(v0.px, v0.py, v0.pz);
        JPH::Vec3 p1(v1.px, v1.py, v1.pz);
        JPH::Vec3 p2(v2.px, v2.py, v2.pz);

        JPH::Vec3 e1 = p1 - p0;
        JPH::Vec3 e2 = p2 - p0;

        float du1 = v1.u - v0.u;
        float dv1 = v1.v - v0.v;
        float du2 = v2.u - v0.u;
        float dv2 = v2.v - v0.v;

        float det = du1 * dv2 - dv1 * du2;
        float r   = (det != 0.0f) ? 1.0f / det : 0.0f;

        JPH::Vec3 t = (e1 * dv2 - e2 * dv1) * r;
        JPH::Vec3 b = (e2 * du1 - e1 * du2) * r;

        tangents[i0] += t;
        tangents[i1] += t;
        tangents[i2] += t;
        bitangents[i0] += b;
        bitangents[i1] += b;
        bitangents[i2] += b;
    }

    result.positions.resize(rawVerts.size());
    result.attributes.resize(rawVerts.size());
    if (hasSkin) {
        result.skins.resize(rawVerts.size());
    }

    for (size_t i = 0; i < rawVerts.size(); ++i) {
        auto& rv = rawVerts[i];

        // 1. Pack Positions
        result.positions[i] = {.position = {rv.px, rv.py, rv.pz}};

        // 2. Compute and Pack Tangents and Attributes
        JPH::Vec3 n(rv.nx, rv.ny, rv.nz);
        JPH::Vec3 t = tangents[i];
        JPH::Vec3 tangentVec;
        float     sign = 1.0f;

        if (t.LengthSq() > 1e-6f) {
            tangentVec = (t - n * n.Dot(t)).Normalized();
            sign       = n.Cross(t).Dot(bitangents[i]) < 0.0f ? -1.0f : 1.0f;
        } else {
            JPH::Vec3 absN(std::abs(n.GetX()), std::abs(n.GetY()), std::abs(n.GetZ()));
            JPH::Vec3 fallbackT = (absN.GetX() < 0.999f) ? JPH::Vec3(1.0f, 0.0f, 0.0f) : JPH::Vec3(0.0f, 1.0f, 0.0f);
            tangentVec          = (fallbackT - n * n.Dot(fallbackT)).Normalized();
        }

        result.attributes[i] = {
            .normal  = Math::PackNormal(rv.nx, rv.ny, rv.nz),
            .tangent = Math::PackNormal(tangentVec.GetX(), tangentVec.GetY(), tangentVec.GetZ(), sign),
            .uv      = Math::PackUV(rv.u, rv.v),
            .color   = Math::PackColor(rv.r, rv.g, rv.b, rv.a)
        };

        // 3. Pack Joints and Weights (using UNORM8 compression)
        if (hasSkin) {
            result.skins[i] = {
                .joints  = {rv.joints[0], rv.joints[1], rv.joints[2], rv.joints[3]},
                .weights = Math::PackColor(rv.weights[0], rv.weights[1], rv.weights[2], rv.weights[3])
            };
        }

        // 4. Update Bounding Box
        result.minB[0] = std::min(result.minB[0], rv.px);
        result.minB[1] = std::min(result.minB[1], rv.py);
        result.minB[2] = std::min(result.minB[2], rv.pz);
        result.maxB[0] = std::max(result.maxB[0], rv.px);
        result.maxB[1] = std::max(result.maxB[1], rv.py);
        result.maxB[2] = std::max(result.maxB[2], rv.pz);
    }

    return result;
}

} // namespace ZHLN
