// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// File: src/engine/glTFImporter.cpp
#include "Zahlen/Components.hpp"
#include "Zahlen/Engine.hpp"
#include "Zahlen/Render.hpp"
#include "ecs/ECS.hpp"
#include "engine/system/AnimationSystem.hpp"
#include "engine/system/LightingSystem.hpp"
#include "physics/Physics.hpp"
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/ScaledShape.h>
#include <Zahlen/CreativeWorksFactory.hpp>
#include <Zahlen/CreativeWorksManager.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Math3D.hpp>
#include <algorithm>
#include <cgltf.h>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <stb_image.h>
#include <string>
#include <threading/TaskSystem.hpp>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ZHLN::CreativeWorksFactory {

// Declare shared caches (linked externally by AnimationSystem)
std::unordered_map<std::string, cgltf_data*> s_GLBCache;
JPH::Array<cgltf_data*>                      s_AnimatedGLBs;

// Structures to hold intermediate parsed data from CPU-parallel phase
struct CPUTextureJob {
    cgltf_image*   image = nullptr;
    std::string    glbPath;
    bool           isSRGB        = true;
    unsigned char* decodedPixels = nullptr;
    int            width         = 0;
    int            height        = 0;
    bool           wasRescaled   = false;
    uint32_t       uploadedIndex = 0;
};

struct CPUPrimitiveJob {
    const cgltf_node*      node = nullptr;
    const cgltf_primitive* prim = nullptr;
    JPH::Mat44             nodeTransform;

    std::vector<VertexPosition>   positions;
    std::vector<VertexAttributes> attributes;
    std::vector<VertexSkin>       skins;
    std::vector<uint32_t>         indices;
    uint32_t                      indexCount = 0;

    float localMin[3]    = {0.0f, 0.0f, 0.0f};
    float localMax[3]    = {0.0f, 0.0f, 0.0f};
    float boundingRadius = 1.0f;

    bool     doubleSided        = false;
    bool     alphaBlend         = false;
    float    baseColorFactor[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    float    metallicFactor     = 1.0f;
    float    roughnessFactor    = 1.0f;
    float    alphaCutoff        = 0.5f;
    uint32_t alphaMode          = 0;

    cgltf_image* albedoImage       = nullptr;
    cgltf_image* normalImage       = nullptr;
    cgltf_image* pbrImage          = nullptr;
    cgltf_image* emissiveImage     = nullptr;
    float        emissiveFactor[4] = {0.0f, 0.0f, 0.0f, 1.0f};

    uint32_t           morphOffset            = 0;
    uint32_t           activeMorphCount       = 0;
    float              defaultMorphWeights[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    std::vector<float> tempDeltas;

    JPH::ShapeRefC meshCollider = nullptr;
    JPH::ShapeRefC boxCollider  = nullptr;

    bool        hasProcedural = false;
    std::string proceduralType;
    float       proceduralScale      = 5.0f;
    float       proceduralRandomness = 1.0f;
};

struct PreparedPart {
    JPH::Vec3      translation;
    JPH::Quat      rotation;
    JPH::Vec3      scale;
    float          maxScale = 1.0f;
    JPH::ShapeRefC shape    = nullptr;
};

// Helper function to handle a single downsample pass (flattens 4 levels of loops)
static unsigned char* DownsampleHalfSize(const unsigned char* src, uint32_t currentW, uint32_t currentH) {
    uint32_t nextW = currentW / 2;
    uint32_t nextH = currentH / 2;

    auto* dst = static_cast<unsigned char*>(std::malloc(static_cast<size_t>(nextW) * nextH * 4));
    if (dst == nullptr) {
        return nullptr;
    }

    for (uint32_t y = 0; y < nextH; ++y) {
        for (uint32_t x = 0; x < nextW; ++x) {
            uint32_t srcX = x * 2;
            uint32_t srcY = y * 2;

            uint32_t r = 0;
            uint32_t g = 0;
            uint32_t b = 0;
            uint32_t a = 0;

            for (uint32_t dy = 0; dy < 2; ++dy) {
                for (uint32_t dx = 0; dx < 2; ++dx) {
                    auto srcIdx = (((static_cast<size_t>(srcY) + dy) * currentW + (srcX + dx)) * 4);
                    r += src[srcIdx + 0];
                    g += src[srcIdx + 1];
                    b += src[srcIdx + 2];
                    a += src[srcIdx + 3];
                }
            }

            size_t dstIdx   = (static_cast<size_t>(y) * nextW + x) * 4;
            dst[dstIdx + 0] = static_cast<unsigned char>(r / 4);
            dst[dstIdx + 1] = static_cast<unsigned char>(g / 4);
            dst[dstIdx + 2] = static_cast<unsigned char>(b / 4);
            dst[dstIdx + 3] = static_cast<unsigned char>(a / 4);
        }
    }
    return dst;
}

static void DecodeAndRescaleTexture(CPUTextureJob& job) {
    int            channels = 0;
    unsigned char* pixels   = nullptr;

    // 1. Decode Image Data
    if (job.image->buffer_view != nullptr) {
        const char* bufferData = (const char*) job.image->buffer_view->buffer->data + job.image->buffer_view->offset;
        pixels                 = stbi_load_from_memory(
            reinterpret_cast<const stbi_uc*>(bufferData), static_cast<int>(job.image->buffer_view->size), &job.width, &job.height, &channels, 4
        );
    } else if (job.image->uri != nullptr) {
        std::filesystem::path glbFolder = std::filesystem::path(job.glbPath).parent_path();
        std::filesystem::path texPath   = glbFolder / job.image->uri;
        pixels                          = stbi_load(texPath.string().c_str(), &job.width, &job.height, &channels, 4);
    }

    if (pixels == nullptr) {
        return;
    }

    // 2. Determine Scale Requirements
    auto               w           = static_cast<uint32_t>(job.width);
    auto               h           = static_cast<uint32_t>(job.height);
    constexpr uint32_t MAX_TEX_DIM = 1024;

    if (w <= MAX_TEX_DIM && h <= MAX_TEX_DIM) {
        job.decodedPixels = pixels;
        return;
    }

    uint32_t targetW    = w;
    uint32_t targetH    = h;
    uint32_t scaleSteps = 0;

    while (targetW > MAX_TEX_DIM || targetH > MAX_TEX_DIM) {
        targetW /= 2;
        targetH /= 2;
        scaleSteps++;
    }

    if (targetW == 0 || targetH == 0 || scaleSteps == 0) {
        job.decodedPixels = pixels;
        return;
    }

    // 3. Process Rescaling Loop
    unsigned char* currentSrc = pixels;
    uint32_t       currentW   = w;
    uint32_t       currentH   = h;

    for (uint32_t step = 0; step < scaleSteps; ++step) {
        unsigned char* nextDst = DownsampleHalfSize(currentSrc, currentW, currentH);
        if (nextDst == nullptr) {
            break; // Memory allocation failed, halt downsampling early
        }

        if (currentSrc != pixels) {
            std::free(currentSrc);
        }

        currentSrc = nextDst;
        currentW /= 2;
        currentH /= 2;
    }

    // 4. Update Job State
    if (currentSrc != pixels) {
        stbi_image_free(pixels);
        pixels          = currentSrc;
        job.width       = static_cast<int>(currentW);
        job.height      = static_cast<int>(currentH);
        job.wasRescaled = true;
    }

    job.decodedPixels = pixels;
}

// Parses and packs geometry attributes on the CPU (Runs in Parallel)
static void ProcessCPUPrimitive(CPUPrimitiveJob& job) {
    const auto& prim = *job.prim;
    const auto* node = job.node;

    cgltf_accessor*  posAcc     = nullptr;
    cgltf_accessor*  normAcc    = nullptr;
    cgltf_accessor*  tangentAcc = nullptr;
    cgltf_accessor*  uvAcc      = nullptr;
    cgltf_accessor*  colorAcc   = nullptr;
    cgltf_accessor*  jointsAcc  = nullptr;
    cgltf_accessor*  weightsAcc = nullptr;
    cgltf_extension* ext        = nullptr;

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
            colorAcc = attr.data;
        } else if (attr.type == cgltf_attribute_type_joints && attr.index == 0) {
            jointsAcc = attr.data;
        } else if (attr.type == cgltf_attribute_type_weights && attr.index == 0) {
            weightsAcc = attr.data;
        }
    }

    if (posAcc == nullptr) {
        return;
    }

