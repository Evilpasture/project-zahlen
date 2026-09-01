// extras/glTF/GLTFImporter.cpp
// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "GLTFImporter.hpp"
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Zahlen/Core/Ranges.hpp>
#include <Zahlen/CreativeWorksFactory.hpp>
#include <Zahlen/CreativeWorksManager.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Math3D.hpp>
#include <Zahlen/Meshlet.hpp>
#include <Zahlen/Render.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <Zahlen/physics/Physics.hpp>
#include <algorithm>
#include <cgltf.h>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <json/JSON.hpp>
#include <memory>
#include <span>
#include <stb_image.h>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ZHLN::GLTF {

namespace {

/// The one custom member this importer reads: Blender writes CSG modifiers as a
/// JSON document inside a JSON string under a node's `extras.csg_data`, so the
/// payload survives round-tripping through tools that only understand string
/// custom properties. Two nested reflected parses -- no bespoke scanner -- and
/// both are why this file lives in extras: they are the engine's real JSON
/// layer, which core deliberately does not carry.
struct NodeExtras {
    std::string csg_data;
};

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

    // VK_EXT_mesh_shader: meshlets are built JIT on the same worker threads
    // that decode the primitive, using the exact same partitioning as zcook.
    MeshletBuildResult meshlets;
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

auto DownsampleHalfSize(const unsigned char* src, uint32_t currentW, uint32_t currentH) -> unsigned char* {
    const uint32_t nextW = currentW / 2;
    const uint32_t nextH = currentH / 2;

    auto* dst = static_cast<unsigned char*>(std::malloc(static_cast<size_t>(nextW) * nextH * 4));
    if (dst == nullptr) {
        return nullptr;
    }

    for (uint32_t y = 0; y < nextH; ++y) {
        for (uint32_t x = 0; x < nextW; ++x) {
            const uint32_t srcX = x * 2;
            const uint32_t srcY = y * 2;

            uint32_t r = 0;
            uint32_t g = 0;
            uint32_t b = 0;
            uint32_t a = 0;

            for (uint32_t dy = 0; dy < 2; ++dy) {
                for (uint32_t dx = 0; dx < 2; ++dx) {
                    const size_t srcIdx = (((static_cast<size_t>(srcY) + dy) * currentW + (srcX + dx)) * 4);
                    r += src[srcIdx + 0];
                    g += src[srcIdx + 1];
                    b += src[srcIdx + 2];
                    a += src[srcIdx + 3];
                }
            }

            const size_t dstIdx = (static_cast<size_t>(y) * nextW + x) * 4;
            dst[dstIdx + 0]     = static_cast<unsigned char>(r / 4);
            dst[dstIdx + 1]     = static_cast<unsigned char>(g / 4);
            dst[dstIdx + 2]     = static_cast<unsigned char>(b / 4);
            dst[dstIdx + 3]     = static_cast<unsigned char>(a / 4);
        }
    }
    return dst;
}

void DecodeAndRescaleTexture(CPUTextureJob& job) {
    int            channels = 0;
    unsigned char* pixels   = nullptr;

    if (job.image->buffer_view != nullptr) {
        const auto* bufferData = static_cast<const char*>(job.image->buffer_view->buffer->data) + job.image->buffer_view->offset;
        pixels                 = stbi_load_from_memory(
            reinterpret_cast<const stbi_uc*>(bufferData), static_cast<int>(job.image->buffer_view->size), &job.width, &job.height, &channels, 4
        );
    } else if (job.image->uri != nullptr && !job.glbPath.empty()) {
        const std::filesystem::path glbFolder = std::filesystem::path(job.glbPath).parent_path();
        const std::filesystem::path texPath   = glbFolder / job.image->uri;
        pixels                                = stbi_load(texPath.string().c_str(), &job.width, &job.height, &channels, 4);
    }

    if (pixels == nullptr) {
        return;
    }

    const auto         w           = static_cast<uint32_t>(job.width);
    const auto         h           = static_cast<uint32_t>(job.height);
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

    cgltf_accessor* posAcc     = nullptr;
    cgltf_accessor* normAcc    = nullptr;
    cgltf_accessor* tangentAcc = nullptr;
    cgltf_accessor* uvAcc      = nullptr;
    cgltf_accessor* colorAcc   = nullptr;
    cgltf_accessor* jointsAcc  = nullptr;
    cgltf_accessor* weightsAcc = nullptr;

    for (cgltf_size a = 0; a < prim.attributes_count; ++a) {
        const auto& attr = prim.attributes[a];
        switch (attr.type) {
            case cgltf_attribute_type_position:
                posAcc = attr.data;
                break;
            case cgltf_attribute_type_normal:
                normAcc = attr.data;
                break;
            case cgltf_attribute_type_tangent:
                tangentAcc = attr.data;
                break;
            case cgltf_attribute_type_texcoord:
                if (attr.index == 0) {
                    uvAcc = attr.data;
                }
                break;
            case cgltf_attribute_type_color:
                if (attr.index == 0) {
                    colorAcc = attr.data;
                }
                break;
            case cgltf_attribute_type_joints:
                if (attr.index == 0) {
                    jointsAcc = attr.data;
                }
                break;
            case cgltf_attribute_type_weights:
                if (attr.index == 0) {
                    weightsAcc = attr.data;
                }
                break;
            default:
                break;
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

        // KHR_materials_emissive_strength is a relative multiplier on the
        // authored factor; kGLTFEmissiveDisplayScale is the glTF [0,1] ->
        // engine HDR unit conversion that applies either way. Without the
        // latter an imported emissive material renders at ~10/255 and never
        // reaches the bloom bright pass (see Zahlen/ModelPrefab.hpp).
        const float strength      = prim.material->has_emissive_strength ? prim.material->emissive_strength.emissive_strength : 1.0f;
        const float emissiveScale = strength * kGLTFEmissiveDisplayScale;

        job.emissiveFactor[0] *= emissiveScale;
        job.emissiveFactor[1] *= emissiveScale;
        job.emissiveFactor[2] *= emissiveScale;

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

        if (prim.material->emissive_texture.texture != nullptr) {
            job.emissiveImage = prim.material->emissive_texture.texture->image;
        }
    }

    const size_t vertexCount = posAcc->count;
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
        const float nLen = std::sqrt(rawNorm[0] * rawNorm[0] + rawNorm[1] * rawNorm[1] + rawNorm[2] * rawNorm[2]);
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
            const auto rawIndex = static_cast<uint32_t>(cgltf_accessor_read_index(prim.indices, idx));
            job.indices[idx]    = std::min(rawIndex, static_cast<uint32_t>(vertexCount > 0 ? vertexCount - 1 : 0));
        }
    } else {
        job.indexCount = static_cast<uint32_t>(job.positions.size());
        job.indices.resize(job.indexCount);
        for (uint32_t idx = 0; idx < job.indexCount; ++idx) {
            job.indices[idx] = idx;
        }
    }

