// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// File: src/engine/CreativeWorksFactory.cpp

// clang-format off
#include <Jolt/Jolt.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/ScaledShape.h>
// clang-format on

#include <Zahlen/Components.hpp>
#include <Zahlen/CreativeWorksFactory.hpp>
#include <Zahlen/CreativeWorksManager.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Font8x8.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Math3D.hpp>
#include <Zahlen/Render.hpp>
#include <algorithm>
#include <cstddef>
#include <ecs/ECS.hpp>
#include <engine/system/AnimationSystem.hpp>
#include <engine/system/LightingSystem.hpp>
#include <gltf/GLTFImporter.hpp> // <-- The only place glTF is allowed to be mentioned
#include <physics/Physics.hpp>
#include <stb_image.h>
#include <threading/TaskSystem.hpp> // Needed for ParallelFor
#define STB_TRUETYPE_IMPLEMENTATION
#include <fontconfig/fontconfig.h>
#include <stb_truetype.h>

namespace ZHLN::CreativeWorksFactory {

// ============================================================================
// SYSTEM FONT
// ============================================================================

static std::string FindSystemFont(const char* fontName) {
    FcConfig*  config = FcInitLoadConfigAndFonts();
    FcPattern* pat    = FcNameParse(reinterpret_cast<const FcChar8*>(fontName));
    FcConfigSubstitute(config, pat, FcMatchPattern);
    FcDefaultSubstitute(pat);

    FcResult    result;
    FcPattern*  match = FcFontMatch(config, pat, &result);
    std::string fontPath;
    if (match != nullptr) {
        FcChar8* file = nullptr;
        if (FcPatternGetString(match, FC_FILE, 0, &file) == FcResultMatch) {
            fontPath = reinterpret_cast<const char*>(file);
        }
        FcPatternDestroy(match);
    }
    FcPatternDestroy(pat);
    FcConfigDestroy(config);
    return fontPath;
}

uint32_t CreateFontAtlasTexture(RenderContext& ctx) {
    std::string fontPath = FindSystemFont("sans-serif");
    if (fontPath.empty()) {
        fontPath = "/usr/share/fonts/TTF/DejaVuSans.ttf";
    }

    Log("Loading TrueType system font: {}", fontPath);

    FILE* f = std::fopen(fontPath.c_str(), "rb");
    if (f == nullptr) {
        Log("ERROR: Failed to open system font file: {}", fontPath);
        return 0;
    }

    std::fseek(f, 0, SEEK_END);
    long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> fontBuffer(size);
    std::fread(fontBuffer.data(), 1, size, f);
    std::fclose(f);

    int fontOffset = stbtt_GetFontOffsetForIndex(fontBuffer.data(), 0);
    fontOffset     = std::max(fontOffset, 0);

    const uint32_t       atlasSize = 512;
    std::vector<uint8_t> alphaBitmap(static_cast<size_t>(atlasSize * atlasSize), 0);

    auto* engine             = GetEngineContext();
    auto& reg                = engine->GetRegistry();
    auto  uiSettingsEntities = reg.GetEntitiesWith<Components::UISettingsComponent>();
    if (uiSettingsEntities.empty()) {
        return 0;
    }
    auto* uiSettings = reg.Get<Components::UISettingsComponent>(uiSettingsEntities[0]);

    stbtt_bakedchar bakedChars[96];
    int             result = stbtt_BakeFontBitmap(fontBuffer.data(), fontOffset, 24.0f, alphaBitmap.data(), atlasSize, atlasSize, 32, 96, bakedChars);

    if (result <= 0) {
        Log("ERROR: stb_truetype failed to bake font bitmap!");
        return 0;
    }

    std::vector<uint32_t> rgbaPixels(static_cast<size_t>(atlasSize * atlasSize));
    for (uint32_t i = 0; i < atlasSize * atlasSize; ++i) {
        uint8_t alpha = alphaBitmap[i];
        rgbaPixels[i] = (static_cast<uint32_t>(alpha) << 24) | 0x00FFFFFF;
    }

    auto tex_res = ctx.CreateTexture(rgbaPixels.data(), atlasSize, atlasSize, false);
    if (!tex_res) {
        Log("ERROR: CreateFontAtlasTexture failed to create Vulkan texture: {}", tex_res.error().Message());
        return 0;
    }
    uint32_t texIdx = tex_res.value();

    for (uint32_t i = 0; i < 96; ++i) {
        const auto& bc                  = bakedChars[i];
        uiSettings->fontAtlas.glyphs[i] = GlyphMetric {
            .x0       = static_cast<float>(bc.x0),
            .y0       = static_cast<float>(bc.y0),
            .x1       = static_cast<float>(bc.x1),
            .y1       = static_cast<float>(bc.y1),
            .xoff     = bc.xoff,
            .yoff     = bc.yoff,
            .xadvance = bc.xadvance
        };
    }
    uiSettings->fontAtlas.textureIndex = texIdx;
    uiSettings->defaultFontAtlasIdx    = texIdx;

    return texIdx;
}

// ============================================================================
// MODEL PREFAB INSTANTIATION (NATIVE)
// ============================================================================

ModelPrefab* LoadModelPrefab(RenderContext& ctx, CreativeWorksManager& assetMgr, std::string_view path) {
    // Pipeline securely crosses the boundary into the isolated glTF module
    return GLTF::LoadGLBPrefab(ctx, assetMgr, path);
}

namespace {

Entity SpawnPrefabRoot(ECS::Registry& reg, std::string_view vPath, const SpawnParams& p) {
    Entity root = reg.Create();
    reg.Add(root, Components::TransformComponent {.position = JPH::Vec3(p.position), .rotation = p.rotation, .scale = p.scale});
    reg.Add(root, Components::NameComponent {.name = String64("Root_" + std::string(vPath))});
    return root;
}

JPH::Mat44 GetNodeLogicalTransform(const ModelPrefab& prefab, int32_t nodeIndex) {
    JPH::Mat44 matrix    = prefab.nodes[nodeIndex].localTransform;
    int32_t    parentIdx = prefab.nodes[nodeIndex].parentIndex;

    while (parentIdx >= 0) {
        matrix    = prefab.nodes[parentIdx].localTransform * matrix;
        parentIdx = prefab.nodes[parentIdx].parentIndex;
    }
    return matrix;
}

struct PreparedPart {
    JPH::Vec3      translation {};
    JPH::Quat      rotation {};
    JPH::Vec3      scale {};
    float          maxScale = 1.0f;
    JPH::ShapeRefC shape    = nullptr;
};

void PreparePrefabPhysics(
    const ModelPrefab&         prefab,
    const JPH::Mat44&          baseTransform,
    bool                       createPhysics,
    bool                       useBoxColliders,
    std::vector<PreparedPart>& outPrepared
) {
    outPrepared.resize(prefab.parts.size());

    TaskSystem::ParallelFor(prefab.parts.size(), 16, [&](uint32_t start, uint32_t end, uint32_t) {
        for (uint32_t i = start; i < end; ++i) {
            const auto& part = prefab.parts[i];
            auto&       prep = outPrepared[i];

            JPH::Mat44 nodeWorld  = GetNodeLogicalTransform(prefab, part.nodeIndex);
            JPH::Mat44 finalLocal = baseTransform * nodeWorld * part.localTransform;

            prep.scale = JPH::Vec3(finalLocal.GetColumn3(0).Length(), finalLocal.GetColumn3(1).Length(), finalLocal.GetColumn3(2).Length());

            if (finalLocal.GetDeterminant3x3() < 0.0f) {
                prep.scale.SetX(-prep.scale.GetX());
            }

            prep.maxScale    = std::max({std::abs(prep.scale.GetX()), std::abs(prep.scale.GetY()), std::abs(prep.scale.GetZ())});
            prep.translation = finalLocal.GetTranslation();

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

Entity InstantiateMeshPart(
    RenderContext&                         ctx,
    ECS::Registry&                         reg,
    PhysicsContext&                        pc,
    const ModelPrefab&                     prefab,
    const ModelPart&                       part,
    const PreparedPart&                    prep,
    const SpawnParams&                     params,
    Entity                                 rootEntity,
    std::unordered_map<int32_t, uint32_t>& allocatedSkeletons
) {
    BufferHandle skinnedVbo = BufferHandle::Invalid;
    if (part.isSkinned && params.isAnimated) {
        skinnedVbo = ctx.CreateSkinnedScratchBuffer(part.mesh.vertexCount);
    }

    uint32_t assignedJointOffset = 0;
    if (part.isSkinned && params.isAnimated && part.skeletonIndex >= 0) {
        auto it = allocatedSkeletons.find(part.skeletonIndex);
        if (it != allocatedSkeletons.end()) {
            assignedJointOffset = it->second;
        } else {
            assignedJointOffset                    = JointAllocator::Allocate(static_cast<uint32_t>(prefab.skeletons[part.skeletonIndex].joints.size()));
            allocatedSkeletons[part.skeletonIndex] = assignedJointOffset;
        }
    }

    Entity   e         = reg.Create();
    Material activeMat = params.materialOverride.pipeline != PipelineHandle::Invalid ? params.materialOverride : part.defaultMaterial;

    DrawFlags flags = DrawFlags::None;
    if (part.isSkinned && params.isAnimated) {
        flags |= DrawFlags::Skinned;
    }
    if (activeMat.alphaMode == 2 || params.isAnimated) {
        flags |= DrawFlags::ExcludeFromTLAS;
    }

    if (params.createPhysics && prep.shape != nullptr) {
        reg.Add(e, Components::TransformComponent {.position = prep.translation, .rotation = prep.rotation, .scale = prep.scale});
        reg.Add(
            e, Components::PhysicsComponent {Physics::CreateRigidBody(
                   pc, prep.shape, JPH::RVec3(prep.translation), prep.rotation, params.isStaticPhysics ? JPH::EMotionType::Static : JPH::EMotionType::Dynamic,
                   params.isStaticPhysics ? static_cast<JPH::ObjectLayer>(0) : static_cast<JPH::ObjectLayer>(1), 0, params.physicsCategory, params.physicsMask
               )}
        );

        if (!params.isStaticPhysics) {
            reg.Add(
                e, Components::PhysicsStateComponent {
                       .currPosition = prep.translation, .prevPosition = prep.translation, .currRotation = prep.rotation, .prevRotation = prep.rotation
                   }
            );
        }
    } else {
        // --- FIX: Normalize basis columns before extracting quaternion to prevent skewing ---
        JPH::Mat44 nodeLocal = GetNodeLogicalTransform(prefab, part.nodeIndex) * part.localTransform;
        JPH::Vec3  localPos  = nodeLocal.GetTranslation();

        JPH::Vec3 c0 = nodeLocal.GetColumn3(0);
        JPH::Vec3 c1 = nodeLocal.GetColumn3(1);
        JPH::Vec3 c2 = nodeLocal.GetColumn3(2);
        JPH::Vec3 localScale(c0.Length(), c1.Length(), c2.Length());

        // Strip scale to form a pure rotation matrix
        if (localScale.GetX() > 1e-5f) {
            c0 /= localScale.GetX();
        } else {
            c0 = JPH::Vec3::sAxisX();
        }

        if (localScale.GetY() > 1e-5f) {
            c1 /= localScale.GetY();
        } else {
            c1 = JPH::Vec3::sAxisY();
        }

        if (localScale.GetZ() > 1e-5f) {
            c2 /= localScale.GetZ();
        } else {
            c2 = JPH::Vec3::sAxisZ();
        }

        JPH::Mat44 rotMat(JPH::Vec4(c0, 0), JPH::Vec4(c1, 0), JPH::Vec4(c2, 0), JPH::Vec4(0, 0, 0, 1));
        JPH::Quat  localRot = rotMat.GetQuaternion().Normalized();

        reg.Add(e, Components::TransformComponent {.position = localPos, .rotation = localRot, .scale = localScale});
        reg.Add(e, Components::HierarchyComponent {.parent = rootEntity});
    }

    reg.Add(e, Components::NameComponent {.name = part.name});
    reg.Add(
        e, Components::MeshComponent {
               .mesh        = part.mesh,
               .material    = activeMat,
               .cullRadius  = part.boundingRadius,
               .localCenter = JPH::Vec3(
                   (part.localMax[0] + part.localMin[0]) * 0.5f, (part.localMax[1] + part.localMin[1]) * 0.5f, (part.localMax[2] + part.localMin[2]) * 0.5f
               ),
               .localTransform      = part.localTransform,
               .jointOffset         = assignedJointOffset,
               .isSkinned           = part.isSkinned && params.isAnimated,
               .skinnedVertexBuffer = skinnedVbo,
               .morphOffset         = part.morphOffset,
               .activeMorphCount    = part.activeMorphCount,
               .morphWeights        = {part.defaultMorphWeights[0], part.defaultMorphWeights[1], part.defaultMorphWeights[2], part.defaultMorphWeights[3]},
               .nodeIndex           = part.nodeIndex,
               .skeletonIndex       = part.skeletonIndex,
               .flags               = flags
           }
    );

    return e;
}

Entity TrySpawnEmissiveVPL(ECS::Registry& reg, const ModelPart& part, const JPH::Mat44& baseTransform, float scaleMult) {
    const float* ef  = part.defaultMaterial.emissiveFactor;
    float        lum = ef[0] * 0.2126f + ef[1] * 0.7152f + ef[2] * 0.0722f;
    if (lum <= 0.01f) {
        return NullEntity;
    }

    JPH::Vec3 localCenter(
        (part.localMax[0] + part.localMin[0]) * 0.5f, (part.localMax[1] + part.localMin[1]) * 0.5f, (part.localMax[2] + part.localMin[2]) * 0.5f
    );
    float partExtent = (part.localMax[0] - part.localMin[0]) + (part.localMax[1] - part.localMin[1]) + (part.localMax[2] - part.localMin[2]);

    Entity glowEnt = reg.Create();
    reg.Add(
        glowEnt,
        Components::TransformComponent {.position = baseTransform * localCenter, .rotation = JPH::Quat::sIdentity(), .scale = JPH::Vec3::sReplicate(1.0f)}
    );
    reg.Add(glowEnt, Components::NameComponent {.name = String64("Glow_" + std::string(part.name.c_str()))});
    reg.Add(
        glowEnt, Components::LightComponent {
                     .type        = LightType::Point,
                     .color       = JPH::Vec3(ef[0], ef[1], ef[2]),
                     .intensity   = lum * 35.0f,
                     .radius      = std::max(partExtent * scaleMult * 0.15f, 0.05f),
                     .direction   = JPH::Vec3(0, -1, 0),
                     .range       = std::max(partExtent * scaleMult * 2.5f, 3.0f),
                     .points      = {},
                     .twoSided    = 0,
                     .shadowLayer = -1
                 }
    );
    return glowEnt;
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
    Entity   rootEntity   = NullEntity;
    uint32_t startIndex   = 0;

    if (!params.createPhysics) {
        rootEntity = SpawnPrefabRoot(reg, prefab.virtualPath.c_str(), params);

        // ADD THIS: Attach a single master AnimatorComponent to the root entity
        if (params.isAnimated && !prefab.animations.empty()) {
            reg.Add(rootEntity, Components::AnimatorComponent {.currentTrackIdx = 0, .currentTrackTime = 0.0f, .currentLoop = true, .prefab = &prefab});
        }

        if (outBuffer != nullptr && maxCount > 0) {
            outBuffer[0] = rootEntity;
            startIndex   = 1;
            spawnedCount = 1;
        }
    }

    JPH::Mat44                baseTransform = Math::CreateTransform(JPH::Vec3(params.position), params.rotation, params.scale);
    std::vector<PreparedPart> preparedParts;
    PreparePrefabPhysics(prefab, baseTransform, params.createPhysics, params.useBoxColliders, preparedParts);

    float                                 scaleMult = std::max({params.scale.GetX(), params.scale.GetY(), params.scale.GetZ()});
    std::unordered_map<int32_t, uint32_t> allocatedSkeletons;

    for (size_t i = 0; i < prefab.parts.size(); ++i) {
        Entity meshEnt = InstantiateMeshPart(ctx, reg, pc, prefab, prefab.parts[i], preparedParts[i], params, rootEntity, allocatedSkeletons);

        if (outBuffer != nullptr && spawnedCount < maxCount) {
            outBuffer[startIndex + (spawnedCount - startIndex)] = meshEnt;
        }
        spawnedCount++;

        Entity glowEnt = TrySpawnEmissiveVPL(reg, prefab.parts[i], baseTransform * GetNodeLogicalTransform(prefab, prefab.parts[i].nodeIndex), scaleMult);
        if (glowEnt != NullEntity) {
            if (outBuffer != nullptr && spawnedCount < maxCount) {
                outBuffer[spawnedCount] = glowEnt;
            }
            spawnedCount++;
        }
    }

    return spawnedCount;
}

void SetupPlayerRagdoll(RenderContext& /*rc*/, PhysicsContext& pc, ECS::Registry& reg, Entity playerEntity, std::span<const Entity> visualParts) {
    const Skeleton* targetSkeleton = nullptr;
    uint32_t        jointOffset    = 0;

    for (Entity part: visualParts) {
        if (auto* meshComp = reg.Get<Components::MeshComponent>(part)) {
            if (auto* animComp = reg.Get<Components::AnimatorComponent>(part)) {
                if ((animComp->prefab != nullptr) && meshComp->skeletonIndex >= 0) {
                    targetSkeleton = &animComp->prefab->skeletons[meshComp->skeletonIndex];
                    jointOffset    = meshComp->jointOffset;
                    break;
                }
            }
        }
    }

    if (targetSkeleton != nullptr) {
        auto* joltSkel = new JPH::Skeleton();
        for (const auto& joint: targetSkeleton->joints) {
            std::string parentName = (joint.parentIndex >= 0) ? targetSkeleton->joints[joint.parentIndex].name.c_str() : "";
            joltSkel->AddJoint(joint.name.c_str(), parentName);
        }
        joltSkel->CalculateParentJointIndices();

        auto IsImportantJoint = [](std::string name) -> bool {
            std::ranges::transform(name, name.begin(), ::tolower);
            return name.contains("hip") || name.contains("pelvis") || name.contains("root") || name.contains("spine") || name.contains("chest") ||
                   name.contains("torso") || name.contains("head") || name.contains("neck") || name.contains("arm") || name.contains("forearm") ||
                   name.contains("thigh") || name.contains("calf") || name.contains("shin");
        };

        std::vector<Physics::RagdollPartParams> parts;
        for (size_t i = 0; i < targetSkeleton->joints.size(); ++i) {
            std::string name = targetSkeleton->joints[i].name.c_str();

            Physics::RagdollPartParams part;
            part.jointIndex       = (uint32_t) i;
            part.parentJointIndex = targetSkeleton->joints[i].parentIndex;
            part.mass             = 1.0f;
            part.enableMotors     = false;

            // Compute global bind pose for the joint
            JPH::Mat44 bindPose = targetSkeleton->joints[i].inverseBindMatrix.Inversed();
            part.position       = JPH::RVec3(bindPose.GetTranslation());
            part.rotation       = bindPose.GetQuaternion().Normalized();

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
                part.shape = Physics::GetOrCreateShape(pc, Physics::ShapeType::Capsule, 0.2f, 0.1f);
                part.mass  = 3.0f;
            } else {
                part.shape = Physics::GetOrCreateShape(pc, Physics::ShapeType::Sphere, 0.08f);
                part.mass  = 0.5f;
            }
            parts.push_back(part);
        }

        auto ragdollInstance = Physics::CreateSkeletalRagdoll(pc, joltSkel, parts);
        ragdollInstance->AddRef();

        reg.Add(
            playerEntity, Components::RagdollComponent {
                              .ragdollInstance  = ragdollInstance.GetPtr(),
                              .state            = RagdollState::Inactive,
                              .prevState        = RagdollState::Inactive,
                              .isAddedToPhysics = 0,
                              .jointOffset      = jointOffset,
                              .jointCount       = static_cast<uint32_t>(targetSkeleton->joints.size()),
                              .skeleton         = targetSkeleton
                          }
        );
        Log("Skeletal Ragdoll successfully generated from Native Skeleton.");
    } else {
        Log("WARNING: SetupPlayerRagdoll failed because no skeleton was found.");
    }
}

// ============================================================================
// DEVICE LOST HOT-RECOVERY (Rebuilding VRAM buffers)
// ============================================================================

static void
    ReuploadAllPrefabs(RenderContext& ctx, CreativeWorksManager& cwMgr, std::unordered_map<BufferHandle, std::pair<Mesh, Material>>& outMeshRebuildMap) {
    uint32_t count = cwMgr.GetCachedPrefabs(nullptr, 0);
    if (count == 0) {
        return;
    }

    std::vector<ModelPrefab*> prefabs(count);
    cwMgr.GetCachedPrefabs(prefabs.data(), count);

    for (auto* prefab: prefabs) {
        std::vector<BufferHandle> oldHandles;
        oldHandles.reserve(prefab->parts.size());
        for (const auto& part: prefab->parts) {
            oldHandles.push_back(part.mesh.posBuffer);
        }

        GLTF::RebuildPrefabGPUResources(ctx, cwMgr, prefab);

        for (size_t i = 0; i < prefab->parts.size(); ++i) {
            if (oldHandles[i] != BufferHandle::Invalid) {
                outMeshRebuildMap[oldHandles[i]] = {prefab->parts[i].mesh, prefab->parts[i].defaultMaterial};
            }
        }
    }
}

void RebuildVulkanResources(RenderContext& ctx, CreativeWorksManager& cwMgr, ECS::Registry& reg) {
    ZHLN::Log("[Engine] Re-uploading all textures and meshes to new Vulkan device...");

    CreateFontAtlasTexture(ctx);

    std::unordered_map<BufferHandle, std::pair<Mesh, Material>> meshRebuildMap;
    ReuploadAllPrefabs(ctx, cwMgr, meshRebuildMap);

    // Create a fresh fallback material in the new Vulkan context
    auto fallbackMat = CreativeWorksFactory::CreateBasicMaterial(ctx).value_or(Material {});

    auto entities = reg.GetEntitiesWith<Components::MeshComponent>();
    auto meshes   = reg.GetRawArray<Components::MeshComponent>();

    for (size_t i = 0; i < entities.size(); ++i) {
        Components::MeshComponent& meshComp     = meshes[i];
        BufferHandle               oldPosBuffer = meshComp.mesh.posBuffer;

        if (meshComp.isSkinned && meshComp.skinnedVertexBuffer != BufferHandle::Invalid) {
            meshComp.skinnedVertexBuffer = ctx.CreateSkinnedScratchBuffer(meshComp.mesh.vertexCount);
        }

        if (oldPosBuffer != BufferHandle::Invalid) {
            auto it = meshRebuildMap.find(oldPosBuffer);
            if (it != meshRebuildMap.end()) {
                meshComp.mesh     = it->second.first;
                meshComp.material = it->second.second;
            } else {
                // Assign a valid fallback material for procedural meshes
                meshComp.material.pipeline = fallbackMat.pipeline;
            }
        }
    }

    // Rebuild Debug VBOs in new context
    for (Entity e: reg.GetEntitiesWith<Components::DebugSettingsComponent>()) {
        if (auto* dbg = reg.Get<Components::DebugSettingsComponent>(e)) {
            Mesh lineMesh          = CreativeWorksFactory::CreateBox(ctx, {0.01f, 0.01f, 0.5f}, {0.0f, 1.0f, 1.0f, 1.0f});
            dbg->debugLineVbo      = lineMesh.posBuffer;
            dbg->debugLinePipeline = fallbackMat.pipeline;
            dbg->debugLineAlbedo   = fallbackMat.albedoIndex;
        }
    }

    for (Entity e: reg.GetEntitiesWith<Components::TextComponent>()) {
        if (auto* text = reg.Get<Components::TextComponent>(e)) {
            text->mesh.posBuffer   = BufferHandle::Invalid;
            text->mesh.attrBuffer  = BufferHandle::Invalid;
            text->mesh.indexBuffer = BufferHandle::Invalid;
        }
    }
}

} // namespace ZHLN::CreativeWorksFactory