    if (posAcc->has_min) {
        std::copy(posAcc->min, posAcc->min + 3, job.localMin);
    }
    if (posAcc->has_max) {
        std::copy(posAcc->max, posAcc->max + 3, job.localMax);
    }

    if (prim.material != nullptr) {
        job.doubleSided = (prim.material->double_sided != 0);
        if (prim.material->alpha_mode == cgltf_alpha_mode_mask) {
            job.alphaMode   = 1;
            job.alphaCutoff = prim.material->alpha_cutoff;
        } else if (prim.material->alpha_mode == cgltf_alpha_mode_blend) {
            job.alphaMode   = 1;
            job.alphaCutoff = 0.5f;
            job.alphaBlend  = false;
        }

        job.emissiveFactor[0] = prim.material->emissive_factor[0];
        job.emissiveFactor[1] = prim.material->emissive_factor[1];
        job.emissiveFactor[2] = prim.material->emissive_factor[2];

        if (prim.material->has_emissive_strength) {
            float strength = prim.material->emissive_strength.emissive_strength;
            job.emissiveFactor[0] *= strength;
            job.emissiveFactor[1] *= strength;
            job.emissiveFactor[2] *= strength;
        }

        if (prim.material->has_pbr_metallic_roughness) {
            const float* c         = prim.material->pbr_metallic_roughness.base_color_factor;
            job.baseColorFactor[0] = c[0];
            job.baseColorFactor[1] = c[1];
            job.baseColorFactor[2] = c[2];
            job.baseColorFactor[3] = c[3];
            job.metallicFactor     = prim.material->pbr_metallic_roughness.metallic_factor;
            job.roughnessFactor    = prim.material->pbr_metallic_roughness.roughness_factor;

            if (prim.material->pbr_metallic_roughness.base_color_texture.texture != nullptr) {
                job.albedoImage = prim.material->pbr_metallic_roughness.base_color_texture.texture->image;
            }
            if (prim.material->pbr_metallic_roughness.metallic_roughness_texture.texture != nullptr) {
                job.pbrImage = prim.material->pbr_metallic_roughness.metallic_roughness_texture.texture->image;
            }
        }
        if (prim.material->normal_texture.texture != nullptr) {
            job.normalImage = prim.material->normal_texture.texture->image;
        }

        // --- Process Procedural Shader Extensions (CPU side only) ---
        for (cgltf_size e = 0; e < prim.material->extensions_count; ++e) {
            if (strcmp(prim.material->extensions[e].name, "ZHLN_procedural_shader") == 0) {
                ext = &prim.material->extensions[e];
                break;
            }
        }

        if ((ext != nullptr) && (ext->data != nullptr)) {
            job.hasProcedural        = true;
            job.proceduralScale      = 5.0f;
            job.proceduralRandomness = 1.0f;

            std::string extData(ext->data);
            if (size_t tPos = extData.find("\"type\":"); tPos != std::string::npos) {
                size_t startQuote = extData.find('"', tPos + 7);
                size_t endQuote   = extData.find('"', startQuote + 1);
                if (startQuote != std::string::npos && endQuote != std::string::npos) {
                    job.proceduralType = extData.substr(startQuote + 1, endQuote - startQuote - 1);
                }
            } else {
                job.proceduralType = "VORONOI";
            }

            if (size_t sPos = extData.find("\"scale\":"); sPos != std::string::npos) {
                job.proceduralScale = std::stof(extData.substr(sPos + 8));
            }
            if (size_t rPos = extData.find("\"randomness\":"); rPos != std::string::npos) {
                job.proceduralRandomness = std::stof(extData.substr(rPos + 13));
            }
        }
    }

    size_t vertexCount = posAcc->count;
    job.positions.resize(vertexCount);
    job.attributes.resize(vertexCount);
    if (jointsAcc != nullptr && weightsAcc != nullptr) {
        job.skins.resize(vertexCount);
    }

    for (size_t vIdx = 0; vIdx < vertexCount; ++vIdx) {
        float rawPos[3] = {0.0f, 0.0f, 0.0f};
        cgltf_accessor_read_float(posAcc, vIdx, rawPos, 3);
        job.positions[vIdx] = {.position = {rawPos[0], rawPos[1], rawPos[2]}};

        float rawNorm[3] = {0.0f, 1.0f, 0.0f};
        if (normAcc != nullptr) {
            cgltf_accessor_read_float(normAcc, vIdx, rawNorm, 3);
        }
        float nLen = std::sqrt(rawNorm[0] * rawNorm[0] + rawNorm[1] * rawNorm[1] + rawNorm[2] * rawNorm[2]);
        if (nLen > 1e-6f) {
            rawNorm[0] /= nLen;
            rawNorm[1] /= nLen;
            rawNorm[2] /= nLen;
        }

        float rawTangent[4] = {1.0f, 0.0f, 0.0f, 1.0f};
        if (tangentAcc != nullptr) {
            cgltf_accessor_read_float(tangentAcc, vIdx, rawTangent, 4);
        }
        float tLen = std::sqrt(rawTangent[0] * rawTangent[0] + rawTangent[1] * rawTangent[1] + rawTangent[2] * rawTangent[2]);
        if (tLen > 1e-6f) {
            rawTangent[0] /= tLen;
            rawTangent[1] /= tLen;
            rawTangent[2] /= tLen;
        }

        float uv[2] = {0.0f, 0.0f};
        if (uvAcc != nullptr) {
            cgltf_accessor_read_float(uvAcc, vIdx, uv, 2);
        }

        float rawColor[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        if (colorAcc != nullptr) {
            cgltf_accessor_read_float(colorAcc, vIdx, rawColor, 4);
        }

        job.attributes[vIdx] = {
            .normal  = Math::PackNormal(rawNorm[0], rawNorm[1], rawNorm[2]),
            .tangent = Math::PackNormal(rawTangent[0], rawTangent[1], rawTangent[2], rawTangent[3]),
            .uv      = Math::PackUV(uv[0], uv[1]),
            .color   = Math::PackColor(rawColor[0], rawColor[1], rawColor[2], rawColor[3])
        };

        if (jointsAcc != nullptr && weightsAcc != nullptr) {
            uint32_t joints[4] = {0, 0, 0, 0};
            cgltf_accessor_read_uint(jointsAcc, vIdx, joints, 4);

            float weights[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            cgltf_accessor_read_float(weightsAcc, vIdx, weights, 4);

            job.skins[vIdx] = {
                .joints =
                    {static_cast<uint16_t>(joints[0]), static_cast<uint16_t>(joints[1]), static_cast<uint16_t>(joints[2]), static_cast<uint16_t>(joints[3])},
                .weights = Math::PackColor(weights[0], weights[1], weights[2], weights[3])
            };
        }
    }

    if (prim.indices != nullptr) {
        job.indexCount = static_cast<uint32_t>(prim.indices->count);
        job.indices.resize(job.indexCount);
        for (size_t idx = 0; idx < job.indexCount; ++idx) {
            job.indices[idx] = static_cast<uint32_t>(cgltf_accessor_read_index(prim.indices, idx));
        }
    } else {
        job.indexCount = static_cast<uint32_t>(job.positions.size());
        job.indices.resize(job.indexCount);
        for (uint32_t idx = 0; idx < job.indexCount; ++idx) {
            job.indices[idx] = idx;
        }
    }

    JPH::Vec3 localCenter((job.localMax[0] + job.localMin[0]) * 0.5f, (job.localMax[1] + job.localMin[1]) * 0.5f, (job.localMax[2] + job.localMin[2]) * 0.5f);

    float maxD2 = 0.0f;
    for (const auto& pos: job.positions) {
        float dx = pos.position[0] - localCenter.GetX();
        float dy = pos.position[1] - localCenter.GetY();
        float dz = pos.position[2] - localCenter.GetZ();
        float d2 = dx * dx + dy * dy + dz * dz;
        maxD2    = std::max(d2, maxD2);
    }
    job.boundingRadius = std::sqrt(maxD2) * 1.15f + 0.5f;

    if (jointsAcc != nullptr && weightsAcc != nullptr) {
        job.boundingRadius *= 3.0f;
    }

    if (prim.targets_count > 0) {
        job.activeMorphCount = std::min((uint32_t) prim.targets_count, 4u);

        if (node->weights_count > 0 && node->weights != nullptr) {
            for (uint32_t w = 0; w < job.activeMorphCount; ++w) {
                job.defaultMorphWeights[w] = node->weights[w];
            }
        } else if (node->mesh->weights != nullptr) {
            for (uint32_t w = 0; w < job.activeMorphCount; ++w) {
                job.defaultMorphWeights[w] = node->mesh->weights[w];
            }
        }

        auto currentVertexCount = static_cast<uint32_t>(job.positions.size());
        job.tempDeltas.resize(static_cast<size_t>(currentVertexCount) * job.activeMorphCount * 4, 0.0f);

        uint32_t deltaPtr = 0;
        for (uint32_t t = 0; t < job.activeMorphCount; ++t) {
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
                    job.tempDeltas[deltaPtr++] = delta[0];
                    job.tempDeltas[deltaPtr++] = delta[1];
                    job.tempDeltas[deltaPtr++] = delta[2];
                    job.tempDeltas[deltaPtr++] = 0.0f;
                }
            } else {
                deltaPtr += currentVertexCount * 4;
            }
        }
    }

