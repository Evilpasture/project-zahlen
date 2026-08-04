// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include <Jolt/Jolt.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/ScaledShape.h>
#include <Zahlen/Components.hpp>
#include <Zahlen/CreativeWorksFactory.hpp>
#include <Zahlen/CreativeWorksManager.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Font8x8.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Math3D.hpp>
#include <Zahlen/ModelPrefab.hpp>
#include <Zahlen/Render.hpp>
#include <Zahlen/SkeletalAnimation.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <Zahlen/physics/Physics.hpp>
#include <algorithm>
#include <cstddef>
#include <engine/system/AnimationSystem.hpp>
#include <engine/system/LightingSystem.hpp>
#include <gltf/GLTFImporter.hpp>
#include <stb_image.h>
#include <Zahlen/Threading/TaskSystem.hpp>
#define STB_TRUETYPE_IMPLEMENTATION
#include <fontconfig/fontconfig.h>
#include <stb_truetype.h>

namespace ZHLN::CreativeWorksFactory {

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

    stbtt_fontinfo fontInfo {};
    if (!stbtt_InitFont(&fontInfo, fontBuffer.data(), fontOffset)) {
        Log("ERROR: stbtt_InitFont failed for {}", fontPath);
        return 0;
    }

    const uint32_t       atlasSize = 512;
    std::vector<uint8_t> alphaBitmap(static_cast<size_t>(atlasSize * atlasSize), 0);

    auto* engine             = GetEngineContext();
    auto& reg                = engine->GetRegistry();
    auto  uiSettingsEntities = reg.GetEntitiesWith<Components::UISettingsComponent>();
    if (uiSettingsEntities.empty()) {
        return 0;
    }
    auto* uiSettings = reg.Get<Components::UISettingsComponent>(uiSettingsEntities[0]);

    // SDF Generation Parameters
    const float   fontSize         = 24.0f;
    const float   scale            = stbtt_ScaleForPixelHeight(&fontInfo, fontSize);
    const int     padding          = 5;
    const uint8_t onedge_value     = 128; // 0.5 in normalized space
    const float   pixel_dist_scale = 128.0f / static_cast<float>(padding);

    uint32_t curX      = 1;
    uint32_t curY      = 1;
    uint32_t rowHeight = 0;

    for (int i = 0; i < 96; ++i) {
        int codepoint = 32 + i;
        int w         = 0;
        int h         = 0;
        int xoff      = 0;
        int yoff      = 0;
        int advance   = 0;
        int lsb       = 0;

        stbtt_GetCodepointHMetrics(&fontInfo, codepoint, &advance, &lsb);
        float xadvance = static_cast<float>(advance) * scale;

        unsigned char* sdf = stbtt_GetCodepointSDF(&fontInfo, scale, codepoint, padding, onedge_value, pixel_dist_scale, &w, &h, &xoff, &yoff);

        if (sdf != nullptr && w > 0 && h > 0) {
            if (curX + w + 1 > atlasSize) {
                curX = 1;
                curY += rowHeight + 1;
                rowHeight = 0;
            }

            if (curY + h + 1 > atlasSize) {
                Log("WARNING: Font atlas size exceeded! Glyphs truncated.");
                stbtt_FreeSDF(sdf, nullptr);
                break;
            }

            for (int row = 0; row < h; ++row) {
                for (int col = 0; col < w; ++col) {
                    alphaBitmap[(curY + row) * atlasSize + (curX + col)] = sdf[row * w + col];
                }
            }

            uiSettings->fontAtlas.glyphs[i] = GlyphMetric {
                .x0       = static_cast<float>(curX),
                .y0       = static_cast<float>(curY),
                .x1       = static_cast<float>(curX + w),
                .y1       = static_cast<float>(curY + h),
                .xoff     = static_cast<float>(xoff),
                .yoff     = static_cast<float>(yoff),
                .xadvance = xadvance
            };

            curX += w + 1;
            rowHeight = std::max(rowHeight, static_cast<uint32_t>(h));
            stbtt_FreeSDF(sdf, nullptr);
        } else {
            if (sdf != nullptr) {
                stbtt_FreeSDF(sdf, nullptr);
            }
            uiSettings->fontAtlas.glyphs[i] = GlyphMetric {.x0 = 0.0f, .y0 = 0.0f, .x1 = 0.0f, .y1 = 0.0f, .xoff = 0.0f, .yoff = 0.0f, .xadvance = xadvance};
        }
    }

