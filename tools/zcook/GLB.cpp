// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// File: tools/zcook/GLB.cpp
//
// Emits a cookable scene from Compiler::IRManifest as a self-contained glTF
// 2.0 binary container. The document model (GLBModel.hpp) is the schema and
// ZHLN::ReflectJSON::SerializeJSON writes it -- no JSON is built by hand.
// The PNG half of the BIN chunk is stb_image_write; the container framing
// (magic, chunk headers, padding) is the one binary shape the layer above
// must stay hand-assembled.
#include "GLB.hpp"
#include "GLBModel.hpp"
#include "Transform.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <json/JSONSchema.hpp>

namespace ZHLN::GLB {

namespace {

// ============================================================================
// Internal Bytecode Processor & Math Library
// ============================================================================

struct float2 {
    float x, y;
};
struct float3 {
    float x, y, z;
};
struct float4 {
    float x, y, z, w;
};

inline float frac(float x) {
    return x - std::floor(x);
}

inline float2 Hash22(float2 p) {
    float3 p3      = {frac(p.x * 0.1031f), frac(p.y * 0.1030f), frac(p.x * 0.0973f)};
    float  dot_val = p3.x * (p3.y + 33.33f) + p3.y * (p3.z + 33.33f) + p3.z * (p3.x + 33.33f);
    p3.x += dot_val;
    p3.y += dot_val;
    p3.z += dot_val;
    return {frac((p3.x + p3.y) * p3.z), frac((p3.x + p3.z) * p3.z)};
}

inline float3 EvaluateVoronoi(float2 uv, float randomness) {
    float2 ip = {std::floor(uv.x), std::floor(uv.y)};
    float2 fp = {frac(uv.x), frac(uv.y)};
    float  md = 8.0f;
    float2 mg, mr;

    for (int y = -1; y <= 1; y++) {
        for (int x = -1; x <= 1; x++) {
            float2 g = {float(x), float(y)};
            float2 h = Hash22({ip.x + g.x, ip.y + g.y});
            float2 o = {h.x * randomness, h.y * randomness};
            float2 r = {g.x + o.x - fp.x, g.y + o.y - fp.y};
            float  d = r.x * r.x + r.y * r.y;
            if (d < md) {
                md = d;
                mr = r;
                mg = g;
            }
        }
    }
    float2 h = Hash22({ip.x + mg.x, ip.y + mg.y});
    return {std::sqrt(md), h.x, h.y};
}

inline float HalfToFloat(uint16_t h) noexcept {
    uint32_t sign     = (h >> 15) & 0x00000001;
    uint32_t exponent = (h >> 10) & 0x0000001f;
    uint32_t mantissa = h & 0x000003ff;

    if (exponent == 0) {
        if (mantissa == 0)
            return sign ? -0.0f : 0.0f;
        return (sign ? -1.0f : 1.0f) * std::ldexp(static_cast<float>(mantissa), -24);
    } else if (exponent == 31) {
        return sign ? -INFINITY : INFINITY;
    }
    return (sign ? -1.0f : 1.0f) * std::ldexp(static_cast<float>(mantissa | 0x0400), static_cast<int>(exponent) - 15 - 10);
}

inline std::array<float, 4> UnpackNormal(uint32_t packed) noexcept {
    float x = (float(packed & 0x3FF) / 1023.0f) * 2.0f - 1.0f;
    float y = (float((packed >> 10) & 0x3FF) / 1023.0f) * 2.0f - 1.0f;
    float z = (float((packed >> 20) & 0x3FF) / 1023.0f) * 2.0f - 1.0f;
    float w = (packed >> 30) > 0 ? 1.0f : -1.0f;
    return {x, y, z, w};
}

/// stb_image_write appends through a context pointer; this is the only
/// result sink the emitter needs (BIN chunk bytes, and the baked .ztex copy).
std::vector<uint8_t> CreatePNGBytes(const std::vector<uint32_t>& rgbaPixels, uint32_t width, uint32_t height) {
    std::vector<uint8_t> pngBytes;
    auto                 append = [](void* ctx, void* data, int size) {
        auto*       out       = static_cast<std::vector<uint8_t>*>(ctx);
        const auto* bytes     = static_cast<const uint8_t*>(data);
        out->insert(out->end(), bytes, bytes + size);
    };
    if (stbi_write_png_to_func(append, &pngBytes, static_cast<int>(width), static_cast<int>(height), 4, rgbaPixels.data(), static_cast<int>(width * 4)) == 0) {
        return {};
    }
    return pngBytes;
}

} // namespace

bool EmitGLB(const Compiler::IRManifest& manifest, const std::string& levelFolder, const std::string& outputPath) {
    std::vector<uint8_t> binBuffer;
    binBuffer.reserve(static_cast<size_t>(16 * 1024 * 1024));

    GlbDocument doc;

    std::unordered_map<std::string, int> meshIdToGlbIndex;
    std::unordered_map<std::string, int> lightIdToGlbIndex;
    std::unordered_map<std::string, int> nodeIdToGlbIndex;
    std::unordered_map<std::string, int> skinIdToGlbIndex;
    std::unordered_map<std::string, int> matIdToGlbIndex;

    uint32_t accIndex   = 0;
    uint32_t bViewIndex = 0;

    // Packed images are the parallel source of the textures[] and images[]
    // arrays: texture.source is the index into both.
    struct PackedImage {
        std::string relativeUri;
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
        FILE*       f        = std::fopen(fullPath.c_str(), "rb");
        if (f == nullptr)
            return -1;

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

        std::string_view mimeType = "image/png";
        if (relativeUri.ends_with(".jpg") || relativeUri.ends_with(".jpeg"))
            mimeType = "image/jpeg";

        doc.bufferViews.push_back(GlbBufferView {.byteOffset = imgOffset, .byteLength = static_cast<uint32_t>(size)});
        uint32_t imgBViewIdx = bViewIndex++;

        int idx = static_cast<int>(packedImages.size());
        packedImages.push_back({.relativeUri = relativeUri});
        doc.textures.push_back(GlbTexture {.sampler = 0, .source = static_cast<uint32_t>(idx)});
        doc.images.push_back(GlbImage {.bufferView = imgBViewIdx, .mimeType = mimeType});

        return idx;
    };

    auto getProceduralTextureIndex = [&](const Compiler::IRMaterial& mat) -> int {
        std::string id = "procedural_" + mat.id;
        for (size_t i = 0; i < packedImages.size(); ++i) {
            if (packedImages[i].relativeUri == id)
                return static_cast<int>(i);
        }

        struct Inst {
            int                op;
            int                in0, in1, in2;
            std::vector<float> p;
        };
        std::vector<Inst> instructions;

        for (size_t i = 0; i < mat.procedural.parameters.size(); ++i) {
            for (const auto& param: mat.procedural.parameters) {
                if (param.name == "inst_" + std::to_string(i)) {
                    Inst inst;
                    inst.op = param.values[0];
                    if (inst.op == 1) {
                        inst.in0 = param.values[1];
                        inst.p.assign(param.values.begin() + 2, param.values.end());
                    } else if (inst.op == 2 || inst.op == 3) {
                        inst.in0 = param.values[1];
                        inst.p.assign(param.values.begin() + 2, param.values.end());
                    } else if (inst.op == 4) {
                        inst.in0 = param.values[1];
                        inst.in1 = param.values[2];
                        inst.in2 = param.values[3];
                        inst.p.assign(param.values.begin() + 4, param.values.end());
                    } else {
                        inst.in0 = inst.in1 = inst.in2 = -1;
                    }
                    instructions.push_back(inst);
                    break;
                }
            }
        }

        uint32_t              width = 512, height = 512;
        std::vector<uint32_t> pixels(static_cast<size_t>(width * height));

        for (uint32_t y = 0; y < height; ++y) {
            for (uint32_t x = 0; x < width; ++x) {
                float2              uv = {float(x) / width, float(y) / height};
                std::vector<float4> regs(instructions.size(), {0, 0, 0, 0});

                for (size_t i = 0; i < instructions.size(); ++i) {
                    const auto& inst = instructions[i];
                    float4      res  = {0, 0, 0, 0};

                    if (inst.op == 0) {
                        res = {uv.x, uv.y, 0, 0};
                    } else if (inst.op == 1) {
                        float4 vec = (inst.in0 >= 0) ? regs[inst.in0] : float4 {0, 0, 0, 0};

                        // 1. Subtract Location translation
                        vec.x -= inst.p[0];
                        vec.y -= inst.p[1];
                        vec.z -= inst.p[2];

                        // 2. Rotate coordinates
                        float cosZ = std::cos(-inst.p[5]);
                        float sinZ = std::sin(-inst.p[5]);
                        float rx   = vec.x * cosZ - vec.y * sinZ;
                        float ry   = vec.x * sinZ + vec.y * cosZ;
                        vec.x      = rx;
                        vec.y      = ry;

                        // 3. Divide by Scale (standard Blender Point Mapping behavior)
                        vec.x /= (inst.p[6] != 0.0f ? inst.p[6] : 1.0f);
                        vec.y /= (inst.p[7] != 0.0f ? inst.p[7] : 1.0f);
                        vec.z /= (inst.p[8] != 0.0f ? inst.p[8] : 1.0f);

                        res = vec;
                    } else if (inst.op == 2) {
                        float4 vec = (inst.in0 >= 0) ? regs[inst.in0] : float4 {0, 0, 0, 0};
                        float3 v   = EvaluateVoronoi({vec.x * inst.p[0], vec.y * inst.p[0]}, inst.p[1]);
                        res        = {v.x, v.y, v.y, 1.0f};
                    } else if (inst.op == 3) {
                        float fac = std::clamp((inst.in0 >= 0) ? regs[inst.in0].x : 0.0f, 0.0f, 1.0f);

                        // inst.p[0] holds 0.0 for CONSTANT interpolation, 1.0 for LINEAR
                        bool is_constant = (inst.p[0] == 0.0f);
                        int  num_els     = inst.p[1];

                        if (num_els == 1)
                            res = {inst.p[3], inst.p[4], inst.p[5], inst.p[6]};
                        else if (num_els > 1) {
                            int e = 0;
                            while (e < num_els - 1 && fac > inst.p[2 + (e + 1) * 5])
                                e++;
                            if (e == num_els - 1) {
                                res = {inst.p[2 + e * 5 + 1], inst.p[2 + e * 5 + 2], inst.p[2 + e * 5 + 3], inst.p[2 + e * 5 + 4]};
                            } else {
                                float  p1 = inst.p[2 + e * 5], p2 = inst.p[2 + (e + 1) * 5];
                                float4 r1 = {inst.p[2 + e * 5 + 1], inst.p[2 + e * 5 + 2], inst.p[2 + e * 5 + 3], inst.p[2 + e * 5 + 4]};

                                if (is_constant) {
                                    // Constant interpolation: keep color flat up to the next stop
                                    res = r1;
                                } else {
                                    // Linear interpolation: blend between current and next stop
                                    float  t  = (fac - p1) / std::max(p2 - p1, 1e-6f);
                                    float4 r2 = {
                                        inst.p[2 + (e + 1) * 5 + 1], inst.p[2 + (e + 1) * 5 + 2], inst.p[2 + (e + 1) * 5 + 3], inst.p[2 + (e + 1) * 5 + 4]
                                    };
                                    res = {r1.x + (r2.x - r1.x) * t, r1.y + (r2.y - r1.y) * t, r1.z + (r2.z - r1.z) * t, r1.w + (r2.w - r1.w) * t};
                                }
                            }
                        }
                    } else if (inst.op == 4) {
                        float  fac   = std::clamp((inst.in0 >= 0) ? regs[inst.in0].x : inst.p[1], 0.0f, 1.0f);
                        float4 a     = (inst.in1 >= 0) ? regs[inst.in1] : float4 {inst.p[2], inst.p[3], inst.p[4], inst.p[5]};
                        float4 b     = (inst.in2 >= 0) ? regs[inst.in2] : float4 {inst.p[6], inst.p[7], inst.p[8], inst.p[9]};
                        int    btype = inst.p[0];
                        if (btype == 0) {
                            res = {a.x + (b.x - a.x) * fac, a.y + (b.y - a.y) * fac, a.z + (b.z - a.z) * fac, 1.0f};
                        } else if (btype == 1) {
                            res = {
                                std::max(a.x, b.x) * fac + a.x * (1 - fac), std::max(a.y, b.y) * fac + a.y * (1 - fac),
                                std::max(a.z, b.z) * fac + a.z * (1 - fac), 1.0f
                            };
                        }
                    }
                    regs[i] = res;
                }

                float4 finalColor     = regs.empty() ? float4 {0, 0, 0, 1} : regs.back();
                auto   r              = static_cast<uint8_t>(std::clamp(finalColor.x, 0.0f, 1.0f) * 255.0f);
                auto   g              = static_cast<uint8_t>(std::clamp(finalColor.y, 0.0f, 1.0f) * 255.0f);
                auto   b              = static_cast<uint8_t>(std::clamp(finalColor.z, 0.0f, 1.0f) * 255.0f);
                pixels[y * width + x] = 0xFF000000u | (uint32_t(b) << 16) | (uint32_t(g) << 8) | r;
            }
        }

        std::vector<uint8_t> pngBytes = CreatePNGBytes(pixels, width, height);

        std::string     texDir = levelFolder + "/textures";
        std::error_code ec;
        std::filesystem::create_directories(texDir, ec);
        std::string ztexPath = texDir + "/baked_" + mat.id + ".ztex";
        FILE*       zf       = std::fopen(ztexPath.c_str(), "wb");
        if (zf != nullptr) {
            std::fwrite(pngBytes.data(), 1, pngBytes.size(), zf);
            std::fclose(zf);
        }

        while (binBuffer.size() % 4 != 0)
            binBuffer.push_back(0);
        auto imgOffset = static_cast<uint32_t>(binBuffer.size());
        binBuffer.insert(binBuffer.end(), pngBytes.begin(), pngBytes.end());
        while (binBuffer.size() % 4 != 0)
            binBuffer.push_back(0);

        doc.bufferViews.push_back(GlbBufferView {.byteOffset = imgOffset, .byteLength = static_cast<uint32_t>(pngBytes.size())});
        uint32_t imgBViewIdx = bViewIndex++;

        int idx = static_cast<int>(packedImages.size());
        packedImages.push_back({.relativeUri = id});
        doc.textures.push_back(GlbTexture {.sampler = 0, .source = static_cast<uint32_t>(idx)});
        doc.images.push_back(GlbImage {.bufferView = imgBViewIdx, .mimeType = "image/png"});

        return idx;
    };

    for (const auto& mat: manifest.materials) {
        int albedoTex = -1;
        if (mat.procedural.active && mat.procedural.type == "NODE_GRAPH") {
            albedoTex = getProceduralTextureIndex(mat);
        } else {
            albedoTex = getTextureIndex(mat.albedoMap);
        }
        int normalTex   = getTextureIndex(mat.normalMap);
        int mrTex       = getTextureIndex(mat.metallicRoughnessMap);
        int emissiveTex = getTextureIndex(mat.emissiveMap);

        GlbMaterial material;
        material.name = mat.id;
        material.pbrMetallicRoughness.baseColorFactor = {mat.baseColor[0], mat.baseColor[1], mat.baseColor[2], mat.baseColor[3]};
        material.pbrMetallicRoughness.metallicFactor  = mat.metallic;
        material.pbrMetallicRoughness.roughnessFactor = mat.roughness;
        if (albedoTex != -1) {
            material.pbrMetallicRoughness.baseColorTexture = GlbTextureRef {.index = static_cast<uint32_t>(albedoTex)};
        }

        if (mat.procedural.active && mat.procedural.type != "NODE_GRAPH") {
            GlbProceduralShader shader;
            shader.type = mat.procedural.type;
            for (const auto& param: mat.procedural.parameters) {
                shader.parameters.emplace(param.name, param.values);
            }
            material.extensions = GlbMaterialExtensions {.ZHLN_procedural_shader = shader};
        }

        if (mat.baseColor[3] < 0.999f) {
            material.alphaMode = "BLEND";
        }
        if (mat.doubleSided) {
            material.doubleSided = true;
        }
        if (normalTex != -1) {
            material.normalTexture = GlbTextureRef {.index = static_cast<uint32_t>(normalTex)};
        }
        if (mrTex != -1) {
            material.metallicRoughnessTexture = GlbTextureRef {.index = static_cast<uint32_t>(mrTex)};
        }

        bool hasEmissive = (mat.emissiveStrength > 0.f) &&
                           ((emissiveTex != -1) || (mat.emissiveFactor[0] > 0.f || mat.emissiveFactor[1] > 0.f || mat.emissiveFactor[2] > 0.f));
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

            material.emissiveFactor = std::array<float, 3> {ef[0], ef[1], ef[2]};
            if (emissiveTex != -1) {
                material.emissiveTexture = GlbTextureRef {.index = static_cast<uint32_t>(emissiveTex)};
            }
            if (strength > 1.f) {
                if (!material.extensions) {
                    material.extensions = GlbMaterialExtensions {};
                }
                material.extensions->KHR_materials_emissive_strength = GlbEmissiveStrength {.emissiveStrength = strength};
            }
        }

        matIdToGlbIndex[mat.id] = static_cast<int>(doc.materials.size());
        doc.materials.push_back(material);
    }

