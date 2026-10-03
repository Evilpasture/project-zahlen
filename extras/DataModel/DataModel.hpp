// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// extras/DataModel/DataModel.hpp
//
// Project-light's native, C++-owned DataModel object graph. This layer is
// intentionally separate from both the LuaJIT runtime and ECS component
// storage: script-facing Instance identities and parentage are stable, while
// the adapter may bind renderable Instances to generation-safe ECS entities.

#include <Jolt/Jolt.h>
#include <Jolt/Math/Vec3.h>
#include <Zahlen/Core/Array.hpp>
#include <Zahlen/Entity.hpp>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace ZHLN::ProjectLight {

using InstanceId = uint64_t;

enum class DataModelError : uint8_t {
    InvalidId,
    DuplicateId,
    IdExhausted,
    DestroyedInstance,
    DataModelExpired,
    CrossDataModelParent,
    ParentCycle,
    RootCannotBeParented,
    InvalidReference,
};

class Instance;
class Player;
using InstancePtr = std::shared_ptr<Instance>;

struct DataModelState;

// Small, main-thread signal primitive for DataModel tree/property events.
// Connections are RAII tokens: keep the returned token alive to stay connected.
template <typename... Args>
class DataModelSignal {
  private:
    using Callback = std::function<void(Args...)>;
    struct SlotState {
        uint64_t nextId = 1;
        ZHLN::Array<std::pair<uint64_t, Callback>, 2> slots;
    };

  public:
    class Connection {
      public:
        Connection() = default;
        ~Connection() { Disconnect(); }
        Connection(const Connection&) = delete;
        auto operator=(const Connection&) -> Connection& = delete;

        Connection(Connection&& other) noexcept:
            m_state(std::move(other.m_state)), m_id(std::exchange(other.m_id, 0)) {}

        auto operator=(Connection&& other) noexcept -> Connection& {
            if (this != &other) {
                Disconnect();
                m_state = std::move(other.m_state);
                m_id = std::exchange(other.m_id, 0);
            }
            return *this;
        }

        void Disconnect() noexcept {
            if (m_id == 0) return;
            if (const auto state = m_state.lock()) {
                auto slot = state->slots.begin();
                while (slot != state->slots.end()) {
                    if (slot->first == m_id) slot = state->slots.erase(slot);
                    else ++slot;
                }
            }
            m_state.reset();
            m_id = 0;
        }

        [[nodiscard]] auto IsConnected() const noexcept -> bool {
            return m_id != 0 && !m_state.expired();
        }

      private:
        friend class DataModelSignal;
        Connection(const std::shared_ptr<SlotState>& state, uint64_t id): m_state(state), m_id(id) {}
        std::weak_ptr<SlotState> m_state;
        uint64_t m_id = 0;
    };

    DataModelSignal() = default;
    DataModelSignal(const DataModelSignal&) = delete;
    auto operator=(const DataModelSignal&) -> DataModelSignal& = delete;
    DataModelSignal(DataModelSignal&&) = delete;
    auto operator=(DataModelSignal&&) -> DataModelSignal& = delete;

    [[nodiscard]] auto Connect(Callback callback) -> Connection {
        if (!callback) return {};
        if (!m_state) m_state = std::make_shared<SlotState>();
        const uint64_t id = m_state->nextId++;
        m_state->slots.emplace_back(id, std::move(callback));
        return Connection {m_state, id};
    }

    void Fire(Args... args) const {
        const std::shared_ptr<SlotState> state = m_state;
        if (!state) return;
        ZHLN::Array<Callback, 2> callbacks;
        callbacks.reserve(state->slots.size());
        for (const auto& slot: state->slots) callbacks.push_back(slot.second);
        for (auto& callback: callbacks) callback(args...);
    }

    [[nodiscard]] auto Empty() const noexcept -> bool { return !m_state || m_state->slots.empty(); }

  private:
    std::shared_ptr<SlotState> m_state;
};

class Instance : public std::enable_shared_from_this<Instance> {
  public:
    virtual ~Instance() = default;

    Instance(const Instance&) = delete;
    auto operator=(const Instance&) -> Instance& = delete;
    Instance(Instance&&) = delete;
    auto operator=(Instance&&) -> Instance& = delete;

