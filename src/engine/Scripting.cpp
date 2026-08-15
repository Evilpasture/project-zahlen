// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "IScriptRuntime.hpp"
#include "LuaScriptRuntime.hpp"
#include "Zahlen/Camera.hpp"
#include "Zahlen/Components.hpp"
#include "Zahlen/Input.hpp"
#include "engine/system/AnimationSystem.hpp"
#include "engine/system/InputSystem.hpp"
#include <Zahlen/Audio.hpp>
#include <Zahlen/Buffer.h>
#include <Zahlen/Console.hpp>
#include <Zahlen/CreativeWorksFactory.hpp>
#include <Zahlen/Entity.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Render.hpp>
#include <Zahlen/Scripting.h>
#include <Zahlen/Scripting.hpp>
#include <Zahlen/Sync.hpp>
#include <Zahlen/Window.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <Zahlen/physics/Physics.hpp>
#include <algorithm>
#include <cgltf.h>
#include <chrono>
#include <cstring>
#include <engine/system/LightingSystem.hpp>
#include <functional>
#include <physics/PhysicsWorld.hpp>
#include <print>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace {
#pragma pack(push, 1)

struct ZHLN_RaycastResult {
    uint64_t entity;
    double   px, py, pz;
    float    nx, ny, nz;
    float    fraction;
    int      hasHit;
};

struct ZHLN_RaycastPenetrationResult {
    uint64_t entity;
    double   epx, epy, epz;
    double   xpx, xpy, xpz;
    float    enx, eny, enz;
    float    xnx, xny, xnz;
    float    entryFraction;
    float    exitFraction;
    float    thickness;
    uint32_t materialID;
    int      hasHit;
};

struct GetBufferArgs {
    ZHLN_BufferView* outView;
};
struct GetECSBufferArgs {
    const char*      componentName;
    ZHLN_BufferView* outView;
};
struct ReleaseBufferArgs {
    void* sync_ptr;
};
struct GetComponentArgs {
    uint64_t    entityRaw;
    const char* componentName;
};
using AddComponentArgs = GetComponentArgs;

struct EntityOnlyArgs {
    uint64_t entityRaw;
};
struct IsKeyDownArgs {
    uint8_t key;
};
struct GetMouseDeltaArgs {
    float* outX;
    float* outY;
};
struct CameraFloatArgs {
    float* outVal;
};
struct SetCameraFOVArgs {
    float fov;
};
struct PlayOneShotArgs {
    const char* filepath;
    float       volume;
};
struct PlayOneShot3DArgs {
    const char* filepath;
    float       x;
    float       y;
    float       z;
    float       volume;
};
struct PlayProceduralBeepArgs {
    float frequency;
    float duration;
    float volume;
};
struct SetCharVelArgs {
    uint64_t entityRaw;
    float    x;
    float    y;
    float    z;
};
struct AddImpulseAtArgs {
    uint64_t entityRaw;
    float    ix;
    float    iy;
    float    iz;
    double   px;
    double   py;
    double   pz;
};
struct RaycastArgs {
    double              ox;
    double              oy;
    double              oz;
    float               dx;
    float               dy;
    float               dz;
    float               maxDist;
    uint64_t            ignoreEntity;
    ZHLN_RaycastResult* outResult;
};
struct RaycastPenetrationArgs {
    double                         ox, oy, oz;
    float                          dx, dy, dz;
    float                          maxDist;
    uint64_t                       ignoreEntity;
    ZHLN_RaycastPenetrationResult* outResult;
};
struct SetMoveInputArgs {
    uint64_t entityRaw;
    float    x;
    float    z;
};
struct UnprojectArgs {
    float   ndcX;
    float   ndcY;
    double* ox;
    double* oy;
    double* oz;
    float*  dx;
    float*  dy;
    float*  dz;
};
struct LogInventoryArgs {
    const char* msg;
};

struct SpawnPrefabArgs {
    char      path[256];
    float     px, py, pz;
    int       createPhysics;
    int       isStatic;
    int       isAnimated;
    uint32_t  maxCount;
    uint64_t* outEntities;
};
struct SetupRagdollArgs {
    uint64_t  playerEntity;
    uint32_t  count;
    uint64_t* visualParts;
};
struct CreateBoxArgs {
    float hx, hy, hz;
    float r, g, b, a;
};
struct CreateMaterialArgs {
    float     r, g, b, a;
    uint64_t* outPipeline;
    uint32_t* outAlbedo;
};
struct SpawnEntityArgs {
    uint8_t shapeType;
    float   p1, p2, p3;
    float   px, py, pz;
    float   rx, ry, rz, rw;
    float   r, g, b, a;
    uint8_t isStatic;
};
struct RegisterDynamicComponentArgs {
    const char* name;
    uint64_t    size;
    uint64_t    alignment;
};

struct SpawnLightArgs {
    float           px, py, pz;
    float           rx, ry, rz, rw;
    float           r, g, b;
    float           intensity;
    float           radius;
    float           dx, dy, dz;
    float           range;
    ZHLN::LightType type;
    uint32_t        twoSided;
};

struct CreateSoundInstanceArgs {
    const char* filepath;
    int         spatialized;
};

struct SoundInstanceArgs {
    uint64_t handle;
};

struct PlayTrackArgs {
    uint64_t entityRaw;
    int32_t  trackIndex;
    float    blendDuration;
    int      loop;
    float    playbackSpeed;
};

struct GetTrackNameArgs {
    uint64_t entityRaw;
    int32_t  trackIndex;
    char     outName[64];
};

struct PlayNoiseBurstArgs {
    uint8_t filterType;
    float   freq;
    float   q;
    float   volume;
    float   duration;
    uint8_t noiseType;
};

struct PlayNoiseBurst3DArgs {
    uint8_t filterType;
    float   freq;
    float   q;
    float   volume;
    float   duration;
    float   x;
    float   y;
    float   z;
    uint8_t noiseType;
};

struct PlayToneSweepArgs {
    uint8_t waveType;
    float   startFreq;
    float   endFreq;
    float   volume;
    float   duration;
};

struct PlayToneSweep3DArgs {
    uint8_t waveType;
    float   startFreq;
    float   endFreq;
    float   volume;
    float   duration;
    float   x;
    float   y;
    float   z;
};

struct CreateLoopSynthArgs {
    uint8_t waveType1;
    uint8_t waveType2;
    uint8_t filterType;
};

struct SetLoopSynthParamsArgs {
    uint64_t handle;
    float    charge;
    float    baseFreq;
    float    filterFreq;
    float    volume;
};

struct StopLoopSynthArgs {
    uint64_t handle;
    float    fadeOutTime;
};

struct SpawnTerrainArgs {
    uint32_t     sampleCount;
    float        worldSize;
    float        maxHeight;
    const float* heights;
    const float* colorsRGBA;
    float        roughness;
    float        metallic;
};

struct CreateTextureArgs {
    const void* data;
    uint32_t    width;
    uint32_t    height;
    uint32_t    isSRGB;
};

struct AddIKChainArgs {
    uint64_t entityRaw;
    int32_t  upperNodeIndex;
    int32_t  lowerNodeIndex;
    int32_t  endNodeIndex;
    float    targetX, targetY, targetZ;
    float    poleX, poleY, poleZ;
    float    weight;
};

struct SetIKTargetArgs {
    uint64_t entityRaw;
    uint32_t chainIndex;
    float    tx, ty, tz;
    float    rx, ry, rz, rw;
    float    weight;
};