    float extentsX = (job.localMax[0] - job.localMin[0]) * 0.5f;
    float extentsY = (job.localMax[1] - job.localMin[1]) * 0.5f;
    float extentsZ = (job.localMax[2] - job.localMin[2]) * 0.5f;

    JPH::ShapeRefC baseBox = new JPH::BoxShape(JPH::Vec3(extentsX, extentsY, extentsZ));
    job.boxCollider        = new JPH::RotatedTranslatedShape(localCenter, JPH::Quat::sIdentity(), baseBox);
    job.meshCollider       = Physics::CreateMeshShape(job.positions.data(), static_cast<uint32_t>(job.positions.size()), job.indices.data(), job.indexCount);
}

namespace {

struct CompiledPrimitive {
    Mesh           mesh;
    Material       defaultMaterial;
    float          boundingRadius;
    float          localMin[3];
    float          localMax[3];
    JPH::ShapeRefC meshCollider;
    JPH::ShapeRefC boxCollider;
    uint32_t       morphOffset      = 0;
    uint32_t       activeMorphCount = 0;
};

// ============================================================================
// HELPERS FOR LoadModelPrefab REFACTOR
// ============================================================================

static void DecomposeStaticMatrices(cgltf_data* data) noexcept {
    // Identify which nodes actually need decomposition (only animated nodes or joints)
    std::unordered_set<const cgltf_node*> nodesToDecompose;
    for (cgltf_size i = 0; i < data->animations_count; ++i) {
        for (cgltf_size j = 0; j < data->animations[i].channels_count; ++j) {
            if (data->animations[i].channels[j].target_node != nullptr) {
                nodesToDecompose.insert(data->animations[i].channels[j].target_node);
            }
        }
    }
    for (cgltf_size i = 0; i < data->skins_count; ++i) {
        for (cgltf_size j = 0; j < data->skins[i].joints_count; ++j) {
            if (data->skins[i].joints[j] != nullptr) {
                nodesToDecompose.insert(data->skins[i].joints[j]);
            }
        }
    }

    for (cgltf_size idx = 0; idx < data->nodes_count; ++idx) {
        cgltf_node* node = &data->nodes[idx];

        // ONLY decompose if the node is actively used by skeleton joints or animation tracks
        if (node->has_matrix && nodesToDecompose.contains(node)) {
            node->has_translation = 1;
            node->translation[0]  = node->matrix[12];
            node->translation[1]  = node->matrix[13];
            node->translation[2]  = node->matrix[14];

            JPH::Vec3 col0(node->matrix[0], node->matrix[1], node->matrix[2]);
            JPH::Vec3 col1(node->matrix[4], node->matrix[5], node->matrix[6]);
            JPH::Vec3 col2(node->matrix[8], node->matrix[9], node->matrix[10]);

            node->has_scale = 1;
            node->scale[0]  = col0.Length();
            node->scale[1]  = col1.Length();
            node->scale[2]  = col2.Length();

            if (node->scale[0] > 1e-6f) {
                col0 /= node->scale[0];
            }
            if (node->scale[1] > 1e-6f) {
                col1 /= node->scale[1];
            }
            if (node->scale[2] > 1e-6f) {
                col2 /= node->scale[2];
            }

            JPH::Mat44 rotMat(JPH::Vec4(col0, 0.0f), JPH::Vec4(col1, 0.0f), JPH::Vec4(col2, 0.0f), JPH::Vec4(0, 0, 0, 1));
            JPH::Quat  rot = rotMat.GetQuaternion().Normalized();

            node->has_rotation = 1;
            node->rotation[0]  = rot.GetX();
            node->rotation[1]  = rot.GetY();
            node->rotation[2]  = rot.GetZ();
            node->rotation[3]  = rot.GetW();

            node->has_matrix = 0;
        }
    }
}

void GatherImagesAndPrimitiveJobs(cgltf_data* data, std::vector<cgltf_image*>& outUniqueImages, std::vector<CPUPrimitiveJob>& outPrimitiveJobs) {
    auto RegisterImage = [&](cgltf_image* img) {
        if (img && std::ranges::find(outUniqueImages, img) == outUniqueImages.end()) {
            outUniqueImages.push_back(img);
        }
    };

    for (cgltf_size i = 0; i < data->nodes_count; ++i) {
        const cgltf_node* node = &data->nodes[i];
        if (node->mesh == nullptr) {
            continue;
        }

        float matrix[16];
        cgltf_node_transform_world(node, matrix);
        JPH::Mat44 nodeTransform(
            JPH::Vec4(matrix[0], matrix[1], matrix[2], matrix[3]), JPH::Vec4(matrix[4], matrix[5], matrix[6], matrix[7]),
            JPH::Vec4(matrix[8], matrix[9], matrix[10], matrix[11]), JPH::Vec4(matrix[12], matrix[13], matrix[14], matrix[15])
        );

        const auto* mesh = node->mesh;
        for (cgltf_size p = 0; p < mesh->primitives_count; ++p) {
            CPUPrimitiveJob job {};
            job.node          = node;
            job.prim          = &mesh->primitives[p];
            job.nodeTransform = nodeTransform;

            const auto& prim = mesh->primitives[p];
            if (prim.material != nullptr) {
                if (prim.material->has_pbr_metallic_roughness) {
                    auto& pbr = prim.material->pbr_metallic_roughness;
                    if (pbr.base_color_texture.texture != nullptr) {
                        job.albedoImage = pbr.base_color_texture.texture->image;
                        RegisterImage(job.albedoImage);
                    }
                    if (pbr.metallic_roughness_texture.texture != nullptr) {
                        job.pbrImage = pbr.metallic_roughness_texture.texture->image;
                        RegisterImage(job.pbrImage);
                    }
                }
                if (prim.material->normal_texture.texture != nullptr) {
                    job.normalImage = prim.material->normal_texture.texture->image;
                    RegisterImage(job.normalImage);
                }
                if (prim.material->emissive_texture.texture != nullptr) {
                    job.emissiveImage = prim.material->emissive_texture.texture->image;
                    RegisterImage(job.emissiveImage);
                }
            }
            outPrimitiveJobs.push_back(std::move(job));
        }
    }
}

void ProcessCPUTasks(
    const std::string&               rawPath,
    const std::vector<cgltf_image*>& uniqueImages,
    std::vector<CPUPrimitiveJob>&    primitiveJobs,
    JPH::Array<CPUTextureJob>&       outTextureJobs
) {
    outTextureJobs.resize(uniqueImages.size());
    for (size_t i = 0; i < uniqueImages.size(); ++i) {
        outTextureJobs[i].image   = uniqueImages[i];
        outTextureJobs[i].glbPath = rawPath;
        outTextureJobs[i].isSRGB  = true;
        for (const auto& primJob: primitiveJobs) {
            if (primJob.normalImage == uniqueImages[i] || primJob.pbrImage == uniqueImages[i]) {
                outTextureJobs[i].isSRGB = false;
                break;
            }
        }
    }

    if (!outTextureJobs.empty()) {
        TaskSystem::ParallelFor(outTextureJobs.size(), 1, [&](uint32_t start, uint32_t end, uint32_t) {
            for (uint32_t i = start; i < end; ++i) {
                DecodeAndRescaleTexture(outTextureJobs[i]);
            }
        });
    }

    if (!primitiveJobs.empty()) {
        TaskSystem::ParallelFor(primitiveJobs.size(), 1, [&](uint32_t start, uint32_t end, uint32_t) {
            for (uint32_t i = start; i < end; ++i) {
                ProcessCPUPrimitive(primitiveJobs[i]);
            }
        });
    }
}

std::unordered_map<cgltf_image*, uint32_t> UploadTexturesToGPU(RenderContext& ctx, JPH::Array<CPUTextureJob>& textureJobs) {
    std::unordered_map<cgltf_image*, uint32_t> imageToBindlessIdx;
    for (auto& texJob: textureJobs) {
        if (texJob.decodedPixels != nullptr) {
            uint32_t index = ctx.CreateTexture(texJob.decodedPixels, texJob.width, texJob.height, texJob.isSRGB);
            if (texJob.wasRescaled) {
                std::free(texJob.decodedPixels);
            } else {
                stbi_image_free(texJob.decodedPixels);
            }
            imageToBindlessIdx[texJob.image] = index;
        } else {
            imageToBindlessIdx[texJob.image] = 1; // Fallback to white
        }
    }
    return imageToBindlessIdx;
}

static CompiledPrimitive GetOrCreateCompiledPrimitive(
    RenderContext&                                                 ctx,
    const CPUPrimitiveJob&                                         primJob,
    const std::unordered_map<cgltf_image*, uint32_t>&              imageToBindlessIdx,
    std::unordered_map<const cgltf_primitive*, CompiledPrimitive>& primCache,
    bool                                                           isMirrored
) {
    auto it = primCache.find(primJob.prim);
    if (it != primCache.end()) {
        return it->second;
    }

    // Create actual Vulkan buffers
    BufferHandle posVbo  = ctx.CreateVertexBuffer(primJob.positions.data(), primJob.positions.size() * sizeof(VertexPosition), sizeof(VertexPosition));
    BufferHandle attrVbo = ctx.CreateVertexBuffer(primJob.attributes.data(), primJob.attributes.size() * sizeof(VertexAttributes), sizeof(VertexAttributes));

    BufferHandle skinVbo = BufferHandle::Invalid;
    if (!primJob.skins.empty()) {
        skinVbo = ctx.CreateVertexBuffer(primJob.skins.data(), primJob.skins.size() * sizeof(VertexSkin), sizeof(VertexSkin));
    }

    BufferHandle ibo = BufferHandle::Invalid;
    if (primJob.indexCount > 0) {
        ibo = ctx.CreateIndexBuffer(primJob.indices.data(), primJob.indexCount * sizeof(uint32_t));
    }

    Mesh subMesh = {
        .posBuffer   = posVbo,
        .attrBuffer  = attrVbo,
        .skinBuffer  = skinVbo,
        .indexBuffer = ibo,
        .vertexCount = static_cast<uint32_t>(primJob.positions.size()),
        .indexCount  = primJob.indexCount
    };

    auto res = ctx.BuildMeshBLAS(subMesh);
    if (!res) [[unlikely]] {
        if (!res.error().Is(VulkanCallError::FeatureNotPresent)) {
            ZHLN::Log("WARNING: glTFImporter: Failed to build mesh BLAS: {}", res.error().Message());
        }
    }

    // Allocate morph targets on GPU
    uint32_t finalMorphOffset = 0;
    if (primJob.activeMorphCount > 0) {
        finalMorphOffset = ctx.AllocateMorphDeltas(static_cast<uint32_t>(primJob.positions.size()) * primJob.activeMorphCount, primJob.tempDeltas.data());
    }

    // Map Materials and assign Bindless Indices
    auto subMaterial_res = CreateBasicMaterial(ctx, primJob.doubleSided || isMirrored, primJob.alphaBlend);
    if (!subMaterial_res) {
        ZHLN::Panic("Failed to create primitive material in glTF Importer: {}", ToString(subMaterial_res.error()));
    }

    Material subMaterial        = subMaterial_res.value();
    subMaterial.alphaMode       = primJob.alphaMode;
    subMaterial.alphaCutoff     = primJob.alphaCutoff;
    subMaterial.metallicFactor  = primJob.metallicFactor;
    subMaterial.roughnessFactor = primJob.roughnessFactor;
    std::memcpy(subMaterial.baseColorFactor, primJob.baseColorFactor, sizeof(float) * 4);

    auto GetBindlessIndex = [&](cgltf_image* img, uint32_t defaultIdx) -> uint32_t {
        if (!img) {
            return defaultIdx;
        }
        auto texIt = imageToBindlessIdx.find(img);
        return (texIt != imageToBindlessIdx.end()) ? texIt->second : defaultIdx;
    };

    // Bake on-the-fly and bind to VRAM on the main thread safely
    if (primJob.hasProcedural) {
        // Determine variant based on parsed type string
        uint32_t variantIdx = 0; // Default: VORONOI
        if (primJob.proceduralType == "PERLIN_NOISE") {
            variantIdx = 1;
        } else if (primJob.proceduralType == "WAVE_MARBLE") {
            variantIdx = 2;
        }

        // Clean, decoupled, Vulkan-free PIMPL call
        uint32_t bakedIndex     = ctx.BakeProceduralTexture(512, 512, variantIdx, primJob.proceduralScale, primJob.proceduralRandomness);
        subMaterial.albedoIndex = bakedIndex;
    } else {
        subMaterial.albedoIndex = GetBindlessIndex(primJob.albedoImage, 1);
    }

    subMaterial.normalIndex   = GetBindlessIndex(primJob.normalImage, 2);
    subMaterial.pbrIndex      = GetBindlessIndex(primJob.pbrImage, 0);
    subMaterial.emissiveIndex = GetBindlessIndex(primJob.emissiveImage, 1);
    std::memcpy(subMaterial.emissiveFactor, primJob.emissiveFactor, sizeof(float) * 4);

    CompiledPrimitive compPrim = {
        .mesh             = subMesh,
        .defaultMaterial  = subMaterial,
        .boundingRadius   = primJob.boundingRadius,
        .localMin         = {primJob.localMin[0], primJob.localMin[1], primJob.localMin[2]},
        .localMax         = {primJob.localMax[0], primJob.localMax[1], primJob.localMax[2]},
        .meshCollider     = primJob.meshCollider,
        .boxCollider      = primJob.boxCollider,
        .morphOffset      = finalMorphOffset,
        .activeMorphCount = primJob.activeMorphCount
    };

    primCache[primJob.prim] = compPrim;
    return compPrim;
}

} // namespace