    [[nodiscard]] auto Id() const noexcept -> InstanceId { return m_id; }
    [[nodiscard]] virtual auto ClassName() const noexcept -> std::string_view { return m_className; }
    [[nodiscard]] virtual auto IsA(std::string_view className) const noexcept -> bool;
    [[nodiscard]] virtual auto IsService() const noexcept -> bool { return false; }
    [[nodiscard]] auto Name() const noexcept -> const std::string& { return m_name; }
    void SetName(std::string name);
    [[nodiscard]] auto Parent() const noexcept -> InstancePtr { return m_parent.lock(); }
    [[nodiscard]] auto IsDestroyed() const noexcept -> bool { return m_destroyed; }
    [[nodiscard]] auto IsDataModelRoot() const noexcept -> bool { return m_isRoot; }

    [[nodiscard]] auto GetChildren() const -> std::vector<InstancePtr>;
    [[nodiscard]] auto GetDescendants() const -> std::vector<InstancePtr>;
    [[nodiscard]] auto GetAncestors() const -> std::vector<InstancePtr>;
    [[nodiscard]] auto FindFirstChild(std::string_view name, bool recursive = false) const -> InstancePtr;
    [[nodiscard]] auto FindFirstChildOfClass(std::string_view className, bool recursive = false) const -> InstancePtr;
    [[nodiscard]] auto FindFirstChildWhichIsA(std::string_view className, bool recursive = false) const -> InstancePtr;
    [[nodiscard]] auto IsDescendantOf(const InstancePtr& ancestor) const noexcept -> bool;
    [[nodiscard]] auto GetFullName() const -> std::string;

    [[nodiscard]] auto SetParent(const InstancePtr& parent) -> std::expected<void, DataModelError>;
    void Destroy();

    // ECS entities are optional implementation bindings, not Instance IDs.
    // They remain generation-safe and are deliberately set by the adapter.
    [[nodiscard]] auto BackingEntity() const noexcept -> std::optional<Entity> { return m_backingEntity; }
    void BindEntity(Entity entity) noexcept;
    void UnbindEntity() noexcept { m_backingEntity.reset(); }

    DataModelSignal<InstancePtr> ChildAdded;
    DataModelSignal<InstancePtr> ChildRemoved;
    DataModelSignal<InstancePtr> DescendantAdded;
    DataModelSignal<InstancePtr> DescendantRemoved;
    DataModelSignal<InstancePtr> Destroying;
    DataModelSignal<InstancePtr, InstancePtr> AncestryChanged;
    DataModelSignal<std::string> PropertyChanged;

  protected:
    Instance(
        const std::shared_ptr<DataModelState>& state,
        InstanceId id,
        std::string className,
        std::string name,
        bool isRoot = false
    );

    void NotifyChanged(std::string_view property);
    [[nodiscard]] auto BelongsTo(const Instance& other) const noexcept -> bool;

  private:
    friend class DataModel;
    friend class Model;
    friend class Player;
    friend class Humanoid;

    void RemoveChild(const InstancePtr& child) noexcept;
    void DestroyChildren();

    std::weak_ptr<DataModelState> m_state;
    InstanceId m_id = 0;
    std::string m_className;
    std::string m_name;
    std::weak_ptr<Instance> m_parent;
    ZHLN::Array<InstancePtr, 4> m_children;
    std::optional<Entity> m_backingEntity;
    bool m_destroyed = false;
    bool m_destroying = false;
    bool m_isRoot = false;
};

class Service : public Instance {
  protected:
    Service(const std::shared_ptr<DataModelState>& state, InstanceId id, std::string className, std::string name);

  public:
    [[nodiscard]] auto IsA(std::string_view className) const noexcept -> bool override;
    [[nodiscard]] auto IsService() const noexcept -> bool override { return true; }
};

class WorkspaceService final : public Service {
  public:
    static constexpr std::string_view ClassNameValue = "WorkspaceService";
    [[nodiscard]] auto CurrentCamera() const noexcept -> InstancePtr;
    [[nodiscard]] auto SetCurrentCamera(const InstancePtr& camera) -> std::expected<void, DataModelError>;

  private:
    friend class DataModel;
    WorkspaceService(const std::shared_ptr<DataModelState>& state, InstanceId id, std::string name);
    std::weak_ptr<Instance> m_currentCamera;
};

class PlayersService final : public Service {
  public:
    static constexpr std::string_view ClassNameValue = "PlayersService";
    [[nodiscard]] auto GetPlayers() const -> std::vector<std::shared_ptr<class Player>>;
    [[nodiscard]] auto FindPlayerByUserId(uint64_t userId) const -> std::shared_ptr<class Player>;
    [[nodiscard]] auto LocalPlayer() const noexcept -> std::shared_ptr<class Player>;
    [[nodiscard]] auto SetLocalPlayer(const std::shared_ptr<class Player>& player) -> std::expected<void, DataModelError>;