struct SetIKTargetEntityArgs {
    uint64_t entityRaw;
    uint32_t chainIndex;
    uint64_t targetEntityRaw;
    float    offsetX, offsetY, offsetZ;
    float    weight;
};

struct DrawLineArgs {
    float ox, oy, oz;
    float dx, dy, dz;
    float r1, g1, b1, a1;
    float r2, g2, b2, a2;
};

struct SetLODArgs {
    uint64_t    entityRaw;
    uint32_t    index;
    const char* meshName;
    float       distance;
};

#pragma pack(pop)

void SafeDestroyEntity(ZHLN::Engine* engine, ZHLN::Entity entity) {
    using namespace ZHLN;
    using namespace ZHLN::ECS;
    auto& reg = engine->GetRegistry();

    std::vector<Entity> childrenToDestroy;

    uint32_t hierarchyID = ComponentFamily::GetTypeID<Components::HierarchyComponent>();
    auto     hEntities   = reg.GetEntitiesByFamilyID(hierarchyID);
    for (Entity e: hEntities) {
        if (auto* hier = reg.Get<Components::HierarchyComponent>(e)) {
            if (hier->parent == entity) {
                childrenToDestroy.push_back(e);
            }
        }
    }

    uint32_t uiRectID  = ComponentFamily::GetTypeID<Components::UIRectComponent>();
    auto     uEntities = reg.GetEntitiesByFamilyID(uiRectID);
    for (Entity e: uEntities) {
        if (auto* rect = reg.Get<Components::UIRectComponent>(e)) {
            if (rect->parentEntity == entity) {
                childrenToDestroy.push_back(e);
            }
        }
    }

    for (ZHLN::Entity child: childrenToDestroy) {
        SafeDestroyEntity(engine, child);
    }

    reg.Destroy(entity);
}
} // namespace

extern std::vector<std::string> s_InvShellLog;
extern bool                     s_InvScrollToBottom;