// ============================================================================
// REFACTORED MAIN IMPORT FUNCTION
// ============================================================================

ModelPrefab* LoadModelPrefab(RenderContext& ctx, CreativeWorksManager& assetMgr, std::string_view path) {
    uint64_t hash = HashCreativeWorkPath(path);
    if (auto* cached = assetMgr.GetCachedPrefab(hash)) {
        return cached;
    }

    cgltf_options opts {};
    cgltf_data*   data = nullptr;

    std::string pathStr(path);
    std::string rawPath = "resources/assets/" + pathStr;

    if (cgltf_parse_file(&opts, rawPath.c_str(), &data) != cgltf_result_success) {
        Log("ERROR: Failed to parse GLB: {}", rawPath);
        return nullptr;
    }

    if (cgltf_load_buffers(&opts, data, rawPath.c_str()) != cgltf_result_success) {
        Log("ERROR: Failed to load GLB buffers: {}", rawPath);
        cgltf_free(data);
        return nullptr;
    }

    auto prefab         = std::make_unique<ModelPrefab>();
    prefab->virtualPath = String256(pathStr);
    prefab->rawData     = data;

    // 1. Decompose static matrices
    DecomposeStaticMatrices(data);

    // 2. Identify unique images and primitives
    std::vector<cgltf_image*>    uniqueImages;
    std::vector<CPUPrimitiveJob> primitiveJobs;
    GatherImagesAndPrimitiveJobs(data, uniqueImages, primitiveJobs);

    // 3. Process CPU-Parallel tasks
    JPH::Array<CPUTextureJob> textureJobs;
    ProcessCPUTasks(rawPath, uniqueImages, primitiveJobs, textureJobs);

    // 4. Upload textures to GPU (Main-thread bound)
    auto imageToBindlessIdx = UploadTexturesToGPU(ctx, textureJobs);

    // 5. Construct GPU meshes & materials sequentially using the primitive cache
    std::unordered_map<const cgltf_primitive*, CompiledPrimitive> primCache;
    JPH::Array<ModelPart>                                         totalParts;

    for (const auto& primJob: primitiveJobs) {
        const auto* node       = primJob.node;
        bool        isMirrored = (primJob.nodeTransform.GetDeterminant3x3() < 0.0f);

        // Resolve unique primitive buffers
        CompiledPrimitive compPrim = GetOrCreateCompiledPrimitive(ctx, primJob, imageToBindlessIdx, primCache, isMirrored);

        // Configure local instanced properties
        Material activeMaterial = compPrim.defaultMaterial;
        if (isMirrored && activeMaterial.pipeline != PipelineHandle::Invalid) {
            // Upgrade base material to a double-sided pipeline variant on mirrored nodes
            auto mirroredMat_res = CreateBasicMaterial(ctx, true, activeMaterial.alphaMode == 2);
            if (!mirroredMat_res) {
                ZHLN::Panic("Failed to compile mirrored double-sided material in glTF Importer: {}", ToString(mirroredMat_res.error()));
            }
            Material mirroredMat        = mirroredMat_res.value();
            mirroredMat.albedoIndex     = activeMaterial.albedoIndex;
            mirroredMat.normalIndex     = activeMaterial.normalIndex;
            mirroredMat.pbrIndex        = activeMaterial.pbrIndex;
            mirroredMat.emissiveIndex   = activeMaterial.emissiveIndex;
            mirroredMat.alphaMode       = activeMaterial.alphaMode;
            mirroredMat.alphaCutoff     = activeMaterial.alphaCutoff;
            mirroredMat.metallicFactor  = activeMaterial.metallicFactor;
            mirroredMat.roughnessFactor = activeMaterial.roughnessFactor;
            std::memcpy(mirroredMat.baseColorFactor, activeMaterial.baseColorFactor, sizeof(float) * 4);
            std::memcpy(mirroredMat.emissiveFactor, activeMaterial.emissiveFactor, sizeof(float) * 4);
            activeMaterial = mirroredMat;
        }

        ModelPart part = {
            .name             = (node->name != nullptr) ? String64(node->name) : String64("Unnamed"),
            .mesh             = compPrim.mesh,
            .defaultMaterial  = activeMaterial,
            .localTransform   = primJob.nodeTransform,
            .jointOffset      = 0,
            .isSkinned        = (node->skin != nullptr),
            .gltfNode         = const_cast<cgltf_node*>(node),
            .gltfSkin         = node->skin,
            .morphOffset      = compPrim.morphOffset,
            .activeMorphCount = compPrim.activeMorphCount,
            .defaultMorphWeights =
                {primJob.defaultMorphWeights[0], primJob.defaultMorphWeights[1], primJob.defaultMorphWeights[2],
                 primJob.defaultMorphWeights[3]}, // Fixed: Read from primJob instead of compPrim
            .boundingRadius = compPrim.boundingRadius,
            .localMin       = {compPrim.localMin[0], compPrim.localMin[1], compPrim.localMin[2]},
            .localMax       = {compPrim.localMax[0], compPrim.localMax[1], compPrim.localMax[2]},
            .meshCollider   = compPrim.meshCollider,
            .boxCollider    = compPrim.boxCollider
        };

        totalParts.push_back(part);
    }

    prefab->partCount = static_cast<uint32_t>(totalParts.size());
    prefab->parts     = new ModelPart[prefab->partCount];
    for (uint32_t i = 0; i < prefab->partCount; ++i) {
        prefab->parts[i] = totalParts[i];
    }

    Log("Parallel Loaded GLB Prefab: {} ({} unique mesh parts compiled)", path, prefab->partCount);

    ModelPrefab* result = prefab.release();
    assetMgr.CachePrefab(hash, result);
    return result;
}

