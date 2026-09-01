// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// clang-format off
#include <Jolt/Jolt.h>
// clang-format on
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
#include <Zahlen/PrefabLoader.hpp>
#include <Zahlen/Render.hpp>
#include <Zahlen/SkeletalAnimation.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <Zahlen/physics/Physics.hpp>
#include <algorithm>
#include <cstddef>
#include <engine/system/AnimationSystem.hpp>
#include <engine/system/ArticulationSystem.hpp>
#include <engine/system/LightingSystem.hpp>
#include <engine/system/TerrainSystem.hpp>
#include <stb_image.h>
#define STB_TRUETYPE_IMPLEMENTATION
#include <fontconfig/fontconfig.h>
#include <stb_truetype.h>

namespace ZHLN::CreativeWorksFactory {

static auto FindSystemFont(const char* fontName) -> std::string {
#ifdef __APPLE__
    const char* macFallbacks[] = {
        "/System/Library/Fonts/Supplemental/Arial.ttf", "/System/Library/Fonts/Supplemental/Helvetica.ttf", "/System/Library/Fonts/Supplemental/Verdana.ttf",
        "/System/Library/Fonts/Supplemental/Courier New.ttf"
    };
    for (const auto* path: macFallbacks) {
        FILE* f = std::fopen(path, "rb");
        if (f != nullptr) {
            std::fclose(f);
            return path;
        }
    }
#endif

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

auto CreateFontAtlasTexture(RenderContext& ctx, ECS::Registry& registry) -> TextureHandle {
    std::string fontPath = FindSystemFont("sans-serif");
    if (fontPath.empty()) {
        fontPath = "/usr/share/fonts/TTF/DejaVuSans.ttf";
    }

    Log("Loading TrueType system font: {}", fontPath);

    FILE* f = std::fopen(fontPath.c_str(), "rb");
    if (f == nullptr) {
        Log("ERROR: Failed to open system font file: {}", fontPath);
        return TextureHandle::Invalid;
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
        return TextureHandle::Invalid;
    }

    const uint32_t       atlasSize = 1024;
    std::vector<uint8_t> alphaBitmap(static_cast<size_t>(atlasSize * atlasSize), 0);

    auto* uiSettings = registry.GetSingleton<Components::UISettingsComponent>();
    if (uiSettings == nullptr) {
        return TextureHandle::Invalid;
    }

    const float   fontSize         = 32.0f;
    const float   scale            = stbtt_ScaleForPixelHeight(&fontInfo, fontSize);
    const int     padding          = 6;
    const uint8_t onedge_value     = 128;
    const float   pixel_dist_scale = 128.0f / static_cast<float>(padding);

    uint32_t curX      = 2;
    uint32_t curY      = 2;
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
            if (curX + w + 2 > atlasSize) {
                curX = 2;
                curY += rowHeight + 2;
                rowHeight = 0;
            }

            if (curY + h + 2 > atlasSize) {
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

            curX += w + 2;
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

    TextureHandle texHandle = ctx.CreateProceduralTexture("FontAtlas", atlasSize, atlasSize, false, rgbaPixels.data());

    uiSettings->fontAtlas.texture = texHandle;
    uiSettings->defaultFontAtlas  = texHandle;

    return texHandle;
}

auto LoadTexture(RenderContext& ctx, CreativeWorksManager& assetMgr, std::string_view path, bool isSRGB) -> uint32_t {
    uint64_t hash = HashCreativeWorkPath(path);

    CreativeWorkLoadRequest req;
    req.assetID = hash;

    if (!assetMgr.LoadSync(req)) {
        ZHLN::Log("WARNING: Failed to load texture asset from VFS: {}", path);
        return 1;
    }

    int            width    = 0;
    int            height   = 0;
    int            channels = 0;
    unsigned char* pixels   = stbi_load_from_memory(static_cast<const stbi_uc*>(req.outData), static_cast<int>(req.outSize), &width, &height, &channels, 4);

    assetMgr.FreeCreativeWorkMemory(req);

    if (pixels == nullptr) {
        ZHLN::Log("ERROR: stbi_load_from_memory failed for texture: {}", path);
        return 1;
    }

    auto texRes = ctx.CreateTexture(pixels, static_cast<uint32_t>(width), static_cast<uint32_t>(height), isSRGB);
    stbi_image_free(pixels);

    return texRes ? *texRes : 1;
}

namespace {

/// The registered model-file reader, or null. Reading a .glb means a container
/// parser, an image decoder and a schema, all of which live in extras/glTF; on
/// a build without them every prefab entry point below reports the miss here
/// and returns null. Logged once, because a scene full of prefabs would
/// otherwise print a line per entity per frame.
[[nodiscard]] auto PrefabBackend() -> const PrefabLoader::Backend* {
    const auto* backend = PrefabLoader::Get();
    if (backend == nullptr) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            ZHLN::Log(
                "[CreativeWorksFactory] no prefab loader registered: .glb/.gltf import lives in extras/glTF. "
                "Call ZHLN::glTF::RegisterPrefabLoader() at startup, or build with -DZHLN_BUILD_EXTRAS=ON and link zahlen_extras."
            );
        }
    }
    return backend;
}

} // namespace

auto LoadModelPrefab(RenderContext& ctx, CreativeWorksManager& assetMgr, std::string_view path) -> ModelPrefab* {
    const auto* backend = PrefabBackend();
    return (backend != nullptr) ? backend->Load(ctx, assetMgr, path) : nullptr;
}

namespace {

auto SpawnPrefabRoot(ECS::Registry& reg, std::string_view vPath, const SpawnParams& p) -> Entity {
    Entity     root     = reg.Create();
    JPH::Mat44 localMat = Math::CreateTransform(JPH::Vec3(p.position), p.rotation, p.scale);
    reg.Add(root, Components::TransformComponent {.position = JPH::Vec3(p.position), .rotation = p.rotation, .scale = p.scale});
    reg.Add(root, Components::WorldTransformComponent {.world = localMat, .previous = localMat});
    reg.Add(root, Components::NameComponent {.name = String64("Root_" + std::string(vPath))});
    return root;
}

auto GetNodeLogicalTransform(const ModelPrefab& prefab, int32_t nodeIndex) -> JPH::Mat44 {
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

    TaskSystem::ParallelFor(prefab.parts.size(), 16, [&](uint32_t start, uint32_t end, uint32_t) -> void {
        for (uint32_t i = start; i < end; ++i) {
            const auto& part = prefab.parts[i];
            auto&       prep = outPrepared[i];

            JPH::Mat44 nodeWorld  = GetNodeLogicalTransform(prefab, part.nodeIndex);
            JPH::Mat44 finalLocal = baseTransform * nodeWorld * part.localTransform;

            const Math::TransformTRS trs = Math::Decompose(finalLocal);

            prep.scale       = trs.scale;
            prep.rotation    = trs.rotation;
            prep.translation = trs.translation;
            prep.maxScale    = std::max({std::abs(trs.scale.GetX()), std::abs(trs.scale.GetY()), std::abs(trs.scale.GetZ())});

            if (createPhysics) {
                JPH::ShapeRefC rawShape = useBoxColliders ? part.boxCollider : part.meshCollider;
                if (rawShape != nullptr) {
                    prep.shape = !prep.scale.IsClose(JPH::Vec3::sReplicate(1.0f), 1e-5f) ? new JPH::ScaledShape(rawShape, prep.scale) : rawShape;
                }
            }
        }
    });
}

auto InstantiateMeshPart(
    RenderContext&                         ctx,
    ECS::Registry&                         reg,
    PhysicsContext&                        pc,
    const ModelPrefab&                     prefab,
    const ModelPart&                       part,
    const PreparedPart&                    prep,
    const SpawnParams&                     params,
    Entity                                 rootEntity,
    std::unordered_map<int32_t, uint32_t>& allocatedSkeletons
) -> Entity {
    const JPH::Mat44 baseTransform = Math::CreateTransform(JPH::Vec3(params.position), params.rotation, params.scale); // <-- ADDED HERE

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

    JPH::Mat44 worldMat = Math::CreateTransform(prep.translation, prep.rotation, prep.scale);

    if (params.createPhysics && prep.shape != nullptr) {
        reg.Add(e, Components::TransformComponent {.position = prep.translation, .rotation = prep.rotation, .scale = prep.scale});
        reg.Add(e, Components::WorldTransformComponent {.world = worldMat, .previous = worldMat});

        reg.Add(
            e, Components::PhysicsComponent {pc.CreateRigidBody(
                   prep.shape, JPH::RVec3(prep.translation), prep.rotation, params.isStaticPhysics ? JPH::EMotionType::Static : JPH::EMotionType::Dynamic,
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
    } else if (part.isSkinned && params.isAnimated) {
        // Skinned meshes are posed by the skeleton in root space
        reg.Add(e, Components::TransformComponent {.position = JPH::Vec3::sZero(), .rotation = JPH::Quat::sIdentity(), .scale = JPH::Vec3::sReplicate(1.0f)});
        reg.Add(e, Components::WorldTransformComponent {.world = baseTransform, .previous = baseTransform});
        reg.Add(e, Components::HierarchyComponent {.parent = rootEntity});
    } else {
        // Non-skinned accessories use their local node offset
        const JPH::Mat44         nodeLocal  = GetNodeLogicalTransform(prefab, part.nodeIndex) * part.localTransform;
        const Math::TransformTRS localTRS   = Math::Decompose(nodeLocal);
        const JPH::Vec3&         localPos   = localTRS.translation;
        const JPH::Quat&         localRot   = localTRS.rotation;
        const JPH::Vec3&         localScale = localTRS.scale;

        reg.Add(e, Components::TransformComponent {.position = localPos, .rotation = localRot, .scale = localScale});
        reg.Add(e, Components::WorldTransformComponent {.world = worldMat, .previous = worldMat});
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
               .flags     = flags,
               .nodeIndex = part.nodeIndex
           }
    );

    if (part.isSkinned && params.isAnimated) {
        reg.Add(e, Components::SkeletalMeshComponent {.jointOffset = assignedJointOffset, .skeletonIndex = part.skeletonIndex});
    }

    if (part.activeMorphCount > 0) {
        reg.Add(
            e, Components::MorphTargetComponent {
                   .offset      = part.morphOffset,
                   .activeCount = part.activeMorphCount,
                   .weights     = {part.defaultMorphWeights[0], part.defaultMorphWeights[1], part.defaultMorphWeights[2], part.defaultMorphWeights[3]}
               }
        );
    }

    return e;
}

// Spawns a cheap point light approximating the bounce from an emissive part.
//
// The light is parented to the part entity and positioned in *part-local*
// space, so it inherits the part's world transform every frame: move or
// animate the model and the glow goes with it. Baking a world position here
// instead is what used to leave a puddle of lights at the spawn point while
// the model itself went dark once it moved.
auto TrySpawnEmissiveVPL(ECS::Registry& reg, const ModelPart& part, Entity parentEntity, float scaleMult) -> Entity {
    // The imported factor is in engine HDR units (kGLTFEmissiveDisplayScale
    // converts glTF's [0,1] on the way in). A light wants the authored colour
    // and an intensity in light units, so the display conversion is undone
    // here -- otherwise opting into VPLs would spawn a 100x overbright lamp.
    static constexpr float kInvDisplayScale = 1.0f / kGLTFEmissiveDisplayScale;

    const float* raw = part.defaultMaterial.emissiveFactor;
    const float  ef[3] {raw[0] * kInvDisplayScale, raw[1] * kInvDisplayScale, raw[2] * kInvDisplayScale};

    float lum = ef[0] * 0.2126f + ef[1] * 0.7152f + ef[2] * 0.0722f;
    if (lum <= 0.01f) {
        return Entity::Null();
    }

    JPH::Vec3 localCenter(
        (part.localMax[0] + part.localMin[0]) * 0.5f, (part.localMax[1] + part.localMin[1]) * 0.5f, (part.localMax[2] + part.localMin[2]) * 0.5f
    );
    float partExtent = (part.localMax[0] - part.localMin[0]) + (part.localMax[1] - part.localMin[1]) + (part.localMax[2] - part.localMin[2]);

    Entity glowEnt = reg.Create();
    reg.Add(glowEnt, Components::TransformComponent {.position = localCenter, .rotation = JPH::Quat::sIdentity(), .scale = JPH::Vec3::sReplicate(1.0f)});
    reg.Add(glowEnt, Components::HierarchyComponent {.parent = parentEntity});
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

auto CreateBox(RenderContext& ctx, ECS::Registry& reg, PhysicsContext* pc, JPH::Vec3Arg halfExtents, const SpawnParams& params) -> Entity {
    JPH::Vec4 boxColor = (params.materialOverride.baseColorFactor[3] >= 0.0f) ?
                             JPH::Vec4(
                                 params.materialOverride.baseColorFactor[0], params.materialOverride.baseColorFactor[1],
                                 params.materialOverride.baseColorFactor[2], params.materialOverride.baseColorFactor[3]
                             ) :
                             JPH::Vec4(0.8f, 0.4f, 0.2f, 1.0f);

    Mesh mesh = CreateBoxMesh(ctx, halfExtents, boxColor);

    Material mat;
    if (params.materialOverride.pipeline != PipelineHandle::Invalid) {
        mat = params.materialOverride;
    } else {
        auto mat_res           = CreateBasicMaterial(ctx, false, false, false);
        mat                    = mat_res.value_or(Material {});
        mat.baseColorFactor[0] = boxColor.GetX();
        mat.baseColorFactor[1] = boxColor.GetY();
        mat.baseColorFactor[2] = boxColor.GetZ();
        mat.baseColorFactor[3] = boxColor.GetW();
        mat.roughnessFactor    = 0.3f;
        mat.metallicFactor     = 0.1f;
    }

    Entity     e         = reg.Create();
    AssetID    meshAsset = HashAssetID("prefab_box_mesh_" + std::to_string(e.index));
    MaterialID matAsset  = HashAssetID("prefab_box_mat_" + std::to_string(e.index));

    ctx.RegisterGPUMesh(meshAsset, mesh);
    ctx.RegisterGPUMaterial(matAsset, mat);

    JPH::Mat44 worldMat = Math::CreateTransform(JPH::Vec3(params.position), params.rotation, params.scale);

    reg.Add(e, Components::NameComponent {.name = String64("Box_" + std::to_string(e.index))});
    reg.Add(e, Components::TransformComponent {.position = JPH::Vec3(params.position), .rotation = params.rotation, .scale = params.scale});
    reg.Add(e, Components::WorldTransformComponent {.world = worldMat, .previous = worldMat});

    float maxExtent = std::max({halfExtents.GetX(), halfExtents.GetY(), halfExtents.GetZ()});
    reg.Add(e, Components::MeshComponent {.meshAsset = meshAsset, .materialAsset = matAsset, .cullRadius = maxExtent * 2.0f});
    reg.Add(e, Components::PBRComponent {.roughness = mat.roughnessFactor, .metallic = mat.metallicFactor});

    if (params.createPhysics && pc != nullptr) {
        // FIXED: Used pc->GetOrCreateShape
        auto shape = pc->GetOrCreateShape(
            Physics::ShapeType::Box, halfExtents.GetX() * params.scale.GetX(), halfExtents.GetY() * params.scale.GetY(),
            halfExtents.GetZ() * params.scale.GetZ()
        );
        // FIXED: Used pc->CreateRigidBody
        auto body = pc->CreateRigidBody(
            shape, params.position, params.rotation, params.isStaticPhysics ? JPH::EMotionType::Static : JPH::EMotionType::Dynamic,
            params.isStaticPhysics ? static_cast<JPH::ObjectLayer>(0) : static_cast<JPH::ObjectLayer>(1), 0, params.physicsCategory, params.physicsMask
        );
        reg.Add(e, Components::PhysicsComponent {body});
        if (!params.isStaticPhysics) {
            reg.Add(
                e, Components::PhysicsStateComponent {
                       .currPosition = JPH::Vec3(params.position),
                       .prevPosition = JPH::Vec3(params.position),
                       .currRotation = params.rotation,
                       .prevRotation = params.rotation
                   }
            );
        }
    }

    return e;
}

auto CreateBox(Engine& engine, JPH::Vec3Arg halfExtents, const SpawnParams& params) -> Entity {
    return CreateBox(engine.GetRenderContext(), engine.GetRegistry(), &engine.GetPhysicsContext(), halfExtents, params);
}

auto CreatePlane(RenderContext& ctx, ECS::Registry& reg, PhysicsContext* pc, float extent, const JPH::Vec4& color, const SpawnParams& params) -> Entity {
    Mesh mesh = CreatePlaneMesh(ctx, extent, color);

    Material mat;
    if (params.materialOverride.pipeline != PipelineHandle::Invalid) {
        mat = params.materialOverride;
    } else {
        auto mat_res           = CreateBasicMaterial(ctx, false, false, false);
        mat                    = mat_res.value_or(Material {});
        mat.baseColorFactor[0] = color.GetX();
        mat.baseColorFactor[1] = color.GetY();
        mat.baseColorFactor[2] = color.GetZ();
        mat.baseColorFactor[3] = color.GetW();
        mat.roughnessFactor    = 0.35f;
        mat.metallicFactor     = 0.15f;
    }

    Entity     e         = reg.Create();
    AssetID    meshAsset = HashAssetID("prefab_plane_mesh_" + std::to_string(e.index));
    MaterialID matAsset  = HashAssetID("prefab_plane_mat_" + std::to_string(e.index));

    ctx.RegisterGPUMesh(meshAsset, mesh);
    ctx.RegisterGPUMaterial(matAsset, mat);

    JPH::Mat44 worldMat = Math::CreateTransform(JPH::Vec3(params.position), params.rotation, params.scale);

    reg.Add(e, Components::NameComponent {.name = String64("Plane_" + std::to_string(e.index))});
    reg.Add(e, Components::TransformComponent {.position = JPH::Vec3(params.position), .rotation = params.rotation, .scale = params.scale});
    reg.Add(e, Components::WorldTransformComponent {.world = worldMat, .previous = worldMat});

    reg.Add(e, Components::MeshComponent {.meshAsset = meshAsset, .materialAsset = matAsset, .cullRadius = extent * 2.0f});
    reg.Add(e, Components::PBRComponent {.roughness = mat.roughnessFactor, .metallic = mat.metallicFactor});

    if (params.createPhysics && pc != nullptr) {
        // FIXED: Using instance methods
        auto shape = pc->GetOrCreateShape(Physics::ShapeType::Plane, 0.0f, 1.0f, 0.0f, 0.0f);
        auto body  = pc->CreateRigidBody(shape, params.position, params.rotation, JPH::EMotionType::Static, 0, 0, params.physicsCategory, params.physicsMask);
        reg.Add(e, Components::PhysicsComponent {body});
    }

    return e;
}

auto CreatePlane(Engine& engine, float extent, const JPH::Vec4& color, const SpawnParams& params) -> Entity {
    return CreatePlane(engine.GetRenderContext(), engine.GetRegistry(), &engine.GetPhysicsContext(), extent, color, params);
}

auto InstantiatePrefab(
    RenderContext&     ctx,
    ECS::Registry&     reg,
    PhysicsContext&    pc,
    const ModelPrefab& prefab,
    const SpawnParams& params,
    Entity*            outBuffer,
    uint32_t           maxCount
) -> uint32_t {
    uint32_t spawnedCount = 0;
    Entity   rootEntity   = Entity::Null();
    uint32_t startIndex   = 0;

    if (!params.createPhysics) {
        rootEntity = SpawnPrefabRoot(reg, prefab.virtualPath.c_str(), params);

        // Keep the prefab/skeleton source available for skinned rigs that
        // contain no authored animation clips.
        if (params.isAnimated && (!prefab.animations.empty() || !prefab.skeletons.empty())) {
            reg.Add(
                rootEntity, Components::AnimatorComponent {
                                .currentTrackIdx  = prefab.animations.empty() ? -1 : 0,
                                .currentTrackTime = 0.0f,
                                .currentLoop      = true,
                                .prefab           = &prefab,
                            }
            );
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

        instantiatedParts[prefab.parts[i].name.c_str()] = meshEnt;

        if (outBuffer != nullptr && spawnedCount < maxCount) {
            outBuffer[startIndex + (spawnedCount - startIndex)] = meshEnt;
        }
        spawnedCount++;

        Entity glowEnt = params.emissiveVirtualLights ? TrySpawnEmissiveVPL(reg, prefab.parts[i], meshEnt, scaleMult) : Entity::Null();
        if (glowEnt != Entity::Null()) {
            if (outBuffer != nullptr && spawnedCount < maxCount) {
                outBuffer[spawnedCount] = glowEnt;
            }
            spawnedCount++;
        }
    }

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

                        reg.Patch<Components::MeshComponent>(opIt->second, [&](auto& cutMesh) -> auto { cutMesh.flags |= DrawFlags::Hidden; });
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

void SetupPlayerRagdoll(PhysicsContext& pc, ECS::Registry& reg, Entity playerEntity, std::span<const Entity> visualParts) {
    const Skeleton* targetSkeleton = nullptr;
    uint32_t        jointOffset    = 0;

    bool skeletonFound = false;
    for (Entity part: visualParts) {
        reg.Patch<Components::SkeletalMeshComponent>(part, [&](auto& skelMesh) -> auto {
            auto*  hier       = reg.Get<Components::HierarchyComponent>(part);
            Entity parentRoot = (hier != nullptr) ? hier->parent : Entity::Null();
            if (parentRoot != Entity::Null()) {
                if (auto* animComp = reg.Get<Components::AnimatorComponent>(parentRoot)) {
                    if ((animComp->prefab != nullptr) && skelMesh.skeletonIndex >= 0) {
                        targetSkeleton = &animComp->prefab->skeletons[skelMesh.skeletonIndex];
                        jointOffset    = skelMesh.jointOffset;
                        skeletonFound  = true;
                    }
                }
            }
        });
        if (skeletonFound) {
            break;
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
            part.jointIndex       = static_cast<uint32_t>(i);
            part.parentJointIndex = targetSkeleton->joints[i].parentIndex;
            part.mass             = 1.0f;
            part.enableMotors     = false;

            JPH::Mat44 bindPose = targetSkeleton->joints[i].inverseBindMatrix.Inversed();
            part.position       = JPH::RVec3(bindPose.GetTranslation());
            part.rotation       = bindPose.GetQuaternion().Normalized();

            std::ranges::transform(name, name.begin(), ::tolower);
            // FIXED: Used pc.GetOrCreateShape instead of Physics::GetOrCreateShape
            if (name.contains("hip") || name.contains("pelvis") || name.contains("root")) {
                part.shape = pc.GetOrCreateShape(Physics::ShapeType::Capsule, 0.4f, 0.2f);
                part.mass  = 15.0f;
            } else if (name.contains("spine") || name.contains("chest") || name.contains("torso")) {
                part.shape         = pc.GetOrCreateShape(Physics::ShapeType::Capsule, 0.5f, 0.25f);
                part.mass          = 20.0f;
                part.enableMotors  = true;
                part.maxMotorForce = 250.0f;
            } else if (name.contains("head") || name.contains("neck")) {
                part.shape         = pc.GetOrCreateShape(Physics::ShapeType::Sphere, 0.3f);
                part.mass          = 8.0f;
                part.enableMotors  = true;
                part.maxMotorForce = 250.0f;
            } else if (IsImportantJoint(name)) {
                part.shape = pc.GetOrCreateShape(Physics::ShapeType::Capsule, 0.2f, 0.1f);
                part.mass  = 3.0f;
            } else {
                part.shape = pc.GetOrCreateShape(Physics::ShapeType::Sphere, 0.08f);
                part.mass  = 0.5f;
            }
            parts.push_back(part);
        }

        // FIXED: Used pc.CreateSkeletalRagdoll
        auto ragdollInstance = pc.CreateSkeletalRagdoll(joltSkel, parts);
        ragdollInstance->AddRef();

        ArticulationSystem::BindSkeleton(jointOffset, *targetSkeleton);

        reg.Add(
            playerEntity, Components::RagdollComponent {
                              .ragdollInstance  = ragdollInstance.GetPtr(),
                              .skeletonAsset    = InvalidAssetID,
                              .state            = RagdollState::Inactive,
                              .prevState        = RagdollState::Inactive,
                              .jointOffset      = jointOffset,
                              .jointCount       = static_cast<uint32_t>(targetSkeleton->joints.size()),
                              .isAddedToPhysics = false
                          }
        );
        Log("Skeletal Ragdoll successfully generated from Native Skeleton.");
    } else {
        Log("WARNING: SetupPlayerRagdoll failed because no skeleton was found.");
    }
}

void SetupPlayerRagdoll(Engine& engine, Entity playerEntity, std::span<const Entity> visualParts) {
    SetupPlayerRagdoll(engine.GetPhysicsContext(), engine.GetRegistry(), playerEntity, visualParts);
}

void RebuildVulkanResources(RenderContext& ctx, CreativeWorksManager& cwMgr, ECS::Registry& reg) {
    ZHLN::Log("[Engine] Device Lost: Clearing GPU asset cache. Next frame will re-upload assets lazily.");

    ctx.ClearGPUCaches();
    CreateFontAtlasTexture(ctx, reg);

    uint32_t count = cwMgr.GetCachedPrefabs(nullptr, 0);
    if (count > 0) {
        std::vector<ModelPrefab*> prefabs(count);
        cwMgr.GetCachedPrefabs(prefabs.data(), count);

        const auto* backend = PrefabBackend();
        for (auto* prefab: prefabs) {
            if (backend != nullptr) {
                backend->RebuildGPUResources(ctx, prefab);
            }
            for (size_t i = 0; i < prefab->parts.size(); ++i) {
                std::string assetKeyStr = std::string(prefab->virtualPath.c_str()) + "#" + prefab->parts[i].name.c_str() + "_" +
                                          std::to_string(prefab->parts[i].nodeIndex);
                ctx.RegisterGPUMesh(HashAssetID(assetKeyStr), prefab->parts[i].mesh);
                ctx.RegisterGPUMaterial(HashAssetID(assetKeyStr + "_mat"), prefab->parts[i].defaultMaterial);
            }
        }
    }
}

auto CreateTerrainFromData(
    RenderContext&     ctx,
    ECS::Registry&     reg,
    PhysicsContext*    pc,
    int                sampleCount,
    float              worldSize,
    const float*       heights,
    const float*       colorsRGBA,
    const SpawnParams& params
) -> Entity {
    Entity e = reg.Create();

    Mesh mesh = CreateTerrainMeshFromData(ctx, sampleCount, worldSize, heights, colorsRGBA);

    Material mat;
    if (params.materialOverride.pipeline != PipelineHandle::Invalid) {
        mat = params.materialOverride;
    } else {
        auto mat_res        = CreateBasicMaterial(ctx, false, false, false);
        mat                 = mat_res.value_or(Material {});
        mat.roughnessFactor = 0.85f;
        mat.metallicFactor  = 0.05f;
    }

    AssetID    meshAsset = HashAssetID("prefab_terraindata_mesh_" + std::to_string(e.index));
    MaterialID matAsset  = HashAssetID("prefab_terraindata_mat_" + std::to_string(e.index));

    ctx.RegisterGPUMesh(meshAsset, mesh);
    ctx.RegisterGPUMaterial(matAsset, mat);

    TerrainData tData {.sampleCount = static_cast<uint32_t>(sampleCount), .worldSize = worldSize, .maxHeight = 35.0f, .heights = {}, .colors = {}};
    if (heights != nullptr) {
        tData.heights.assign(heights, heights + (static_cast<ptrdiff_t>(sampleCount * sampleCount)));
    }
    if (colorsRGBA != nullptr) {
        tData.colors.assign(colorsRGBA, colorsRGBA + (static_cast<ptrdiff_t>(sampleCount * sampleCount * 4)));
    }
    TerrainHandle tHandle  = TerrainSystem::RegisterTerrainData(std::move(tData));
    JPH::Mat44    worldMat = Math::CreateTransform(JPH::Vec3(params.position), params.rotation, params.scale);

    reg.Add(e, Components::NameComponent {.name = String64("TerrainData_" + std::to_string(e.index))});
    reg.Add(e, Components::TransformComponent {.position = JPH::Vec3(params.position), .rotation = params.rotation, .scale = params.scale});
    reg.Add(e, Components::WorldTransformComponent {.world = worldMat, .previous = worldMat});

    reg.Add(e, Components::MeshComponent {.meshAsset = meshAsset, .materialAsset = matAsset, .cullRadius = worldSize * 1.5f});
    reg.Add(e, Components::PBRComponent {.roughness = mat.roughnessFactor, .metallic = mat.metallicFactor});
    reg.Add(
        e, Components::TerrainComponent {
               .sampleCount   = static_cast<uint32_t>(sampleCount),
               .worldSize     = worldSize,
               .maxHeight     = 35.0f,
               .roughness     = mat.roughnessFactor,
               .metallic      = mat.metallicFactor,
               .terrainHandle = tHandle
           }
    );

    if (params.createPhysics && pc != nullptr && heights != nullptr) {
        auto shape = Physics::CreateHeightFieldShape(heights, sampleCount, worldSize);
        // FIXED: Used pc->CreateRigidBody
        auto body = pc->CreateRigidBody(shape, params.position, params.rotation, JPH::EMotionType::Static, 0);
        reg.Add(e, Components::PhysicsComponent {body});
    }

    return e;
}

auto CreateTerrainFromData(Engine& engine, int sampleCount, float worldSize, const float* heights, const float* colorsRGBA, const SpawnParams& params)
    -> Entity {
    return CreateTerrainFromData(
        engine.GetRenderContext(), engine.GetRegistry(), &engine.GetPhysicsContext(), sampleCount, worldSize, heights, colorsRGBA, params
    );
}

auto CreateTerrain(
    RenderContext&     ctx,
    ECS::Registry&     reg,
    PhysicsContext*    pc,
    size_t             sampleCount,
    float              worldSize,
    float              maxHeight,
    TerrainType        type,
    const SpawnParams& params
) -> Entity {
    Entity e = reg.Create();

    TerrainData tData {.sampleCount = static_cast<uint32_t>(sampleCount), .worldSize = worldSize, .maxHeight = maxHeight, .heights = {}, .colors = {}};
    tData.heights.resize(sampleCount * sampleCount);

    Mesh mesh = CreateTerrainMesh(ctx, sampleCount, worldSize, maxHeight, tData.heights.data(), type);

    Material mat;
    if (params.materialOverride.pipeline != PipelineHandle::Invalid) {
        mat = params.materialOverride;
    } else {
        auto mat_res        = CreateBasicMaterial(ctx, false, false, false);
        mat                 = mat_res.value_or(Material {});
        mat.roughnessFactor = 0.85f;
        mat.metallicFactor  = 0.05f;
    }

    AssetID    meshAsset = HashAssetID("prefab_terrain_mesh_" + std::to_string(e.index));
    MaterialID matAsset  = HashAssetID("prefab_terrain_mat_" + std::to_string(e.index));

    ctx.RegisterGPUMesh(meshAsset, mesh);
    ctx.RegisterGPUMaterial(matAsset, mat);

    TerrainHandle tHandle  = TerrainSystem::RegisterTerrainData(std::move(tData));
    JPH::Mat44    worldMat = Math::CreateTransform(JPH::Vec3(params.position), params.rotation, params.scale);

    reg.Add(e, Components::NameComponent {.name = String64("Terrain_" + std::to_string(e.index))});
    reg.Add(e, Components::TransformComponent {.position = JPH::Vec3(params.position), .rotation = params.rotation, .scale = params.scale});
    reg.Add(e, Components::WorldTransformComponent {.world = worldMat, .previous = worldMat});

    reg.Add(e, Components::MeshComponent {.meshAsset = meshAsset, .materialAsset = matAsset, .cullRadius = worldSize * 1.5f});
    reg.Add(e, Components::PBRComponent {.roughness = 0.85f, .metallic = 0.05f});
    reg.Add(
        e, Components::TerrainComponent {
               .sampleCount   = static_cast<uint32_t>(sampleCount),
               .worldSize     = worldSize,
               .maxHeight     = maxHeight,
               .roughness     = 0.85f,
               .metallic      = 0.05f,
               .terrainHandle = tHandle
           }
    );

    if (params.createPhysics && pc != nullptr) {
        const TerrainData* stored = TerrainSystem::GetTerrainData(tHandle);
        if (stored != nullptr && !stored->heights.empty()) {
            auto shape = Physics::CreateHeightFieldShape(stored->heights.data(), sampleCount, worldSize);
            auto body  = pc->CreateRigidBody(shape, params.position, params.rotation, JPH::EMotionType::Static, 0);
            reg.Add(e, Components::PhysicsComponent {body});
        }
    }

    return e;
}

auto CreateTerrain(Engine& engine, int sampleCount, float worldSize, float maxHeight, TerrainType type, const SpawnParams& params) -> Entity {
    return CreateTerrain(engine.GetRenderContext(), engine.GetRegistry(), &engine.GetPhysicsContext(), sampleCount, worldSize, maxHeight, type, params);
}

auto LoadModelPrefab(Engine& engine, std::string_view path) -> ModelPrefab* {
    return LoadModelPrefab(engine.GetRenderContext(), engine.GetCreativeWorksManager(), path);
}

auto InstantiatePrefab(Engine& engine, const ModelPrefab& prefab, const SpawnParams& params, Entity* outBuffer, uint32_t maxCount) -> uint32_t {
    return InstantiatePrefab(engine.GetRenderContext(), engine.GetRegistry(), engine.GetPhysicsContext(), prefab, params, outBuffer, maxCount);
}

auto InstantiatePrefab(Engine& engine, std::string_view path, const SpawnParams& params, Entity* outBuffer, uint32_t maxCount) -> uint32_t {
    ModelPrefab* prefab = LoadModelPrefab(engine, path);
    if (prefab == nullptr) {
        return 0;
    }
    return InstantiatePrefab(engine, *prefab, params, outBuffer, maxCount);
}

auto LoadModelPrefabFromMemory(Engine& engine, std::span<const uint8_t> bytes, std::string_view virtualPath) -> ModelPrefab* {
    const auto* backend = PrefabBackend();
    return (backend != nullptr) ? backend->LoadFromMemory(engine.GetRenderContext(), engine.GetCreativeWorksManager(), bytes, virtualPath) : nullptr;
}

auto InstantiatePrefabFromMemory(
    Engine&                  engine,
    std::span<const uint8_t> bytes,
    std::string_view         virtualPath,
    const SpawnParams&       params,
    Entity*                  outBuffer,
    uint32_t                 maxCount
) -> uint32_t {
    const auto* prefab = LoadModelPrefabFromMemory(engine, bytes, virtualPath);
    if (prefab == nullptr) {
        return 0;
    }
    return InstantiatePrefab(engine, *prefab, params, outBuffer, maxCount);
}

} // namespace ZHLN::CreativeWorksFactory