  private:
    friend class DataModel;
    PlayersService(const std::shared_ptr<DataModelState>& state, InstanceId id, std::string name);
    std::weak_ptr<class Player> m_localPlayer;
};

class LightingService final : public Service {
  public:
    static constexpr std::string_view ClassNameValue = "LightingService";

  private:
    friend class DataModel;
    LightingService(const std::shared_ptr<DataModelState>& state, InstanceId id, std::string name);
};

class PhysicsService final : public Service {
  public:
    static constexpr std::string_view ClassNameValue = "PhysicsService";
    [[nodiscard]] auto ServerAuthority() const noexcept -> bool { return m_serverAuthority; }
    void SetServerAuthority(bool value);

  private:
    friend class DataModel;
    PhysicsService(const std::shared_ptr<DataModelState>& state, InstanceId id, std::string name);
    bool m_serverAuthority = false;
};

class SoundService final : public Service {
  public:
    static constexpr std::string_view ClassNameValue = "SoundService";

  private:
    friend class DataModel;
    SoundService(const std::shared_ptr<DataModelState>& state, InstanceId id, std::string name);
};

// A service class known to project-light but not yet represented by a typed
// wrapper still participates in DataModel::GetService and keeps its real class
// name and ID. This is not a second service runtime.
class OpaqueService final : public Service {
  private:
    friend class DataModel;
    OpaqueService(
        const std::shared_ptr<DataModelState>& state,
        InstanceId id,
        std::string className,
        std::string name
    );
};

class Folder final : public Instance {
  private:
    friend class DataModel;
    Folder(const std::shared_ptr<DataModelState>& state, InstanceId id, std::string name);
};

class Model final : public Instance {
  public:
    [[nodiscard]] auto PrimaryPart() const noexcept -> std::shared_ptr<class BasePart>;
    [[nodiscard]] auto SetPrimaryPart(const std::shared_ptr<class BasePart>& part) -> std::expected<void, DataModelError>;
    [[nodiscard]] auto IsA(std::string_view className) const noexcept -> bool override;

  private:
    friend class DataModel;
    Model(const std::shared_ptr<DataModelState>& state, InstanceId id, std::string name);
    std::weak_ptr<class BasePart> m_primaryPart;
};

class BasePart : public Instance {
  public:
    [[nodiscard]] auto IsA(std::string_view className) const noexcept -> bool override;

    [[nodiscard]] auto Position() const noexcept -> const JPH::Vec3& { return m_position; }
    void SetPosition(const JPH::Vec3& value);
    // project-light scripts express rotations as Euler degrees; renderer and
    // physics adapters may convert this at their boundary to JPH::Quat.
    [[nodiscard]] auto Rotation() const noexcept -> const JPH::Vec3& { return m_rotationEuler; }
    void SetRotation(const JPH::Vec3& value);
    [[nodiscard]] auto Size() const noexcept -> const JPH::Vec3& { return m_size; }
    void SetSize(const JPH::Vec3& value);
    [[nodiscard]] auto Color() const noexcept -> const JPH::Vec3& { return m_color; }
    void SetColor(const JPH::Vec3& value);
    [[nodiscard]] auto Anchored() const noexcept -> bool { return m_anchored; }
    void SetAnchored(bool value);
    [[nodiscard]] auto CanCollide() const noexcept -> bool { return m_canCollide; }
    void SetCanCollide(bool value);
    [[nodiscard]] auto Transparency() const noexcept -> float { return m_transparency; }
    void SetTransparency(float value);
    [[nodiscard]] auto NetworkOwner() const noexcept -> std::shared_ptr<Player>;
    [[nodiscard]] auto SetNetworkOwner(const std::shared_ptr<Player>& player) -> std::expected<void, DataModelError>;

  protected:
    BasePart(const std::shared_ptr<DataModelState>& state, InstanceId id, std::string className, std::string name);

