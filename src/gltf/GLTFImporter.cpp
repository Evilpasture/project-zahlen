// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// File: src/gltf/GLTFImporter.cpp

#include "GLTFImporter.hpp"
#include "Zahlen/JSON.hpp"
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Zahlen/CreativeWorksFactory.hpp>
#include <Zahlen/CreativeWorksManager.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Math3D.hpp>
#include <Zahlen/Render.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <Zahlen/physics/Physics.hpp>
#include <algorithm>
#include <cgltf.h>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <stb_image.h>
#include <unordered_map>
#include <vector>

namespace ZHLN::GLTF {

namespace {
struct NodeExtras {
    std::string csg_data; // Will hold the serialized JSON string of modifiers
};
// ============================================================================
// INTERNAL CPU PARSING & TEXTURE WORKSTRUCTURES
// ============================================================================

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

struct CompiledPrimitive {
    Mesh           mesh;
    Material       defaultMaterial;
    float          boundingRadius   = 1.0f;
    float          localMin[3]      = {0.0f, 0.0f, 0.0f};
    float          localMax[3]      = {0.0f, 0.0f, 0.0f};
    JPH::ShapeRefC meshCollider     = nullptr;
    JPH::ShapeRefC boxCollider      = nullptr;
    uint32_t       morphOffset      = 0;
    uint32_t       activeMorphCount = 0;
};

unsigned char* DownsampleHalfSize(const unsigned char* src, uint32_t currentW, uint32_t currentH) {
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

void DecodeAndRescaleTexture(CPUTextureJob& job) {
    int            channels = 0;
    unsigned char* pixels   = nullptr;

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

    unsigned char* currentSrc = pixels;
    uint32_t       currentW   = w;
    uint32_t       currentH   = h;

    for (uint32_t step = 0; step < scaleSteps; ++step) {
        unsigned char* nextDst = DownsampleHalfSize(currentSrc, currentW, currentH);
        if (nextDst == nullptr) {
            break;
        }

        if (currentSrc != pixels) {
            std::free(currentSrc);
        }

        currentSrc = nextDst;
        currentW /= 2;
        currentH /= 2;
    }

    if (currentSrc != pixels) {
        stbi_image_free(pixels);
        pixels          = currentSrc;
        job.width       = static_cast<int>(currentW);
        job.height      = static_cast<int>(currentH);
        job.wasRescaled = true;
    }

    job.decodedPixels = pixels;
}

void ProcessCPUPrimitive(CPUPrimitiveJob& job) {
    const auto& prim = *job.prim;
    const auto* node = job.node;

    cgltf_accessor* posAcc     = nullptr;
    cgltf_accessor* normAcc    = nullptr;
    cgltf_accessor* tangentAcc = nullptr;
    cgltf_accessor* uvAcc      = nullptr;
    cgltf_accessor* colorAcc   = nullptr;
    cgltf_accessor* jointsAcc  = nullptr;
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
            uint32_t rawIndex = static_cast<uint32_t>(cgltf_accessor_read_index(prim.indices, idx));

            /*
             * NOTE: Malformed or multi-primitive glTF files can contain index accessors whose
             * values exceed the local primitive's vertex count. On Vulkan, where vertex
             * positions are fetched via bindless raw buffer loads (`vk::RawBufferLoad`), an
             * out-of-bounds index causes an immediate GPU MMU page fault at heap boundaries,
             * resulting in VK_ERROR_DEVICE_LOST. We strictly clamp indices to [0, vertexCount - 1].
             */
            job.indices[idx] = std::min(rawIndex, static_cast<uint32_t>(vertexCount > 0 ? vertexCount - 1 : 0));
        }
    } else {
        job.indexCount = static_cast<uint32_t>(job.positions.size());
        job.indices.resize(job.indexCount);
        for (uint32_t idx = 0; idx < job.indexCount; ++idx) {
            job.indices[idx] = idx;
        }
    }