    const JPH::Vec3 localCenter(
        (job.localMax[0] + job.localMin[0]) * 0.5f, (job.localMax[1] + job.localMin[1]) * 0.5f, (job.localMax[2] + job.localMin[2]) * 0.5f
    );
    float maxD2 = 0.0f;
    for (const auto& pos: job.positions) {
        const float dx = pos.position[0] - localCenter.GetX();
        const float dy = pos.position[1] - localCenter.GetY();
        const float dz = pos.position[2] - localCenter.GetZ();
        maxD2          = std::max(dx * dx + dy * dy + dz * dz, maxD2);
    }
    job.boundingRadius = std::sqrt(maxD2) * 1.15f + 0.5f;

    const float extentsX = (job.localMax[0] - job.localMin[0]) * 0.5f;
    const float extentsY = (job.localMax[1] - job.localMin[1]) * 0.5f;
    const float extentsZ = (job.localMax[2] - job.localMin[2]) * 0.5f;

    const JPH::ShapeRefC baseBox = new JPH::BoxShape(JPH::Vec3(extentsX, extentsY, extentsZ));
    job.boxCollider              = new JPH::RotatedTranslatedShape(localCenter, JPH::Quat::sIdentity(), baseBox);

    if (prim.type == cgltf_primitive_type_triangles && jointsAcc == nullptr) {
        job.meshCollider = Physics::CreateMeshShape(job.positions.data(), static_cast<uint32_t>(job.positions.size()), job.indices.data(), job.indexCount);
    }