  private:
    friend class DataModel;
    JPH::Vec3 m_position = JPH::Vec3::sZero();
    // Protocol-facing Euler degrees; convert only in the physics/ECS adapter.
    JPH::Vec3 m_rotationEuler = JPH::Vec3::sZero();
    JPH::Vec3 m_size = JPH::Vec3(4.0f, 1.0f, 2.0f);
    JPH::Vec3 m_color = JPH::Vec3(0.75f, 0.75f, 0.75f);
    std::weak_ptr<Player> m_networkOwner;
    bool m_anchored = false;
    bool m_canCollide = true;
    float m_transparency = 0.0f;
};

enum class PartShape : uint8_t {
    Cube,
    Sphere,
    Wedge,
    CornerWedge,
    Cylinder,
    Capsule,
    Cone,
    Head,
    Block = Cube,
};

enum class PartSurface : uint8_t {
    Smooth,
    Studs,
    Inlet,
};

class Part final : public BasePart {
  public:
    [[nodiscard]] auto IsA(std::string_view className) const noexcept -> bool override;
    [[nodiscard]] auto Shape() const noexcept -> PartShape { return m_shape; }
    void SetShape(PartShape value);
    [[nodiscard]] auto FrontSurface() const noexcept -> PartSurface { return m_frontSurface; }
    void SetFrontSurface(PartSurface value);
    [[nodiscard]] auto BackSurface() const noexcept -> PartSurface { return m_backSurface; }
    void SetBackSurface(PartSurface value);
    [[nodiscard]] auto TopSurface() const noexcept -> PartSurface { return m_topSurface; }
    void SetTopSurface(PartSurface value);
    [[nodiscard]] auto BottomSurface() const noexcept -> PartSurface { return m_bottomSurface; }
    void SetBottomSurface(PartSurface value);
    [[nodiscard]] auto LeftSurface() const noexcept -> PartSurface { return m_leftSurface; }
    void SetLeftSurface(PartSurface value);
    [[nodiscard]] auto RightSurface() const noexcept -> PartSurface { return m_rightSurface; }
    void SetRightSurface(PartSurface value);

  private:
    friend class DataModel;
    Part(const std::shared_ptr<DataModelState>& state, InstanceId id, std::string name);
    PartShape m_shape = PartShape::Cube;
    PartSurface m_frontSurface = PartSurface::Smooth;
    PartSurface m_backSurface = PartSurface::Smooth;
    PartSurface m_topSurface = PartSurface::Studs;
    PartSurface m_bottomSurface = PartSurface::Inlet;
    PartSurface m_leftSurface = PartSurface::Smooth;
    PartSurface m_rightSurface = PartSurface::Smooth;
};

class MeshPart final : public BasePart {
  public:
    [[nodiscard]] auto IsA(std::string_view className) const noexcept -> bool override;
    [[nodiscard]] auto MeshId() const noexcept -> const std::string& { return m_meshId; }
    void SetMeshId(std::string value);

  private:
    friend class DataModel;
    MeshPart(const std::shared_ptr<DataModelState>& state, InstanceId id, std::string name);
    std::string m_meshId;
};

class SpawnPoint final : public Instance {
  public:
    [[nodiscard]] auto Position() const noexcept -> const JPH::Vec3& { return m_position; }
    void SetPosition(const JPH::Vec3& value);
    [[nodiscard]] auto Rotation() const noexcept -> const JPH::Vec3& { return m_rotation; }
    void SetRotation(const JPH::Vec3& value);

  private:
    friend class DataModel;
    SpawnPoint(const std::shared_ptr<DataModelState>& state, InstanceId id, std::string name);
    JPH::Vec3 m_position = JPH::Vec3::sZero();
    // Protocol-facing Euler degrees; convert only in the physics/ECS adapter.
    JPH::Vec3 m_rotation = JPH::Vec3::sZero();
};

enum class DecalFace : uint8_t { Top, Bottom, Front, Back, Left, Right };
enum class DecalWrapMode : uint8_t { Stretch, Repeat };

class Decal final : public Instance {
  public:
    [[nodiscard]] auto TextureId() const noexcept -> const std::string& { return m_textureId; }
    void SetTextureId(std::string value);
    [[nodiscard]] auto Color() const noexcept -> const JPH::Vec3& { return m_color; }
    void SetColor(const JPH::Vec3& value);
    [[nodiscard]] auto Face() const noexcept -> DecalFace { return m_face; }
    void SetFace(DecalFace value);
    [[nodiscard]] auto WrapMode() const noexcept -> DecalWrapMode { return m_wrapMode; }
    void SetWrapMode(DecalWrapMode value);
    [[nodiscard]] auto Scale() const noexcept -> float { return m_scale; }
    void SetScale(float value);
    [[nodiscard]] auto Transparency() const noexcept -> float { return m_transparency; }
    void SetTransparency(float value);