namespace {

// ----------------------------------------------------------------------------
// HELPER 1: Safe glTF World Matrix extractor
// ----------------------------------------------------------------------------
JPH::Mat44 GetNodeWorldMat(const cgltf_node* node, const JPH::Mat44& baseTransform) {
    float m[16];
    cgltf_node_transform_world(node, m);
    JPH::Mat44 local(
        JPH::Vec4(m[0], m[1], m[2], m[3]), JPH::Vec4(m[4], m[5], m[6], m[7]), JPH::Vec4(m[8], m[9], m[10], m[11]), JPH::Vec4(m[12], m[13], m[14], m[15])
    );
    return baseTransform * local;
}

// ----------------------------------------------------------------------------
// HELPER 2: Non-physics root structural anchor
// ----------------------------------------------------------------------------
Entity SpawnPrefabRoot(ECS::Registry& reg, std::string_view vPath, const SpawnParams& p) {
    Entity root = reg.Create();
    reg.Add(
        root, Components::TransformComponent {
                  .position = JPH::Vec3(p.position), // Natively casts RVec3 to float Vec3
                  .rotation = p.rotation,
                  .scale    = p.scale
              }
    );
    reg.Add(root, Components::NameComponent {.name = String64("Root_" + std::string(vPath))});
    return root;
}

// ----------------------------------------------------------------------------
// HELPER 3: Parallel math & Jolt Shape scaling pass
// ----------------------------------------------------------------------------
void PreparePrefabPhysics(
    const ModelPrefab&         prefab,
    const JPH::Mat44&          baseTransform,
    bool                       createPhysics,
    bool                       useBoxColliders,
    std::vector<PreparedPart>& outPrepared
) {
    outPrepared.resize(prefab.partCount);

    TaskSystem::ParallelFor(prefab.partCount, 16, [&](uint32_t start, uint32_t end, uint32_t) {
        for (uint32_t i = start; i < end; ++i) {
            const auto& part = prefab.parts[i];
            auto&       prep = outPrepared[i];

            JPH::Mat44 finalLocal = baseTransform * part.localTransform;

            prep.scale = JPH::Vec3(finalLocal.GetColumn3(0).Length(), finalLocal.GetColumn3(1).Length(), finalLocal.GetColumn3(2).Length());

            // ============================================================================
            // PRESERVE REFLECTION (NEGATIVE SCALE)
            // ============================================================================
            if (finalLocal.GetDeterminant3x3() < 0.0f) {
                prep.scale.SetX(-prep.scale.GetX());
            }
            // ============================================================================

            prep.maxScale    = std::max({std::abs(prep.scale.GetX()), std::abs(prep.scale.GetY()), std::abs(prep.scale.GetZ())});
            prep.translation = finalLocal.GetTranslation();

            // Strip sign from columns to extract a clean, uncorrupted rotation basis
            JPH::Vec3 absScale(std::abs(prep.scale.GetX()), std::abs(prep.scale.GetY()), std::abs(prep.scale.GetZ()));
            JPH::Vec3 c0 = absScale.GetX() > 1e-6f ? finalLocal.GetColumn3(0) / prep.scale.GetX() : JPH::Vec3::sAxisX();
            JPH::Vec3 c1 = absScale.GetY() > 1e-6f ? finalLocal.GetColumn3(1) / prep.scale.GetY() : JPH::Vec3::sAxisY();
            JPH::Vec3 c2 = absScale.GetZ() > 1e-6f ? finalLocal.GetColumn3(2) / prep.scale.GetZ() : JPH::Vec3::sAxisZ();

            JPH::Mat44 rotMat(JPH::Vec4(c0, 0), JPH::Vec4(c1, 0), JPH::Vec4(c2, 0), JPH::Vec4(0, 0, 0, 1));
            prep.rotation = rotMat.GetQuaternion().Normalized();

            if (createPhysics) {
                JPH::ShapeRefC rawShape = useBoxColliders ? part.boxCollider : part.meshCollider;
                if (rawShape != nullptr) {
                    prep.shape = !prep.scale.IsClose(JPH::Vec3::sReplicate(1.0f), 1e-5f) ? new JPH::ScaledShape(rawShape, prep.scale) : rawShape;
                }
            }
        }
    });
}

// ----------------------------------------------------------------------------
// HELPER 4: Single Renderable Part Spawner
// ----------------------------------------------------------------------------
Entity InstantiateMeshPart(
    RenderContext&                             ctx,
    ECS::Registry&                             reg,
    PhysicsContext&                            pc,
    const ModelPart&                           part,
    const PreparedPart&                        prep,
    const SpawnParams&                         params,
    Entity                                     rootEntity,
    cgltf_data*                                gltfData,
    std::unordered_map<cgltf_skin*, uint32_t>& allocatedSkins
) {
    BufferHandle skinnedVbo = BufferHandle::Invalid;
    if (part.isSkinned && params.isAnimated) {
        skinnedVbo = ctx.CreateSkinnedScratchBuffer(part.mesh.vertexCount);
    }

    uint32_t assignedJointOffset = 0;
    if (part.isSkinned && params.isAnimated) {
        auto it = allocatedSkins.find(part.gltfSkin);
        if (it != allocatedSkins.end()) {
            assignedJointOffset = it->second;
        } else {
            assignedJointOffset           = JointAllocator::Allocate(static_cast<uint32_t>(part.gltfSkin->joints_count));
            allocatedSkins[part.gltfSkin] = assignedJointOffset;
        }
    }

    Entity   e         = reg.Create();
    Material activeMat = params.materialOverride.pipeline != PipelineHandle::Invalid ? params.materialOverride : part.defaultMaterial;

    // Resolve runtime DrawFlags
    DrawFlags flags = DrawFlags::None;
    if (part.isSkinned && params.isAnimated) {
        flags |= DrawFlags::Skinned;
    }
    // Exclude from the Raytracing Acceleration Structure if the material is transparent
    if (activeMat.alphaMode == 2) {
        flags |= DrawFlags::ExcludeFromTLAS;
    }

    if (params.isAnimated) {
        flags |= DrawFlags::ExcludeFromTLAS;
    }

    if (params.createPhysics) {
        reg.Add(e, Components::TransformComponent {.position = prep.translation, .rotation = prep.rotation, .scale = prep.scale});
        reg.Add(
            e, Components::MeshComponent {
                   .mesh        = part.mesh,
                   .material    = activeMat,
                   .cullRadius  = part.boundingRadius,
                   .localCenter = JPH::Vec3(
                       (part.localMax[0] + part.localMin[0]) * 0.5f, (part.localMax[1] + part.localMin[1]) * 0.5f, (part.localMax[2] + part.localMin[2]) * 0.5f
                   ),
                   .localTransform      = JPH::Mat44::sIdentity(),
                   .prevTransform       = JPH::Mat44::sIdentity(),
                   .worldTransform      = JPH::Mat44::sIdentity(),
                   .jointOffset         = assignedJointOffset,
                   .isSkinned           = part.isSkinned && params.isAnimated,
                   .skinnedVertexBuffer = skinnedVbo,
                   .morphOffset         = part.morphOffset,
                   .activeMorphCount    = part.activeMorphCount,
                   .morphWeights        = {part.defaultMorphWeights[0], part.defaultMorphWeights[1], part.defaultMorphWeights[2], part.defaultMorphWeights[3]},
                   .gltfNode            = part.gltfNode,
                   .gltfSkin            = part.gltfSkin,
                   .flags               = flags
               }
        );
        reg.Add(e, Components::NameComponent {.name = part.name});

        if (part.isSkinned && params.isAnimated) {
            reg.Add(
                e, Components::AnimatorComponent {
                       .currentTrackIdx      = 0, // Default to first track (usually Idle)
                       .currentTrackTime     = 0.0f,
                       .currentPlaybackSpeed = 1.0f,
                       .currentLoop          = true,
                       .prevTrackIdx         = -1,
                       .prevTrackTime        = 0.0f,
                       .prevPlaybackSpeed    = 1.0f,
                       .blendFactor          = 1.0f,
                       .blendDuration        = 0.15f,
                       .isFinished           = false,
                       .gltfData             = gltfData // Safely linked to the source asset tree
                   }
            );
        }

        if (prep.shape != nullptr) {
            reg.Add(
                e,
                Components::PhysicsComponent {Physics::CreateRigidBody(
                    pc, prep.shape, JPH::RVec3(prep.translation), prep.rotation, params.isStaticPhysics ? JPH::EMotionType::Static : JPH::EMotionType::Dynamic,
                    params.isStaticPhysics ? static_cast<JPH::ObjectLayer>(0) : static_cast<JPH::ObjectLayer>(1), 0, params.physicsCategory, params.physicsMask
                )}
            );

            // ============================================================================
            // ONLY ADD INTERPOLATION STATE TO DYNAMIC (MOVING) OBJECTS
            // ============================================================================
            if (!params.isStaticPhysics) {
                reg.Add(
                    e, Components::PhysicsStateComponent {
                           .currPosition = prep.translation, .prevPosition = prep.translation, .currRotation = prep.rotation, .prevRotation = prep.rotation
                       }
                );
            }
            // ============================================================================
        }
    } else {
        JPH::Vec3 nodePos = part.localTransform.GetTranslation();
        JPH::Vec3 nodeScale(part.localTransform.GetColumn3(0).Length(), part.localTransform.GetColumn3(1).Length(), part.localTransform.GetColumn3(2).Length());

        JPH::Vec3 nc0 = nodeScale.GetX() > 1e-6f ? part.localTransform.GetColumn3(0) / nodeScale.GetX() : JPH::Vec3::sAxisX();
        JPH::Vec3 nc1 = nodeScale.GetY() > 1e-6f ? part.localTransform.GetColumn3(1) / nodeScale.GetY() : JPH::Vec3::sAxisY();
        JPH::Vec3 nc2 = nodeScale.GetZ() > 1e-6f ? part.localTransform.GetColumn3(2) / nodeScale.GetZ() : JPH::Vec3::sAxisZ();

        JPH::Mat44 nRotMat(JPH::Vec4(nc0, 0), JPH::Vec4(nc1, 0), JPH::Vec4(nc2, 0), JPH::Vec4(0, 0, 0, 1));

        reg.Add(e, Components::TransformComponent {.position = nodePos, .rotation = nRotMat.GetQuaternion().Normalized(), .scale = nodeScale});
        reg.Add(
            e, Components::MeshComponent {
                   .mesh     = part.mesh,
                   .material = activeMat,

                   .cullRadius = part.boundingRadius,

                   .localCenter = JPH::Vec3(
                       (part.localMax[0] + part.localMin[0]) * 0.5f, (part.localMax[1] + part.localMin[1]) * 0.5f, (part.localMax[2] + part.localMin[2]) * 0.5f
                   ),
                   .localTransform      = part.isSkinned ? JPH::Mat44::sIdentity() : part.localTransform,
                   .prevTransform       = part.isSkinned ? JPH::Mat44::sIdentity() : part.localTransform,
                   .worldTransform      = JPH::Mat44::sIdentity(),
                   .jointOffset         = assignedJointOffset,
                   .isSkinned           = part.isSkinned && params.isAnimated,
                   .skinnedVertexBuffer = skinnedVbo,
                   .morphOffset         = part.morphOffset,
                   .activeMorphCount    = part.activeMorphCount,
                   .morphWeights        = {part.defaultMorphWeights[0], part.defaultMorphWeights[1], part.defaultMorphWeights[2], part.defaultMorphWeights[3]},
                   .gltfNode            = part.gltfNode,
                   .gltfSkin            = part.gltfSkin,
                   .flags               = flags
               }
        );
        reg.Add(e, Components::NameComponent {.name = part.name});
        reg.Add(e, Components::HierarchyComponent {.parent = rootEntity});
        if (part.isSkinned && params.isAnimated) {
            reg.Add(
                e, Components::AnimatorComponent {
                       .currentTrackIdx      = 0, // Default to first track (usually Idle)
                       .currentTrackTime     = 0.0f,
                       .currentPlaybackSpeed = 1.0f,
                       .currentLoop          = true,
                       .prevTrackIdx         = -1,
                       .prevTrackTime        = 0.0f,
                       .prevPlaybackSpeed    = 1.0f,
                       .blendFactor          = 1.0f,
                       .blendDuration        = 0.15f,
                       .isFinished           = false,
                       .gltfData             = gltfData // Safely linked to the source asset tree
                   }
            );
        }
    }
    return e;
}

// ----------------------------------------------------------------------------
// HELPER 5: Auto-Glow VPL Spawner for Emissive Meshes
// ----------------------------------------------------------------------------
Entity TrySpawnEmissiveVPL(ECS::Registry& reg, const ModelPart& part, const JPH::Mat44& baseTransform, float scaleMult) {
    const float* ef  = part.defaultMaterial.emissiveFactor;
    float        lum = ef[0] * 0.2126f + ef[1] * 0.7152f + ef[2] * 0.0722f;
    if (lum <= 0.01f) {
        return NullEntity;
    }

    JPH::Vec3 localCenter(
        (part.localMax[0] + part.localMin[0]) * 0.5f, (part.localMax[1] + part.localMin[1]) * 0.5f, (part.localMax[2] + part.localMin[2]) * 0.5f
    );

    JPH::Mat44 worldMat   = baseTransform * part.localTransform;
    float      partExtent = (part.localMax[0] - part.localMin[0]) + (part.localMax[1] - part.localMin[1]) + (part.localMax[2] - part.localMin[2]);

    Entity glowEnt = reg.Create();
    reg.Add(
        glowEnt, Components::TransformComponent {.position = worldMat * localCenter, .rotation = JPH::Quat::sIdentity(), .scale = JPH::Vec3::sReplicate(1.0f)}
    );
    reg.Add(glowEnt, Components::NameComponent {.name = String64("Glow_" + std::string(part.name.c_str()))});
    reg.Add(
        glowEnt, Components::LightComponent {
                     .type      = LightType::Point,
                     .color     = JPH::Vec3(ef[0], ef[1], ef[2]),
                     .intensity = lum * 35.0f,
                     .radius    = std::max(partExtent * scaleMult * 0.15f, 0.05f),
                     .direction = JPH::Vec3(0, -1, 0),
                     .range     = std::max(partExtent * scaleMult * 2.5f, 3.0f),
                     .points    = {},
                     .twoSided  = 0
                 }
    );
    return glowEnt;
}

// ----------------------------------------------------------------------------
// HELPER 6: KHR_lights_punctual glTF Authored Light Spawner
// ----------------------------------------------------------------------------
uint32_t InstantiateAuthoredLights(
    ECS::Registry&    reg,
    const cgltf_data* rawData,
    const JPH::Mat44& baseTransform,
    Entity*           outBuffer,
    uint32_t          startIdx,
    uint32_t          maxCount
) {
    if (rawData == nullptr) {
        return 0;
    }
    uint32_t count = 0;

    for (cgltf_size i = 0; i < rawData->nodes_count; ++i) {
        const cgltf_node* node = &rawData->nodes[i];
        if (node->light == nullptr) {
            continue;
        }

        JPH::Mat44 worldMat = GetNodeWorldMat(node, baseTransform);
        JPH::Vec3  dir      = -worldMat.GetColumn3(2); // Local -Z emission axis
        dir                 = (dir.LengthSq() > 1e-6f) ? dir.Normalized() : JPH::Vec3(0, -1, 0);

        const cgltf_light* l    = node->light;
        LightType          type = (l->type == cgltf_light_type_directional) ? LightType::Directional :
                                  (l->type == cgltf_light_type_spot)        ? LightType::Spot :
                                                                              LightType::Point;

        Entity ent = reg.Create();
        reg.Add(
            ent, Components::TransformComponent {
                     .position = worldMat.GetTranslation(), .rotation = worldMat.GetQuaternion().Normalized(), .scale = JPH::Vec3::sReplicate(1.0f)
                 }
        );
        reg.Add(ent, Components::NameComponent {.name = (node->name != nullptr) ? String64(node->name) : String64("GLB_Light")});
        reg.Add(
            ent, Components::LightComponent {
                     .type        = type,
                     .color       = JPH::Vec3(l->color[0], l->color[1], l->color[2]),
                     .intensity   = l->intensity,
                     .radius      = 0.1f,
                     .direction   = dir,
                     .range       = (l->range > 0.0f) ? l->range : 100.0f,
                     .points      = {},
                     .twoSided    = 0,
                     .shadowLayer = -1
                 }
        );

        if (outBuffer != nullptr && (startIdx + count) < maxCount) {
            outBuffer[startIdx + count] = ent;
        }
        count++;
    }
    return count;
}

} // namespace