    std::vector<uint32_t> rgbaPixels(static_cast<size_t>(atlasSize * atlasSize));
    for (uint32_t i = 0; i < atlasSize * atlasSize; ++i) {
        uint8_t dist  = alphaBitmap[i];
        rgbaPixels[i] = (static_cast<uint32_t>(dist) << 24) | 0x00FFFFFF;
    }

    auto tex_res = ctx.CreateTexture(rgbaPixels.data(), atlasSize, atlasSize, false);
    if (!tex_res) {
        Log("ERROR: CreateFontAtlasTexture failed to create Vulkan texture: {}", tex_res.error().Message());
        return 0;
    }
    uint32_t texIdx = tex_res.value();

    uiSettings->fontAtlas.textureIndex = texIdx;
    uiSettings->defaultFontAtlasIdx    = texIdx;

    return texIdx;
}

uint32_t LoadTexture(RenderContext& ctx, CreativeWorksManager& assetMgr, std::string_view path, bool isSRGB) {
    uint64_t hash = HashCreativeWorkPath(path);

    CreativeWorkLoadRequest req;
    req.assetID = hash;

    // 1. Fetch raw compressed file bytes from VFS / PAK
    if (!assetMgr.LoadSync(req)) {
        ZHLN::Log("WARNING: Failed to load texture asset from VFS: {}", path);
        return 1; // Fallback to white pixel (Slot 1)
    }

    // 2. Decompress PNG/JPG in memory
    int            width    = 0;
    int            height   = 0;
    int            channels = 0;
    unsigned char* pixels   = stbi_load_from_memory(static_cast<const stbi_uc*>(req.outData), static_cast<int>(req.outSize), &width, &height, &channels, 4);

    // Free VFS memory immediately
    assetMgr.FreeCreativeWorkMemory(req);

    if (pixels == nullptr) {
        ZHLN::Log("ERROR: stbi_load_from_memory failed for texture: {}", path);
        return 1;
    }

    // 3. Register raw pixels with Vulkan GPU
    auto texRes = ctx.CreateTexture(pixels, static_cast<uint32_t>(width), static_cast<uint32_t>(height), isSRGB);
    stbi_image_free(pixels);

    return texRes ? *texRes : 1;
}