  private:
    friend class DataModel;
    Decal(const std::shared_ptr<DataModelState>& state, InstanceId id, std::string name);
    std::string m_textureId;
    JPH::Vec3 m_color = JPH::Vec3(1.0f, 1.0f, 1.0f);
    DecalFace m_face = DecalFace::Front;
    DecalWrapMode m_wrapMode = DecalWrapMode::Stretch;
    float m_scale = 1.0f;
    float m_transparency = 0.0f;
};

class Sound final : public Instance {
  public:
    [[nodiscard]] auto SoundId() const noexcept -> const std::string& { return m_soundId; }
    void SetSoundId(std::string value);
    [[nodiscard]] auto Volume() const noexcept -> float { return m_volume; }
    void SetVolume(float value);
    [[nodiscard]] auto Loops() const noexcept -> bool { return m_loops; }
    void SetLoops(bool value);
    [[nodiscard]] auto Playing() const noexcept -> bool { return m_playing; }
    void SetPlaying(bool value);
    void Play();
    void Stop();

    DataModelSignal<> Ended;

  private:
    friend class DataModel;
    Sound(const std::shared_ptr<DataModelState>& state, InstanceId id, std::string name);
    std::string m_soundId;
    float m_volume = 1.0f;
    bool m_loops = false;
    bool m_playing = false;
};

enum class HumanoidState : uint8_t { Idle, Running, Jumping, Freefall, FallingDown, GettingUp };

class Motor final : public Instance {
  public:
    [[nodiscard]] auto IsA(std::string_view className) const noexcept -> bool override;
    [[nodiscard]] auto Part1() const noexcept -> std::shared_ptr<BasePart>;
    [[nodiscard]] auto SetPart1(const std::shared_ptr<BasePart>& part) -> std::expected<void, DataModelError>;
    [[nodiscard]] auto Part2() const noexcept -> std::shared_ptr<BasePart>;
    [[nodiscard]] auto SetPart2(const std::shared_ptr<BasePart>& part) -> std::expected<void, DataModelError>;
    [[nodiscard]] auto Offset1() const noexcept -> const JPH::Vec3& { return m_offset1; }
    void SetOffset1(const JPH::Vec3& value);
    [[nodiscard]] auto Offset2() const noexcept -> const JPH::Vec3& { return m_offset2; }
    void SetOffset2(const JPH::Vec3& value);
    [[nodiscard]] auto CurrentAngle() const noexcept -> float { return m_currentAngle; }
    void SetCurrentAngle(float value);
    [[nodiscard]] auto DesiredAngle() const noexcept -> float { return m_desiredAngle; }
    void SetDesiredAngle(float value);
    [[nodiscard]] auto MaxVelocity() const noexcept -> float { return m_maxVelocity; }
    void SetMaxVelocity(float value);

  private:
    friend class DataModel;
    Motor(const std::shared_ptr<DataModelState>& state, InstanceId id, std::string name);
    std::weak_ptr<BasePart> m_part1;
    std::weak_ptr<BasePart> m_part2;
    JPH::Vec3 m_offset1 = JPH::Vec3::sZero();
    JPH::Vec3 m_offset2 = JPH::Vec3::sZero();
    float m_currentAngle = 0.0f;
    float m_desiredAngle = 0.0f;
    float m_maxVelocity = 30.0f;
};

class Humanoid final : public Instance {
  public:
    [[nodiscard]] auto WalkSpeed() const noexcept -> float { return m_walkSpeed; }
    void SetWalkSpeed(float value);
    [[nodiscard]] auto JumpPower() const noexcept -> float { return m_jumpPower; }
    void SetJumpPower(float value);
    [[nodiscard]] auto Health() const noexcept -> float { return m_health; }
    void SetHealth(float value);
    [[nodiscard]] auto MaxHealth() const noexcept -> float { return m_maxHealth; }
    void SetMaxHealth(float value);
    [[nodiscard]] auto RootPart() const noexcept -> std::shared_ptr<BasePart>;
    [[nodiscard]] auto SetRootPart(const std::shared_ptr<BasePart>& part) -> std::expected<void, DataModelError>;
    [[nodiscard]] auto State() const noexcept -> HumanoidState { return m_state; }
    void SetState(HumanoidState value);
    [[nodiscard]] auto IsA(std::string_view className) const noexcept -> bool override;

