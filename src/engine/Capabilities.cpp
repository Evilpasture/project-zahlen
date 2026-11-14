// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// One translation unit owns the heavy includes. The capability headers stay
// small: they name value types and operations, not renderer, Jolt or audio
// implementation headers.

#include <Zahlen/Audio/Audio.hpp>
#include <Zahlen/Math/Vec3.hpp>
#include <Zahlen/Math/Vec4.hpp>
#include <Zahlen/Math/Matrix.hpp>
#include <Zahlen/Engine/AssetManager.hpp>
#include <Zahlen/Engine/Capabilities/AssetLibrary.hpp>
#include <Zahlen/Engine/Capabilities/AudioEngine.hpp>
#include <Zahlen/Engine/Capabilities/DebugDraw.hpp>
#include <Zahlen/Engine/Capabilities/EngineState.hpp>
#include <Zahlen/Engine/Capabilities/Entities.hpp>
#include <Zahlen/ECS/Inspection.hpp>
#include <Zahlen/Engine/Capabilities/GraphicsDevice.hpp>
#include <Zahlen/Engine/Capabilities/InputActions.hpp>
#include <Zahlen/Engine/Capabilities/InputHaptics.hpp>
#include <Zahlen/Engine/Capabilities/InputReader.hpp>
#include <Zahlen/Engine/Capabilities/Simulation.hpp>
#include <Zahlen/Engine/Engine.hpp>
#include <Zahlen/Engine/Kernel.hpp>
#include <Zahlen/Engine/World.hpp>
#include <Zahlen/GUI/FontLoader.hpp>
#include "InputContext.hpp"
#include <Zahlen/Physics/Physics.hpp>
#include <Zahlen/Physics/MathConversions.hpp>
#include <Zahlen/Render/RenderContext.hpp>
#include <Zahlen/Render/SceneData.hpp>

