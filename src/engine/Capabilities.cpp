// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// One translation unit owns the heavy includes. The capability headers stay
// small: they name value types and operations, not renderer, Jolt or audio
// implementation headers.

#include <Zahlen/Audio/Audio.hpp>
#include <Zahlen/Engine/AssetManager.hpp>
#include <Zahlen/Engine/Capabilities/AssetLibrary.hpp>
#include <Zahlen/Engine/Capabilities/AudioEngine.hpp>
#include <Zahlen/Engine/Capabilities/DebugDraw.hpp>
#include <Zahlen/Engine/Capabilities/EngineState.hpp>
#include <Zahlen/Engine/Capabilities/Entities.hpp>
#include <Zahlen/Engine/Capabilities/GraphicsDevice.hpp>
#include <Zahlen/Engine/Capabilities/Simulation.hpp>
#include <Zahlen/Engine/Engine.hpp>
#include <Zahlen/Engine/Internal/EngineAccess.hpp>
#include <Zahlen/Engine/Kernel.hpp>
#include <Zahlen/Engine/World.hpp>
#include <Zahlen/Physics/Physics.hpp>
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

auto Entities::Registry() const noexcept -> ECS::Registry& {
    return ZHLN::EngineAccess::RegistryOf(*_engine);
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
auto GraphicsDevice::CreateProceduralTexture(std::string_view name, Extent2D extent, std::span<const uint32_t> pixels, bool isSRGB, bool generateMips)
    -> TextureID {
    return _ctx->CreateProceduralTexture(name, extent, pixels, isSRGB, generateMips);
}
void GraphicsDevice::UnloadTexture(TextureID handle) {
    _ctx->UnloadTexture(handle);
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
    const auto position = _ctx->BodyPosition(handle);
    if (!position) {
        return std::nullopt;
    }
    return DVec3 {*position};
}
auto Simulation::BodyState(Physics::BodyID handle) const noexcept -> std::optional<Physics::BodyStateSnapshot> {
    return _ctx->BodyState(handle);
}
auto Simulation::CharacterVelocity(Physics::BodyID handle) const -> FVec3 {
    return FVec3 {_ctx->GetCharacterVelocity(handle)};
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

// --- Diagnostics --------------------------------------------------------------

auto EngineState::CullingStats() const noexcept -> ZHLN::CullingStats {
    return ZHLN::EngineAccess::WorldOf(*_engine).GetCullingStats();
}
void EngineState::ResetCullingStats() noexcept {
    ZHLN::EngineAccess::WorldOf(*_engine).GetCullingStats() = {};
}
auto EngineState::Configuration() const noexcept -> EngineConfig {
    return ZHLN::EngineAccess::KernelOf(*_engine).GetConfig();
}

} // namespace ZHLN