namespace ZHLN { namespace {

struct SyncPolicy {
    static void Acquire(BufferSync* sync) {
        if (sync->viewExportCount.fetch_add(1, std::memory_order::acquire) == 0) {
            sync->shadowLock.lock();
        }
    }
    static void Release(BufferSync* sync) {
        if (sync->viewExportCount.fetch_sub(1, std::memory_order::release) == 1) {
            sync->shadowLock.unlock();
        }
    }
};

struct ViewComposer {
    template <typename TOwner, typename TData, typename... Dims>
    static ZHLN_BufferView Build(const TOwner* owner, TData* data, const char* format, Dims... dims) {
        auto* sync = reinterpret_cast<BufferSync*>(const_cast<TOwner*>(owner));
        SyncPolicy::Acquire(sync);

        ZHLN_BufferView view = {};
        view.buf             = (void*) data;
        view.obj             = (void*) sync;
        view.itemsize        = sizeof(TData);
        std::strncpy(view.format, format, 7);
        view.readonly = 0;

        view.ndim        = sizeof...(dims);
        size_t d_array[] = {static_cast<size_t>(dims)...};

        size_t stride = sizeof(TData);
        for (int i = (int) view.ndim - 1; i >= 0; --i) {
            view.shape[i]   = d_array[i];
            view.strides[i] = stride;
            stride *= d_array[i];
        }

        view.len   = stride;
        view.flags = ZHLN_BUFFER_CONTIGUOUS | ZHLN_BUFFER_WRITABLE;
        if (((uintptr_t) view.buf % 32) == 0) {
            view.flags |= ZHLN_BUFFER_ALIGNED_32;
        }

        return view;
    }
};

using CommandHandler = std::function<uint64_t(ZHLN::Engine*, const void*)>;

static std::vector<CommandHandler>                    s_JumpTable;
static std::unordered_map<std::string_view, uint32_t> s_StringToIntMap;

static void RegisterCmd(std::string_view name, CommandHandler handler) {
    auto id = static_cast<uint32_t>(s_JumpTable.size());
    s_JumpTable.push_back(std::move(handler));
    s_StringToIntMap[name] = id;
}

template <typename TArgs = void, bool RequireEngine = true, typename Fn>
CommandHandler MakeCmd(Fn fn) {
    return [fn = std::move(fn)](ZHLN::Engine* engine, const void* args) -> uint64_t {
        if constexpr (RequireEngine) {
            if (!engine) {
                return 0;
            }
        }
        if constexpr (std::is_void_v<TArgs>) {
            return fn(engine);
        } else {
            if (!args) {
                return 0;
            }
            return fn(engine, *static_cast<const TArgs*>(args));
        }
    };
}

struct ComponentRegistryEntry {
    void* (*add)(ZHLN::ECS::Registry&, ZHLN::Entity) = nullptr;
    std::function<ZHLN_BufferView(ZHLN::ECS::Registry&)> getBuffer;
};

std::unordered_map<std::string_view, ComponentRegistryEntry> s_ComponentRegistry;

template <typename T, typename... Dims>
void RegisterComponentType(std::string_view name, const char* format, Dims... dims) {
    s_ComponentRegistry[name] = ComponentRegistryEntry {
        .add       = [](ZHLN::ECS::Registry& reg, ZHLN::Entity entity) -> void* { return &reg.template Add<T>(entity, T {}); },
        .getBuffer = [format, ... dims = dims](ZHLN::ECS::Registry& reg) -> ZHLN_BufferView {
            auto raw = reg.GetRawArray<T>();
            if constexpr (sizeof...(Dims) > 0) {
                return ZHLN::ViewComposer::Build(&reg, raw.data(), format, raw.size(), dims...);
            } else {
                return ZHLN::ViewComposer::Build(&reg, raw.data(), format, raw.size());
            }
        }
    };
}

template <typename T, typename... Dims>
void RegisterComponentTypeReadOnly(std::string_view name, const char* format, Dims... dims) {
    s_ComponentRegistry[name] = ComponentRegistryEntry {.add = nullptr, .getBuffer = [format, ... dims = dims](ZHLN::ECS::Registry& reg) -> ZHLN_BufferView {
                                                            auto raw = reg.GetRawArray<T>();
                                                            if constexpr (sizeof...(Dims) > 0) {
                                                                return ZHLN::ViewComposer::Build(&reg, raw.data(), format, raw.size(), dims...);
                                                            } else {
                                                                return ZHLN::ViewComposer::Build(&reg, raw.data(), format, raw.size());
                                                            }
                                                        }};
}

void InitComponentRegistry() {
    if (!s_ComponentRegistry.empty()) {
        return;
    }

    ZHLN::Reflect::ForEachNestedType<Components>([]<typename Comp>() {
        std::string_view name = ZHLN::Reflect::TypeName<Comp>();

        s_ComponentRegistry[name] = ComponentRegistryEntry {
            .add = [](ZHLN::ECS::Registry& reg, ZHLN::Entity entity) -> void* {
                if constexpr (std::is_same_v<Comp, Components::PhysicsComponent>) {
                    return nullptr; // Read-only physics handle
                } else if constexpr (std::is_default_constructible_v<Comp>) {
                    return &reg.template Add<Comp>(entity, Comp {});
                } else {
                    return nullptr;
                }
            },
            .getBuffer = [](ZHLN::ECS::Registry& reg) -> ZHLN_BufferView {
                auto             raw        = reg.GetRawArray<Comp>();
                constexpr size_t floatCount = ZHLN::Reflect::GetFloatFieldsCount<Comp>();

                if constexpr (std::is_same_v<Comp, Components::PhysicsComponent>) {
                    return ZHLN::ViewComposer::Build(&reg, raw.data(), "Q", raw.size());
                } else if constexpr (floatCount > 0) {
                    // Auto-detected float-only struct (e.g. PBRComponent) -> 2D float view ("f")
                    return ZHLN::ViewComposer::Build(&reg, raw.data(), "f", raw.size(), floatCount);
                } else {
                    // General struct -> 1D byte buffer view ("B")
                    return ZHLN::ViewComposer::Build(&reg, raw.data(), "B", raw.size());
                }
            }
        };
    });
}

void RegisterCreativeWorkCommands() {
    RegisterCmd("SpawnPrefab", MakeCmd<SpawnPrefabArgs>([](ZHLN::Engine* engine, const SpawnPrefabArgs& a) -> uint64_t {
                    auto& rc  = engine->GetRenderContext();
                    auto& reg = engine->GetRegistry();
                    auto& pc  = engine->GetPhysicsContext();

                    auto* prefab = ZHLN::CreativeWorksFactory::LoadModelPrefab(rc, engine->GetCreativeWorksManager(), a.path);
                    if (!prefab) {
                        return 0;
                    }

                    ZHLN::CreativeWorksFactory::SpawnParams params;
                    params.position        = JPH::RVec3(a.px, a.py, a.pz);
                    params.createPhysics   = (a.createPhysics != 0);
                    params.isStaticPhysics = (a.isStatic != 0);
                    params.isAnimated      = (a.isAnimated != 0);
                    params.useBoxColliders = false;

                    std::vector<ZHLN::Entity> temp_buffer(a.maxCount);
                    uint32_t count = ZHLN::CreativeWorksFactory::InstantiatePrefab(rc, reg, pc, *prefab, params, temp_buffer.data(), a.maxCount);

                    uint32_t writtenCount = std::min(count, a.maxCount);
                    for (uint32_t i = 0; i < writtenCount; ++i) {
                        a.outEntities[i] = temp_buffer[i].Pack();
                    }
                    return writtenCount;
                }));

    RegisterCmd("SetLODLevel", MakeCmd<SetLODArgs>([](ZHLN::Engine* engine, const SetLODArgs& a) -> uint64_t {
                    auto  e   = ZHLN::Entity::Unpack(a.entityRaw);
                    auto& reg = engine->GetRegistry();

                    auto* lod = reg.Get<ZHLN::Components::LODComponent>(e);
                    if (!lod) {
                        lod = &reg.Add(e, ZHLN::Components::LODComponent {});
                    }

                    if (a.index < ZHLN::Components::LODComponent::MAX_LODS) {
                        lod->levels[a.index].meshAsset = HashAssetID(a.meshName);
                        lod->levels[a.index].distance  = a.distance;
                        lod->count                     = std::max(lod->count, static_cast<uint8_t>(a.index + 1));
                    }
                    return 1;
                }));

    RegisterCmd(
        "SpawnTerrain", MakeCmd<SpawnTerrainArgs>([](ZHLN::Engine* engine, const SpawnTerrainArgs& a) -> uint64_t {
            uint32_t samples   = (a.sampleCount > 0) ? a.sampleCount : 128;
            float    worldSize = (a.worldSize > 0.0f) ? a.worldSize : 200.0f;
            float    maxHeight = (a.maxHeight > 0.0f) ? a.maxHeight : 25.0f;

            ZHLN::CreativeWorksFactory::SpawnParams params {.createPhysics = true, .isStaticPhysics = true, .roughness = a.roughness, .metallic = a.metallic};

            ZHLN::Entity e = ZHLN::NullEntity;
            if (a.heights != nullptr && a.colorsRGBA != nullptr) {
                e = ZHLN::CreativeWorksFactory::CreateTerrainFromData(*engine, samples, worldSize, a.heights, a.colorsRGBA, params);
            } else {
                e = ZHLN::CreativeWorksFactory::CreateTerrain(*engine, samples, worldSize, maxHeight, ZHLN::CreativeWorksFactory::TerrainType::Default, params);
            }

            return e.Pack();
        })
    );

    RegisterCmd(
        "SetupRagdoll", MakeCmd<SetupRagdollArgs>([](ZHLN::Engine* engine, const SetupRagdollArgs& a) -> uint64_t {
            std::vector<ZHLN::Entity> parts(a.count);
            for (uint32_t i = 0; i < a.count; ++i) {
                parts[i] = ZHLN::Entity::Unpack(a.visualParts[i]);
            }
            ZHLN::CreativeWorksFactory::SetupPlayerRagdoll(engine->GetPhysicsContext(), engine->GetRegistry(), ZHLN::Entity::Unpack(a.playerEntity), parts);
            return 1;
        })
    );

    RegisterCmd("CreateBox", MakeCmd<CreateBoxArgs>([](ZHLN::Engine* engine, const CreateBoxArgs& a) -> uint64_t {
                    ZHLN::Mesh mesh =
                        ZHLN::CreativeWorksFactory::CreateBoxMesh(engine->GetRenderContext(), JPH::Vec3(a.hx, a.hy, a.hz), JPH::Vec4(a.r, a.g, a.b, a.a));
                    return static_cast<uint64_t>(mesh.posBuffer);
                }));

    RegisterCmd("CreateBasicMaterial", MakeCmd<CreateMaterialArgs>([](ZHLN::Engine* engine, const CreateMaterialArgs& a) -> uint64_t {
                    auto mat_res = ZHLN::CreativeWorksFactory::CreateBasicMaterial(engine->GetRenderContext(), false, a.a < 1.0f);
                    if (!mat_res) {
                        ZHLN::Log("ERROR: CreateBasicMaterial from Lua failed: {}", mat_res.error().Message());
                        return 0;
                    }
                    ZHLN::Material mat     = mat_res.value();
                    mat.baseColorFactor[0] = a.r;
                    mat.baseColorFactor[1] = a.g;
                    mat.baseColorFactor[2] = a.b;
                    mat.baseColorFactor[3] = a.a;
                    *a.outPipeline         = static_cast<uint64_t>(mat.pipeline);
                    *a.outAlbedo           = static_cast<uint64_t>(mat.albedoMap);
                    return 1;
                }));

    RegisterCmd(
        "SpawnEntity", MakeCmd<SpawnEntityArgs>([](ZHLN::Engine* engine, const SpawnEntityArgs& a) -> uint64_t {
            auto type = static_cast<ZHLN::Physics::ShapeType>(a.shapeType);

            if (type == ZHLN::Physics::ShapeType::Plane) {
                return ZHLN::CreativeWorksFactory::CreatePlane(
                           *engine, a.p1, {a.r, a.g, a.b, a.a},
                           ZHLN::CreativeWorksFactory::SpawnParams {
                               .position = {a.px, a.py, a.pz}, .rotation = {a.rx, a.ry, a.rz, a.rw}, .createPhysics = true, .isStaticPhysics = (a.isStatic != 0)
                           }
                )
                    .Pack();
            } else if (type == ZHLN::Physics::ShapeType::Box) {
                return ZHLN::CreativeWorksFactory::CreateBox(
                           *engine, JPH::Vec3(a.p1, a.p2, a.p3),
                           ZHLN::CreativeWorksFactory::SpawnParams {
                               .position        = {a.px, a.py, a.pz},
                               .rotation        = {a.rx, a.ry, a.rz, a.rw},
                               .createPhysics   = true,
                               .isStaticPhysics = (a.isStatic != 0),
                               .color           = {a.r, a.g, a.b, a.a}
                           }
                ).Pack();
            }

            auto& rc  = engine->GetRenderContext();
            auto& pc  = engine->GetPhysicsContext();
            auto& reg = engine->GetRegistry();

            ZHLN::Mesh mesh          = ZHLN::CreativeWorksFactory::CreateBoxMesh(rc, JPH::Vec3(a.p1, a.p1, a.p1), {a.r, a.g, a.b, a.a});
            auto       shape         = pc.GetOrCreateShape(type, a.p1);
            float      cullRadius    = a.p1 * 2.0f;
            bool       isTransparent = (a.a < 1.0f);

            auto mat_res = ZHLN::CreativeWorksFactory::CreateBasicMaterial(rc, false, isTransparent);
            if (!mat_res) {
                ZHLN::Panic("Failed to create basic material inside SpawnEntity: {}", mat_res.error().Message());
            }
            ZHLN::Material mat     = mat_res.value();
            mat.baseColorFactor[0] = a.r;
            mat.baseColorFactor[1] = a.g;
            mat.baseColorFactor[2] = a.b;
            mat.baseColorFactor[3] = a.a;

            ZHLN::Entity e = reg.Create();

            AssetID    entityMeshAsset = HashAssetID("procedural_mesh_" + std::to_string(e.index));
            MaterialID entityMatAsset  = HashAssetID("procedural_mat_" + std::to_string(e.index));
            rc.RegisterGPUMesh(entityMeshAsset, mesh);
            rc.RegisterGPUMaterial(entityMatAsset, mat);

            JPH::Quat  rotation(a.rx, a.ry, a.rz, a.rw);
            JPH::Mat44 worldMat = ZHLN::Math::CreateTransform(JPH::Vec3(a.px, a.py, a.pz), rotation, JPH::Vec3(1.0f, 1.0f, 1.0f));

            reg.Add(e, ZHLN::Components::TransformComponent {.position = {a.px, a.py, a.pz}, .rotation = rotation, .scale = {1.0f, 1.0f, 1.0f}});
            reg.Add(e, ZHLN::Components::WorldTransformComponent {.world = worldMat, .previous = worldMat});

            ZHLN::DrawFlags flags = ZHLN::DrawFlags::None;
            if (isTransparent) {
                flags |= ZHLN::DrawFlags::ExcludeFromTLAS;
            }

            reg.Add(
                e, ZHLN::Components::MeshComponent {.meshAsset = entityMeshAsset, .materialAsset = entityMatAsset, .cullRadius = cullRadius, .flags = flags}
            );

            reg.Add(
                e, ZHLN::Components::PhysicsComponent {pc.CreateRigidBody(
                       shape, JPH::RVec3(a.px, a.py, a.pz), rotation, a.isStatic ? JPH::EMotionType::Static : JPH::EMotionType::Dynamic,
                       a.isStatic ? static_cast<JPH::ObjectLayer>(0) : static_cast<JPH::ObjectLayer>(1), 0
                   )}
            );
            reg.Add(
                e, ZHLN::Components::PhysicsStateComponent {
                       .currPosition = {a.px, a.py, a.pz}, .prevPosition = {a.px, a.py, a.pz}, .currRotation = rotation, .prevRotation = rotation
                   }
            );

            return e.Pack();
        })
    );

    RegisterCmd("SpawnLight", MakeCmd<SpawnLightArgs>([](ZHLN::Engine* engine, const SpawnLightArgs& a) -> uint64_t {
                    auto& reg = engine->GetRegistry();

                    ZHLN::Entity e = reg.Create();
                    reg.Add(
                        e, ZHLN::Components::TransformComponent {
                               .position = {a.px, a.py, a.pz}, .rotation = JPH::Quat(a.rx, a.ry, a.rz, a.rw), .scale = {1.0f, 1.0f, 1.0f}
                           }
                    );
                    reg.Add(e, ZHLN::Components::NameComponent {.name = ZHLN::String64("SpawnedLight")});
                    reg.Add(
                        e, Components::LightComponent {
                               .type      = a.type,
                               .color     = JPH::Vec3(a.r, a.g, a.b),
                               .intensity = a.intensity,
                               .radius    = a.radius,
                               .direction = JPH::Vec3(a.dx, a.dy, a.dz),
                               .range     = a.range,
                               .points    = {},
                               .twoSided  = a.twoSided
                           }
                    );
                    return e.Pack();
                }));

    RegisterCmd("CreateTexture", MakeCmd<CreateTextureArgs>([](ZHLN::Engine* engine, const CreateTextureArgs& a) -> uint64_t {
                    if (a.data == nullptr || a.width == 0 || a.height == 0) {
                        return 1; // Fallback to white texture on invalid data
                    }
                    auto res = engine->GetRenderContext().CreateTexture(a.data, a.width, a.height, a.isSRGB != 0);
                    return res.value_or(1);
                }));
}

void RegisterPhysicsCommands() {
    RegisterCmd("GetPhysicsPositions", MakeCmd<GetBufferArgs>([](ZHLN::Engine* engine, const GetBufferArgs& a) -> uint64_t {
                    const auto& world = engine->GetPhysicsContext().GetWorld();
                    *a.outView        = ZHLN::ViewComposer::Build(&world, world.positions, (sizeof(JPH::Real) == 8) ? "d" : "f", world.count.load(), 4);
                    return 0;
                }));

    RegisterCmd("GetPhysicsLinearVelocities", MakeCmd<GetBufferArgs>([](ZHLN::Engine* engine, const GetBufferArgs& a) -> uint64_t {
                    const auto& world = engine->GetPhysicsContext().GetWorld();
                    *a.outView        = ZHLN::ViewComposer::Build(&world, world.linearVelocities, "f", world.count.load(), 4);
                    return 0;
                }));

    RegisterCmd("GetPhysicsContactEvents", MakeCmd<GetBufferArgs>([](ZHLN::Engine* engine, const GetBufferArgs& a) -> uint64_t {
                    auto        events = engine->GetPhysicsContext().GetContactEvents();
                    const char* fmt    = (sizeof(JPH::Real) == 8) ? "EvtD" : "EvtF";
                    *a.outView         = ZHLN::ViewComposer::Build(&engine->GetPhysicsContext().GetWorld(), events.first, fmt, events.second);
                    return 0;
                }));

    RegisterCmd("SetCharacterVelocity", MakeCmd<SetCharVelArgs>([](ZHLN::Engine* engine, const SetCharVelArgs& a) -> uint64_t {
                    engine->GetPhysicsContext().SetCharacterVelocity(ZHLN::Entity::Unpack(a.entityRaw), JPH::Vec3(a.x, a.y, a.z));
                    return 0;
                }));

    RegisterCmd("IsCharacterOnGround", MakeCmd<EntityOnlyArgs>([](ZHLN::Engine* engine, const EntityOnlyArgs& a) -> uint64_t {
                    return engine->GetPhysicsContext().IsCharacterOnGround(ZHLN::Entity::Unpack(a.entityRaw)) ? 1 : 0;
                }));

    RegisterCmd("SetLinearVelocity", MakeCmd<SetCharVelArgs>([](ZHLN::Engine* engine, const SetCharVelArgs& a) -> uint64_t {
                    engine->GetPhysicsContext().SetLinearVelocity(ZHLN::Entity::Unpack(a.entityRaw), JPH::Vec3(a.x, a.y, a.z));
                    return 0;
                }));

    RegisterCmd("AddImpulse", MakeCmd<SetCharVelArgs>([](ZHLN::Engine* engine, const SetCharVelArgs& a) -> uint64_t {
                    engine->GetPhysicsContext().AddImpulse(ZHLN::Entity::Unpack(a.entityRaw), JPH::Vec3(a.x, a.y, a.z));
                    return 0;
                }));

    RegisterCmd("AddImpulseAt", MakeCmd<AddImpulseAtArgs>([](ZHLN::Engine* engine, const AddImpulseAtArgs& a) -> uint64_t {
                    engine->GetPhysicsContext().AddImpulse(ZHLN::Entity::Unpack(a.entityRaw), JPH::Vec3(a.ix, a.iy, a.iz), JPH::RVec3(a.px, a.py, a.pz));
                    return 0;
                }));

    RegisterCmd("Raycast", MakeCmd<RaycastArgs>([](ZHLN::Engine* engine, const RaycastArgs& a) -> uint64_t {
                    ZHLN::Entity ignore = a.ignoreEntity != 0 ? ZHLN::Entity::Unpack(a.ignoreEntity) : ZHLN::Entity {};
                    auto         res    = engine->GetPhysicsContext().Raycast(JPH::RVec3(a.ox, a.oy, a.oz), JPH::Vec3(a.dx, a.dy, a.dz), a.maxDist, ignore);
                    a.outResult->hasHit = res.hasHit ? 1 : 0;
                    if (res.hasHit) {
                        a.outResult->entity   = res.handle.Pack();
                        a.outResult->px       = res.position.GetX();
                        a.outResult->py       = res.position.GetY();
                        a.outResult->pz       = res.position.GetZ();
                        a.outResult->nx       = res.normal.GetX();
                        a.outResult->ny       = res.normal.GetY();
                        a.outResult->nz       = res.normal.GetZ();
                        a.outResult->fraction = res.fraction;
                    }
                    return 0;
                }));

    RegisterCmd("RaycastPenetration", MakeCmd<RaycastPenetrationArgs>([](ZHLN::Engine* engine, const RaycastPenetrationArgs& a) -> uint64_t {
                    ZHLN::Entity ignore = a.ignoreEntity != 0 ? ZHLN::Entity::Unpack(a.ignoreEntity) : ZHLN::Entity {};
                    auto res = engine->GetPhysicsContext().RaycastPenetration(JPH::RVec3(a.ox, a.oy, a.oz), JPH::Vec3(a.dx, a.dy, a.dz), a.maxDist, ignore);
                    a.outResult->hasHit = res.hasHit ? 1 : 0;
                    if (res.hasHit) {
                        a.outResult->entity        = res.handle.Pack();
                        a.outResult->epx           = res.entryPosition.GetX();
                        a.outResult->epy           = res.entryPosition.GetY();
                        a.outResult->epz           = res.entryPosition.GetZ();
                        a.outResult->xpx           = res.exitPosition.GetX();
                        a.outResult->xpy           = res.exitPosition.GetY();
                        a.outResult->xpz           = res.exitPosition.GetZ();
                        a.outResult->enx           = res.entryNormal.GetX();
                        a.outResult->eny           = res.entryNormal.GetY();
                        a.outResult->enz           = res.entryNormal.GetZ();
                        a.outResult->xnx           = res.exitNormal.GetX();
                        a.outResult->xny           = res.exitNormal.GetY();
                        a.outResult->xnz           = res.exitNormal.GetZ();
                        a.outResult->entryFraction = res.entryFraction;
                        a.outResult->exitFraction  = res.exitFraction;
                        a.outResult->thickness     = res.thickness;
                        a.outResult->materialID    = res.materialID;
                    }
                    return 0;
                }));

    RegisterCmd("SetMovementInput", MakeCmd<SetMoveInputArgs>([](ZHLN::Engine* engine, const SetMoveInputArgs& a) -> uint64_t {
                    if (auto* move = engine->GetRegistry().Get<ZHLN::Components::MovementComponent>(ZHLN::Entity::Unpack(a.entityRaw))) {
                        move->inputX = a.x;
                        move->inputZ = a.z;
                    }
                    return 0;
                }));

    RegisterCmd("SetJumpIntent", MakeCmd<EntityOnlyArgs>([](ZHLN::Engine* engine, const EntityOnlyArgs& a) -> uint64_t {
                    if (auto* move = engine->GetRegistry().Get<ZHLN::Components::MovementComponent>(ZHLN::Entity::Unpack(a.entityRaw))) {
                        move->jumpRequested = true;
                    }
                    return 0;
                }));

    RegisterCmd("UnprojectScreenToWorld", MakeCmd<UnprojectArgs>([](ZHLN::Engine* engine, const UnprojectArgs& a) -> uint64_t {
                    auto winSize = engine->GetWindow().GetSize();
                    if (winSize.width == 0 || winSize.height == 0)
                        return 0;

                    float       aspect    = (float) winSize.width / (float) winSize.height;
                    const auto& cam       = engine->GetCamera();
                    JPH::Mat44  invVP     = (cam.GetProjectionMatrix(aspect) * cam.GetViewMatrix()).Inversed();
                    JPH::Vec4   nearWorld = invVP * JPH::Vec4(a.ndcX, a.ndcY, 0.0f, 1.0f);
                    JPH::Vec4   farWorld  = invVP * JPH::Vec4(a.ndcX, a.ndcY, 1.0f, 1.0f);
                    JPH::Vec3 pNear = JPH::Vec3(nearWorld.GetX() / nearWorld.GetW(), nearWorld.GetY() / nearWorld.GetW(), nearWorld.GetZ() / nearWorld.GetW());
                    JPH::Vec3 pFar  = JPH::Vec3(farWorld.GetX() / farWorld.GetW(), farWorld.GetY() / farWorld.GetW(), farWorld.GetZ() / farWorld.GetW());
                    JPH::Vec3 dir   = (pFar - pNear).Normalized();

                    *a.ox = pNear.GetX();
                    *a.oy = pNear.GetY();
                    *a.oz = pNear.GetZ();
                    *a.dx = dir.GetX();
                    *a.dy = dir.GetY();
                    *a.dz = dir.GetZ();
                    return 0;
                }));
}

void RegisterInputAndCameraCommands() {
    RegisterCmd("IsKeyDown", MakeCmd<IsKeyDownArgs>([](ZHLN::Engine* engine, const IsKeyDownArgs& a) -> uint64_t {
                    auto& reg  = engine->GetRegistry();
                    auto  ents = reg.GetEntitiesWith<ZHLN::Components::InputStateComponent>();
                    if (ents.empty()) {
                        return 0;
                    }
                    auto* state = reg.Get<ZHLN::Components::InputStateComponent>(ents[0]);
                    return (state != nullptr && state->IsKeyDown(a.key)) ? 1 : 0;
                }));

    RegisterCmd("GetMouseDelta", MakeCmd<GetMouseDeltaArgs>([](ZHLN::Engine* engine, const GetMouseDeltaArgs& a) -> uint64_t {
                    auto& reg  = engine->GetRegistry();
                    auto  ents = reg.GetEntitiesWith<ZHLN::Components::InputStateComponent>();
                    if (ents.empty()) {
                        *a.outX = 0.0f;
                        *a.outY = 0.0f;
                        return 0;
                    }
                    auto* state = reg.Get<ZHLN::Components::InputStateComponent>(ents[0]);
                    *a.outX     = (state != nullptr) ? state->GetMouseDeltaX() : 0.0f;
                    *a.outY     = (state != nullptr) ? state->GetMouseDeltaY() : 0.0f;
                    return 0;
                }));

    RegisterCmd("GetCameraYaw", MakeCmd<CameraFloatArgs>([](ZHLN::Engine* engine, const CameraFloatArgs& a) -> uint64_t {
                    *a.outVal = engine->GetCamera().yaw;
                    return 0;
                }));

    RegisterCmd("GetCameraFOV", MakeCmd<CameraFloatArgs>([](ZHLN::Engine* engine, const CameraFloatArgs& a) -> uint64_t {
                    *a.outVal = engine->GetCamera().fov;
                    return 0;
                }));

    RegisterCmd("SetCameraFOV", MakeCmd<SetCameraFOVArgs>([](ZHLN::Engine* engine, const SetCameraFOVArgs& a) -> uint64_t {
                    engine->GetCamera().fov = a.fov;
                    return 0;
                }));

    RegisterCmd("GetTotalTime", MakeCmd<CameraFloatArgs>([](ZHLN::Engine*, const CameraFloatArgs& a) -> uint64_t {
                    static auto start = std::chrono::high_resolution_clock::now();
                    auto        now   = std::chrono::high_resolution_clock::now();
                    *a.outVal         = std::chrono::duration<float>(now - start).count();
                    return 0;
                }));
}

void RegisterAudioCommands() {
    RegisterCmd("PlayOneShot", MakeCmd<PlayOneShotArgs>([](ZHLN::Engine* engine, const PlayOneShotArgs& a) -> uint64_t {
                    if (!a.filepath) {
                        return 0;
                    }
                    engine->GetAudioContext().PlayOneShot(a.filepath, a.volume);
                    return 0;
                }));

    RegisterCmd("PlayOneShot3D", MakeCmd<PlayOneShot3DArgs>([](ZHLN::Engine* engine, const PlayOneShot3DArgs& a) -> uint64_t {
                    if (!a.filepath) {
                        return 0;
                    }
                    engine->GetAudioContext().PlayOneShot3D(a.filepath, JPH::Vec3(a.x, a.y, a.z), a.volume);
                    return 0;
                }));

    RegisterCmd("PlayProceduralBeep", MakeCmd<PlayProceduralBeepArgs>([](ZHLN::Engine* engine, const PlayProceduralBeepArgs& a) -> uint64_t {
                    engine->GetAudioContext().PlayProceduralBeep(a.frequency, a.duration, a.volume);
                    return 0;
                }));

    RegisterCmd("CreateSoundInstance", MakeCmd<CreateSoundInstanceArgs>([](ZHLN::Engine* engine, const CreateSoundInstanceArgs& a) -> uint64_t {
                    if (!a.filepath) {
                        return 0;
                    }
                    void* handle = engine->GetAudioContext().CreateSoundInstance(a.filepath, a.spatialized != 0);
                    return reinterpret_cast<uint64_t>(handle);
                }));

    RegisterCmd("PlaySoundInstance", MakeCmd<SoundInstanceArgs>([](ZHLN::Engine* engine, const SoundInstanceArgs& a) -> uint64_t {
                    engine->GetAudioContext().PlaySoundInstance(reinterpret_cast<void*>(a.handle));
                    return 0;
                }));

    RegisterCmd("StopSoundInstance", MakeCmd<SoundInstanceArgs>([](ZHLN::Engine* engine, const SoundInstanceArgs& a) -> uint64_t {
                    engine->GetAudioContext().StopSoundInstance(reinterpret_cast<void*>(a.handle));
                    return 0;
                }));

    RegisterCmd("DestroySoundInstance", MakeCmd<SoundInstanceArgs>([](ZHLN::Engine* engine, const SoundInstanceArgs& a) -> uint64_t {
                    engine->GetAudioContext().DestroySoundInstance(reinterpret_cast<void*>(a.handle));
                    return 0;
                }));

    // --- Procedural Synthesis & Filter Commands ---

    RegisterCmd("PlayNoiseBurst", MakeCmd<PlayNoiseBurstArgs>([](ZHLN::Engine* engine, const PlayNoiseBurstArgs& a) -> uint64_t {
                    engine->GetAudioContext().PlayNoiseBurst(
                        static_cast<ZHLN::AudioFilterType>(a.filterType), a.freq, a.q, a.volume, a.duration, static_cast<ZHLN::AudioNoiseType>(a.noiseType)
                    );
                    return 0;
                }));

    RegisterCmd("PlayNoiseBurst3D", MakeCmd<PlayNoiseBurst3DArgs>([](ZHLN::Engine* engine, const PlayNoiseBurst3DArgs& a) -> uint64_t {
                    engine->GetAudioContext().PlayNoiseBurst3D(
                        static_cast<ZHLN::AudioFilterType>(a.filterType), a.freq, a.q, a.volume, a.duration, JPH::Vec3(a.x, a.y, a.z),
                        static_cast<ZHLN::AudioNoiseType>(a.noiseType)
                    );
                    return 0;
                }));

    RegisterCmd("PlayToneSweep", MakeCmd<PlayToneSweepArgs>([](ZHLN::Engine* engine, const PlayToneSweepArgs& a) -> uint64_t {
                    engine->GetAudioContext().PlayToneSweep(static_cast<ZHLN::AudioWaveformType>(a.waveType), a.startFreq, a.endFreq, a.volume, a.duration);
                    return 0;
                }));

    RegisterCmd("PlayToneSweep3D", MakeCmd<PlayToneSweep3DArgs>([](ZHLN::Engine* engine, const PlayToneSweep3DArgs& a) -> uint64_t {
                    engine->GetAudioContext().PlayToneSweep3D(
                        static_cast<ZHLN::AudioWaveformType>(a.waveType), a.startFreq, a.endFreq, a.volume, a.duration, JPH::Vec3(a.x, a.y, a.z)
                    );
                    return 0;
                }));

    RegisterCmd("CreateLoopSynth", MakeCmd<CreateLoopSynthArgs>([](ZHLN::Engine* engine, const CreateLoopSynthArgs& a) -> uint64_t {
                    void* handle = engine->GetAudioContext().CreateLoopSynth(
                        static_cast<ZHLN::AudioWaveformType>(a.waveType1), static_cast<ZHLN::AudioWaveformType>(a.waveType2),
                        static_cast<ZHLN::AudioFilterType>(a.filterType)
                    );
                    return reinterpret_cast<uint64_t>(handle);
                }));

    RegisterCmd("SetLoopSynthParams", MakeCmd<SetLoopSynthParamsArgs>([](ZHLN::Engine* engine, const SetLoopSynthParamsArgs& a) -> uint64_t {
                    engine->GetAudioContext().SetLoopSynthParams(reinterpret_cast<void*>(a.handle), a.charge, a.baseFreq, a.filterFreq, a.volume);
                    return 0;
                }));

    RegisterCmd("StopLoopSynth", MakeCmd<StopLoopSynthArgs>([](ZHLN::Engine* engine, const StopLoopSynthArgs& a) -> uint64_t {
                    engine->GetAudioContext().StopLoopSynth(reinterpret_cast<void*>(a.handle), a.fadeOutTime);
                    return 0;
                }));
}

void RegisterECSCommands() {
    RegisterCmd("CreateEntity", MakeCmd<void>([](ZHLN::Engine* engine) -> uint64_t { return engine->GetRegistry().Create().Pack(); }));

    RegisterCmd("DestroyEntity", MakeCmd<EntityOnlyArgs>([](ZHLN::Engine* engine, const EntityOnlyArgs& a) -> uint64_t {
                    SafeDestroyEntity(engine, ZHLN::Entity::Unpack(a.entityRaw));
                    return 0;
                }));

    RegisterCmd("GetComponent", MakeCmd<GetComponentArgs>([](ZHLN::Engine* engine, const GetComponentArgs& a) -> uint64_t {
                    if (!a.componentName)
                        return 0;
                    uint32_t familyID = ZHLN::ECS::Registry::GetFamilyIDFromName(a.componentName);
                    if (familyID == 0xFFFFFFFF)
                        return 0;
                    return std::bit_cast<uint64_t>(engine->GetRegistry().GetRawByFamily(ZHLN::Entity::Unpack(a.entityRaw), familyID));
                }));

    RegisterCmd("AddComponent", MakeCmd<AddComponentArgs>([](ZHLN::Engine* engine, const AddComponentArgs& a) -> uint64_t {
                    if (!a.componentName)
                        return 0;
                    auto             entity = ZHLN::Entity::Unpack(a.entityRaw);
                    auto&            reg    = engine->GetRegistry();
                    std::string_view name(a.componentName);
                    void*            ptr = nullptr;

                    auto it = s_ComponentRegistry.find(name);
                    if (it != s_ComponentRegistry.end() && it->second.add != nullptr) {
                        ptr = it->second.add(reg, entity);
                    } else {
                        uint32_t familyID = ZHLN::ECS::Registry::GetFamilyIDFromName(name);
                        if (familyID != 0xFFFFFFFF) {
                            ptr = reg.AddDynamic(entity, familyID);
                        }
                    }
                    return std::bit_cast<uint64_t>(ptr);
                }));

    RegisterCmd("RegisterDynamicComponent", MakeCmd<RegisterDynamicComponentArgs>([](ZHLN::Engine* engine, const RegisterDynamicComponentArgs& a) -> uint64_t {
                    if (!a.name)
                        return 0;
                    return engine->GetRegistry().RegisterComponentDynamic(a.name, a.size, a.alignment);
                }));

    RegisterCmd("GetECSBuffer", MakeCmd<GetECSBufferArgs>([](ZHLN::Engine* engine, const GetECSBufferArgs& a) -> uint64_t {
                    if (!a.componentName)
                        return 0;
                    auto&            reg = engine->GetRegistry();
                    std::string_view name(a.componentName);

                    auto it = s_ComponentRegistry.find(name);
                    if (it != s_ComponentRegistry.end()) {
                        *a.outView = it->second.getBuffer(reg);
                    } else {
                        *a.outView = {};
                    }
                    return 0;
                }));

    RegisterCmd("GetECSEntities", MakeCmd<GetECSBufferArgs>([](ZHLN::Engine* engine, const GetECSBufferArgs& a) -> uint64_t {
                    if (!a.componentName)
                        return 0;
                    auto&    reg      = engine->GetRegistry();
                    uint32_t familyID = ZHLN::ECS::Registry::GetFamilyIDFromName(a.componentName);
                    if (familyID != 0xFFFFFFFF) {
                        auto entities = reg.GetEntitiesByFamilyID(familyID);
                        *a.outView    = ZHLN::ViewComposer::Build(&reg, const_cast<ZHLN::Entity*>(entities.data()), "Q", entities.size());
                    } else {
                        *a.outView = {};
                    }
                    return 0;
                }));

    RegisterCmd("ReleaseBuffer", MakeCmd<ReleaseBufferArgs, false>([](ZHLN::Engine*, const ReleaseBufferArgs& a) -> uint64_t {
                    if (a.sync_ptr == nullptr) {
                        return 0;
                    }
                    ZHLN::SyncPolicy::Release(static_cast<ZHLN::BufferSync*>(a.sync_ptr));
                    return 0;
                }));

    RegisterCmd("LogInventoryShell", MakeCmd<LogInventoryArgs>([](ZHLN::Engine*, const LogInventoryArgs& a) -> uint64_t {
                    if (!a.msg)
                        return 0;
                    std::string str(a.msg);
                    size_t      pos = 0;
                    while (pos < str.size()) {
                        size_t next_nl = str.find('\n', pos);
                        if (next_nl == std::string::npos) {
                            s_InvShellLog.push_back(str.substr(pos));
                            break;
                        }
                        s_InvShellLog.push_back(str.substr(pos, next_nl - pos));
                        pos = next_nl + 1;
                    }
                    s_InvScrollToBottom = true;
                    std::println(stdout, "[InvShell Output]\n{}", a.msg);
                    std::fflush(stdout);
                    return 0;
                }));
}

void RegisterSystemCommands() {
    RegisterCmd("ProvokeDeviceLost", MakeCmd<void>([](ZHLN::Engine* engine) -> uint64_t {
                    engine->ProvokeDeviceLost();
                    return 1;
                }));

    RegisterCmd("DrawLine", MakeCmd<DrawLineArgs>([](ZHLN::Engine* engine, const DrawLineArgs& a) -> uint64_t {
                    engine->GetRenderContext().DrawLine(
                        JPH::Vec3(a.ox, a.oy, a.oz), JPH::Vec3(a.dx, a.dy, a.dz), JPH::Vec4(a.r1, a.g1, a.b1, a.a1), JPH::Vec4(a.r2, a.g2, a.b2, a.a2)
                    );
                    return 1;
                }));

    RegisterCmd("InitPlayer", MakeCmd<void>([](ZHLN::Engine* engine) -> uint64_t {
                    using namespace ZHLN;
                    auto& reg = engine->GetRegistry();

                    auto _ = CreativeWorksFactory::CreatePlane(
                        *engine, 1000.0f, {0.6f, 0.6f, 0.6f, 1.0f},
                        CreativeWorksFactory::SpawnParams {.position = {0.0f, 0.0f, 0.0f}, .createPhysics = true, .isStaticPhysics = true}
                    );

                    ZHLN::Entity playerEntity = reg.Create();
                    reg.Add(playerEntity, Components::PlayerTagComponent {});
                    reg.Add(playerEntity, Components::TransformComponent {.position = {0.0f, 3.0f, 0.0f}});
                    reg.Add(playerEntity, Components::MovementComponent {});
                    reg.Add(playerEntity, ZHLN::Components::InputComponent {});
                    ZHLN::Entity charPhys = engine->GetPhysicsContext().CreateCharacter(JPH::RVec3(0.0f, 3.0f, 0.0f));
                    reg.Add(playerEntity, Components::PhysicsComponent {charPhys});
                    reg.Add(playerEntity, Components::PhysicsStateComponent {.currPosition = {0.0f, 3.0f, 0.0f}, .prevPosition = {0.0f, 3.0f, 0.0f}});

                    auto camEnts = reg.GetEntitiesWith<ZHLN::Components::MainCameraTagComponent>();
                    if (!camEnts.empty()) {
                        ZHLN::Entity camEnt = camEnts[0];
                        reg.Add(
                            camEnt, Components::TargetCameraComponent {
                                        .target            = playerEntity,
                                        .distance          = 4.5f,
                                        .targetDistance    = 4.5f,
                                        .yaw               = -90.0f,
                                        .pitch             = -10.0f,
                                        .stiffness         = 15.0f,
                                        .vignetteIntensity = 1.10f,
                                        .vignettePower     = 1.50f,
                                        .fov               = 45.0f,
                                        .targetFov         = 45.0f
                                    }
                        );
                        reg.Add(camEnt, Components::InputComponent {});
                    }
                    return playerEntity.Pack();
                }));

    RegisterCmd("GetAnimationTrackCount", MakeCmd<EntityOnlyArgs>([](ZHLN::Engine* engine, const EntityOnlyArgs& a) -> uint64_t {
                    auto entity = ZHLN::Entity::Unpack(a.entityRaw);
                    if (auto* anim = engine->GetRegistry().Get<ZHLN::Components::AnimatorComponent>(entity)) {
                        if (anim->prefab != nullptr) {
                            return static_cast<uint64_t>(anim->prefab->animations.size());
                        }
                    }
                    return 0;
                }));

    RegisterCmd("GetAnimationTrackName", MakeCmd<GetTrackNameArgs>([](ZHLN::Engine* engine, const GetTrackNameArgs& a) -> uint64_t {
                    auto entity = ZHLN::Entity::Unpack(a.entityRaw);
                    if (auto* anim = engine->GetRegistry().Get<ZHLN::Components::AnimatorComponent>(entity)) {
                        if (anim->prefab != nullptr) {
                            if (a.trackIndex >= 0 && a.trackIndex < static_cast<int32_t>(anim->prefab->animations.size())) {
                                const auto& name = anim->prefab->animations[a.trackIndex].name;
                                std::strncpy(const_cast<char*>(a.outName), name.c_str(), 63);
                                const_cast<char*>(a.outName)[63] = '\0';
                                return 1;
                            }
                        }
                    }
                    return 0;
                }));

    RegisterCmd("PlayAnimationTrack", MakeCmd<PlayTrackArgs>([](ZHLN::Engine* engine, const PlayTrackArgs& a) -> uint64_t {
                    auto entity = ZHLN::Entity::Unpack(a.entityRaw);
                    if (auto* anim = engine->GetRegistry().Get<ZHLN::Components::AnimatorComponent>(entity)) {
                        if (anim->prefab != nullptr) {
                            if (a.trackIndex >= 0 && a.trackIndex < static_cast<int32_t>(anim->prefab->animations.size())) {
                                if (anim->currentTrackIdx != a.trackIndex) {
                                    anim->prevTrackIdx      = anim->currentTrackIdx;
                                    anim->prevTrackTime     = anim->currentTrackTime;
                                    anim->prevPlaybackSpeed = anim->currentPlaybackSpeed;

                                    anim->currentTrackIdx      = a.trackIndex;
                                    anim->currentTrackTime     = 0.0f;
                                    anim->currentPlaybackSpeed = a.playbackSpeed;
                                    anim->currentLoop          = (a.loop != 0);

                                    anim->blendFactor   = 0.0f;
                                    anim->blendDuration = a.blendDuration;
                                    anim->isFinished    = false;
                                } else {
                                    anim->currentLoop          = (a.loop != 0);
                                    anim->currentPlaybackSpeed = a.playbackSpeed;
                                    if (anim->isFinished && anim->currentLoop) {
                                        anim->isFinished       = false;
                                        anim->currentTrackTime = 0.0f;
                                    }
                                }
                                return 1;
                            }
                        }
                    }
                    return 0;
                }));

    RegisterCmd("AddIKChain", MakeCmd<AddIKChainArgs>([](ZHLN::Engine* engine, const AddIKChainArgs& a) -> uint64_t {
                    auto  entity = ZHLN::Entity::Unpack(a.entityRaw);
                    auto& reg    = engine->GetRegistry();

                    auto* ikComp = reg.Get<Components::TwoBoneIKComponent>(entity);
                    if (ikComp == nullptr) {
                        ikComp = &reg.Add(entity, Components::TwoBoneIKComponent {});
                    }

                    Components::TwoBoneIKChain chain;
                    chain.upperNodeIndex = a.upperNodeIndex;
                    chain.lowerNodeIndex = a.lowerNodeIndex;
                    chain.endNodeIndex   = a.endNodeIndex;
                    chain.targetPosition = JPH::Vec3(a.targetX, a.targetY, a.targetZ);
                    chain.poleVector     = JPH::Vec3(a.poleX, a.poleY, a.poleZ);
                    chain.weight         = a.weight;

                    ikComp->chains.push_back(chain);
                    return static_cast<uint64_t>(ikComp->chains.size() - 1);
                }));

    RegisterCmd("SetIKTarget", MakeCmd<SetIKTargetArgs>([](ZHLN::Engine* engine, const SetIKTargetArgs& a) -> uint64_t {
                    auto entity = ZHLN::Entity::Unpack(a.entityRaw);
                    if (auto* ikComp = engine->GetRegistry().Get<Components::TwoBoneIKComponent>(entity)) {
                        if (a.chainIndex < ikComp->chains.size()) {
                            auto& chain          = ikComp->chains[a.chainIndex];
                            chain.targetPosition = JPH::Vec3(a.tx, a.ty, a.tz);
                            chain.targetRotation = JPH::Quat(a.rx, a.ry, a.rz, a.rw);
                            chain.weight         = a.weight;
                            return 1;
                        }
                    }
                    return 0;
                }));

    RegisterCmd("SetIKTargetEntity", MakeCmd<SetIKTargetEntityArgs>([](ZHLN::Engine* engine, const SetIKTargetEntityArgs& a) -> uint64_t {
                    auto entity = ZHLN::Entity::Unpack(a.entityRaw);
                    if (auto* ikComp = engine->GetRegistry().Get<Components::TwoBoneIKComponent>(entity)) {
                        if (a.chainIndex < ikComp->chains.size()) {
                            auto& chain        = ikComp->chains[a.chainIndex];
                            chain.targetEntity = ZHLN::Entity::Unpack(a.targetEntityRaw);
                            chain.targetOffset = JPH::Vec3(a.offsetX, a.offsetY, a.offsetZ);
                            chain.weight       = a.weight;
                            return 1;
                        }
                    }
                    return 0;
                }));
}

void RegisterFFICommands() {
    if (!s_JumpTable.empty()) {
        return;
    }
    InitComponentRegistry();

    RegisterCreativeWorkCommands();
    RegisterPhysicsCommands();
    RegisterInputAndCameraCommands();
    RegisterAudioCommands();
    RegisterECSCommands();
    RegisterSystemCommands();
}
}} // namespace ZHLN