namespace ZHLN {

// --- Entities -----------------------------------------------------------------

auto Entities::Create() -> Entity {
    return Registry().Create();
}

void Entities::Despawn(Entity entity) noexcept {
    DespawnEntity(*_engine, entity);
}

auto Entities::IsAlive(Entity entity) const noexcept -> bool {
    return Registry().IsAlive(entity);
}

auto Entities::Inspect() const noexcept -> ECS::Inspection { return ECS::Inspection {Registry()}; }

auto Entities::Registry() const noexcept -> ECS::Registry& {
    return _engine->GetInternalRegistry();
}

// --- GraphicsDevice -----------------------------------------------------------

void GraphicsDevice::SetResolution(Extent2D resolution) {
    _ctx->SetResolution(resolution);
}
void GraphicsDevice::SetViewport(const ViewportRect& rect) noexcept {
    _ctx->SetViewport(rect);
}
auto GraphicsDevice::Viewport() const noexcept -> ViewportRect {
    return _ctx->GetViewport();
}
auto GraphicsDevice::ViewportAspect() const noexcept -> float {
    return _ctx->GetViewportAspect();
}
auto GraphicsDevice::Info() const noexcept -> RenderInfo {
    return _ctx->GetInfo();
}
auto GraphicsDevice::FrameIndex() const noexcept -> uint32_t {
    return _ctx->GetFrameIndex();
}
auto GraphicsDevice::FramebufferSize() const -> std::optional<Extent2D> {
    return _ctx->GetFramebufferSize();
}
auto GraphicsDevice::PacedDeltaTime() const noexcept -> std::optional<float> {
    return _ctx->GetPacedDeltaTime();
}
auto GraphicsDevice::PresentTiming() const noexcept -> PresentTimingMetrics {
    return _ctx->GetPresentTiming();
}
auto GraphicsDevice::CaptureFrame() noexcept -> std::expected<CapturedFrame, ErrorCode> {
    return _ctx->CaptureFrame();
}
void GraphicsDevice::ApplySettings(const GraphicsSettings& settings) noexcept {
    _ctx->ApplySettings(settings);
}
auto GraphicsDevice::Settings() const noexcept -> const GraphicsSettings& {
    return _ctx->GetSettings();
}
void GraphicsDevice::SetAntiAliasing(AAState state) {
    _ctx->SetAAState(state);
}
void GraphicsDevice::ClearCaches() noexcept {
    _ctx->ClearGPUCaches();
}
auto GraphicsDevice::SetEnvironmentRadiance(const EnvironmentRadianceDesc& desc) noexcept -> std::expected<void, ErrorCode> {
    return _ctx->SetEnvironmentRadiance(desc);
}
auto GraphicsDevice::CreateMaterial(const MaterialDesc& desc) -> std::expected<ZHLN::Material, ErrorCode> {
    return _ctx->CreateMaterial(desc);
}
auto GraphicsDevice::CreateBuffer(BufferDesc desc) -> BufferID {
    return _ctx->CreateBuffer(desc);
}
namespace {
[[nodiscard]] auto BufferFromBytes(RenderContext& ctx, BufferUsage usage, std::span<const std::byte> bytes) -> BufferID {
    return ctx.CreateBuffer(BufferDesc {.usage = usage, .data = bytes, .stride = 1});
}
} // namespace
auto GraphicsDevice::CreateVertexBuffer(std::span<const std::byte> bytes) -> BufferID {
    return BufferFromBytes(*_ctx, BufferUsage::Vertex, bytes);
}
auto GraphicsDevice::CreateIndexBuffer(std::span<const std::byte> bytes) -> BufferID {
    return BufferFromBytes(*_ctx, BufferUsage::Index, bytes);
}
auto GraphicsDevice::CreateStorageBuffer(std::span<const std::byte> bytes) -> BufferID {
    return BufferFromBytes(*_ctx, BufferUsage::Storage, bytes);
}
auto GraphicsDevice::CreateStorageBufferElements(size_t count) -> BufferID {
    return _ctx->CreateBuffer<BufferUsage::Storage>(count);
}
auto GraphicsDevice::CreateSkinScratchBuffer(size_t count) -> BufferID {
    return _ctx->CreateBuffer<BufferUsage::SkinScratch>(count);
}
auto GraphicsDevice::CreateMeshletBuffer(std::span<const MeshletDesc> meshlets) -> BufferID {
    return _ctx->CreateMeshletBuffer(meshlets);
}
auto GraphicsDevice::UploadMeshlets(const MeshletBuildResult& built) -> MeshletStreams {
    return _ctx->UploadMeshlets(built);
}
void GraphicsDevice::DestroyBuffer(BufferID handle) {
    _ctx->DestroyBuffer(handle);
}
auto GraphicsDevice::UpdateBuffer(BufferID handle, std::span<const std::byte> bytes) noexcept -> std::expected<void, ErrorCode> {
    return _ctx->UpdateBuffer(handle, bytes);
}
auto GraphicsDevice::CreateTexture(std::span<const std::byte> rgba, Extent2D extent, bool isSRGB) -> std::expected<TextureID, ErrorCode> {
    return _ctx->CreateTexture(rgba, extent, isSRGB);
}
auto GraphicsDevice::CreateTexture(std::string_view name, std::span<const std::byte> rgba, Extent2D extent, bool isSRGB, bool generateMips)
    -> std::expected<TextureID, ErrorCode> {
    return _ctx->CreateTexture(name, rgba, extent, isSRGB, generateMips);
}
auto GraphicsDevice::CreateTextureCube(std::array<std::span<const std::byte>, 6> faces, uint32_t faceSize) -> std::expected<TextureID, ErrorCode> {
    return _ctx->CreateTextureCube(faces, faceSize);
}
auto GraphicsDevice::CreateTextureHDR(std::string_view name, std::span<const std::byte> rgba16f, Extent2D extent, bool generateMips)
    -> std::expected<TextureID, ErrorCode> {
    return _ctx->CreateTextureHDR(name, rgba16f, extent, generateMips);
}
auto GraphicsDevice::BakeProceduralTexture(uint32_t width, uint32_t height, uint32_t variantIdx, float scale, float randomness)
    -> std::expected<TextureID, ErrorCode> {
    return _ctx->BakeProceduralTexture(width, height, variantIdx, scale, randomness);
}
auto GraphicsDevice::CreateProceduralTexture(std::string_view name, Extent2D extent, std::span<const uint32_t> pixels, bool isSRGB, bool generateMips)
    -> TextureID {
    return _ctx->CreateProceduralTexture(name, extent, pixels, isSRGB, generateMips);
}
void GraphicsDevice::UnloadTexture(TextureID handle) {
    _ctx->UnloadTexture(handle);
}
auto GraphicsDevice::CreateRenderTexture(uint32_t width, uint32_t height, bool hdr) -> std::expected<TextureID, ErrorCode> {
    return _ctx->CreateRenderTexture(width, height, hdr);
}
void GraphicsDevice::DestroyRenderTexture(TextureID handle) noexcept {
    _ctx->DestroyRenderTexture(handle);
}
auto GraphicsDevice::Mesh(AssetID id) const noexcept -> std::optional<ZHLN::Mesh> {
    return _ctx->GetGPUMesh(id);
}
auto GraphicsDevice::Material(MaterialID id) const noexcept -> std::optional<ZHLN::Material> {
    return _ctx->GetGPUMaterial(id);
}
void GraphicsDevice::RegisterMesh(AssetID id, ZHLN::Mesh mesh) noexcept {
    _ctx->RegisterGPUMesh(id, mesh);
}
void GraphicsDevice::UnregisterMesh(AssetID id) noexcept {
    _ctx->UnregisterGPUMesh(id);
}
void GraphicsDevice::RegisterMaterial(MaterialID id, ZHLN::Material mat) noexcept {
    _ctx->RegisterGPUMaterial(id, mat);
}
void GraphicsDevice::UnregisterMaterial(MaterialID id) noexcept {
    _ctx->UnregisterGPUMaterial(id);
}
void GraphicsDevice::DestroyMesh(const ZHLN::Mesh& mesh) noexcept {
    _ctx->DestroyMesh(mesh);
}
auto GraphicsDevice::BuildMeshBLAS(ZHLN::Mesh& mesh) noexcept -> RenderResult {
    return _ctx->BuildMeshBLAS(mesh);
}
void GraphicsDevice::UpdateJointMatrices(uint32_t offset, std::span<const ZHLN::Mat4x4> matrices) {
    _ctx->UpdateJointMatrices(offset, matrices);
}
auto GraphicsDevice::AllocateMorphDeltas(std::span<const float> deltas) -> uint32_t {
    return _ctx->AllocateMorphDeltas(deltas);
}
auto GraphicsDevice::BeginFrame() noexcept -> FrameOutcome<FrameScope> {
    return _ctx->BeginFrame();
}
void GraphicsDevice::Draw(const ZHLN::Material& material, const ZHLN::Mesh& mesh, const DrawParams& params) noexcept {
    _ctx->Draw(material, mesh, params);
}
void GraphicsDevice::DrawCSG(const ZHLN::Material& material, const ZHLN::Mesh& mesh, const CSGDrawParams& params) noexcept {
    _ctx->DrawCSG(material, mesh, params);
}

// --- Simulation ---------------------------------------------------------------

auto Simulation::Raycast(DVec3 origin, FVec3 direction, float maxDistance, Physics::BodyID ignore) const -> Physics::RaycastResult {
    return _ctx->Raycast(origin, direction, maxDistance, ignore);
}
auto Simulation::BodiesInSphere(DVec3 center, float radius) const -> std::vector<Physics::BodyID> {
    return _ctx->OverlapSphere(center, radius);
}
auto Simulation::DebugDrawData(bool shapes, bool constraints, bool wireframe) const -> Physics::DebugDrawData {
    return _ctx->GetDebugDrawData(shapes, constraints, wireframe);
}
auto Simulation::BodyPosition(Physics::BodyID handle) const noexcept -> std::optional<DVec3> {
    return _ctx->BodyPosition(handle);
}
auto Simulation::BodyState(Physics::BodyID handle) const noexcept -> std::optional<Physics::BodyStateSnapshot> {
    return _ctx->BodyState(handle);
}
auto Simulation::BodyLinearVelocity(Physics::BodyID handle) const noexcept -> std::optional<FVec3> {
    return _ctx->BodyLinearVelocity(handle);
}
void Simulation::ClearMeshShapeCache() noexcept {
    _ctx->ClearMeshShapeCache();
}
auto Simulation::CreateRigidBody(
    const Physics::AnyShape& shape, DVec3 position, FVec4 rotation, Physics::MotionType motion, Physics::ObjectLayer layer,
    uint32_t materialID, uint32_t category, uint32_t mask, Physics::Activation activation, FVec3 scale
) -> Physics::BodyID {
    return _ctx->CreateRigidBody(shape, position, rotation, motion, layer, materialID, category, mask, activation, scale);
}
auto Simulation::CharacterVelocity(Physics::BodyID handle) const -> FVec3 {
    return Physics::FromJoltVector(_ctx->GetCharacterVelocity(handle));
}
auto Simulation::IsCharacterOnGround(Physics::BodyID handle) const -> bool {
    return _ctx->IsCharacterOnGround(handle);
}
auto Simulation::IsBodyDynamic(Physics::BodyID handle) const -> bool {
    return _ctx->IsBodyDynamic(handle);
}
void Simulation::AddImpulse(Physics::BodyID handle, FVec3 impulse) {
    _ctx->AddImpulse(handle, impulse);
}
void Simulation::SetLinearVelocity(Physics::BodyID handle, FVec3 velocity) {
    _ctx->SetLinearVelocity(handle, velocity);
}
void Simulation::SetKinematicPose(Physics::BodyID handle, DVec3 position, FVec4 rotation) {
    _ctx->SetKinematicPose(handle, position, rotation);
}
void Simulation::SetCollisionFilter(Physics::BodyID handle, uint32_t category, uint32_t mask) {
    _ctx->SetCollisionFilter(handle, category, mask);
}
void Simulation::RegisterMaterial(uint32_t id, float friction, float restitution) {
    _ctx->RegisterMaterial(id, friction, restitution);
}
void Simulation::DestroyBody(Physics::BodyID handle) {
    _ctx->DestroyBody(handle);
}
void Simulation::DestroyBodies(std::span<const Physics::BodyID> handles) {
    _ctx->DestroyBodies(handles);
}
void Simulation::SetCharacterVelocity(Physics::BodyID handle, FVec3 velocity) {
    _ctx->SetCharacterVelocity(handle, velocity);
}
void Simulation::Step(float deltaTime) {
    _ctx->Step(deltaTime);
}
auto Simulation::CreateCharacter(DVec3 position) -> Physics::BodyID {
    return _ctx->CreateCharacter(position);
}
auto Simulation::CreateCharacter(DVec3 position, const Physics::CharacterParams& params) -> Physics::BodyID {
    return _ctx->CreateCharacter(position, params);
}
auto Simulation::CreateCustomCharacter(const Physics::CharacterCreateDesc& desc) -> Physics::BodyID {
    Physics::CharacterParams params {};
    if (desc.useCustomCapsule && desc.capsuleRadius > 0.0F && desc.capsuleHalfHeight > 0.0F) {
        params.shape = _ctx->GetOrCreateShape(Physics::ShapeType::Capsule, desc.capsuleRadius, desc.capsuleHalfHeight);
    }
    params.maxSlopeAngle            = desc.maxSlopeRadians;
    params.maxStrength              = desc.maxStrength;
    params.characterPadding         = desc.characterPadding;
    params.penetrationRecoverySpeed = desc.penetrationRecoverySpeed;
    params.category                 = desc.category;
    params.mask                     = desc.mask;
    return _ctx->CreateCharacter(desc.position, params);
}
void Simulation::DestroyRagdolls(std::span<const Physics::RagdollID> handles) noexcept {
    _ctx->DestroyRagdolls(handles);
}

// --- DebugDraw ----------------------------------------------------------------

void DebugDraw::AddLine(const FVec3& start, const FVec3& end, const FVec4& color) {
    _scene->AddLine(start, end, color);
}
void DebugDraw::AddLine(const FVec3& start, const FVec3& end, const FVec4& colorStart, const FVec4& colorEnd) {
    _scene->AddLine(start, end, colorStart, colorEnd);
}
auto DebugDraw::Lines() const noexcept -> std::span<const LineSegment> {
    return std::span<const LineSegment> {_scene->lines.data(), _scene->lines.size()};
}
void DebugDraw::AddBillboards(TextureID texture, std::span<const BillboardQuad> quads, bool additive) {
    _scene->AddBillboards(texture, quads, additive);
}
void DebugDraw::AddDebugTriangles(std::span<const VertexPosition> positions, std::span<const VertexSurface> surfaces) {
    _scene->AddDebugTriangles(positions, surfaces);
}
void DebugDraw::Clear() noexcept {
    _scene->Clear();
}
void DebugDraw::SetFrameDelta(float dt) noexcept {
    _scene->dt = dt;
}
void DebugDraw::SubmitParticles(const ParticleEmitterSubmission& submission) {
    _scene->particleEmitters.push_back(submission);
}
void DebugDraw::SubmitMeshParticles(const MeshParticleEmitterSubmission& submission) {
    _scene->meshParticleEmitters.push_back(submission);
}

// --- AssetLibrary -------------------------------------------------------------

auto AssetLibrary::CachedPrefab(uint64_t hash) noexcept -> ZHLN::Optional<ModelPrefab&> {
    return _assets->GetCachedPrefab(hash);
}
auto AssetLibrary::CachedPrefab(uint64_t hash) const noexcept -> ZHLN::Optional<const ModelPrefab&> {
    return _assets->GetCachedPrefab(hash);
}

auto AssetLibrary::WithCachedPrefabImpl(uint64_t hash, void (*visit)(const void*, const ModelPrefab&), const void* ctx) const -> bool {
    return _assets->WithCachedPrefab(hash, [&](const ModelPrefab& prefab) { visit(ctx, prefab); });
}
auto AssetLibrary::BorrowCachedPrefab(uint64_t hash) const noexcept -> const ModelPrefab* {
    return _assets->BorrowCachedPrefab(hash);
}
void AssetLibrary::CachePrefab(uint64_t hash, ModelPrefab* prefab) {
    _assets->CachePrefab(hash, prefab);
}
void AssetLibrary::CachePrefab(uint64_t hash, std::unique_ptr<ModelPrefab> prefab) {
    _assets->CachePrefab(hash, std::move(prefab));
}
auto AssetLibrary::CachedPrefabs(ModelPrefab** outPrefabs, uint32_t maxCount) -> uint32_t {
    return _assets->GetCachedPrefabs(outPrefabs, maxCount);
}
void AssetLibrary::UseGraphicsDevice(GraphicsDevice graphics) noexcept {
    _assets->UseRenderContext(*graphics._ctx);
}
void AssetLibrary::ClearCache() noexcept {
    _assets->ClearCache();
}
void AssetLibrary::CacheFont(uint64_t hash, std::unique_ptr<GUI::BakedFontAsset> font) {
    _assets->CacheFont(hash, std::move(font));
}
auto AssetLibrary::ReadVirtualFile(std::string_view path) const -> std::optional<std::vector<uint8_t>> {
    AssetLoadRequest req;
    req.assetID = HashAssetPath(path);
    if (!_assets->LoadSync(req) || (req.outData == nullptr) || (req.outSize == 0)) {
        return std::nullopt;
    }
    const auto*          bytes = static_cast<const uint8_t*>(req.outData);
    std::vector<uint8_t> out(bytes, bytes + req.outSize);
    _assets->FreeMemory(req);
    return out;
}
auto AssetLibrary::VFS() noexcept -> FS::VirtualFileSystem& {
    return _assets->VFS();
}
auto AssetLibrary::VFS() const noexcept -> const FS::VirtualFileSystem& {
    return _assets->VFS();
}
auto AssetLibrary::CacheCollisionMesh(CollisionAssetID id, Physics::MeshGeometry geometry) -> bool {
    return _assets->CacheCollisionMesh(id, std::move(geometry));
}
auto AssetLibrary::FindCollisionMesh(CollisionAssetID id) const noexcept -> std::shared_ptr<const Physics::MeshGeometry> {
    return _assets->FindCollisionMesh(id);
}
auto AssetLibrary::CacheEnvironmentImage(std::string_view key, EnvironmentImage image) -> bool {
    return _assets->CacheEnvironmentImage(key, std::move(image));
}
auto AssetLibrary::FindEnvironmentImage(std::string_view key) const noexcept -> std::optional<EnvironmentImageView> {
    return _assets->FindEnvironmentImage(key);
}
void AssetLibrary::SetRemoteFetcher(Remote::AsyncAssetFetcher* fetcher) noexcept {
    _assets->SetRemoteFetcher(fetcher);
}

// --- AudioEngine --------------------------------------------------------------

auto AudioEngine::Initialized() const noexcept -> bool {
    return _ctx->IsInitialized();
}
void AudioEngine::PostEvent(const AudioEvent& event) noexcept {
    _ctx->PostEvent(event);
}
auto AudioEngine::CreateVoice(std::string_view filepath, bool spatialized, bool looping, float volume) -> VoiceID {
    return _ctx->CreateVoice(filepath, spatialized, looping, volume);
}
auto AudioEngine::CreateVoice(std::string_view filepath, const VoiceDesc& desc) -> VoiceID {
    return _ctx->CreateVoice(filepath, desc);
}
void AudioEngine::PlayVoice(VoiceID handle) {
    _ctx->PlayVoice(handle);
}
void AudioEngine::StopVoice(VoiceID handle, float fadeOutSeconds) {
    _ctx->StopVoice(handle, fadeOutSeconds);
}
auto AudioEngine::IsVoiceValid(VoiceID handle) const noexcept -> bool {
    return _ctx->IsVoiceValid(handle);
}
void AudioEngine::SetVoiceVolume(VoiceID handle, float volume) {
    _ctx->SetVoiceVolume(handle, volume);
}
void AudioEngine::SetVoiceLooping(VoiceID handle, bool looping) {
    _ctx->SetVoiceLooping(handle, looping);
}
void AudioEngine::SetVoicePosition(VoiceID handle, const FVec3& position) {
    _ctx->SetVoicePosition(handle, position);
}

// --- InputReader --------------------------------------------------------------
//
// Every query here answers with a value. The router's InputState is read
// through the seat accessor and immediately projected: no caller ever holds the
// reference this file held for the length of one expression.
//
// These read raw physical state. UI capture is deliberately *not* applied here:
// it is a policy the router applies when it evaluates action maps, and a scene
// that wants to honour it can ask UiCapture() and gate itself. Applying it here
// as well would mean a held key silently stops reporting as held the moment the
// GUI takes focus, which is not what "physical state" means.

auto InputReader::Action(ActionID id, PlayerSeat seat) const noexcept -> ActionState {
    return _context->Action(id, seat);
}
auto InputReader::ActionDown(ActionID id, PlayerSeat seat) const noexcept -> bool {
    return _context->ActionDown(id, seat);
}
auto InputReader::ActionPressed(ActionID id, PlayerSeat seat) const noexcept -> bool {
    return _context->ActionPressed(id, seat);
}
auto InputReader::ActionHeld(ActionID id, PlayerSeat seat) const noexcept -> bool {
    return _context->ActionHeld(id, seat);
}
auto InputReader::ActionReleased(ActionID id, PlayerSeat seat) const noexcept -> bool {
    return _context->ActionReleased(id, seat);
}
auto InputReader::ActionAxis1D(ActionID id, PlayerSeat seat) const noexcept -> float {
    return _context->ActionAxis1D(id, seat);
}
auto InputReader::ActionAxis2D(ActionID id, PlayerSeat seat) const noexcept -> Axis2DValue {
    return _context->ActionAxis2D(id, seat);
}

auto InputReader::KeyDown(KeyCode key, PlayerSeat seat) const noexcept -> bool {
    return _context->SeatKeyboardMouse(seat).IsKeyDown(key);
}
auto InputReader::KeyPressed(KeyCode key, PlayerSeat seat) const noexcept -> bool {
    return _context->SeatKeyboardMouse(seat).IsKeyPressed(key);
}
auto InputReader::KeyHeld(KeyCode key, PlayerSeat seat) const noexcept -> bool {
    return _context->SeatKeyboardMouse(seat).IsKeyHeld(key);
}
auto InputReader::KeyReleased(KeyCode key, PlayerSeat seat) const noexcept -> bool {
    return _context->SeatKeyboardMouse(seat).IsKeyReleased(key);
}

auto InputReader::MouseButtonDown(MouseButton button, PlayerSeat seat) const noexcept -> bool {
    return _context->SeatKeyboardMouse(seat).IsMouseButtonDown(button);
}
auto InputReader::MouseButtonPressed(MouseButton button, PlayerSeat seat) const noexcept -> bool {
    return _context->SeatKeyboardMouse(seat).IsMouseButtonPressed(button);
}
auto InputReader::MouseButtonHeld(MouseButton button, PlayerSeat seat) const noexcept -> bool {
    return _context->SeatKeyboardMouse(seat).IsMouseButtonHeld(button);
}
auto InputReader::MouseButtonReleased(MouseButton button, PlayerSeat seat) const noexcept -> bool {
    return _context->SeatKeyboardMouse(seat).IsMouseButtonReleased(button);
}

auto InputReader::Modifiers(PlayerSeat seat) const noexcept -> KeyModifiers {
    return _context->SeatKeyboardMouse(seat).Modifiers();
}
auto InputReader::HasModifiers(KeyModifiers required, bool exact, PlayerSeat seat) const noexcept -> bool {
    return _context->SeatKeyboardMouse(seat).HasModifiers(required, exact);
}
auto InputReader::MousePosition(PlayerSeat seat) const noexcept -> Axis2DValue {
    const InputState& kbm = _context->SeatKeyboardMouse(seat);
    return Axis2DValue {.x = kbm.mouseX, .y = kbm.mouseY};
}
auto InputReader::MouseDelta(PlayerSeat seat) const noexcept -> Axis2DValue {
    const InputState& kbm = _context->SeatKeyboardMouse(seat);
    return Axis2DValue {.x = kbm.mouseDeltaX, .y = kbm.mouseDeltaY};
}
auto InputReader::MouseWheel(PlayerSeat seat) const noexcept -> float {
    return _context->SeatKeyboardMouse(seat).mouseWheel;
}
auto InputReader::UiCapture(PlayerSeat seat) const noexcept -> UiCaptureState {
    return _context->UiCapture(seat);
}
auto InputReader::LastActiveDevice(PlayerSeat seat) const noexcept -> InputDeviceKind {
    return _context->LastActiveDevice(seat);
}
auto InputReader::Snapshot(PlayerSeat seat) const noexcept -> InputState {
    return _context->SeatKeyboardMouse(seat);
}

auto InputReader::ConnectedGamepadCount() const noexcept -> size_t {
    return _context->ConnectedGamepadCount();
}
auto InputReader::GamepadConnected(GamepadSlot gamepadSlot) const noexcept -> bool {
    const auto pad = _context->Gamepad(gamepadSlot);
    return static_cast<bool>(pad) && pad->connected;
}
auto InputReader::GamepadSeat(GamepadSlot gamepadSlot) const noexcept -> PlayerSeat {
    return _context->GamepadSeat(gamepadSlot);
}
auto InputReader::GamepadButtonDown(GamepadSlot gamepadSlot, GamepadButton button) const noexcept -> bool {
    const auto pad = _context->Gamepad(gamepadSlot);
    return static_cast<bool>(pad) && pad->IsButtonDown(button);
}
auto InputReader::GamepadButtonPressed(GamepadSlot gamepadSlot, GamepadButton button) const noexcept -> bool {
    const auto pad = _context->Gamepad(gamepadSlot);
    return static_cast<bool>(pad) && pad->IsButtonPressed(button);
}
auto InputReader::GamepadButtonReleased(GamepadSlot gamepadSlot, GamepadButton button) const noexcept -> bool {
    const auto pad = _context->Gamepad(gamepadSlot);
    return static_cast<bool>(pad) && pad->IsButtonReleased(button);
}
auto InputReader::GamepadAxisValue(GamepadSlot gamepadSlot, GamepadAxis axis) const noexcept -> float {
    const auto pad = _context->Gamepad(gamepadSlot);
    return static_cast<bool>(pad) ? pad->Axis(axis) : 0.0f;
}
auto InputReader::GamepadLeftStick(GamepadSlot gamepadSlot) const noexcept -> StickSample {
    const auto pad = _context->Gamepad(gamepadSlot);
    return static_cast<bool>(pad) ? pad->LeftStick() : StickSample {};
}
auto InputReader::GamepadRightStick(GamepadSlot gamepadSlot) const noexcept -> StickSample {
    const auto pad = _context->Gamepad(gamepadSlot);
    return static_cast<bool>(pad) ? pad->RightStick() : StickSample {};
}
auto InputReader::GamepadLeftTrigger(GamepadSlot gamepadSlot) const noexcept -> float {
    const auto pad = _context->Gamepad(gamepadSlot);
    return static_cast<bool>(pad) ? pad->LeftTrigger() : 0.0f;
}
auto InputReader::GamepadRightTrigger(GamepadSlot gamepadSlot) const noexcept -> float {
    const auto pad = _context->Gamepad(gamepadSlot);
    return static_cast<bool>(pad) ? pad->RightTrigger() : 0.0f;
}

auto InputReader::HasActionMap(std::string_view mapName) const noexcept -> bool {
    return static_cast<bool>(_context->FindActionMap(mapName));
}
auto InputReader::IsActionMapEnabled(std::string_view mapName) const noexcept -> bool {
    const auto found = _context->FindActionMap(mapName);
    return static_cast<bool>(found) && found->Enabled();
}
auto InputReader::RoutingPolicy() const noexcept -> SeatRoutingPolicy {
    return _context->RoutingPolicy();
}
auto InputReader::SeatReceivesKeyboardMouse(PlayerSeat seat) const noexcept -> bool {
    return _context->SeatReceivesKeyboardMouse(seat);
}
auto InputReader::ActiveRumble(GamepadSlot gamepadSlot) const noexcept -> GamepadRumble {
    return _context->ActiveRumble(gamepadSlot);
}
auto InputReader::CaptureStatus() const noexcept -> BindingCaptureStatus {
    return _context->CaptureStatus();
}
auto InputReader::SerializeBindings() const -> std::string {
    return _context->SerializeBindings();
}

// --- InputHaptics -------------------------------------------------------------

auto InputHaptics::SetGamepadRumble(GamepadSlot gamepadSlot, const GamepadRumble& rumble) noexcept -> bool {
    return _context->SetGamepadRumble(gamepadSlot, rumble);
}
auto InputHaptics::RumbleSeat(PlayerSeat seat, const GamepadRumble& rumble) noexcept -> uint32_t {
    return _context->RumbleSeat(seat, rumble);
}
void InputHaptics::StopRumble(GamepadSlot gamepadSlot) noexcept {
    _context->StopRumble(gamepadSlot);
}
void InputHaptics::StopAllRumble() noexcept {
    _context->StopAllRumble();
}
auto InputHaptics::ActiveRumble(GamepadSlot gamepadSlot) const noexcept -> GamepadRumble {
    return _context->ActiveRumble(gamepadSlot);
}

// --- InputActions -------------------------------------------------------------
//
// Authoring. The two rules this file keeps, and that the header cannot state
// strongly enough:
//
//   1. No map or action reference is returned. The router stores its maps in a
//      std::vector, so a reference handed out by CreateActionMap() is invalidated
//      by the next CreateActionMap() -- and the aliasing it allowed made every
//      authoring call site read like it owned the storage.
//   2. A mutation that names something that is not there is a false / null
//      ActionID, not an assert: a settings screen loading a stale config is a
//      normal event, not a programming error.

namespace {
// The map and action a name pair resolves to, or nullptr. One place that answers
// "does this exist", so the dozen operations below cannot disagree about it.
[[nodiscard]] auto FindMap(InputContext& context, std::string_view mapName) noexcept -> InputActionMap* {
    const auto found = context.FindActionMap(mapName);
    return found ? std::addressof(*found) : nullptr;
}

// The same question with an id in hand. ActionID is the hash of the name, so
// this is the lookup the router itself uses when it evaluates a binding.
[[nodiscard]] auto FindAction(InputContext& context, std::string_view mapName, ActionID id) noexcept -> InputAction* {
    InputActionMap* map = FindMap(context, mapName);
    if (map == nullptr) {
        return nullptr;
    }
    const auto found = map->FindAction(id);
    return found ? std::addressof(*found) : nullptr;
}

[[nodiscard]] auto FindAction(InputContext& context, std::string_view mapName, std::string_view actionName) noexcept -> InputAction* {
    InputActionMap* map = FindMap(context, mapName);
    if (map == nullptr) {
        return nullptr;
    }
    // ActionID is the hash of the name, so the lookup is the same one the
    // runtime uses; an action authored under one name cannot be found by
    // another spelling, which is the point of hashing it here too.
    const auto found = map->FindAction(ActionID(actionName));
    return found ? std::addressof(*found) : nullptr;
}
} // namespace

auto InputActions::CreateActionMap(std::string_view name, int32_t priority, bool enabled) noexcept -> bool {
    if (name.empty()) {
        return false;
    }
    InputActionMap& map = _context->CreateActionMap(name, priority);
    map.SetEnabled(enabled);
    return true;
}

auto InputActions::ConfigureActionMap(const ActionMapConfig& config) noexcept -> bool {
    const std::string_view mapName {config.name.c_str(), config.name.size()};
    if (mapName.empty()) {
        return false;
    }
    InputActionMap& map = _context->CreateActionMap(mapName, config.priority);
    map.SetEnabled(config.enabled);
    map.SetSeatMask(config.seatMask);
    map.SetBlocksLowerPriority(config.blocksLowerPriority);
    map.SetConsumePhysicalInputs(config.consumePhysicalInputs);
    return true;
}

auto InputActions::RemoveActionMap(std::string_view name) noexcept -> bool {
    return _context->RemoveActionMap(name);
}

auto InputActions::SetActionMapEnabled(std::string_view name, bool enabled) noexcept -> bool {
    return _context->SetActionMapEnabled(name, enabled);
}

auto InputActions::SetActionMapEnabledForSeat(std::string_view name, PlayerSeat seat, bool enabled) noexcept -> bool {
    return _context->SetActionMapEnabledForSeat(name, seat, enabled);
}

auto InputActions::SetActionMapBlocksLowerPriority(std::string_view name, bool blocks) noexcept -> bool {
    InputActionMap* map = FindMap(*_context, name);
    if (map == nullptr) {
        return false;
    }
    map->SetBlocksLowerPriority(blocks);
    return true;
}

auto InputActions::SetActionMapConsumePhysicalInputs(std::string_view name, bool consume) noexcept -> bool {
    InputActionMap* map = FindMap(*_context, name);
    if (map == nullptr) {
        return false;
    }
    map->SetConsumePhysicalInputs(consume);
    return true;
}

auto InputActions::AddAction(std::string_view mapName, std::string_view actionName, ActionKind kind, bool clampUnitCircle,
                             float actuationThreshold) noexcept -> ActionID {
    InputActionMap* map = FindMap(*_context, mapName);
    if (map == nullptr || actionName.empty()) {
        return ActionID {};
    }
    InputAction& action = map->AddAction(actionName, kind);
    action.SetClampUnitCircle(clampUnitCircle);
    action.SetActuationThreshold(actuationThreshold);
    return action.Id();
}

auto InputActions::ConfigureAction(const ActionConfig& config) noexcept -> ActionID {
    const std::string_view mapName {config.mapName.c_str(), config.mapName.size()};
    const std::string_view actionName {config.actionName.c_str(), config.actionName.size()};
    if (mapName.empty() || actionName.empty()) {
        return ActionID {};
    }
    if (config.bindingCount > 0U && config.bindings == nullptr) {
        return ActionID {};
    }
    InputActionMap* map = FindMap(*_context, mapName);
    if (map == nullptr) {
        return ActionID {};
    }

    InputAction& action = map->AddAction(actionName, config.kind);
    action.SetClampUnitCircle(config.clampUnitCircle);
    action.SetActuationThreshold(config.actuationThreshold);

    if (config.replaceBindings) {
        action.ReplaceBindings(std::span<const InputBinding> {config.bindings, config.bindingCount});
    } else {
        for (uint32_t i = 0; i < config.bindingCount; ++i) {
            action.AppendBinding(config.bindings[i]);
        }
    }
    return action.Id();
}

auto InputActions::AddBinding(std::string_view mapName, std::string_view actionName, const InputBinding& binding) noexcept -> bool {
    InputAction* action = FindAction(*_context, mapName, actionName);
    if (action == nullptr) {
        return false;
    }
    action->AppendBinding(binding);
    return true;
}

auto InputActions::Rebind(std::string_view mapName, std::string_view actionName, size_t slotIndex, const InputBinding& binding) noexcept -> bool {
    InputAction* action = FindAction(*_context, mapName, actionName);
    return action != nullptr && action->Rebind(slotIndex, binding);
}

auto InputActions::ClearBindings(std::string_view mapName, std::string_view actionName) noexcept -> bool {
    InputAction* action = FindAction(*_context, mapName, actionName);
    if (action == nullptr) {
        return false;
    }
    action->ClearBindings();
    return true;
}

auto InputActions::RemoveBinding(std::string_view mapName, std::string_view actionName, size_t slotIndex) noexcept -> bool {
    InputAction* action = FindAction(*_context, mapName, actionName);
    return action != nullptr && action->RemoveBinding(slotIndex);
}

auto InputActions::BindAction(std::string_view mapName, ActionID action, const InputBinding& binding) noexcept -> bool {
    InputAction* target = FindAction(*_context, mapName, action);
    if (target == nullptr) {
        return false;
    }
    target->AppendBinding(binding);
    return true;
}

auto InputActions::RebindAction(std::string_view mapName, ActionID action, size_t slotIndex, const InputBinding& binding) noexcept -> bool {
    InputAction* target = FindAction(*_context, mapName, action);
    return target != nullptr && target->Rebind(slotIndex, binding);
}

auto InputActions::ClearActionBindings(std::string_view mapName, ActionID action) noexcept -> bool {
    InputAction* target = FindAction(*_context, mapName, action);
    if (target == nullptr) {
        return false;
    }
    target->ClearBindings();
    return true;
}

auto InputActions::RemoveActionBinding(std::string_view mapName, ActionID action, size_t slotIndex) noexcept -> bool {
    InputAction* target = FindAction(*_context, mapName, action);
    return target != nullptr && target->RemoveBinding(slotIndex);
}

auto InputActions::BindingsForAction(std::string_view mapName, ActionID action) const -> std::vector<InputBinding> {
    const auto map = std::as_const(*_context).FindActionMap(mapName);
    if (!map) {
        return {};
    }
    const auto found = map->FindAction(action);
    if (!found) {
        return {};
    }
    const std::span<const InputBinding> bindings = found->Bindings();
    return std::vector<InputBinding>(bindings.begin(), bindings.end());
}

auto InputActions::BindingsFor(std::string_view mapName, std::string_view actionName) const -> std::vector<InputBinding> {
    // A const view of the router still has to answer this, and FindActionMap's
    // const overload is what keeps it honest: this copies out of the engine's
    // storage without ever becoming a handle on it.
    const auto map = std::as_const(*_context).FindActionMap(mapName);
    if (!map) {
        return {};
    }
    const auto action = map->FindAction(ActionID(actionName));
    if (!action) {
        return {};
    }
    const std::span<const InputBinding> bindings = action->Bindings();
    return std::vector<InputBinding>(bindings.begin(), bindings.end());
}

void InputActions::SnapshotAllDefaults() {
    _context->SnapshotAllDefaults();
}

auto InputActions::ResetActionMapToDefaults(std::string_view mapName) noexcept -> bool {
    return _context->ResetActionMapToDefaults(mapName);
}

void InputActions::ResetAllToDefaults() {
    _context->ResetAllToDefaults();
}

auto InputActions::FindConflictingActions(std::string_view mapName, const InputBinding& candidate, std::string_view ignoreAction) const
    -> std::vector<ActionID> {
    return _context->FindConflictingActions(mapName, candidate, ignoreAction);
}

void InputActions::BeginBindingCapture(PlayerSeat seat, bool captureAxes, KeyCode cancelKey) noexcept {
    _context->BeginBindingCapture(seat, captureAxes, cancelKey);
}

void InputActions::CancelBindingCapture() noexcept {
    _context->CancelBindingCapture();
}

auto InputActions::TakeCapturedBinding() noexcept -> Optional<InputBinding> {
    return _context->TakeCapturedBinding();
}

auto InputActions::SerializeBindings() const -> std::string {
    return _context->SerializeBindings();
}

auto InputActions::DeserializeBindings(std::string_view text) -> std::expected<void, ErrorCode> {
    return _context->DeserializeBindings(text);
}

void InputActions::SetRoutingPolicy(SeatRoutingPolicy policy) noexcept {
    _context->SetRoutingPolicy(policy);
}

void InputActions::SetKeyboardMouseSeat(PlayerSeat seat, bool enabled) noexcept {
    _context->SetKeyboardMouseSeat(seat, enabled);
}

auto InputActions::AssignGamepadToSeat(GamepadSlot gamepadSlot, PlayerSeat seat) noexcept -> bool {
    return _context->AssignGamepadToSeat(gamepadSlot, seat);
}

void InputActions::UnassignGamepad(GamepadSlot gamepadSlot) noexcept {
    _context->UnassignGamepad(gamepadSlot);
}

void InputActions::SetGamepadDeadzones(GamepadSlot gamepadSlot, const DeadzoneConfig& leftStick, const DeadzoneConfig& rightStick,
                                       float triggerDz) noexcept {
    _context->SetGamepadDeadzones(gamepadSlot, leftStick, rightStick, triggerDz);
}

void InputActions::SetSeatDeadzones(PlayerSeat seat, const DeadzoneConfig& leftStick, const DeadzoneConfig& rightStick, float triggerDz) noexcept {
    _context->SetSeatDeadzones(seat, leftStick, rightStick, triggerDz);
}

auto InputActions::Reader() const noexcept -> InputReader {
    return InputReader {_context};
}

auto InputActions::Haptics() noexcept -> InputHaptics {
    return InputHaptics {_context};
}

auto InputActions::Action(ActionID id, PlayerSeat seat) const noexcept -> ActionState {
    return _context->Action(id, seat);
}
auto InputActions::HasActionMap(std::string_view mapName) const noexcept -> bool {
    return static_cast<bool>(_context->FindActionMap(mapName));
}
auto InputActions::IsActionMapEnabled(std::string_view mapName) const noexcept -> bool {
    const auto found = _context->FindActionMap(mapName);
    return static_cast<bool>(found) && found->Enabled();
}
auto InputActions::RoutingPolicy() const noexcept -> SeatRoutingPolicy {
    return _context->RoutingPolicy();
}
auto InputActions::SeatReceivesKeyboardMouse(PlayerSeat seat) const noexcept -> bool {
    return _context->SeatReceivesKeyboardMouse(seat);
}
auto InputActions::CaptureStatus() const noexcept -> BindingCaptureStatus {
    return _context->CaptureStatus();
}
auto InputActions::ConnectedGamepadCount() const noexcept -> size_t {
    return _context->ConnectedGamepadCount();
}

auto InputActions::SetGamepadRumble(GamepadSlot gamepadSlot, const GamepadRumble& rumble) noexcept -> bool {
    return _context->SetGamepadRumble(gamepadSlot, rumble);
}
auto InputActions::RumbleSeat(PlayerSeat seat, const GamepadRumble& rumble) noexcept -> uint32_t {
    return _context->RumbleSeat(seat, rumble);
}
void InputActions::StopRumble(GamepadSlot gamepadSlot) noexcept {
    _context->StopRumble(gamepadSlot);
}
void InputActions::StopAllRumble() noexcept {
    _context->StopAllRumble();
}

} // namespace ZHLN