    JPH::Vec3 localCenter((job.localMax[0] + job.localMin[0]) * 0.5f, (job.localMax[1] + job.localMin[1]) * 0.5f, (job.localMax[2] + job.localMin[2]) * 0.5f);
    float     maxD2 = 0.0f;
    for (const auto& pos: job.positions) {
        float dx = pos.position[0] - localCenter.GetX();
        float dy = pos.position[1] - localCenter.GetY();
        float dz = pos.position[2] - localCenter.GetZ();
        maxD2    = std::max(dx * dx + dy * dy + dz * dz, maxD2);
    }
    job.boundingRadius = std::sqrt(maxD2) * 1.15f + 0.5f;

    float extentsX = (job.localMax[0] - job.localMin[0]) * 0.5f;
    float extentsY = (job.localMax[1] - job.localMin[1]) * 0.5f;
    float extentsZ = (job.localMax[2] - job.localMin[2]) * 0.5f;

    JPH::ShapeRefC baseBox = new JPH::BoxShape(JPH::Vec3(extentsX, extentsY, extentsZ));
    job.boxCollider        = new JPH::RotatedTranslatedShape(localCenter, JPH::Quat::sIdentity(), baseBox);

    if (prim.type == cgltf_primitive_type_triangles && jointsAcc == nullptr) {
        job.meshCollider = Physics::CreateMeshShape(job.positions.data(), static_cast<uint32_t>(job.positions.size()), job.indices.data(), job.indexCount);
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
            auto tex_res = ctx.CreateTexture(texJob.decodedPixels, texJob.width, texJob.height, texJob.isSRGB);
            if (texJob.wasRescaled) {
                std::free(texJob.decodedPixels);
            } else {
                stbi_image_free(texJob.decodedPixels);
            }

            uint32_t index = 1;
            if (tex_res) {
                index = tex_res.value();
            }
            imageToBindlessIdx[texJob.image] = index;
        } else {
            imageToBindlessIdx[texJob.image] = 1;
        }
    }
    return imageToBindlessIdx;
}