    DataModelSignal<HumanoidState, HumanoidState> StateChanged;

  private:
    friend class DataModel;
    Humanoid(const std::shared_ptr<DataModelState>& state, InstanceId id, std::string name);
    float m_walkSpeed = 16.0f;
    float m_jumpPower = 50.0f;
    float m_health = 100.0f;
    float m_maxHealth = 100.0f;
    HumanoidState m_state = HumanoidState::Idle;
    std::weak_ptr<BasePart> m_rootPart;
};

class Player final : public Instance {
  public:
    [[nodiscard]] auto UserId() const noexcept -> uint64_t { return m_userId; }
    void SetUserId(uint64_t value);
    [[nodiscard]] auto Character() const noexcept -> std::shared_ptr<Model>;
    [[nodiscard]] auto SetCharacter(const std::shared_ptr<Model>& character) -> std::expected<void, DataModelError>;
    [[nodiscard]] auto IsA(std::string_view className) const noexcept -> bool override;

  private:
    friend class DataModel;
    Player(const std::shared_ptr<DataModelState>& state, InstanceId id, std::string name);
    uint64_t m_userId = 0;
    std::weak_ptr<Model> m_character;
};

// Unknown server classes can still retain their identity and tree position;
// class-specific behavior can be added without changing Instance ownership.
class OpaqueInstance final : public Instance {
  private:
    friend class DataModel;
    OpaqueInstance(
        const std::shared_ptr<DataModelState>& state,
        InstanceId id,
        std::string className,
        std::string name
    );
};

class DataModel final {
  public:
    DataModel();
    ~DataModel();
    DataModel(const DataModel&) = delete;
    auto operator=(const DataModel&) -> DataModel& = delete;
    DataModel(DataModel&&) = delete;
    auto operator=(DataModel&&) -> DataModel& = delete;

    [[nodiscard]] auto Root() const noexcept -> InstancePtr { return m_root; }
    [[nodiscard]] auto FindById(InstanceId id) const noexcept -> InstancePtr;
    [[nodiscard]] auto GetService(std::string_view nameOrClass) const -> std::shared_ptr<Service>;

    template <typename T>
    [[nodiscard]] auto GetService(std::string_view nameOrClass) const -> std::shared_ptr<T> {
        static_assert(std::is_base_of_v<Service, T>);
        const std::shared_ptr<Service> service = GetService(nameOrClass);
        if (!service) return {};
        if constexpr (std::is_same_v<T, Service>) {
            return service;
        } else if constexpr (requires { T::ClassNameValue; }) {
            if (service->ClassName() != T::ClassNameValue) return {};
            return std::static_pointer_cast<T>(service);
        } else {
            return {};
        }
    }

    template <typename T>
    [[nodiscard]] auto Create(std::string name = {}) -> std::expected<std::shared_ptr<T>, DataModelError> {
        auto id = AllocateId();
        if (!id) return std::unexpected(id.error());
        return CreateWithId<T>(*id, std::move(name));
    }

    template <typename T>
    [[nodiscard]] auto CreateWithId(InstanceId id, std::string name = {}) -> std::expected<std::shared_ptr<T>, DataModelError> {
        static_assert(std::is_base_of_v<Instance, T>);
        if (id == 0) return std::unexpected(DataModelError::InvalidId);
        if (HasEverUsedId(id)) return std::unexpected(DataModelError::DuplicateId);
        std::shared_ptr<T> instance;
        if constexpr (std::is_same_v<T, BasePart>) {
            instance.reset(new BasePart(m_state, id, "BasePart", std::move(name)));
        } else {
            instance.reset(new T(m_state, id, std::move(name)));
        }
        RegisterInstance(instance);
        return instance;
    }

    [[nodiscard]] auto CreateInstance(std::string_view className, std::string name = {})
        -> std::expected<InstancePtr, DataModelError>;
    [[nodiscard]] auto CreateInstanceWithId(InstanceId id, std::string_view className, std::string name = {})
        -> std::expected<InstancePtr, DataModelError>;

    void Clear();

  private:
    [[nodiscard]] auto AllocateId() noexcept -> std::expected<InstanceId, DataModelError>;
    [[nodiscard]] auto HasEverUsedId(InstanceId id) const noexcept -> bool;
    void RegisterInstance(const InstancePtr& instance);

    std::shared_ptr<DataModelState> m_state;
    InstancePtr m_root;
};

} // namespace ZHLN::ProjectLight