    // VK_EXT_mesh_shader: partition the primitive into meshlets. Only triangle
    // lists can be clustered; everything else keeps the legacy vertex path
    // (meshletCount == 0 makes the renderer fall back automatically).
    if (prim.type == cgltf_primitive_type_triangles) {
        job.meshlets = BuildMeshlets(job.indices, job.positions);
    }
}

void GatherImagesAndPrimitiveJobs(const cgltf_data* data, std::vector<cgltf_image*>& outUniqueImages, std::vector<CPUPrimitiveJob>& outPrimitiveJobs) {
    auto RegisterImage = [&](cgltf_image* img) -> void {
        if (img != nullptr && std::ranges::find(outUniqueImages, img) == outUniqueImages.end()) {
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
        const JPH::Mat44 nodeTransform(
            JPH::Vec4(matrix[0], matrix[1], matrix[2], matrix[3]), JPH::Vec4(matrix[4], matrix[5], matrix[6], matrix[7]),
            JPH::Vec4(matrix[8], matrix[9], matrix[10], matrix[11]), JPH::Vec4(matrix[12], matrix[13], matrix[14], matrix[15])
        );

        const auto* mesh = node->mesh;
        for (cgltf_size p = 0; p < mesh->primitives_count; ++p) {
            CPUPrimitiveJob job {.node = node, .prim = &mesh->primitives[p], .nodeTransform = nodeTransform};

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
    const std::string&               textureSearchPath,
    const std::vector<cgltf_image*>& uniqueImages,
    std::vector<CPUPrimitiveJob>&    primitiveJobs,
    JPH::Array<CPUTextureJob>&       outTextureJobs
) {
    outTextureJobs.resize(uniqueImages.size());
    for (size_t i = 0; i < uniqueImages.size(); ++i) {
        outTextureJobs[i] = {.image = uniqueImages[i], .glbPath = textureSearchPath, .isSRGB = true};

        for (const auto& primJob: primitiveJobs) {
            if (primJob.normalImage == uniqueImages[i] || primJob.pbrImage == uniqueImages[i]) {
                outTextureJobs[i].isSRGB = false;
                break;
            }
        }
    }

    if (!outTextureJobs.empty()) {
        TaskSystem::ParallelFor(outTextureJobs.size(), 1, [&](uint32_t start, uint32_t end, uint32_t) -> void {
            for (uint32_t i = start; i < end; ++i) {
                DecodeAndRescaleTexture(outTextureJobs[i]);
            }
        });
    }

    if (!primitiveJobs.empty()) {
        TaskSystem::ParallelFor(primitiveJobs.size(), 1, [&](uint32_t start, uint32_t end, uint32_t) -> void {
            for (uint32_t i = start; i < end; ++i) {
                ProcessCPUPrimitive(primitiveJobs[i]);
            }
        });
    }
}

auto UploadTexturesToGPU(RenderContext& ctx, std::string_view virtualPath, JPH::Array<CPUTextureJob>& textureJobs)
    -> std::unordered_map<cgltf_image*, TextureHandle> {
    std::unordered_map<cgltf_image*, TextureHandle> imageToHandle;
    imageToHandle.reserve(textureJobs.size());

    for (size_t i = 0; i < textureJobs.size(); ++i) {
        auto& texJob = textureJobs[i];
        if (texJob.decodedPixels != nullptr) {
            const auto tex_res = ctx.CreateTexture(texJob.decodedPixels, texJob.width, texJob.height, texJob.isSRGB);
            if (texJob.wasRescaled) {
                std::free(texJob.decodedPixels);
            } else {
                stbi_image_free(texJob.decodedPixels);
            }

            const uint32_t    bindlessIdx = tex_res ? *tex_res : 1;
            const std::string texName     = std::format("{}#tex_{}", virtualPath, i);

            imageToHandle[texJob.image] = ctx.RegisterTexture(texName, bindlessIdx, texJob.isSRGB);
        } else {
            imageToHandle[texJob.image] = TextureHandle::Invalid;
        }
    }

    return imageToHandle;
}

auto GetOrCreateCompiledPrimitive(
    RenderContext&                                                 ctx,
    const CPUPrimitiveJob&                                         primJob,
    const std::unordered_map<cgltf_image*, TextureHandle>&         imageToHandle,
    std::unordered_map<const cgltf_primitive*, CompiledPrimitive>& primCache,
    bool                                                           isMirrored
) -> CompiledPrimitive {
    if (const auto it = primCache.find(primJob.prim); it != primCache.end()) {
        return it->second;
    }

    const BufferHandle posVbo = ctx.CreateVertexBuffer(primJob.positions.data(), primJob.positions.size() * sizeof(VertexPosition), sizeof(VertexPosition));
    const BufferHandle attrVbo =
        ctx.CreateVertexBuffer(primJob.attributes.data(), primJob.attributes.size() * sizeof(VertexAttributes), sizeof(VertexAttributes));

    const BufferHandle skinVbo = !primJob.skins.empty() ?
                                     ctx.CreateVertexBuffer(primJob.skins.data(), primJob.skins.size() * sizeof(VertexSkin), sizeof(VertexSkin)) :
                                     BufferHandle::Invalid;

    const BufferHandle ibo = (primJob.indexCount > 0) ? ctx.CreateIndexBuffer(primJob.indices.data(), primJob.indexCount * sizeof(uint32_t)) :
                                                        BufferHandle::Invalid;

    // VK_EXT_mesh_shader streams. They are plain storage buffers read through
    // BDA by the task/mesh shaders; the vertex/index buffers above stay live
    // for BLAS builds and for the legacy vertex pipeline.
    const bool hasMeshlets = !primJob.meshlets.Empty();

    const BufferHandle meshletVbo =
        hasMeshlets ? ctx.CreateStorageBuffer(primJob.meshlets.meshlets.data(), primJob.meshlets.meshlets.size() * sizeof(GPUMeshlet), sizeof(GPUMeshlet)) :
                      BufferHandle::Invalid;
    const BufferHandle meshletVertexVbo =
        hasMeshlets ? ctx.CreateStorageBuffer(primJob.meshlets.vertices.data(), primJob.meshlets.vertices.size() * sizeof(uint32_t), sizeof(uint32_t)) :
                      BufferHandle::Invalid;
    const BufferHandle meshletTriVbo =
        hasMeshlets ? ctx.CreateStorageBuffer(primJob.meshlets.triangles.data(), primJob.meshlets.triangles.size(), sizeof(uint8_t)) : BufferHandle::Invalid;

    Mesh subMesh = {
        .posBuffer           = posVbo,
        .attrBuffer          = attrVbo,
        .skinBuffer          = skinVbo,
        .indexBuffer         = ibo,
        .vertexCount         = static_cast<uint32_t>(primJob.positions.size()),
        .indexCount          = primJob.indexCount,
        .meshletBuffer       = meshletVbo,
        .meshletVertexBuffer = meshletVertexVbo,
        .meshletTriBuffer    = meshletTriVbo,
        .meshletCount        = hasMeshlets ? static_cast<uint32_t>(primJob.meshlets.meshlets.size()) : 0u
    };

    if (auto res = ctx.BuildMeshBLAS(subMesh); !res) [[unlikely]] {
        if (!res.error().Is(RenderFeatureError::FeatureNotSupported)) {
            ZHLN::Log("WARNING: GLTF Importer: Failed to build mesh BLAS: {}", res.error().Message());
        }
    }

    const uint32_t finalMorphOffset =
        (primJob.activeMorphCount > 0) ?
            ctx.AllocateMorphDeltas(static_cast<uint32_t>(primJob.positions.size()) * primJob.activeMorphCount, primJob.tempDeltas.data()) :
            0;

    const Material subMaterial =
        CreativeWorksFactory::CreateMaterial(
            ctx, {.doubleSided = primJob.doubleSided || isMirrored,
                  .alphaBlend  = primJob.alphaBlend,
                  .alphaMode   = primJob.alphaMode,
                  .alphaCutoff = primJob.alphaCutoff,
                  .metallic    = primJob.metallicFactor,
                  .roughness   = primJob.roughnessFactor,
                  .baseColor   = {primJob.baseColorFactor[0], primJob.baseColorFactor[1], primJob.baseColorFactor[2], primJob.baseColorFactor[3]},
                  .emissive    = {primJob.emissiveFactor[0], primJob.emissiveFactor[1], primJob.emissiveFactor[2], primJob.emissiveFactor[3]},
                  .albedoMap   = imageToHandle | ZHLN::Ranges::FindOr(primJob.albedoImage, TextureHandle::Invalid),
                  .normalMap   = imageToHandle | ZHLN::Ranges::FindOr(primJob.normalImage, TextureHandle::Invalid),
                  .pbrMap      = imageToHandle | ZHLN::Ranges::FindOr(primJob.pbrImage, TextureHandle::Invalid),
                  .emissiveMap = imageToHandle | ZHLN::Ranges::FindOr(primJob.emissiveImage, TextureHandle::Invalid)}
        )
            .value_or(Material {});

    const CompiledPrimitive compPrim = {
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

/**
 * @brief Common builder that constructs and caches a ModelPrefab from loaded cgltf_data.
 * Adheres strictly to aggregate initialization and DRY across disk & memory pathways.
 */
auto BuildModelPrefab(RenderContext& ctx, CreativeWorksManager& cwMgr, cgltf_data* data, std::string_view virtualPath, std::string_view textureSearchPath)
    -> ModelPrefab* {
    // RAII guard ensures cgltf_data is cleanly freed on function exit
    const std::unique_ptr<cgltf_data, decltype(&cgltf_free)> dataGuard(data, &cgltf_free);

    auto prefab         = std::make_unique<ModelPrefab>();
    prefab->virtualPath = String256(virtualPath);

    // ------------------------------------------------------------------------
    // 1. Flatten Nodes with Aggregate Initialization
    // ------------------------------------------------------------------------
    std::unordered_map<const cgltf_node*, int32_t> nodeMap;
    nodeMap.reserve(data->nodes_count);
    prefab->nodes.reserve(data->nodes_count);

    for (cgltf_size i = 0; i < data->nodes_count; ++i) {
        const cgltf_node* node = &data->nodes[i];
        nodeMap[node]          = static_cast<int32_t>(i);

        float m[16];
        cgltf_node_transform_local(node, m);
        const JPH::Mat44 localTransform(
            JPH::Vec4(m[0], m[1], m[2], m[3]), JPH::Vec4(m[4], m[5], m[6], m[7]), JPH::Vec4(m[8], m[9], m[10], m[11]), JPH::Vec4(m[12], m[13], m[14], m[15])
        );

        prefab->nodes.push_back(
            ModelNode {
                .name           = (node->name != nullptr) ? String64(node->name) : String64("Unnamed"),
                .parentIndex    = -1,
                .localTransform = localTransform,
                .hasMesh        = (node->mesh != nullptr)
            }
        );
    }

    for (cgltf_size i = 0; i < data->nodes_count; ++i) {
        if (data->nodes[i].parent != nullptr) {
            prefab->nodes[i].parentIndex = nodeMap[data->nodes[i].parent];
        }
    }

    // ------------------------------------------------------------------------
    // 2. Build Skeletons with Aggregate Initialization
    // ------------------------------------------------------------------------
    std::unordered_map<const cgltf_skin*, int32_t> skinMap;
    skinMap.reserve(data->skins_count);
    prefab->skeletons.reserve(data->skins_count);

    for (cgltf_size i = 0; i < data->skins_count; ++i) {
        const cgltf_skin* skin = &data->skins[i];
        skinMap[skin]          = static_cast<int32_t>(i);

        std::vector<Joint> joints;
        joints.reserve(skin->joints_count);

        for (cgltf_size j = 0; j < skin->joints_count; ++j) {
            const cgltf_node* jointNode = skin->joints[j];

            int32_t parentIdx = -1;
            if (jointNode->parent != nullptr) {
                for (cgltf_size p = 0; p < skin->joints_count; ++p) {
                    if (skin->joints[p] == jointNode->parent) {
                        parentIdx = static_cast<int32_t>(p);
                        break;
                    }
                }
            }

            JPH::Mat44 ibm = JPH::Mat44::sIdentity();
            if (skin->inverse_bind_matrices != nullptr) {
                float ibmRaw[16];
                cgltf_accessor_read_float(skin->inverse_bind_matrices, j, ibmRaw, 16);
                ibm = JPH::Mat44(
                    JPH::Vec4(ibmRaw[0], ibmRaw[1], ibmRaw[2], ibmRaw[3]), JPH::Vec4(ibmRaw[4], ibmRaw[5], ibmRaw[6], ibmRaw[7]),
                    JPH::Vec4(ibmRaw[8], ibmRaw[9], ibmRaw[10], ibmRaw[11]), JPH::Vec4(ibmRaw[12], ibmRaw[13], ibmRaw[14], ibmRaw[15])
                );
            }

            joints.push_back(
                Joint {
                    .name              = (jointNode->name != nullptr) ? String64(jointNode->name) : String64("Joint"),
                    .parentIndex       = parentIdx,
                    .nodeIndex         = nodeMap[jointNode],
                    .inverseBindMatrix = ibm
                }
            );
        }

        prefab->skeletons.push_back(Skeleton {.name = (skin->name != nullptr) ? String64(skin->name) : String64("Skeleton"), .joints = std::move(joints)});
    }

    // ------------------------------------------------------------------------
    // 3. Build Animations with Aggregate Initialization
    // ------------------------------------------------------------------------
    prefab->animations.reserve(data->animations_count);

    for (cgltf_size i = 0; i < data->animations_count; ++i) {
        const cgltf_animation& anim = data->animations[i];

        float                         duration = 0.0f;
        std::vector<AnimationChannel> channels;
        channels.reserve(anim.channels_count);

        for (cgltf_size c = 0; c < anim.channels_count; ++c) {
            const cgltf_animation_channel& chan = anim.channels[c];
            if (chan.target_node == nullptr) {
                continue;
            }

            const auto pathType = [&]() -> ZHLN::AnimationPathType {
                switch (chan.target_path) {
                    case cgltf_animation_path_type_translation:
                        return AnimationPathType::Translation;
                    case cgltf_animation_path_type_rotation:
                        return AnimationPathType::Rotation;
                    case cgltf_animation_path_type_scale:
                        return AnimationPathType::Scale;
                    case cgltf_animation_path_type_weights:
                        return AnimationPathType::Weights;
                    default:
                        return AnimationPathType::Translation;
                }
            }();

            const auto interpType = [&]() -> ZHLN::InterpolationType {
                switch (chan.sampler->interpolation) {
                    case cgltf_interpolation_type_step:
                        return InterpolationType::Step;
                    case cgltf_interpolation_type_cubic_spline:
                        return InterpolationType::CubicSpline;
                    default:
                        return InterpolationType::Linear;
                }
            }();

            const size_t       numKeys = chan.sampler->input->count;
            std::vector<float> keyTimes(numKeys);
            for (size_t k = 0; k < numKeys; ++k) {
                cgltf_accessor_read_float(chan.sampler->input, k, &keyTimes[k], 1);
                duration = std::max(duration, keyTimes[k]);
            }

            const size_t       comps       = (pathType == AnimationPathType::Rotation) ? 4 : 3;
            const size_t       outputCount = chan.sampler->output->count;
            std::vector<float> keyValues(outputCount * comps);
            for (size_t k = 0; k < outputCount; ++k) {
                cgltf_accessor_read_float(chan.sampler->output, k, &keyValues[k * comps], comps);
            }

            channels.push_back(
                AnimationChannel {
                    .targetNodeIndex = nodeMap[chan.target_node],
                    .path            = pathType,
                    .interpolation   = interpType,
                    .keyTimes        = std::move(keyTimes),
                    .keyValues       = std::move(keyValues)
                }
            );
        }

        prefab->animations.push_back(
            AnimationClip {.name = (anim.name != nullptr) ? String64(anim.name) : String64("Anim"), .duration = duration, .channels = std::move(channels)}
        );
    }

    // ------------------------------------------------------------------------
    // 4. Process GPU Textures & Geometry
    // ------------------------------------------------------------------------
    std::vector<cgltf_image*>    uniqueImages;
    std::vector<CPUPrimitiveJob> primitiveJobs;
    GatherImagesAndPrimitiveJobs(data, uniqueImages, primitiveJobs);

    JPH::Array<CPUTextureJob> textureJobs;
    ProcessCPUTasks(std::string(textureSearchPath), uniqueImages, primitiveJobs, textureJobs);
    const auto imageToBindlessIdx = UploadTexturesToGPU(ctx, virtualPath, textureJobs);

    std::unordered_map<const cgltf_primitive*, CompiledPrimitive> primCache;
    prefab->parts.reserve(primitiveJobs.size());

    for (const auto& primJob: primitiveJobs) {
        const auto* node       = primJob.node;
        const bool  isMirrored = (primJob.nodeTransform.GetDeterminant3x3() < 0.0f);
        const auto  compPrim   = GetOrCreateCompiledPrimitive(ctx, primJob, imageToBindlessIdx, primCache, isMirrored);

        const std::string assetKeyStr   = std::string(virtualPath) + "#part" + std::to_string(prefab->parts.size());
        const AssetID     meshAsset     = HashAssetID(assetKeyStr);
        const MaterialID  materialAsset = HashAssetID(assetKeyStr + "_mat");

        const int32_t skeletonIdx = (node->skin != nullptr) ? skinMap[node->skin] : -1;
        const bool    isSkinned   = (node->skin != nullptr) && !primJob.skins.empty();

        std::vector<CSGModifier> csgModifiers;
        if (node->extras.start_offset != node->extras.end_offset) {
            const std::string_view extras_json(data->json + node->extras.start_offset, node->extras.end_offset - node->extras.start_offset);
            if (const auto extras_res = ZHLN::ReflectJSON::TryParse<NodeExtras>(extras_json)) {
                if (auto csg_res = ZHLN::ReflectJSON::TryParse<std::vector<CSGModifier>>(extras_res->csg_data)) {
                    csgModifiers = std::move(*csg_res);
                }
            }
        }

        prefab->parts.push_back(
            ModelPart {
                .name             = (node->name != nullptr) ? String64(node->name) : String64("Unnamed"),
                .meshAsset        = meshAsset,
                .materialAsset    = materialAsset,
                .mesh             = compPrim.mesh,
                .defaultMaterial  = compPrim.defaultMaterial,
                .localTransform   = JPH::Mat44::sIdentity(),
                .jointOffset      = 0,
                .isSkinned        = isSkinned,
                .nodeIndex        = nodeMap[node],
                .skeletonIndex    = skeletonIdx,
                .morphOffset      = compPrim.morphOffset,
                .activeMorphCount = compPrim.activeMorphCount,
                .defaultMorphWeights =
                    {primJob.defaultMorphWeights[0], primJob.defaultMorphWeights[1], primJob.defaultMorphWeights[2], primJob.defaultMorphWeights[3]},
                .boundingRadius = compPrim.boundingRadius,
                .localMin       = {compPrim.localMin[0], compPrim.localMin[1], compPrim.localMin[2]},
                .localMax       = {compPrim.localMax[0], compPrim.localMax[1], compPrim.localMax[2]},
                .meshCollider   = compPrim.meshCollider,
                .boxCollider    = compPrim.boxCollider,
                .csgModifiers   = std::move(csgModifiers)
            }
        );
    }

    Log("Loaded GLB Prefab: {} ({} parts, {} animations)", virtualPath, prefab->parts.size(), prefab->animations.size());

    ModelPrefab* const result = prefab.release();
    cwMgr.CachePrefab(HashCreativeWorkPath(virtualPath), result);
    return result;
}

} // namespace

// ============================================================================
// Public Entry Points
// ============================================================================

auto LoadGLBPrefab(RenderContext& ctx, CreativeWorksManager& cwMgr, std::string_view path) -> ModelPrefab* {
    const uint64_t hash = HashCreativeWorkPath(path);
    if (auto* const cached = cwMgr.GetCachedPrefab(hash)) {
        return cached;
    }

    const std::string pathStr(path);
    const std::string rawPath = "resources/assets/" + pathStr;

    cgltf_options opts {};
    cgltf_data*   data = nullptr;

    if (cgltf_parse_file(&opts, rawPath.c_str(), &data) != cgltf_result_success) {
        Log("ERROR: Failed to parse GLB from file: {}", rawPath);
        return nullptr;
    }

    if (cgltf_load_buffers(&opts, data, rawPath.c_str()) != cgltf_result_success) {
        Log("ERROR: Failed to load GLB buffers from file: {}", rawPath);
        cgltf_free(data);
        return nullptr;
    }

    return BuildModelPrefab(ctx, cwMgr, data, path, rawPath);
}

auto LoadGLBPrefabFromMemory(RenderContext& ctx, CreativeWorksManager& cwMgr, std::span<const uint8_t> bytes, std::string_view virtualPath) -> ModelPrefab* {
    const uint64_t hash = HashCreativeWorkPath(virtualPath);
    if (auto* const cached = cwMgr.GetCachedPrefab(hash)) {
        return cached;
    }

    cgltf_options opts {};
    cgltf_data*   data = nullptr;

    if (cgltf_parse(&opts, bytes.data(), bytes.size(), &data) != cgltf_result_success) {
        Log("ERROR: Failed to parse in-memory GLB: {}", virtualPath);
        return nullptr;
    }

    if (cgltf_load_buffers(&opts, data, nullptr) != cgltf_result_success) {
        Log("ERROR: Failed to load in-memory GLB buffers: {}", virtualPath);
        cgltf_free(data);
        return nullptr;
    }

    return BuildModelPrefab(ctx, cwMgr, data, virtualPath, {});
}

void RebuildPrefabGPUResources(RenderContext& ctx, ModelPrefab* prefab) {
    if (prefab == nullptr) {
        return;
    }

    cgltf_options     opts {};
    cgltf_data*       data    = nullptr;
    const std::string rawPath = "resources/assets/" + std::string(prefab->virtualPath.c_str());

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
    const auto imageToBindlessIdx = UploadTexturesToGPU(ctx, prefab->virtualPath.c_str(), textureJobs);

    std::unordered_map<const cgltf_primitive*, CompiledPrimitive> primCache;

    for (size_t i = 0; i < primitiveJobs.size() && i < prefab->parts.size(); ++i) {
        const auto& primJob    = primitiveJobs[i];
        const bool  isMirrored = (primJob.nodeTransform.GetDeterminant3x3() < 0.0f);

        const auto compPrim = GetOrCreateCompiledPrimitive(ctx, primJob, imageToBindlessIdx, primCache, isMirrored);

        prefab->parts[i].mesh            = compPrim.mesh;
        prefab->parts[i].defaultMaterial = compPrim.defaultMaterial;
    }

    cgltf_free(data);
}

auto InstantiatePrefabFromMemory(
    Engine&                                  engine,
    std::span<const uint8_t>                 bytes,
    std::string_view                         virtualPath,
    const CreativeWorksFactory::SpawnParams& params,
    Entity*                                  outBuffer,
    uint32_t                                 maxCount
) -> uint32_t {
    const auto* prefab = LoadGLBPrefabFromMemory(engine.GetRenderContext(), engine.GetCreativeWorksManager(), bytes, virtualPath);
    if (prefab == nullptr) {
        return 0;
    }
    return CreativeWorksFactory::InstantiatePrefab(engine, *prefab, params, outBuffer, maxCount);
}

void RebuildCachedPrefabs(RenderContext& ctx, CreativeWorksManager& cwMgr) {
    const uint32_t count = cwMgr.GetCachedPrefabs(nullptr, 0);
    if (count == 0) {
        return;
    }

    std::vector<ModelPrefab*> prefabs(count);
    cwMgr.GetCachedPrefabs(prefabs.data(), count);

    for (auto* prefab: prefabs) {
        RebuildPrefabGPUResources(ctx, prefab);
        for (size_t i = 0; i < prefab->parts.size(); ++i) {
            const std::string assetKey = std::string(prefab->virtualPath.c_str()) + "#" + prefab->parts[i].name.c_str() + "_" +
                                         std::to_string(prefab->parts[i].nodeIndex);
            ctx.RegisterGPUMesh(HashAssetID(assetKey), prefab->parts[i].mesh);
            ctx.RegisterGPUMaterial(HashAssetID(assetKey + "_mat"), prefab->parts[i].defaultMaterial);
        }
    }
}

void InstallDeviceLostHandler(Engine& engine) {
    engine.AddDeviceLostCallback(
        [](Engine& e) {
            RebuildCachedPrefabs(e.GetRenderContext(), e.GetCreativeWorksManager());
        }
    );
}

} // namespace ZHLN::GLTF