CompiledPrimitive GetOrCreateCompiledPrimitive(
    RenderContext&                                                 ctx,
    CreativeWorksManager&                                          cwMgr,
    const CPUPrimitiveJob&                                         primJob,
    const std::unordered_map<cgltf_image*, uint32_t>&              imageToBindlessIdx,
    std::unordered_map<const cgltf_primitive*, CompiledPrimitive>& primCache,
    bool                                                           isMirrored
) {
    auto it = primCache.find(primJob.prim);
    if (it != primCache.end()) {
        return it->second;
    }

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

    if (auto res = ctx.BuildMeshBLAS(subMesh); !res) [[unlikely]] {
        if (!res.error().Is(VulkanCallError::FeatureNotPresent)) {
            ZHLN::Log("WARNING: GLTF Importer: Failed to build mesh BLAS: {}", res.error().Message());
        }
    }

    uint32_t finalMorphOffset = 0;
    if (primJob.activeMorphCount > 0) {
        finalMorphOffset = ctx.AllocateMorphDeltas(static_cast<uint32_t>(primJob.positions.size()) * primJob.activeMorphCount, primJob.tempDeltas.data());
    }

    auto     subMaterial_res    = CreativeWorksFactory::CreateBasicMaterial(ctx, primJob.doubleSided || isMirrored, primJob.alphaBlend);
    Material subMaterial        = subMaterial_res.value();
    subMaterial.alphaMode       = primJob.alphaMode;
    subMaterial.alphaCutoff     = primJob.alphaCutoff;
    subMaterial.metallicFactor  = primJob.metallicFactor;
    subMaterial.roughnessFactor = primJob.roughnessFactor;
    std::memcpy(subMaterial.baseColorFactor, primJob.baseColorFactor, sizeof(float) * 4);

    auto GetHandle = [&](cgltf_image* img) -> TextureHandle {
        if (!img) return TextureHandle::Invalid;
        return TextureHandle(static_cast<uint64_t>(CreativeWorksFactory::LoadTexture(ctx, cwMgr, img->uri ? img->uri : "", true)));
    };

    subMaterial.albedoMap   = GetHandle(primJob.albedoImage);
    subMaterial.normalMap   = GetHandle(primJob.normalImage);
    subMaterial.pbrMap      = GetHandle(primJob.pbrImage);
    subMaterial.emissiveMap = GetHandle(primJob.emissiveImage);
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
// MAIN EXPORTED BOUNDARY FUNCTIONS
// ============================================================================

ModelPrefab* LoadGLBPrefab(RenderContext& ctx, CreativeWorksManager& cwMgr, std::string_view path) {
    uint64_t hash = HashCreativeWorkPath(path);
    if (auto* cached = cwMgr.GetCachedPrefab(hash)) {
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

    // --- 1. Flatten Nodes ---
    std::unordered_map<const cgltf_node*, int32_t> nodeMap;
    prefab->nodes.resize(data->nodes_count);
    for (cgltf_size i = 0; i < data->nodes_count; ++i) {
        cgltf_node* node = &data->nodes[i];
        nodeMap[node]    = static_cast<int32_t>(i);

        ModelNode& n = prefab->nodes[i];
        n.name       = (node->name != nullptr) ? String64(node->name) : String64("Unnamed");
        n.hasMesh    = (node->mesh != nullptr);

        float m[16];
        cgltf_node_transform_local(node, m);
        n.localTransform = JPH::Mat44(
            JPH::Vec4(m[0], m[1], m[2], m[3]), JPH::Vec4(m[4], m[5], m[6], m[7]), JPH::Vec4(m[8], m[9], m[10], m[11]), JPH::Vec4(m[12], m[13], m[14], m[15])
        );
    }

    for (cgltf_size i = 0; i < data->nodes_count; ++i) {
        if (data->nodes[i].parent != nullptr) {
            prefab->nodes[i].parentIndex = nodeMap[data->nodes[i].parent];
        }
    }

    // --- 2. Build Skeletons ---
    std::unordered_map<const cgltf_skin*, int32_t> skinMap;
    prefab->skeletons.resize(data->skins_count);
    for (cgltf_size i = 0; i < data->skins_count; ++i) {
        const cgltf_skin* skin = &data->skins[i];
        skinMap[skin]          = static_cast<int32_t>(i);

        Skeleton& skel = prefab->skeletons[i];
        skel.name      = (skin->name != nullptr) ? String64(skin->name) : String64("Skeleton");
        skel.joints.resize(skin->joints_count);

        for (cgltf_size j = 0; j < skin->joints_count; ++j) {
            cgltf_node* jointNode    = skin->joints[j];
            skel.joints[j].name      = (jointNode->name != nullptr) ? String64(jointNode->name) : String64("Joint");
            skel.joints[j].nodeIndex = nodeMap[jointNode];

            skel.joints[j].parentIndex = -1;
            if (jointNode->parent != nullptr) {
                for (cgltf_size p = 0; p < skin->joints_count; ++p) {
                    if (skin->joints[p] == jointNode->parent) {
                        skel.joints[j].parentIndex = static_cast<int32_t>(p);
                        break;
                    }
                }
            }

            if (skin->inverse_bind_matrices != nullptr) {
                float ibmRaw[16];
                cgltf_accessor_read_float(skin->inverse_bind_matrices, j, ibmRaw, 16);
                skel.joints[j].inverseBindMatrix = JPH::Mat44(
                    JPH::Vec4(ibmRaw[0], ibmRaw[1], ibmRaw[2], ibmRaw[3]), JPH::Vec4(ibmRaw[4], ibmRaw[5], ibmRaw[6], ibmRaw[7]),
                    JPH::Vec4(ibmRaw[8], ibmRaw[9], ibmRaw[10], ibmRaw[11]), JPH::Vec4(ibmRaw[12], ibmRaw[13], ibmRaw[14], ibmRaw[15])
                );
            }
        }
    }

    // --- 3. Build Animations ---
    prefab->animations.resize(data->animations_count);
    for (cgltf_size i = 0; i < data->animations_count; ++i) {
        const cgltf_animation& anim = data->animations[i];
        AnimationClip&         clip = prefab->animations[i];
        clip.name                   = (anim.name != nullptr) ? String64(anim.name) : String64("Anim");

        for (cgltf_size c = 0; c < anim.channels_count; ++c) {
            const cgltf_animation_channel& chan = anim.channels[c];
            if (chan.target_node == nullptr) {
                continue;
            }

            AnimationChannel nativeChan;
            nativeChan.targetNodeIndex = nodeMap[chan.target_node];

            if (chan.target_path == cgltf_animation_path_type_translation) {
                nativeChan.path = AnimationPathType::Translation;
            } else if (chan.target_path == cgltf_animation_path_type_rotation) {
                nativeChan.path = AnimationPathType::Rotation;
            } else if (chan.target_path == cgltf_animation_path_type_scale) {
                nativeChan.path = AnimationPathType::Scale;
            } else if (chan.target_path == cgltf_animation_path_type_weights) {
                nativeChan.path = AnimationPathType::Weights;
            }

            if (chan.sampler->interpolation == cgltf_interpolation_type_step) {
                nativeChan.interpolation = InterpolationType::Step;
            } else if (chan.sampler->interpolation == cgltf_interpolation_type_cubic_spline) {
                nativeChan.interpolation = InterpolationType::CubicSpline;
            } else {
                nativeChan.interpolation = InterpolationType::Linear;
            }

            size_t numKeys = chan.sampler->input->count;
            nativeChan.keyTimes.resize(numKeys);
            for (size_t k = 0; k < numKeys; ++k) {
                cgltf_accessor_read_float(chan.sampler->input, k, &nativeChan.keyTimes[k], 1);
                clip.duration = std::max(clip.duration, nativeChan.keyTimes[k]);
            }

            size_t comps       = (nativeChan.path == AnimationPathType::Rotation) ? 4 : 3;
            size_t outputCount = chan.sampler->output->count;
            nativeChan.keyValues.resize(outputCount * comps);
            for (size_t k = 0; k < outputCount; ++k) {
                cgltf_accessor_read_float(chan.sampler->output, k, &nativeChan.keyValues[k * comps], comps);
            }

            clip.channels.push_back(std::move(nativeChan));
        }
    }

    // --- 4. Process GPU Textures & Geometry ---
    std::vector<cgltf_image*>    uniqueImages;
    std::vector<CPUPrimitiveJob> primitiveJobs;
    GatherImagesAndPrimitiveJobs(data, uniqueImages, primitiveJobs);

    JPH::Array<CPUTextureJob> textureJobs;
    ProcessCPUTasks(rawPath, uniqueImages, primitiveJobs, textureJobs);
    auto imageToBindlessIdx = UploadTexturesToGPU(ctx, textureJobs);

    std::unordered_map<const cgltf_primitive*, CompiledPrimitive> primCache;

    for (const auto& primJob: primitiveJobs) {
        const auto*       node       = primJob.node;
        bool              isMirrored = (primJob.nodeTransform.GetDeterminant3x3() < 0.0f);
        CompiledPrimitive compPrim   = GetOrCreateCompiledPrimitive(ctx, cwMgr, primJob, imageToBindlessIdx, primCache, isMirrored);

        ModelPart part;
        part.name            = (node->name != nullptr) ? String64(node->name) : String64("Unnamed");
        part.mesh            = compPrim.mesh;
        part.defaultMaterial = compPrim.defaultMaterial;

        // Pre-compute 64-bit numerical IDs at load time
        std::string assetKeyStr = pathStr + "#part" + std::to_string(prefab->parts.size());
        part.meshAsset          = HashAssetID(assetKeyStr);
        part.materialAsset      = HashAssetID(assetKeyStr + "_mat");

        part.localTransform = JPH::Mat44::sIdentity();
        part.nodeIndex      = nodeMap[node];
        part.isSkinned      = (node->skin != nullptr) || !primJob.skins.empty();

        if (node->skin != nullptr) {
            part.skeletonIndex = skinMap[node->skin];
        }

        part.morphOffset      = compPrim.morphOffset;
        part.activeMorphCount = compPrim.activeMorphCount;
        for (int m = 0; m < 4; ++m) {
            part.defaultMorphWeights[m] = primJob.defaultMorphWeights[m];
        }

        part.boundingRadius = compPrim.boundingRadius;
        part.localMin[0]    = compPrim.localMin[0];
        part.localMin[1]    = compPrim.localMin[1];
        part.localMin[2]    = compPrim.localMin[2];
        part.localMax[0]    = compPrim.localMax[0];
        part.localMax[1]    = compPrim.localMax[1];
        part.localMax[2]    = compPrim.localMax[2];
        part.meshCollider   = compPrim.meshCollider;
        part.boxCollider    = compPrim.boxCollider;

        // ====================================================================
        // PARSE CSG METADATA VIA JSON.hpp
        // ====================================================================
        if (node->extras.start_offset != node->extras.end_offset) {
            std::string_view extras_json(data->json + node->extras.start_offset, node->extras.end_offset - node->extras.start_offset);

            // 1. Parse the outer "extras" object to find "csg_data"
            auto extras_res = ZHLN::ReflectJSON::TryParse<NodeExtras>(extras_json);
            if (extras_res) {
                // 2. Parse the inner "csg_data" string as a JSON array of CSGModifiers
                auto csg_res = ZHLN::ReflectJSON::TryParse<std::vector<CSGModifier>>(extras_res->csg_data);
                if (csg_res) {
                    part.csgModifiers = std::move(*csg_res);
                }
            }
        }

        prefab->parts.push_back(std::move(part));
    }

    Log("Loaded GLB Prefab natively: {} ({} parts, {} animations)", path, prefab->parts.size(), prefab->animations.size());

    // 5. Memory is entirely decoupled now!
    cgltf_free(data);

    ModelPrefab* result = prefab.release();
    cwMgr.CachePrefab(hash, result);
    return result;
}

void RebuildPrefabGPUResources(RenderContext& ctx, CreativeWorksManager& cwMgr, ModelPrefab* prefab) {
    if (prefab == nullptr) {
        return;
    }

    cgltf_options opts {};
    cgltf_data*   data    = nullptr;
    std::string   rawPath = "resources/assets/" + std::string(prefab->virtualPath.c_str());

    if (cgltf_parse_file(&opts, rawPath.c_str(), &data) != cgltf_result_success) {
        return;
    }
    if (cgltf_load_buffers(&opts, data, rawPath.c_str()) != cgltf_result_success) {
        cgltf_free(data);
        return;
    }

    std::vector<cgltf_image*>    uniqueImages;
    std::vector<CPUPrimitiveJob> primitiveJobs;
    GatherImagesAndPrimitiveJobs(data, uniqueImages, primitiveJobs);

    JPH::Array<CPUTextureJob> textureJobs;
    ProcessCPUTasks(rawPath, uniqueImages, primitiveJobs, textureJobs);
    auto imageToBindlessIdx = UploadTexturesToGPU(ctx, textureJobs);

    std::unordered_map<const cgltf_primitive*, CompiledPrimitive> primCache;

    for (size_t i = 0; i < primitiveJobs.size() && i < prefab->parts.size(); ++i) {
        const auto& primJob    = primitiveJobs[i];
        bool        isMirrored = (primJob.nodeTransform.GetDeterminant3x3() < 0.0f);

        CompiledPrimitive compPrim = GetOrCreateCompiledPrimitive(ctx, cwMgr, primJob, imageToBindlessIdx, primCache, isMirrored);

        prefab->parts[i].mesh            = compPrim.mesh;
        prefab->parts[i].defaultMaterial = compPrim.defaultMaterial;
    }

    cgltf_free(data);
}

} // namespace ZHLN::GLTF