    if (!doc.textures.empty()) {
        doc.samplers.push_back(GlbSampler {});
    }

    for (const auto& mesh: manifest.meshes) {
        std::string  binPath  = levelFolder + "/" + mesh.binFile;
        CompiledMesh compiled = CompileRawMesh(mesh, binPath);
        if (compiled.positions.empty()) {
            continue;
        }

        meshIdToGlbIndex[mesh.id] = static_cast<int>(doc.meshes.size());

        auto vertexCount = static_cast<uint32_t>(compiled.positions.size());

        // 1. Pack Positions (directly from compiled.positions)
        auto   posOffset = static_cast<uint32_t>(binBuffer.size());
        size_t posBytes  = compiled.positions.size() * sizeof(VertexPosition);
        binBuffer.insert(
            binBuffer.end(), reinterpret_cast<const uint8_t*>(compiled.positions.data()), reinterpret_cast<const uint8_t*>(compiled.positions.data()) + posBytes
        );
        while (binBuffer.size() % 4 != 0)
            binBuffer.push_back(0);

        doc.bufferViews.push_back(GlbBufferView {.byteOffset = posOffset, .byteLength = static_cast<uint32_t>(posBytes), .target = 34962});
        uint32_t posBViewIdx = bViewIndex++;

        // 2. Unpack and Pack Normals (FLOAT3)
        auto normOffset = static_cast<uint32_t>(binBuffer.size());
        for (const auto& attr: compiled.attributes) {
            auto  n       = UnpackNormal(attr.normal.data);
            float nFlt[3] = {n[0], n[1], n[2]};
            binBuffer.insert(binBuffer.end(), reinterpret_cast<const uint8_t*>(nFlt), reinterpret_cast<const uint8_t*>(nFlt) + 12);
        }
        while (binBuffer.size() % 4 != 0)
            binBuffer.push_back(0);

        doc.bufferViews.push_back(GlbBufferView {.byteOffset = normOffset, .byteLength = vertexCount * 12, .target = 34962});
        uint32_t normBViewIdx = bViewIndex++;

        // 3. Unpack and Pack Tangents (FLOAT4)
        auto tangOffset = static_cast<uint32_t>(binBuffer.size());
        for (const auto& attr: compiled.attributes) {
            auto  t       = UnpackNormal(attr.tangent.data);
            float tFlt[4] = {t[0], t[1], t[2], t[3]};
            binBuffer.insert(binBuffer.end(), reinterpret_cast<const uint8_t*>(tFlt), reinterpret_cast<const uint8_t*>(tFlt) + 16);
        }
        while (binBuffer.size() % 4 != 0)
            binBuffer.push_back(0);

        doc.bufferViews.push_back(GlbBufferView {.byteOffset = tangOffset, .byteLength = vertexCount * 16, .target = 34962});
        uint32_t tangBViewIdx = bViewIndex++;

        // 4. Unpack and Pack UVs (FLOAT2)
        auto uvOffset = static_cast<uint32_t>(binBuffer.size());
        for (const auto& attr: compiled.attributes) {
            float u        = HalfToFloat(attr.uv.data & 0xFFFF);
            float v        = HalfToFloat(attr.uv.data >> 16);
            float uvFlt[2] = {u, v};
            binBuffer.insert(binBuffer.end(), reinterpret_cast<const uint8_t*>(uvFlt), reinterpret_cast<const uint8_t*>(uvFlt) + 8);
        }
        while (binBuffer.size() % 4 != 0)
            binBuffer.push_back(0);

        doc.bufferViews.push_back(GlbBufferView {.byteOffset = uvOffset, .byteLength = vertexCount * 8, .target = 34962});
        uint32_t uvBViewIdx = bViewIndex++;

        // 5. Pack Colors (directly as UNORM8)
        auto colorOffset = static_cast<uint32_t>(binBuffer.size());
        for (const auto& attr: compiled.attributes) {
            uint32_t col = attr.color.data;
            binBuffer.insert(binBuffer.end(), reinterpret_cast<const uint8_t*>(&col), reinterpret_cast<const uint8_t*>(&col) + 4);
        }
        while (binBuffer.size() % 4 != 0)
            binBuffer.push_back(0);

        doc.bufferViews.push_back(GlbBufferView {.byteOffset = colorOffset, .byteLength = vertexCount * 4, .target = 34962});
        uint32_t colorBViewIdx = bViewIndex++;

        // 6. Indices (IBO)
        auto   iboOffset = static_cast<uint32_t>(binBuffer.size());
        size_t iboBytes  = compiled.indices.size() * sizeof(uint32_t);
        if (iboBytes > 0) {
            binBuffer.insert(
                binBuffer.end(), reinterpret_cast<uint8_t*>(compiled.indices.data()), reinterpret_cast<uint8_t*>(compiled.indices.data()) + iboBytes
            );
        }
        while (binBuffer.size() % 4 != 0) {
            binBuffer.push_back(0);
        }

        doc.bufferViews.push_back(GlbBufferView {.byteOffset = iboOffset, .byteLength = static_cast<uint32_t>(iboBytes), .target = 34963});
        uint32_t iboBViewIdx = bViewIndex++;

        uint32_t posAcc   = accIndex++;
        uint32_t normAcc  = accIndex++;
        uint32_t tangAcc  = accIndex++;
        uint32_t uvAcc    = accIndex++;
        uint32_t colorAcc = accIndex++;

        doc.accessors.push_back(GlbAccessor {
            .bufferView    = posBViewIdx,
            .componentType = 5126,
            .count         = vertexCount,
            .type          = "VEC3",
            .min           = {compiled.minB[0], compiled.minB[1], compiled.minB[2]},
            .max           = {compiled.maxB[0], compiled.maxB[1], compiled.maxB[2]},
        });
        doc.accessors.push_back(GlbAccessor {.bufferView = normBViewIdx, .componentType = 5126, .count = vertexCount, .type = "VEC3"});
        doc.accessors.push_back(GlbAccessor {.bufferView = tangBViewIdx, .componentType = 5126, .count = vertexCount, .type = "VEC4"});
        doc.accessors.push_back(GlbAccessor {.bufferView = uvBViewIdx, .componentType = 5126, .count = vertexCount, .type = "VEC2"});

        // Use 5121 (UNSIGNED_BYTE) normalized=true for vertex colors
        doc.accessors.push_back(GlbAccessor {.bufferView = colorBViewIdx, .componentType = 5121, .count = vertexCount, .type = "VEC4", .normalized = true});

        uint32_t jointsAcc  = 0;
        uint32_t weightsAcc = 0;

        if (compiled.isSkinned) {
            // joints: uint16_t[4] -> 5123 (UNSIGNED_SHORT)
            while (binBuffer.size() % 4 != 0) {
                binBuffer.push_back(0);
            }
            auto   jboOffset = static_cast<uint32_t>(binBuffer.size());
            size_t jboBytes  = compiled.skins.size() * 8;
            for (const auto& s: compiled.skins) {
                binBuffer.insert(binBuffer.end(), reinterpret_cast<const uint8_t*>(s.joints), reinterpret_cast<const uint8_t*>(s.joints) + 8);
            }
            while (binBuffer.size() % 4 != 0) {
                binBuffer.push_back(0);
            }

            doc.bufferViews.push_back(GlbBufferView {.byteOffset = jboOffset, .byteLength = static_cast<uint32_t>(jboBytes), .target = 34962});
            uint32_t jboBViewIdx = bViewIndex++;

            jointsAcc = accIndex++;
            doc.accessors.push_back(GlbAccessor {.bufferView = jboBViewIdx, .componentType = 5123, .count = vertexCount, .type = "VEC4"});

            // weights: PackedRGBA8 -> 5121 (UNSIGNED_BYTE) normalized=true
            while (binBuffer.size() % 4 != 0) {
                binBuffer.push_back(0);
            }
            auto wboOffset = static_cast<uint32_t>(binBuffer.size());
            for (const auto& s: compiled.skins) {
                uint32_t w = s.weights.data;
                binBuffer.insert(binBuffer.end(), reinterpret_cast<const uint8_t*>(&w), reinterpret_cast<const uint8_t*>(&w) + 4);
            }
            while (binBuffer.size() % 4 != 0) {
                binBuffer.push_back(0);
            }

            doc.bufferViews.push_back(GlbBufferView {.byteOffset = wboOffset, .byteLength = vertexCount * 4, .target = 34962});
            uint32_t wboBViewIdx = bViewIndex++;

            weightsAcc = accIndex++;
            doc.accessors.push_back(
                GlbAccessor {.bufferView = wboBViewIdx, .componentType = 5121, .count = vertexCount, .type = "VEC4", .normalized = true}
            );
        }

        // Compile morph targets (shape keys)
        std::vector<GlbMorphTarget> targets;
        for (const auto& target: mesh.morphTargets) {
            std::string targetBinPath = levelFolder + "/" + target.binFile;
            FILE*       tbf           = std::fopen(targetBinPath.c_str(), "rb");
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
            auto   targetOffset = static_cast<uint32_t>(binBuffer.size());
            size_t targetBytes  = compiledOffsets.size() * sizeof(float);
            binBuffer.insert(
                binBuffer.end(), reinterpret_cast<uint8_t*>(compiledOffsets.data()), reinterpret_cast<uint8_t*>(compiledOffsets.data()) + targetBytes
            );

            doc.bufferViews.push_back(GlbBufferView {.byteOffset = targetOffset, .byteLength = static_cast<uint32_t>(targetBytes)});
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

            doc.accessors.push_back(GlbAccessor {
                .bufferView    = targetBViewIdx,
                .componentType = 5126,
                .count         = vertexCount,
                .type          = "VEC3",
                .min           = {minO[0], minO[1], minO[2]},
                .max           = {maxO[0], maxO[1], maxO[2]},
            });

            targets.push_back(GlbMorphTarget {.POSITION = targetAccIdx});
        }

        GlbMesh glbMesh;
        glbMesh.name = mesh.id;
        for (size_t p = 0; p < compiled.primitives.size(); ++p) {
            const auto& prim     = compiled.primitives[p];
            uint32_t    indexAcc = accIndex++;

            doc.accessors.push_back(GlbAccessor {
                .bufferView    = iboBViewIdx,
                .byteOffset    = prim.vertexOffset,
                .componentType = 5125,
                .count         = prim.vertexCount,
                .type          = "SCALAR",
            });

            GlbPrimitive glbPrim;
            glbPrim.attributes.POSITION   = posAcc;
            glbPrim.attributes.NORMAL     = normAcc;
            glbPrim.attributes.TANGENT    = tangAcc;
            glbPrim.attributes.TEXCOORD_0 = uvAcc;
            glbPrim.attributes.COLOR_0    = colorAcc;
            if (compiled.isSkinned) {
                glbPrim.attributes.JOINTS_0  = jointsAcc;
                glbPrim.attributes.WEIGHTS_0 = weightsAcc;
            }
            glbPrim.indices  = indexAcc;
            glbPrim.targets  = targets;

            auto it = matIdToGlbIndex.find(prim.materialId);
            if (it != matIdToGlbIndex.end()) {
                glbPrim.material = static_cast<uint32_t>(it->second);
            }

            glbMesh.primitives.push_back(glbPrim);
        }
        doc.meshes.push_back(glbMesh);
    }