uint32_t InstantiatePrefab(
    RenderContext&     ctx,
    ECS::Registry&     reg,
    PhysicsContext&    pc,
    const ModelPrefab& prefab,
    const SpawnParams& params,
    Entity*            outBuffer,
    uint32_t           maxCount
) {
    uint32_t spawnedCount = 0;

    if (params.isAnimated && prefab.rawData != nullptr) {
        if (std::ranges::find(s_AnimatedGLBs, prefab.rawData) == s_AnimatedGLBs.end()) {
            s_AnimatedGLBs.push_back(prefab.rawData);
        }
    }

    Entity   rootEntity = NullEntity;
    uint32_t startIndex = 0;

    if (!params.createPhysics) {
        rootEntity = SpawnPrefabRoot(reg, prefab.virtualPath, params);
        if (outBuffer != nullptr && maxCount > 0) {
            outBuffer[0] = rootEntity;
            startIndex   = 1;
            spawnedCount = 1;
        }
    }

    JPH::Mat44 baseTransform = Math::CreateTransform(JPH::Vec3(params.position), params.rotation, params.scale);

    std::vector<PreparedPart> preparedParts;
    PreparePrefabPhysics(prefab, baseTransform, params.createPhysics, params.useBoxColliders, preparedParts);

    float scaleMult = std::max({params.scale.GetX(), params.scale.GetY(), params.scale.GetZ()});

    std::unordered_map<cgltf_skin*, uint32_t> allocatedSkins;

    for (uint32_t i = 0; i < prefab.partCount; ++i) {
        Entity meshEnt = InstantiateMeshPart(ctx, reg, pc, prefab.parts[i], preparedParts[i], params, rootEntity, prefab.rawData, allocatedSkins);
        if (outBuffer != nullptr && spawnedCount < maxCount) {
            outBuffer[startIndex + (spawnedCount - startIndex)] = meshEnt;
        }
        spawnedCount++;

        // Auto-spawn companion Point Light for glowing meshes
        Entity glowEnt = TrySpawnEmissiveVPL(reg, prefab.parts[i], baseTransform, scaleMult);
        if (glowEnt != NullEntity) {
            if (outBuffer != nullptr && spawnedCount < maxCount) {
                outBuffer[spawnedCount] = glowEnt;
            }
            spawnedCount++;
        }
    }

    // Instantiate authored KHR_lights_punctual glTF lights
    spawnedCount += InstantiateAuthoredLights(reg, prefab.rawData, baseTransform, outBuffer, spawnedCount, maxCount);

    return spawnedCount;
}

