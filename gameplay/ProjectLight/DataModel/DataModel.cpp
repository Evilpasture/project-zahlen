// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// gameplay/ProjectLight/DataModel/DataModel.cpp

#include <DataModel/DataModel.hpp>
#include <Zahlen/Core/Defer.hpp>
#include <Zahlen/Core/HashMap.hpp>
#include <Zahlen/Core/Reflection/Core.hpp>
#include <algorithm>
#include <limits>
#include <ranges>

namespace ZHLN::ProjectLight {

struct DataModelState {
    InstanceId                                             nextId = 1;
    ZHLN::HashMap<InstanceId, std::weak_ptr<Instance>, 16> instances;
};

namespace {

template <typename T>
consteval auto ReflectedClassName() noexcept -> std::string_view {
    return ZHLN::Reflect::TypeName<T>();
}

auto SameVec3(const JPH::Vec3& lhs, const JPH::Vec3& rhs) noexcept -> bool {
    return lhs.GetX() == rhs.GetX() && lhs.GetY() == rhs.GetY() && lhs.GetZ() == rhs.GetZ();
}

auto WithDefaultName(std::string name, std::string_view fallback) -> std::string {
    return name.empty() ? std::string(fallback) : std::move(name);
}

static_assert(ReflectedClassName<WorkspaceService>() == WorkspaceService::ClassNameValue);
static_assert(ReflectedClassName<PlayersService>() == PlayersService::ClassNameValue);
static_assert(ReflectedClassName<LightingService>() == LightingService::ClassNameValue);

auto IsOpaqueServiceClass(std::string_view className) noexcept -> bool {
    return className == "CoreService" || className == "HistoryService" || className == "MathService" || className == "RunService" ||
           className == "ScriptService" || className == "SelectionService" || className == "ShaderService" || className == "UserInputService";
}

auto DefaultServiceName(std::string_view className, std::string name) -> std::string {
    if (!name.empty()) {
        return name;
    }
    if (className == "HistoryService") {
        return "History";
    }
    if (className == "SelectionService") {
        return "Selection";
    }
    return std::string(className);
}

} // namespace

Instance::Instance(const std::shared_ptr<DataModelState>& state, InstanceId id, std::string className, std::string name, bool isRoot):
    m_state(state), m_id(id), m_className(std::move(className)), m_name(name.empty() ? m_className : std::move(name)), m_isRoot(isRoot) {
}

auto Instance::IsA(std::string_view className) const noexcept -> bool {
    return className == "Object" || className == ReflectedClassName<Instance>() || className == ClassName();
}

void Instance::SetName(std::string name) {
    if (m_destroyed || m_name == name) {
        return;
    }
    m_name = std::move(name);
    NotifyChanged("Name");
}

auto Instance::GetChildren() const -> std::vector<InstancePtr> {
    std::vector<InstancePtr> result;
    result.reserve(m_children.size());
    for (const InstancePtr& child: m_children) {
        if (child && !child->IsDestroyed()) {
            result.push_back(child);
        }
    }
    return result;
}

auto Instance::GetDescendants() const -> std::vector<InstancePtr> {
    std::vector<InstancePtr>    result;
    ZHLN::Array<InstancePtr, 8> stack;
    stack.reserve(m_children.size());
    for (const auto& it: std::ranges::reverse_view(m_children)) {
        if (it && !it->IsDestroyed()) {
            stack.push_back(it);
        }
    }
    while (!stack.empty()) {
        InstancePtr current = std::move(stack.back());
        stack.pop_back();
        result.push_back(current);
        for (auto it = current->m_children.rbegin(); it != current->m_children.rend(); ++it) {
            if (*it && !(*it)->IsDestroyed()) {
                stack.push_back(*it);
            }
        }
    }
    return result;
}

auto Instance::GetAncestors() const -> std::vector<InstancePtr> {
    std::vector<InstancePtr> ancestors;
    for (InstancePtr current = m_parent.lock(); current; current = current->m_parent.lock()) {
        ancestors.push_back(current);
    }
    return ancestors;
}

auto Instance::FindFirstChild(std::string_view name, bool recursive) const -> InstancePtr {
    for (const InstancePtr& child: m_children) {
        if (child && !child->IsDestroyed() && child->Name() == name) {
            return child;
        }
    }
    if (!recursive) {
        return {};
    }
    for (const InstancePtr& child: m_children) {
        if (!child || child->IsDestroyed()) {
            continue;
        }
        if (InstancePtr found = child->FindFirstChild(name, true)) {
            return found;
        }
    }
    return {};
}

auto Instance::FindFirstChildOfClass(std::string_view className, bool recursive) const -> InstancePtr {
    for (const InstancePtr& child: m_children) {
        if (child && !child->IsDestroyed() && child->ClassName() == className) {
            return child;
        }
    }
    if (!recursive) {
        return {};
    }
    for (const InstancePtr& child: m_children) {
        if (!child || child->IsDestroyed()) {
            continue;
        }
        if (InstancePtr found = child->FindFirstChildOfClass(className, true)) {
            return found;
        }
    }
    return {};
}

auto Instance::FindFirstChildWhichIsA(std::string_view className, bool recursive) const -> InstancePtr {
    for (const InstancePtr& child: m_children) {
        if (child && !child->IsDestroyed() && child->IsA(className)) {
            return child;
        }
    }
    if (!recursive) {
        return {};
    }
    for (const InstancePtr& child: m_children) {
        if (!child || child->IsDestroyed()) {
            continue;
        }
        if (InstancePtr found = child->FindFirstChildWhichIsA(className, true)) {
            return found;
        }
    }
    return {};
}

auto Instance::IsDescendantOf(const InstancePtr& ancestor) const noexcept -> bool {
    if (!ancestor) {
        return false;
    }
    auto current = m_parent.lock();
    while (current) {
        if (current == ancestor) {
            return true;
        }
        current = current->m_parent.lock();
    }
    return false;
}

auto Instance::GetFullName() const -> std::string {
    ZHLN::Array<std::string_view, 8> names;
    ZHLN::Array<InstancePtr, 8>      keepAlive;
    const Instance*                  current = this;
    while (current != nullptr) {
        names.push_back(current->Name());
        InstancePtr parent = current->m_parent.lock();
        current            = parent.get();
        if (parent) {
            keepAlive.push_back(std::move(parent));
        }
    }
    std::string result;
    for (auto& name: std::ranges::reverse_view(names)) {
        if (!result.empty()) {
            result.push_back('.');
        }
        result.append(name);
    }
    return result;
}

auto Instance::SetParent(const InstancePtr& parent) -> std::expected<void, DataModelError> {
    if (m_destroyed || (m_destroying && parent)) {
        return std::unexpected(DataModelError::DestroyedInstance);
    }
    if (m_isRoot) {
        return std::unexpected(DataModelError::RootCannotBeParented);
    }

    const auto state = m_state.lock();
    if (!state && parent) {
        return std::unexpected(DataModelError::DataModelExpired);
    }
    if (parent) {
        const auto parentState = parent->m_state.lock();
        if (!parentState) {
            return std::unexpected(DataModelError::DataModelExpired);
        }
        if (parentState != state) {
            return std::unexpected(DataModelError::CrossDataModelParent);
        }
        for (InstancePtr ancestor = parent; ancestor; ancestor = ancestor->m_parent.lock()) {
            if (ancestor->m_destroyed || ancestor->m_destroying) {
                return std::unexpected(DataModelError::DestroyedInstance);
            }
            if (ancestor.get() == this) {
                return std::unexpected(DataModelError::ParentCycle);
            }
        }
    }

    const InstancePtr oldParent = m_parent.lock();
    if (oldParent == parent) {
        return {};
    }

    const InstancePtr           self = shared_from_this();
    ZHLN::Array<InstancePtr, 8> subtree {self};
    const auto                  descendants = GetDescendants();
    subtree.insert(subtree.end(), descendants.begin(), descendants.end());

    ZHLN::Array<InstancePtr, 8> oldAncestors;
    for (InstancePtr ancestor = oldParent; ancestor; ancestor = ancestor->m_parent.lock()) {
        oldAncestors.push_back(ancestor);
    }

    if (oldParent) {
        oldParent->RemoveChild(self);
    }
    m_parent = parent;
    if (parent) {
        parent->m_children.push_back(self);
    }

    if (oldParent) {
        oldParent->ChildRemoved.Fire(self);
    }
    for (const InstancePtr& ancestor: oldAncestors) {
        for (const InstancePtr& descendant: subtree) {
            ancestor->DescendantRemoved.Fire(descendant);
        }
    }

    if (parent) {
        parent->ChildAdded.Fire(self);
    }
    for (InstancePtr ancestor = parent; ancestor; ancestor = ancestor->m_parent.lock()) {
        for (const InstancePtr& descendant: subtree) {
            ancestor->DescendantAdded.Fire(descendant);
        }
    }

    for (const InstancePtr& moved: subtree) {
        moved->AncestryChanged.Fire(moved, moved->Parent());
    }
    NotifyChanged("Parent");
    return {};
}

void Instance::Destroy() {
    if (m_destroyed || m_destroying || m_isRoot) {
        return;
    }

    const InstancePtr              self                = shared_from_this();
    const std::vector<InstancePtr> originalDescendants = GetDescendants();
    m_destroying                                       = true;
    [[maybe_unused]] auto resetDestroying              = ZHLN::defer([this] { m_destroying = false; });
    Destroying.Fire(self);

    // Detach the subtree root while all descendant links are intact. Ancestors
    // therefore receive one complete DescendantRemoved sequence, then child
    // destruction releases the tree from the inside out.
    if (const InstancePtr parent = m_parent.lock()) {
        const auto result = SetParent({});
        if (!result) {
            return;
        }
    }

    m_destroyed = true;
    DestroyChildren();
    for (const InstancePtr& descendant: originalDescendants) {
        if (descendant && !descendant->IsDestroyed()) {
            descendant->Destroy();
        }
    }
    // The ECS adapter owns entity teardown; Instance only drops its binding.
    m_backingEntity.reset();
}

void Instance::BindEntity(Entity entity) noexcept {
    if (m_destroyed || entity == Entity::Null()) {
        m_backingEntity.reset();
    } else {
        m_backingEntity = entity;
    }
}

void Instance::NotifyChanged(std::string_view property) const noexcept {
    PropertyChanged.Fire(std::string(property));
}

auto Instance::BelongsTo(const Instance& other) const noexcept -> bool {
    const auto state = m_state.lock();
    return state && state == other.m_state.lock();
}

void Instance::RemoveChild(const InstancePtr& child) noexcept {
    for (auto* it = m_children.begin(); it != m_children.end(); ++it) {
        if (*it == child) {
            m_children.erase(it);
            return;
        }
    }
}

void Instance::DestroyChildren() {
    while (!m_children.empty()) {
        InstancePtr child = m_children.back();
        if (!child) {
            m_children.pop_back();
            continue;
        }
        child->Destroy();
        // Destroy() normally unparents the child. Keep the container invariant
        // if a lifecycle callback mutates the tree midway through teardown.
        if (!m_children.empty() && m_children.back() == child) {
            m_children.pop_back();
            child->m_parent.reset();
        }
    }
}

Service::Service(const std::shared_ptr<DataModelState>& state, InstanceId id, std::string className, std::string name):
    Instance(state, id, std::move(className), std::move(name)) {
}

auto Service::IsA(std::string_view className) const noexcept -> bool {
    if (className == "Workspace" && ClassName() == ReflectedClassName<WorkspaceService>()) {
        return true;
    }
    if (className == "Players" && ClassName() == ReflectedClassName<PlayersService>()) {
        return true;
    }
    if (className == "Lighting" && ClassName() == ReflectedClassName<LightingService>()) {
        return true;
    }
    return className == ReflectedClassName<Service>() || Instance::IsA(className);
}

WorkspaceService::WorkspaceService(const std::shared_ptr<DataModelState>& state, InstanceId id, std::string name):
    Service(state, id, std::string(ReflectedClassName<WorkspaceService>()), WithDefaultName(std::move(name), "Workspace")) {
}

auto WorkspaceService::CurrentCamera() const noexcept -> InstancePtr {
    InstancePtr camera = m_currentCamera.lock();
    return camera && !camera->IsDestroyed() ? camera : InstancePtr {};
}

auto WorkspaceService::SetCurrentCamera(const InstancePtr& camera) -> std::expected<void, DataModelError> {
    if (IsDestroyed()) {
        return std::unexpected(DataModelError::DestroyedInstance);
    }
    if (camera && (camera->IsDestroyed() || !camera->IsA("Camera") || !BelongsTo(*camera))) {
        return std::unexpected(DataModelError::InvalidReference);
    }
    if (m_currentCamera.lock() == camera) {
        return {};
    }
    m_currentCamera = camera;
    NotifyChanged("CurrentCamera");
    return {};
}

PlayersService::PlayersService(const std::shared_ptr<DataModelState>& state, InstanceId id, std::string name):
    Service(state, id, std::string(ReflectedClassName<PlayersService>()), WithDefaultName(std::move(name), "Players")) {
}

auto PlayersService::GetPlayers() const -> std::vector<std::shared_ptr<Player>> {
    std::vector<std::shared_ptr<Player>> players;
    for (const InstancePtr& child: GetChildren()) {
        if (child->ClassName() == ReflectedClassName<Player>()) {
            players.push_back(std::static_pointer_cast<Player>(child));
        }
    }
    return players;
}

auto PlayersService::FindPlayerByUserId(uint64_t userId) const -> std::shared_ptr<Player> {
    for (const auto& player: GetPlayers()) {
        if (player->UserId() == userId) {
            return player;
        }
    }
    return {};
}

auto PlayersService::LocalPlayer() const noexcept -> std::shared_ptr<Player> {
    auto player = m_localPlayer.lock();
    return player && !player->IsDestroyed() ? player : std::shared_ptr<Player> {};
}

auto PlayersService::SetLocalPlayer(const std::shared_ptr<Player>& player) -> std::expected<void, DataModelError> {
    if (IsDestroyed()) {
        return std::unexpected(DataModelError::DestroyedInstance);
    }
    const auto* const self = static_cast<const Instance*>(this);
    if (player) {
        const InstancePtr parent = player->Parent();
        if (player->IsDestroyed() || parent.get() != self || !BelongsTo(*player)) {
            return std::unexpected(DataModelError::InvalidReference);
        }
    }
    if (m_localPlayer.lock() == player) {
        return {};
    }
    m_localPlayer = player;
    NotifyChanged("LocalPlayer");
    return {};
}

LightingService::LightingService(const std::shared_ptr<DataModelState>& state, InstanceId id, std::string name):
    Service(state, id, std::string(ReflectedClassName<LightingService>()), WithDefaultName(std::move(name), "Lighting")) {
}

PhysicsService::PhysicsService(const std::shared_ptr<DataModelState>& state, InstanceId id, std::string name):
    Service(state, id, std::string(ReflectedClassName<PhysicsService>()), WithDefaultName(std::move(name), "Physics")) {
}

void PhysicsService::SetServerAuthority(bool value) {
    if (IsDestroyed() || m_serverAuthority == value) {
        return;
    }
    m_serverAuthority = value;
    NotifyChanged("ServerAuthority");
}

SoundService::SoundService(const std::shared_ptr<DataModelState>& state, InstanceId id, std::string name):
    Service(state, id, std::string(ReflectedClassName<SoundService>()), WithDefaultName(std::move(name), "SoundService")) {
}

OpaqueService::OpaqueService(const std::shared_ptr<DataModelState>& state, InstanceId id, const std::string& className, std::string name):
    Service(state, id, className, DefaultServiceName(className, std::move(name))) {
}

Folder::Folder(const std::shared_ptr<DataModelState>& state, InstanceId id, std::string name):
    Instance(state, id, std::string(ReflectedClassName<Folder>()), std::move(name)) {
}

Model::Model(const std::shared_ptr<DataModelState>& state, InstanceId id, std::string name):
    Instance(state, id, std::string(ReflectedClassName<Model>()), std::move(name)) {
}

auto Model::PrimaryPart() const noexcept -> std::shared_ptr<BasePart> {
    auto part = m_primaryPart.lock();
    if (!part || part->IsDestroyed()) {
        return {};
    }
    return part;
}

auto Model::SetPrimaryPart(const std::shared_ptr<BasePart>& part) -> std::expected<void, DataModelError> {
    if (IsDestroyed()) {
        return std::unexpected(DataModelError::DestroyedInstance);
    }
    if (part && (part->IsDestroyed() || !BelongsTo(*part) || !part->IsDescendantOf(shared_from_this()))) {
        return std::unexpected(DataModelError::InvalidReference);
    }
    if (m_primaryPart.lock() == part) {
        return {};
    }
    m_primaryPart = part;
    NotifyChanged("PrimaryPart");
    return {};
}

auto Model::IsA(std::string_view className) const noexcept -> bool {
    return className == ReflectedClassName<Model>() || className == "PVInstance" || Instance::IsA(className);
}

BasePart::BasePart(const std::shared_ptr<DataModelState>& state, InstanceId id, std::string className, std::string name):
    Instance(state, id, std::move(className), std::move(name)) {
}

auto BasePart::IsA(std::string_view className) const noexcept -> bool {
    return className == ReflectedClassName<BasePart>() || className == "PVInstance" || Instance::IsA(className);
}

auto BasePart::NetworkOwner() const noexcept -> std::shared_ptr<Player> {
    auto player = m_networkOwner.lock();
    return player && !player->IsDestroyed() ? player : std::shared_ptr<Player> {};
}

void BasePart::SetPosition(const JPH::Vec3& value) {
    if (IsDestroyed() || SameVec3(m_position, value)) {
        return;
    }
    m_position = value;
    NotifyChanged("Position");
}

void BasePart::SetRotation(const JPH::Vec3& value) {
    if (IsDestroyed() || SameVec3(m_rotationEuler, value)) {
        return;
    }
    m_rotationEuler = value;
    NotifyChanged("Rotation");
}

void BasePart::SetSize(const JPH::Vec3& value) {
    if (IsDestroyed() || SameVec3(m_size, value)) {
        return;
    }
    m_size = value;
    NotifyChanged("Size");
}

void BasePart::SetColor(const JPH::Vec3& value) {
    if (IsDestroyed() || SameVec3(m_color, value)) {
        return;
    }
    m_color = value;
    NotifyChanged("Color");
}

void BasePart::SetAnchored(bool value) {
    if (IsDestroyed() || m_anchored == value) {
        return;
    }
    m_anchored = value;
    NotifyChanged("Anchored");
}

void BasePart::SetCanCollide(bool value) {
    if (IsDestroyed() || m_canCollide == value) {
        return;
    }
    m_canCollide = value;
    NotifyChanged("CanCollide");
}

void BasePart::SetTransparency(float value) {
    if (IsDestroyed()) {
        return;
    }
    value = std::clamp(value, 0.0f, 1.0f);
    if (m_transparency == value) {
        return;
    }
    m_transparency = value;
    NotifyChanged("Transparency");
}

auto BasePart::SetNetworkOwner(const std::shared_ptr<Player>& player) -> std::expected<void, DataModelError> {
    if (IsDestroyed()) {
        return std::unexpected(DataModelError::DestroyedInstance);
    }
    if (player && (player->IsDestroyed() || !BelongsTo(*player))) {
        return std::unexpected(DataModelError::InvalidReference);
    }
    if (m_networkOwner.lock() == player) {
        return {};
    }
    m_networkOwner = player;
    NotifyChanged("NetworkOwner");
    return {};
}

Part::Part(const std::shared_ptr<DataModelState>& state, InstanceId id, std::string name):
    BasePart(state, id, std::string(ReflectedClassName<Part>()), std::move(name)) {
}

auto Part::IsA(std::string_view className) const noexcept -> bool {
    return className == ReflectedClassName<Part>() || BasePart::IsA(className);
}

void Part::SetShape(PartShape value) {
    if (IsDestroyed() || m_shape == value) {
        return;
    }
    m_shape = value;
    NotifyChanged("Shape");
}

void Part::SetFrontSurface(PartSurface value) {
    if (IsDestroyed() || m_frontSurface == value) {
        return;
    }
    m_frontSurface = value;
    NotifyChanged("FrontSurface");
}

void Part::SetBackSurface(PartSurface value) {
    if (IsDestroyed() || m_backSurface == value) {
        return;
    }
    m_backSurface = value;
    NotifyChanged("BackSurface");
}

void Part::SetTopSurface(PartSurface value) {
    if (IsDestroyed() || m_topSurface == value) {
        return;
    }
    m_topSurface = value;
    NotifyChanged("TopSurface");
}

void Part::SetBottomSurface(PartSurface value) {
    if (IsDestroyed() || m_bottomSurface == value) {
        return;
    }
    m_bottomSurface = value;
    NotifyChanged("BottomSurface");
}

void Part::SetLeftSurface(PartSurface value) {
    if (IsDestroyed() || m_leftSurface == value) {
        return;
    }
    m_leftSurface = value;
    NotifyChanged("LeftSurface");
}

void Part::SetRightSurface(PartSurface value) {
    if (IsDestroyed() || m_rightSurface == value) {
        return;
    }
    m_rightSurface = value;
    NotifyChanged("RightSurface");
}

MeshPart::MeshPart(const std::shared_ptr<DataModelState>& state, InstanceId id, std::string name):
    BasePart(state, id, std::string(ReflectedClassName<MeshPart>()), std::move(name)) {
}

auto MeshPart::IsA(std::string_view className) const noexcept -> bool {
    return className == ReflectedClassName<MeshPart>() || BasePart::IsA(className);
}

void MeshPart::SetMeshId(std::string value) {
    if (IsDestroyed() || m_meshId == value) {
        return;
    }
    m_meshId = std::move(value);
    NotifyChanged("MeshId");
}

SpawnPoint::SpawnPoint(const std::shared_ptr<DataModelState>& state, InstanceId id, std::string name):
    Instance(state, id, std::string(ReflectedClassName<SpawnPoint>()), std::move(name)) {
}

void SpawnPoint::SetPosition(const JPH::Vec3& value) {
    if (IsDestroyed() || SameVec3(m_position, value)) {
        return;
    }
    m_position = value;
    NotifyChanged("Position");
}

void SpawnPoint::SetRotation(const JPH::Vec3& value) {
    if (IsDestroyed() || SameVec3(m_rotation, value)) {
        return;
    }
    m_rotation = value;
    NotifyChanged("Rotation");
}

Decal::Decal(const std::shared_ptr<DataModelState>& state, InstanceId id, std::string name):
    Instance(state, id, std::string(ReflectedClassName<Decal>()), std::move(name)) {
}

void Decal::SetTextureId(std::string value) {
    if (IsDestroyed() || m_textureId == value) {
        return;
    }
    m_textureId = std::move(value);
    NotifyChanged("TextureId");
}

void Decal::SetColor(const JPH::Vec3& value) {
    if (IsDestroyed() || SameVec3(m_color, value)) {
        return;
    }
    m_color = value;
    NotifyChanged("Color");
}

void Decal::SetFace(DecalFace value) {
    if (IsDestroyed() || m_face == value) {
        return;
    }
    m_face = value;
    NotifyChanged("Face");
}

void Decal::SetWrapMode(DecalWrapMode value) {
    if (IsDestroyed() || m_wrapMode == value) {
        return;
    }
    m_wrapMode = value;
    NotifyChanged("WrapMode");
}

void Decal::SetScale(float value) {
    if (IsDestroyed()) {
        return;
    }
    value = std::max(0.0f, value);
    if (m_scale == value) {
        return;
    }
    m_scale = value;
    NotifyChanged("Scale");
}

void Decal::SetTransparency(float value) {
    if (IsDestroyed()) {
        return;
    }
    value = std::clamp(value, 0.0f, 1.0f);
    if (m_transparency == value) {
        return;
    }
    m_transparency = value;
    NotifyChanged("Transparency");
}

Sound::Sound(const std::shared_ptr<DataModelState>& state, InstanceId id, std::string name):
    Instance(state, id, std::string(ReflectedClassName<Sound>()), std::move(name)) {
}

void Sound::SetSoundId(std::string value) {
    if (IsDestroyed() || m_soundId == value) {
        return;
    }
    m_soundId = std::move(value);
    NotifyChanged("SoundId");
}

void Sound::SetVolume(float value) {
    if (IsDestroyed()) {
        return;
    }
    value = std::clamp(value, 0.0f, 10.0f);
    if (m_volume == value) {
        return;
    }
    m_volume = value;
    NotifyChanged("Volume");
}

void Sound::SetLoops(bool value) {
    if (IsDestroyed() || m_loops == value) {
        return;
    }
    m_loops = value;
    NotifyChanged("Loops");
}

void Sound::SetPlaying(bool value) {
    if (IsDestroyed() || m_playing == value) {
        return;
    }
    m_playing = value;
    NotifyChanged("Playing");
}

void Sound::Play() {
    SetPlaying(true);
}

void Sound::Stop() {
    SetPlaying(false);
}

Motor::Motor(const std::shared_ptr<DataModelState>& state, InstanceId id, std::string name):
    Instance(state, id, std::string(ReflectedClassName<Motor>()), std::move(name)) {
}

auto Motor::IsA(std::string_view className) const noexcept -> bool {
    return className == ReflectedClassName<Motor>() || className == "Weld" || className == "JointInstance" || Instance::IsA(className);
}

auto Motor::Part1() const noexcept -> std::shared_ptr<BasePart> {
    auto part = m_part1.lock();
    return part && !part->IsDestroyed() ? part : std::shared_ptr<BasePart> {};
}

auto Motor::SetPart1(const std::shared_ptr<BasePart>& part) -> std::expected<void, DataModelError> {
    if (IsDestroyed()) {
        return std::unexpected(DataModelError::DestroyedInstance);
    }
    if (part && (part->IsDestroyed() || !BelongsTo(*part))) {
        return std::unexpected(DataModelError::InvalidReference);
    }
    if (m_part1.lock() == part) {
        return {};
    }
    m_part1 = part;
    NotifyChanged("Part1");
    return {};
}

auto Motor::Part2() const noexcept -> std::shared_ptr<BasePart> {
    auto part = m_part2.lock();
    return part && !part->IsDestroyed() ? part : std::shared_ptr<BasePart> {};
}

auto Motor::SetPart2(const std::shared_ptr<BasePart>& part) -> std::expected<void, DataModelError> {
    if (IsDestroyed()) {
        return std::unexpected(DataModelError::DestroyedInstance);
    }
    if (part && (part->IsDestroyed() || !BelongsTo(*part))) {
        return std::unexpected(DataModelError::InvalidReference);
    }
    if (m_part2.lock() == part) {
        return {};
    }
    m_part2 = part;
    NotifyChanged("Part2");
    return {};
}

void Motor::SetOffset1(const JPH::Vec3& value) {
    if (IsDestroyed() || SameVec3(m_offset1, value)) {
        return;
    }
    m_offset1 = value;
    NotifyChanged("Offset1");
}

void Motor::SetOffset2(const JPH::Vec3& value) {
    if (IsDestroyed() || SameVec3(m_offset2, value)) {
        return;
    }
    m_offset2 = value;
    NotifyChanged("Offset2");
}

void Motor::SetCurrentAngle(float value) {
    if (IsDestroyed() || m_currentAngle == value) {
        return;
    }
    m_currentAngle = value;
    NotifyChanged("CurrentAngle");
}

void Motor::SetDesiredAngle(float value) {
    if (IsDestroyed() || m_desiredAngle == value) {
        return;
    }
    m_desiredAngle = value;
    NotifyChanged("DesiredAngle");
}

void Motor::SetMaxVelocity(float value) {
    if (IsDestroyed() || m_maxVelocity == value) {
        return;
    }
    m_maxVelocity = value;
    NotifyChanged("MaxVelocity");
}

Humanoid::Humanoid(const std::shared_ptr<DataModelState>& state, InstanceId id, std::string name):
    Instance(state, id, std::string(ReflectedClassName<Humanoid>()), std::move(name)) {
}

auto Humanoid::RootPart() const noexcept -> std::shared_ptr<BasePart> {
    auto part = m_rootPart.lock();
    return part && !part->IsDestroyed() ? part : std::shared_ptr<BasePart> {};
}

void Humanoid::SetWalkSpeed(float value) {
    if (IsDestroyed() || m_walkSpeed == value) {
        return;
    }
    m_walkSpeed = value;
    NotifyChanged("WalkSpeed");
}

void Humanoid::SetJumpPower(float value) {
    if (IsDestroyed() || m_jumpPower == value) {
        return;
    }
    m_jumpPower = value;
    NotifyChanged("JumpPower");
}

void Humanoid::SetHealth(float value) {
    if (IsDestroyed() || m_health == value) {
        return;
    }
    m_health = value;
    NotifyChanged("Health");
}

void Humanoid::SetMaxHealth(float value) {
    if (IsDestroyed() || m_maxHealth == value) {
        return;
    }
    m_maxHealth = value;
    NotifyChanged("MaxHealth");
}

void Humanoid::SetState(HumanoidState value) {
    if (IsDestroyed() || m_state == value) {
        return;
    }
    const HumanoidState oldState = m_state;
    m_state                      = value;
    NotifyChanged("State");
    StateChanged.Fire(oldState, value);
}

auto Humanoid::SetRootPart(const std::shared_ptr<BasePart>& part) -> std::expected<void, DataModelError> {
    if (IsDestroyed()) {
        return std::unexpected(DataModelError::DestroyedInstance);
    }
    if (part && (part->IsDestroyed() || !BelongsTo(*part))) {
        return std::unexpected(DataModelError::InvalidReference);
    }
    if (m_rootPart.lock() == part) {
        return {};
    }
    m_rootPart = part;
    NotifyChanged("RootPart");
    return {};
}

auto Humanoid::IsA(std::string_view className) const noexcept -> bool {
    return className == ReflectedClassName<Humanoid>() || Instance::IsA(className);
}

Player::Player(const std::shared_ptr<DataModelState>& state, InstanceId id, std::string name):
    Instance(state, id, std::string(ReflectedClassName<Player>()), std::move(name)) {
}

auto Player::Character() const noexcept -> std::shared_ptr<Model> {
    auto character = m_character.lock();
    return character && !character->IsDestroyed() ? character : std::shared_ptr<Model> {};
}

void Player::SetUserId(uint64_t value) {
    if (IsDestroyed() || m_userId == value) {
        return;
    }
    m_userId = value;
    NotifyChanged("UserId");
}

auto Player::SetCharacter(const std::shared_ptr<Model>& character) -> std::expected<void, DataModelError> {
    if (IsDestroyed()) {
        return std::unexpected(DataModelError::DestroyedInstance);
    }
    if (character && (character->IsDestroyed() || !BelongsTo(*character))) {
        return std::unexpected(DataModelError::InvalidReference);
    }
    if (m_character.lock() == character) {
        return {};
    }
    m_character = character;
    NotifyChanged("Character");
    return {};
}

auto Player::IsA(std::string_view className) const noexcept -> bool {
    return className == ReflectedClassName<Player>() || Instance::IsA(className);
}

OpaqueInstance::OpaqueInstance(const std::shared_ptr<DataModelState>& state, InstanceId id, std::string className, std::string name):
    Instance(state, id, std::move(className), std::move(name)) {
}

DataModel::DataModel(): m_state(std::make_shared<DataModelState>()) {
    // ID zero is reserved for the local DataModel root until a server snapshot
    // supplies its stable UID; ordinary Instances use nonzero protocol IDs.
    m_root = InstancePtr(new Instance(m_state, 0, "DataModel", "game", true));
}

DataModel::~DataModel() {
    Clear();
    m_root.reset();
    m_state.reset();
}

auto DataModel::AllocateId() noexcept -> std::expected<InstanceId, DataModelError> {
    if (!m_state || m_state->nextId == 0) {
        return std::unexpected(DataModelError::IdExhausted);
    }
    while (m_state->instances.Find(m_state->nextId)) {
        if (m_state->nextId == std::numeric_limits<InstanceId>::max()) {
            return std::unexpected(DataModelError::IdExhausted);
        }
        ++m_state->nextId;
    }
    const InstanceId id = m_state->nextId;
    if (m_state->nextId == std::numeric_limits<InstanceId>::max()) {
        m_state->nextId = 0;
    } else {
        ++m_state->nextId;
    }
    return id;
}

auto DataModel::HasEverUsedId(InstanceId id) const noexcept -> bool {
    return m_state && static_cast<bool>(m_state->instances.Find(id));
}

void DataModel::RegisterInstance(const InstancePtr& instance) {
    if (!instance || instance->Id() == 0) {
        return;
    }
    m_state->instances.Insert(instance->Id(), std::weak_ptr<Instance> {instance});
    if (instance->Id() >= m_state->nextId) {
        m_state->nextId = instance->Id() == std::numeric_limits<InstanceId>::max() ? 0 : instance->Id() + 1;
    }
}

auto DataModel::FindById(InstanceId id) const noexcept -> InstancePtr {
    if (!m_state) {
        return {};
    }
    if (m_root && m_root->Id() == id) {
        return m_root;
    }
    const auto found = m_state->instances.Find(id);
    if (!found) {
        return {};
    }
    InstancePtr instance = (*found).lock();
    if (!instance || instance->IsDestroyed()) {
        return {};
    }
    return instance;
}

auto DataModel::GetService(std::string_view nameOrClass) const -> std::shared_ptr<Service> {
    for (const InstancePtr& child: m_root->GetChildren()) {
        if (!child->IsService()) {
            continue;
        }
        const auto service = std::static_pointer_cast<Service>(child);
        if (service->Name() == nameOrClass || service->ClassName() == nameOrClass) {
            return service;
        }
        const auto serviceClass = service->ClassName();
        if ((nameOrClass == "Workspace" && serviceClass == ReflectedClassName<WorkspaceService>()) ||
            (nameOrClass == "Players" && serviceClass == ReflectedClassName<PlayersService>()) ||
            (nameOrClass == "Lighting" && serviceClass == ReflectedClassName<LightingService>())) {
            return service;
        }
    }
    return {};
}

auto DataModel::CreateInstance(std::string_view className, std::string name) -> std::expected<InstancePtr, DataModelError> {
    auto id = AllocateId();
    if (!id) {
        return std::unexpected(id.error());
    }
    return CreateInstanceWithId(*id, className, std::move(name));
}

auto DataModel::CreateInstanceWithId(InstanceId id, std::string_view className, std::string name) -> std::expected<InstancePtr, DataModelError> {
    if (id == 0) {
        return std::unexpected(DataModelError::InvalidId);
    }

    if (className == "DataModel") {
        if (m_root->Id() == id) {
            if (!name.empty()) {
                m_root->SetName(std::move(name));
            }
            return m_root;
        }
        if (m_root->Id() != 0 || HasEverUsedId(id)) {
            return std::unexpected(DataModelError::DuplicateId);
        }
        m_root->m_id = id;
        if (!name.empty()) {
            m_root->SetName(std::move(name));
        }
        RegisterInstance(m_root);
        return m_root;
    }

    auto asInstance = [this, id, &name]<typename T>() -> std::expected<InstancePtr, DataModelError> {
        auto typed = CreateWithId<T>(id, name);
        if (!typed) {
            return std::unexpected(typed.error());
        }
        return std::static_pointer_cast<Instance>(*typed);
    };

    if (className == ReflectedClassName<WorkspaceService>() || className == "Workspace") {
        return asInstance.template operator()<WorkspaceService>();
    }
    if (className == ReflectedClassName<PlayersService>() || className == "Players") {
        return asInstance.template operator()<PlayersService>();
    }
    if (className == ReflectedClassName<LightingService>() || className == "Lighting") {
        return asInstance.template operator()<LightingService>();
    }
    if (className == ReflectedClassName<PhysicsService>()) {
        return asInstance.template operator()<PhysicsService>();
    }
    if (className == ReflectedClassName<SoundService>()) {
        return asInstance.template operator()<SoundService>();
    }
    if (IsOpaqueServiceClass(className)) {
        if (HasEverUsedId(id)) {
            return std::unexpected(DataModelError::DuplicateId);
        }
        auto service = InstancePtr(new OpaqueService(m_state, id, std::string(className), std::move(name)));
        RegisterInstance(service);
        return service;
    }
    if (className == ReflectedClassName<Folder>()) {
        return asInstance.template operator()<Folder>();
    }
    if (className == ReflectedClassName<Model>()) {
        return asInstance.template operator()<Model>();
    }
    if (className == ReflectedClassName<BasePart>()) {
        return asInstance.template operator()<BasePart>();
    }
    if (className == ReflectedClassName<Part>()) {
        return asInstance.template operator()<Part>();
    }
    if (className == ReflectedClassName<MeshPart>()) {
        return asInstance.template operator()<MeshPart>();
    }
    if (className == ReflectedClassName<SpawnPoint>()) {
        return asInstance.template operator()<SpawnPoint>();
    }
    if (className == ReflectedClassName<Decal>()) {
        return asInstance.template operator()<Decal>();
    }
    if (className == ReflectedClassName<Sound>()) {
        return asInstance.template operator()<Sound>();
    }
    if (className == ReflectedClassName<Motor>()) {
        return asInstance.template operator()<Motor>();
    }
    if (className == ReflectedClassName<Humanoid>()) {
        return asInstance.template operator()<Humanoid>();
    }
    if (className == ReflectedClassName<Player>()) {
        return asInstance.template operator()<Player>();
    }

    if (HasEverUsedId(id)) {
        return std::unexpected(DataModelError::DuplicateId);
    }
    auto instance = InstancePtr(new OpaqueInstance(m_state, id, std::string(className), std::move(name)));
    RegisterInstance(instance);
    return instance;
}

void DataModel::Clear() {
    if (!m_root) {
        return;
    }
    const auto children = m_root->GetChildren();
    for (const InstancePtr& child: children) {
        if (child && !child->IsDestroyed()) {
            child->Destroy();
        }
    }
}

} // namespace ZHLN::ProjectLight