ModelPrefab* LoadModelPrefab(RenderContext& ctx, CreativeWorksManager& assetMgr, std::string_view path) {
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
    // Pure integer ID usage - cast scoped enum to uint64_t directly
    AssetID    meshAsset = part.meshAsset;
    MaterialID matAsset  = params.materialOverride.pipeline != PipelineHandle::Invalid ? static_cast<uint64_t>(params.materialOverride.pipeline) :
                                                                                         part.materialAsset;

    Material activeMat = params.materialOverride.pipeline != PipelineHandle::Invalid ? params.materialOverride : part.defaultMaterial;

    ctx.RegisterGPUMesh(meshAsset, part.mesh);
    ctx.RegisterGPUMaterial(matAsset, activeMat);

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

    Entity    e     = reg.Create();
    DrawFlags flags = DrawFlags::None;
    if (part.isSkinned && params.isAnimated) {
        flags |= DrawFlags::Skinned;
    }
    if (activeMat.alphaMode == 2) {
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
        JPH::Mat44 nodeLocal = GetNodeLogicalTransform(prefab, part.nodeIndex) * part.localTransform;
        JPH::Vec3  localPos  = nodeLocal.GetTranslation();

        JPH::Vec3 c0 = nodeLocal.GetColumn3(0);
        JPH::Vec3 c1 = nodeLocal.GetColumn3(1);
        JPH::Vec3 c2 = nodeLocal.GetColumn3(2);
        JPH::Vec3 localScale(c0.Length(), c1.Length(), c2.Length());

        if (localScale.GetX() > 1e-5f)
            c0 /= localScale.GetX();
        else
            c0 = JPH::Vec3::sAxisX();
        if (localScale.GetY() > 1e-5f)
            c1 /= localScale.GetY();
        else
            c1 = JPH::Vec3::sAxisY();
        if (localScale.GetZ() > 1e-5f)
            c2 /= localScale.GetZ();
        else
            c2 = JPH::Vec3::sAxisZ();

        JPH::Mat44 rotMat(JPH::Vec4(c0, 0), JPH::Vec4(c1, 0), JPH::Vec4(c2, 0), JPH::Vec4(0, 0, 0, 1));
        JPH::Quat  localRot = rotMat.GetQuaternion().Normalized();

        reg.Add(e, Components::TransformComponent {.position = localPos, .rotation = localRot, .scale = localScale});
        reg.Add(e, Components::HierarchyComponent {.parent = rootEntity});
    }

    reg.Add(e, Components::NameComponent {.name = part.name});

    reg.Add(
        e, Components::MeshComponent {
               .meshAsset     = meshAsset,
               .materialAsset = matAsset,
               .cullRadius    = part.boundingRadius,
               .localCenter   = JPH::Vec3(
                   (part.localMax[0] + part.localMin[0]) * 0.5f, (part.localMax[1] + part.localMin[1]) * 0.5f, (part.localMax[2] + part.localMin[2]) * 0.5f
               ),
               .localTransform   = part.localTransform,
               .jointOffset      = assignedJointOffset,
               .isSkinned        = part.isSkinned && params.isAnimated,
               .morphOffset      = part.morphOffset,
               .activeMorphCount = part.activeMorphCount,
               .morphWeights     = {part.defaultMorphWeights[0], part.defaultMorphWeights[1], part.defaultMorphWeights[2], part.defaultMorphWeights[3]},
               .nodeIndex        = part.nodeIndex,
               .skeletonIndex    = part.skeletonIndex,
               .flags            = flags
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

    std::unordered_map<std::string, Entity> instantiatedParts;

    for (size_t i = 0; i < prefab.parts.size(); ++i) {
        Entity meshEnt = InstantiateMeshPart(ctx, reg, pc, prefab, prefab.parts[i], preparedParts[i], params, rootEntity, allocatedSkeletons);

        // Store the name to entity mapping for CSG resolution
        instantiatedParts[prefab.parts[i].name.c_str()] = meshEnt;

        if (outBuffer != nullptr && spawnedCount < maxCount) {
            outBuffer[startIndex + (spawnedCount - startIndex)] = meshEnt;
        }
        spawnedCount++;

        // --- PRESERVED EMISSIVE VPL GENERATOR ---
        Entity glowEnt = TrySpawnEmissiveVPL(reg, prefab.parts[i], baseTransform * GetNodeLogicalTransform(prefab, prefab.parts[i].nodeIndex), scaleMult);
        if (glowEnt != NullEntity) {
            if (outBuffer != nullptr && spawnedCount < maxCount) {
                outBuffer[spawnedCount] = glowEnt;
            }
            spawnedCount++;
        }
    }

    // --- RESOLVE CSG MODIFIERS AND LINK ECS COMPONENTS ---
    for (const auto& part: prefab.parts) {
        if (!part.csgModifiers.empty()) {
            std::string partName = part.name.c_str();
            auto        it       = instantiatedParts.find(partName);
            if (it != instantiatedParts.end()) {
                Entity targetEntity = it->second;

                Components::CSGComponent csgComp;
                for (const auto& mod: part.csgModifiers) {
                    auto opIt = instantiatedParts.find(mod.operand_name);
                    if (opIt != instantiatedParts.end()) {
                        csgComp.modifiers.push_back({.operation = mod.operation, .operandEntity = opIt->second});

                        // Exclude the operand cutter from standard main/shadow draw passes
                        Patch<Components::MeshComponent>(reg, opIt->second, [&](auto& cutMesh) {
                            cutMesh.flags |= DrawFlags::Hidden;
                        });
                    }
                }

                if (!csgComp.modifiers.empty()) {
                    reg.Add(targetEntity, std::move(csgComp));
                }
            }
        }
    }

    return spawnedCount;
}

void SetupPlayerRagdoll(RenderContext& /*rc*/, PhysicsContext& pc, ECS::Registry& reg, Entity playerEntity, std::span<const Entity> visualParts) {
    const Skeleton* targetSkeleton = nullptr;
    uint32_t        jointOffset    = 0;

    bool skeletonFound = false;
    for (Entity part: visualParts) {
        Patch<Components::MeshComponent>(reg, part, [&](auto& meshComp) {
            Patch<Components::AnimatorComponent>(reg, part, [&](auto& animComp) {
                if ((animComp.prefab != nullptr) && meshComp.skeletonIndex >= 0) {
                    targetSkeleton = &animComp.prefab->skeletons[meshComp.skeletonIndex];
                    jointOffset    = meshComp.jointOffset;
                    skeletonFound  = true;
                }
            });
        });
        if (skeletonFound) break;
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
// DEVICE LOST RECOVERY
// ============================================================================

void RebuildVulkanResources(RenderContext& ctx, CreativeWorksManager& cwMgr, ECS::Registry& /*reg*/) {
    ZHLN::Log("[Engine] Device Lost: Clearing GPU asset cache. Next frame will re-upload assets lazily.");

    ctx.ClearGPUCaches();
    CreateFontAtlasTexture(ctx);

    uint32_t count = cwMgr.GetCachedPrefabs(nullptr, 0);
    if (count > 0) {
        std::vector<ModelPrefab*> prefabs(count);
        cwMgr.GetCachedPrefabs(prefabs.data(), count);

        for (auto* prefab: prefabs) {
            GLTF::RebuildPrefabGPUResources(ctx, cwMgr, prefab);
            for (size_t i = 0; i < prefab->parts.size(); ++i) {
                std::string assetKeyStr = std::string(prefab->virtualPath.c_str()) + "#" + prefab->parts[i].name.c_str() + "_" +
                                          std::to_string(prefab->parts[i].nodeIndex);
                ctx.RegisterGPUMesh(HashAssetID(assetKeyStr), prefab->parts[i].mesh);
                ctx.RegisterGPUMaterial(HashAssetID(assetKeyStr + "_mat"), prefab->parts[i].defaultMaterial);
            }
        }
    }
}

ModelPrefab* LoadModelPrefab(Engine& engine, std::string_view path) {
    return LoadModelPrefab(engine.GetRenderContext(), engine.GetCreativeWorksManager(), path);
}

uint32_t InstantiatePrefab(Engine& engine, const ModelPrefab& prefab, const SpawnParams& params, Entity* outBuffer, uint32_t maxCount) {
    return InstantiatePrefab(engine.GetRenderContext(), engine.GetRegistry(), engine.GetPhysicsContext(), prefab, params, outBuffer, maxCount);
}

uint32_t InstantiatePrefab(Engine& engine, std::string_view path, const SpawnParams& params, Entity* outBuffer, uint32_t maxCount) {
    ModelPrefab* prefab = LoadModelPrefab(engine, path);
    if (prefab == nullptr) {
        return 0;
    }
    return InstantiatePrefab(engine, *prefab, params, outBuffer, maxCount);
}

void SetupPlayerRagdoll(Engine& engine, Entity playerEntity, std::span<const Entity> visualParts) {
    SetupPlayerRagdoll(engine.GetRenderContext(), engine.GetPhysicsContext(), engine.GetRegistry(), playerEntity, visualParts);
}

} // namespace ZHLN::CreativeWorksFactory