// Helper to compile Jolt Skeleton on-the-fly from glTF joint hierarchies
static JPH::Ref<JPH::Skeleton> BuildJoltSkeletonFromCgltf(const cgltf_skin* skin) {
    auto* skeleton = new JPH::Skeleton();
    for (size_t i = 0; i < skin->joints_count; ++i) {
        cgltf_node* jointNode  = skin->joints[i];
        std::string name       = (jointNode->name != nullptr) ? jointNode->name : "joint_" + std::to_string(i);
        std::string parentName = ((jointNode->parent != nullptr) && (jointNode->parent->name != nullptr)) ? jointNode->parent->name : "";
        skeleton->AddJoint(name, parentName);
    }
    skeleton->CalculateParentJointIndices();
    return skeleton;
}

void SetupPlayerRagdoll([[maybe_unused]] RenderContext& rc, PhysicsContext& pc, ECS::Registry& reg, Entity playerEntity, std::span<const Entity> visualParts) {
    const cgltf_skin* pomniSkin = nullptr;
    for (Entity part: visualParts) {
        if (auto* meshComp = reg.Get<Components::MeshComponent>(part)) {
            if (meshComp->gltfSkin != nullptr) {
                pomniSkin = static_cast<const cgltf_skin*>(meshComp->gltfSkin);
                break;
            }
        }
    }

    if (pomniSkin != nullptr) {
        auto skeleton = BuildJoltSkeletonFromCgltf(pomniSkin);

        // 1. Define which joint names are allowed to be physical
        auto IsImportantJoint = [](std::string name) -> bool {
            std::ranges::transform(name, name.begin(), ::tolower);
            return name.contains("hip") || name.contains("pelvis") || name.contains("root") || name.contains("spine") || name.contains("chest") ||
                   name.contains("torso") || name.contains("head") || name.contains("neck") || name.contains("upper_arm") || name.contains("forearm") ||
                   name.contains("thigh") || name.contains("calf") || name.contains("shin");
        };

        // 2. Define the GetNodeWorldTRS lambda before the loop
        auto GetNodeWorldTRS = [](const cgltf_node* node, JPH::RVec3& outPos, JPH::Quat& outRot) {
            float m[16];
            cgltf_node_transform_world(node, m);
            outPos = JPH::RVec3(m[12], m[13], m[14]);

            JPH::Vec3 col0(m[0], m[1], m[2]);
            JPH::Vec3 col1(m[4], m[5], m[6]);
            JPH::Vec3 col2(m[8], m[9], m[10]);

            JPH::Mat44 rotMat(JPH::Vec4(col0, 0), JPH::Vec4(col1, 0), JPH::Vec4(col2, 0), JPH::Vec4(0, 0, 0, 1));
            outRot = rotMat.GetQuaternion().Normalized();
        };

        std::vector<Physics::RagdollPartParams> parts;
        // Process every single joint in the skeleton to satisfy Jolt's 1-on-1 requirements
        for (size_t i = 0; i < pomniSkin->joints_count; ++i) {
            std::string name = (pomniSkin->joints[i]->name != nullptr) ? pomniSkin->joints[i]->name : "";

            Physics::RagdollPartParams part;
            part.jointIndex       = (uint32_t) i;
            part.parentJointIndex = skeleton->GetJoint(i).mParentJointIndex;
            part.mass             = 1.0f;
            part.enableMotors     = false;

            GetNodeWorldTRS(pomniSkin->joints[i], part.position, part.rotation);

            // Map specific shapes to key bones
            std::ranges::transform(name, name.begin(), ::tolower);
            if (name.contains("hip") || name.contains("pelvis") || name.contains("root")) {
                part.shape = Physics::GetOrCreateShape(pc, Physics::ShapeType::Capsule, 0.4f, 0.2f);
                part.mass  = 15.0f;
            } else if (name.contains("spine") || name.contains("chest") || name.contains("torso")) {
                part.shape         = Physics::GetOrCreateShape(pc, Physics::ShapeType::Capsule, 0.5f, 0.25f);
                part.mass          = 20.0f;
                part.enableMotors  = true;
                part.maxMotorForce = 250.0f;
            } else if (name.contains("head") || name.contains("neck")) {
                part.shape         = Physics::GetOrCreateShape(pc, Physics::ShapeType::Sphere, 0.3f);
                part.mass          = 8.0f;
                part.enableMotors  = true;
                part.maxMotorForce = 250.0f;
            } else if (IsImportantJoint(name)) {
                // Default limb shape (small capsule)
                part.shape = Physics::GetOrCreateShape(pc, Physics::ShapeType::Capsule, 0.2f, 0.1f);
                part.mass  = 3.0f;
            } else {
                // Non-essential joints (fingers, hair, etc.) get a stable, lightweight dummy shape
                // safely above the 0.05m Jolt convex radius to prevent division-by-zero scale
                // errors
                part.shape = Physics::GetOrCreateShape(pc, Physics::ShapeType::Sphere, 0.08f);
                part.mass  = 0.5f;
            }

            parts.push_back(part);
        }

        // Create the skeletal ragdoll using the fully populated parts list
        auto ragdollInstance = Physics::CreateSkeletalRagdoll(pc, skeleton.GetPtr(), parts);
        ragdollInstance->AddRef();

        uint32_t jointOffset = 0;
        if (!visualParts.empty()) {
            if (auto* meshComp = reg.Get<Components::MeshComponent>(visualParts[0])) {
                jointOffset = meshComp->jointOffset;
            }
        }

        reg.Add(
            playerEntity, Components::RagdollComponent {
                              .ragdollInstance  = ragdollInstance.GetPtr(),
                              .state            = RagdollState::Inactive,
                              .prevState        = RagdollState::Inactive,
                              .isAddedToPhysics = 0,
                              .jointOffset      = jointOffset,
                              .jointCount       = (uint32_t) pomniSkin->joints_count,
                              .gltfSkin         = const_cast<cgltf_skin*>(pomniSkin)
                          }
        );
        Log("Skeletal Ragdoll successfully generated with simplified key bones.");
    } else {
        Log("WARNING: SetupPlayerRagdoll failed because no skeletal skin was found.");
    }
}

