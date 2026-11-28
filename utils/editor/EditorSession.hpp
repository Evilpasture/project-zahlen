// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <Scene/Scene.hpp>
#include <Zahlen/COM/GraphicsTypes.hpp>
#include <Zahlen/COM/IEngine.hpp>
#include <Zahlen/Core/COM/Ref.hpp>
#include <UI/HostUI.hpp>
#include <Zahlen/Math/Camera.hpp>
#include <editor/GUIEditor.hpp>
#include <expected>
#include <filesystem>
#include <unordered_map>
namespace ZHLN::Editor {
// Host-owned editor state. The custom loop stages even paused frames; camera
// movement, picking, widgets and rendering do not require simulation to run.
enum class SessionError : uint8_t { MissingCamera = 1, WriteFailed, NotStarted };
class Session {
  public:
    Session() = default;
    ~Session();
    Session(const Session&)                           = delete;
    auto        operator=(const Session&) -> Session& = delete;
    State state;
    bool        simulationRunning = false;
    auto        Start(COM::IEngine&, bool buildBLAS = true) -> std::expected<void, ErrorCode>;
    auto        Frame(COM::IEngine&, HostUI&, float) -> std::expected<void, ErrorCode>;
    void        Stop(COM::IEngine&);
    auto        Save(COM::IEngine&, const std::filesystem::path&) -> std::expected<void, ErrorCode>;
    auto        SpawnShape(COM::IEngine&, int kind, FVec3 position) -> std::expected<Entity, ErrorCode>;
    auto        Pick(COM::IEngine&, const Camera&, const ViewportRect&, float mouseX, float mouseY) -> Entity;

  private:
    struct FallbackMesh {
        AssetID  assetID    = InvalidAssetID;
        Mesh     mesh {};
        uint64_t generation = 0;
    };
    auto                                               RefreshPrimitives(COM::IEngine&) -> std::expected<void, ErrorCode>;
    void                                               ReleaseFallbackMeshes(COM::IEngine&);
    std::unordered_map<uint64_t, Scene::Material> _materials;
    std::unordered_map<uint64_t, FallbackMesh>         _fallbackMeshes;
    auto                                               Draw(COM::IEngine&, HostUI&, float) -> std::expected<void, ErrorCode>;
    bool                                               _started   = false;
    bool                                               _buildBLAS = true, _saveDown = false, _escapeDown = false, _mouseDown = false;
    COM::Ref<COM::IEngine>                             _engine;
    ViewportRect                                       _previousViewport {};
};
} // namespace ZHLN::Editor