extern "C" {

using namespace ZHLN;

ZHLN_API ZHLN_Engine* ZHLN_GetEngineContext() {
    return reinterpret_cast<ZHLN_Engine*>(ZHLN::GetEngineContext());
}

ZHLN_API uint32_t ZHLN_GetCommandID(const char* cmdName) {
    if (cmdName == nullptr) {
        return 0xFFFFFFFF;
    }
    RegisterFFICommands();

    std::string_view view(cmdName);
    auto             it = s_StringToIntMap.find(view);
    if (it != s_StringToIntMap.end()) {
        return it->second;
    }

    ZHLN::Log("WARNING: ZHLN_GetCommandID could not resolve '{}' to a known command.", cmdName);
    return 0xFFFFFFFF;
}

ZHLN_API uint64_t ZHLN_DispatchCommand(ZHLN_Engine* engine_handle, uint32_t cmdID, const void* args) {
    if (cmdID >= s_JumpTable.size()) [[unlikely]] {
        return 0;
    }
    return s_JumpTable[cmdID](reinterpret_cast<ZHLN::Engine*>(engine_handle), args);
}

} // extern "C"

namespace ZHLN {

ScriptRunner::ScriptRunner(): _runtime(std::make_unique<LuaScriptRuntime>()) {
}

ScriptRunner::~ScriptRunner() = default;

void ScriptRunner::RunFile(std::string_view path) {
    _runtime->RunFile(path);
}

void ScriptRunner::CallUpdate(Engine* engine, float dt) {
    if (engine != nullptr) {
        _runtime->Initialize(engine);
        _runtime->TickUpdate(engine, dt);
    }
}

void ScriptRunner::ExecuteString(std::string_view code) {
    _runtime->ExecuteString(code);
}

void ScriptRunner::ReloadFile(std::string_view path) {
    _runtime->ReloadFile(path);
}

} // namespace ZHLN