void ReuploadAllPrefabs(RenderContext& ctx, CreativeWorksManager& assetMgr, std::unordered_map<BufferHandle, std::pair<Mesh, Material>>& outMeshRebuildMap) {
    // 1. Query the actual number of active cached prefabs
    uint32_t count = assetMgr.GetCachedPrefabs(nullptr, 0);
    if (count == 0) {
        return;
    }

    // 2. Allocate the local vector and retrieve the pointers safely under lock
    std::vector<ModelPrefab*> prefabs(count);
    assetMgr.GetCachedPrefabs(prefabs.data(), count);

    for (auto* prefab: prefabs) {
        cgltf_data* data = prefab->rawData;
        if (data == nullptr) {
            continue;
        }

        // Gather all tasks based on the glTF data
        std::vector<cgltf_image*>    uniqueImages;
        std::vector<CPUPrimitiveJob> primitiveJobs;
        GatherImagesAndPrimitiveJobs(data, uniqueImages, primitiveJobs);

        JPH::Array<CPUTextureJob> textureJobs;
        ProcessCPUTasks(std::string(prefab->virtualPath.c_str()), uniqueImages, primitiveJobs, textureJobs);

        // Upload texture resources
        auto imageToBindlessIdx = UploadTexturesToGPU(ctx, textureJobs);

        std::unordered_map<const cgltf_primitive*, CompiledPrimitive> primCache;

        for (size_t i = 0; i < primitiveJobs.size(); ++i) {
            const auto& primJob    = primitiveJobs[i];
            bool        isMirrored = (primJob.nodeTransform.GetDeterminant3x3() < 0.0f);

            // Compile and upload the primitive's mesh and material
            CompiledPrimitive compPrim = GetOrCreateCompiledPrimitive(ctx, primJob, imageToBindlessIdx, primCache, isMirrored);

            if (i < prefab->partCount) {
                // Record the mapping: OLD posBuffer handle -> NEW Mesh / Material
                BufferHandle oldPosBuffer = prefab->parts[i].mesh.posBuffer;
                if (oldPosBuffer != BufferHandle::Invalid) {
                    outMeshRebuildMap[oldPosBuffer] = {compPrim.mesh, compPrim.defaultMaterial};
                }

                // Update the cached prefab's internal part handles
                prefab->parts[i].mesh            = compPrim.mesh;
                prefab->parts[i].defaultMaterial = compPrim.defaultMaterial;
            }
        }
    }
}

void RebuildVulkanResources(RenderContext& ctx, CreativeWorksManager& assetMgr, ECS::Registry& reg) {
    ZHLN::Log("[Engine] Re-uploading all textures and meshes to new Vulkan device...");

    // 1. Re-bake system font atlas
    CreateFontAtlasTexture(ctx);

    // 2. Map of OLD posBuffer -> NEW Mesh and Material
    std::unordered_map<BufferHandle, std::pair<Mesh, Material>> meshRebuildMap;

    // 3. Re-upload all cached prefabs and populate the rebuild map
    ReuploadAllPrefabs(ctx, assetMgr, meshRebuildMap);

    // 4. Update existing MeshComponents in the registry
    auto entities = reg.GetEntitiesWith<Components::MeshComponent>();
    auto meshes   = reg.GetRawArray<Components::MeshComponent>();
    for (size_t i = 0; i < entities.size(); ++i) {
        Components::MeshComponent& meshComp = meshes[i];

        // Extract the old VBO handle to look up its newly compiled counterpart
        BufferHandle oldPosBuffer = meshComp.mesh.posBuffer;

        // If it is a skinned mesh, recreate its scratch buffer
        if (meshComp.isSkinned && meshComp.skinnedVertexBuffer != BufferHandle::Invalid) {
            meshComp.skinnedVertexBuffer = ctx.CreateSkinnedScratchBuffer(meshComp.mesh.vertexCount);
        }

        if (oldPosBuffer != BufferHandle::Invalid) {
            auto it = meshRebuildMap.find(oldPosBuffer);
            if (it != meshRebuildMap.end()) {
                meshComp.mesh     = it->second.first;
                meshComp.material = it->second.second;
            }
        }
    }

    // 5. Reset TextComponents so they rebuild their mesh buffers on the next frame
    for (Entity e: reg.GetEntitiesWith<Components::TextComponent>()) {
        if (auto* text = reg.Get<Components::TextComponent>(e)) {
            text->mesh.posBuffer   = BufferHandle::Invalid;
            text->mesh.attrBuffer  = BufferHandle::Invalid;
            text->mesh.indexBuffer = BufferHandle::Invalid;
        }
    }
}

} // namespace ZHLN::CreativeWorksFactory