    for (size_t i = 0; i < manifest.lights.size(); ++i) {
        lightIdToGlbIndex[manifest.lights[i].id] = static_cast<int>(i);
    }

    std::unordered_map<std::string, std::vector<std::string>> nodeChildren;
    for (const auto& node: manifest.nodes) {
        if (!node.parentId.empty()) {
            nodeChildren[node.parentId].push_back(node.id);
        }
    }
    for (const auto& skin: manifest.skins) {
        for (size_t i = 0; i < skin.joints.size(); ++i) {
            if (i < skin.parents.size() && !skin.parents[i].empty()) {
                nodeChildren[skin.parents[i]].push_back(skin.joints[i]);
            }
        }
    }

    int                                  glbNodeIdx = 0;
    std::vector<const Compiler::IRNode*> nodesToEmit;
    for (const auto& node: manifest.nodes) {
        bool isTargetedByAnim = false;
        for (const auto& anim: manifest.animations) {
            for (const auto& chan: anim.channels) {
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

    std::vector<std::pair<std::string, std::array<float, 16>>> jointsToEmit;
    for (const auto& skin: manifest.skins) {
        for (size_t i = 0; i < skin.joints.size(); ++i) {
            const auto& jointId = skin.joints[i];
            if (nodeIdToGlbIndex.contains(jointId)) {
                continue;
            }

            std::array<float, 16> matrix {};
            for (int m = 0; m < 16; ++m) {
                matrix[static_cast<size_t>(m)] = (i * 16 + m < skin.restPose.size()) ? skin.restPose[i * 16 + m] : (m == 0 || m == 5 || m == 10 || m == 15 ? 1.0f : 0.0f);
            }
            nodeIdToGlbIndex[jointId] = glbNodeIdx++;
            jointsToEmit.emplace_back(jointId, matrix);
        }
    }

    doc.nodes.resize(static_cast<size_t>(glbNodeIdx));

    for (const auto& skin: manifest.skins) {
        while (binBuffer.size() % 4 != 0) {
            binBuffer.push_back(0);
        }
        auto   ibmOffset = static_cast<uint32_t>(binBuffer.size());
        size_t ibmBytes  = skin.inverseBindMatrices.size() * sizeof(float);
        if (ibmBytes > 0) {
            binBuffer.insert(
                binBuffer.end(), reinterpret_cast<const uint8_t*>(skin.inverseBindMatrices.data()),
                reinterpret_cast<const uint8_t*>(skin.inverseBindMatrices.data()) + ibmBytes
            );
        }

        doc.bufferViews.push_back(GlbBufferView {.byteOffset = ibmOffset, .byteLength = static_cast<uint32_t>(ibmBytes)});
        uint32_t ibmBViewIdx = bViewIndex++;

        uint32_t ibmAccIdx = accIndex++;
        doc.accessors.push_back(GlbAccessor {
            .bufferView    = ibmBViewIdx,
            .componentType = 5126,
            .count         = static_cast<uint32_t>(skin.joints.size()),
            .type          = "MAT4",
        });

        GlbSkin glbSkin;
        glbSkin.name = skin.name;
        glbSkin.inverseBindMatrices = ibmAccIdx;
        for (const auto& jointId: skin.joints) {
            glbSkin.joints.push_back(static_cast<uint32_t>(nodeIdToGlbIndex[jointId]));
        }

        skinIdToGlbIndex[skin.id] = static_cast<int>(doc.skins.size());
        doc.skins.push_back(glbSkin);
    }

    auto childrenOf = [&](const std::string& nodeId) -> std::vector<uint32_t> {
        std::vector<uint32_t> activeChildren;
        auto                  cIt = nodeChildren.find(nodeId);
        if (cIt == nodeChildren.end() || cIt->second.empty()) {
            return activeChildren;
        }
        for (const auto& childId: cIt->second) {
            auto childIt = nodeIdToGlbIndex.find(childId);
            if (childIt != nodeIdToGlbIndex.end())
                activeChildren.push_back(static_cast<uint32_t>(childIt->second));
        }
        return activeChildren;
    };

    for (const auto& [jointId, matrix]: jointsToEmit) {
        int nIdx = nodeIdToGlbIndex[jointId];
        doc.nodes[static_cast<size_t>(nIdx)] = GlbNode {
            .name     = jointId,
            .matrix   = matrix,
            .children = childrenOf(jointId),
        };
    }

    for (const auto* nodePtr: nodesToEmit) {
        const auto& node = *nodePtr;
        int         nIdx = nodeIdToGlbIndex[node.id];

        const float* matrixData = node.worldMatrix;
        if (!node.parentId.empty()) {
            auto pIt = nodeIdToGlbIndex.find(node.parentId);
            if (pIt != nodeIdToGlbIndex.end())
                matrixData = node.localMatrix;
        }

        GlbNode glbNode;
        glbNode.name = node.id;
        std::copy_n(matrixData, 16, glbNode.matrix.begin());

        if (!node.meshId.empty() && node.visible) {
            auto it = meshIdToGlbIndex.find(node.meshId);
            if (it != meshIdToGlbIndex.end()) {
                glbNode.mesh = static_cast<uint32_t>(it->second);
                auto mIt = std::ranges::find_if(manifest.meshes, [&](const auto& m) { return m.id == node.meshId; });
                if (mIt != manifest.meshes.end() && !mIt->morphTargets.empty()) {
                    glbNode.weights.assign(mIt->morphTargets.size(), 0.0f);
                }
            }
        }
        if (!node.skinId.empty()) {
            auto sit = skinIdToGlbIndex.find(node.skinId);
            if (sit != skinIdToGlbIndex.end())
                glbNode.skin = static_cast<uint32_t>(sit->second);
        }
        if (!node.lightId.empty()) {
            auto lit = lightIdToGlbIndex.find(node.lightId);
            if (lit != lightIdToGlbIndex.end())
                glbNode.extensions = GlbNodeExtensions {.KHR_lights_punctual = {.light = static_cast<uint32_t>(lit->second)}};
        }
        glbNode.children = childrenOf(node.id);

        doc.nodes[static_cast<size_t>(nIdx)] = glbNode;
    }

    for (const auto& anim: manifest.animations) {
        GlbAnimation glbAnim;
        glbAnim.name = anim.name;

        for (size_t sIdx = 0; sIdx < anim.samplers.size(); ++sIdx) {
            const auto& s           = anim.samplers[sIdx];
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

            doc.bufferViews.push_back(GlbBufferView {.byteOffset = glbInOffset, .byteLength = s.inputLength});
            uint32_t inBViewIdx = bViewIndex++;

            doc.bufferViews.push_back(GlbBufferView {.byteOffset = glbOutOffset, .byteLength = s.outputLength});
            uint32_t outBViewIdx = bViewIndex++;

            uint32_t        keyCount   = s.inputLength / sizeof(float);
            std::string_view outputType = "VEC3"; // string literal storage: outlives the accessors
            for (const auto& chan: anim.channels) {
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
                    std::memcpy(&maxTime, inputBytes.data() + (keyCount - 1) * sizeof(float), sizeof(float));
                }
            }

            uint32_t inAccIdx = accIndex++;
            doc.accessors.push_back(GlbAccessor {
                .bufferView    = inBViewIdx,
                .componentType = 5126,
                .count         = keyCount,
                .type          = "SCALAR",
                .min           = {minTime},
                .max           = {maxTime},
            });

            uint32_t outAccIdx = accIndex++;
            doc.accessors.push_back(GlbAccessor {.bufferView = outBViewIdx, .componentType = 5126, .count = keyCount, .type = outputType});

            glbAnim.samplers.push_back(GlbAnimSampler {
                .input         = inAccIdx,
                .interpolation = s.interpolation.empty() ? std::string_view {"LINEAR"} : std::string_view {s.interpolation},
                .output        = outAccIdx,
            });
        }

        for (const auto& chan: anim.channels) {
            auto nIt = nodeIdToGlbIndex.find(chan.targetNodeId);
            if (nIt == nodeIdToGlbIndex.end())
                continue;
            glbAnim.channels.push_back(GlbAnimChannel {
                .sampler = chan.samplerId,
                .target  = {.node = static_cast<uint32_t>(nIt->second), .path = chan.targetPath},
            });
        }

        if (!glbAnim.channels.empty() && !glbAnim.samplers.empty()) {
            doc.animations.push_back(glbAnim);
        }
    }

    std::vector<int> rootNodeIndices;
    for (const auto& node: manifest.nodes) {
        if (node.parentId.empty() || !nodeIdToGlbIndex.contains(node.parentId)) {
            auto it = nodeIdToGlbIndex.find(node.id);
            if (it != nodeIdToGlbIndex.end())
                rootNodeIndices.push_back(it->second);
        }
    }
    for (const auto& skin: manifest.skins) {
        for (size_t i = 0; i < skin.joints.size(); ++i) {
            if (i < skin.parents.size()) {
                std::string parentId = skin.parents[i];
                if (parentId.empty() || !nodeIdToGlbIndex.contains(parentId)) {
                    auto it = nodeIdToGlbIndex.find(skin.joints[i]);
                    if (it != nodeIdToGlbIndex.end()) {
                        int idx = it->second;
                        if (std::ranges::find(rootNodeIndices, idx) == rootNodeIndices.end()) {
                            bool isChild = false;
                            for (const auto& pair: nodeChildren) {
                                if (nodeIdToGlbIndex.contains(pair.first)) {
                                    if (std::ranges::find(pair.second, skin.joints[i]) != pair.second.end()) {
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

    std::vector<std::string_view> usedExts;
    if (!manifest.lights.empty())
        usedExts.emplace_back("KHR_lights_punctual");
    bool usesEmissiveStrength = false;
    for (const auto& mat: manifest.materials) {
        bool hasEmissive = (mat.emissiveStrength > 0.f) &&
                           ((!mat.emissiveMap.empty()) || (mat.emissiveFactor[0] > 0.f || mat.emissiveFactor[1] > 0.f || mat.emissiveFactor[2] > 0.f));
        if (hasEmissive && mat.emissiveStrength > 1.f) {
            usesEmissiveStrength = true;
            break;
        }
    }
    if (usesEmissiveStrength)
        usedExts.emplace_back("KHR_materials_emissive_strength");

    usedExts.emplace_back("ZHLN_procedural_shader");

    doc.extensionsUsed = usedExts;
    if (!manifest.lights.empty()) {
        GlbRootExtensions rootExtensions;
        for (const auto& l: manifest.lights) {
            rootExtensions.KHR_lights_punctual.lights.push_back(GlbKhrLight {
                .name      = l.id,
                .type      = l.type,
                .color     = {l.color[0], l.color[1], l.color[2]},
                .intensity = l.intensity,
            });
        }
        doc.extensions = rootExtensions;
    }

    doc.scenes.push_back(GlbScene {.nodes = {rootNodeIndices.begin(), rootNodeIndices.end()}});
    doc.buffers.push_back(GlbBuffer {.byteLength = static_cast<uint32_t>(binBuffer.size())});

    std::string json = ZHLN::ReflectJSON::SerializeJSON(doc, 2, {.omitEmpty = true});

    while (json.length() % 4 != 0)
        json += ' ';

    auto     jsonChunkLength = static_cast<uint32_t>(json.length());
    auto     binChunkLength  = static_cast<uint32_t>(binBuffer.size());
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

} // namespace ZHLN::GLB
