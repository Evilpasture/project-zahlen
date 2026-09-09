// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/engine/Engine.cpp
#include <GLFW/glfw3.h>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <optional>
#include <thread>
#include <vector>
#include "tty/TTYBackend.hpp"
#include <Zahlen/Audio.hpp>
#include <Zahlen/Camera.hpp>
#include <Zahlen/CommandLine.hpp>
#include <Zahlen/Components.hpp>
#include <Zahlen/CreativeWorksFactory.hpp>
#include <Zahlen/CreativeWorksManager.hpp>
#include "DefaultPreset.hpp"
#include "EngineGlobals.hpp"
#include <Zahlen/Engine.hpp>
#include "EngineAccess.hpp"
#include <Zahlen/FrameScheduler.hpp>
#include <Zahlen/Input.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Render.hpp>
#include <Zahlen/Scripting.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <Zahlen/Window.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <Zahlen/ecs/EntityCommandBuffer.hpp>
#include <Zahlen/ecs/SystemGraph.hpp>
#include <Zahlen/physics/Physics.hpp>
#include <Zahlen/FileSystemWatcher.hpp>
#include "NativeScriptModule.hpp"
#include "Platform.hpp"
#include "ArticulationSystem.hpp"
#include "CullingSystem.hpp"
#include <filesystem>
#include <Zahlen/Threading/Thread.hpp>

namespace ZHLN {

struct EngineImpl {
    // Declared first so it outlives every callback-owning client during normal
    // and partial-initialization teardown.
    std::unique_ptr<FileSystemWatcher>    fileSystemWatcher;
    std::vector<std::unique_ptr<Window>>  windows;
    std::vector<ViewportDesc>             extraViewports; // parallel to windows[1..]
    std::unique_ptr<RenderContext>        renderContext;
    std::unique_ptr<PhysicsContext>       physicsContext;
    std::unique_ptr<AudioContext>         audioContext;
    std::unique_ptr<CreativeWorksManager> assetManager;
    std::unique_ptr<ScriptRunner>         scriptRunner;
    std::unique_ptr<NativeScriptModule>   nativeScriptModule;
    FileWatchHandle                        bootLuaWatch = 0;
    FileWatchHandle                        bootFennelWatch = 0;
    GameplayDriver                         activeGameplayDriver = GameplayDriver::Cpp;

    Engine::UICallback                      uiCallback = nullptr;
    std::vector<Engine::DeviceLostCallback> deviceLostCallbacks;

    Camera        mainCamera;
    ECS::Registry registry;

    FrameScheduler                            scheduler;
    std::unique_ptr<ECS::SystemGraph>         updateGraph;
    std::unique_ptr<ECS::SystemGraph>         renderGraph;
    std::unique_ptr<ECS::EntityCommandBuffer> mainECB;
    std::unique_ptr<CullingSystem>            cullingSystem;
    std::unique_ptr<ArticulationSystem>        articulationSystem;
    JPH::Array<Entity>                        visibleEntities;
    JPH::Array<Entity>                        visibleShadowEntities;
    float                                     currentAlpha = 0.0f;

    // Built once per engine, not once per scene: the glyph packing costs
    // 96 SDF rasterisations, and the upload burns a 1024x1024 bindless texture
    // that nothing ever releases. The scene owns a *copy* in UISettingsComponent,
    // which Registry::Clear() throws away, so the engine keeps the authoritative
    // one and re-seeds each new scene from it. See InitializeDefaultScene.
    std::optional<FontAtlas> fontAtlas;

    void*    gameState    = nullptr;
    uint64_t frameCounter = 0;
    bool     joltAcquired = false;
    bool     glfwAcquired = false;
    EngineConfig config;
};

auto EngineFrameStepAccess::NativeGameplayModule(Engine& engine) -> NativeScriptModule& {
    return *engine._impl->nativeScriptModule;
}

auto EngineFrameStepAccess::Config(Engine& engine) -> const EngineConfig& {
    return engine._impl->config;
}

auto EngineFrameStepAccess::PersistentFontAtlas(Engine& engine) -> std::optional<FontAtlas>& {
    return engine._impl->fontAtlas;
}

Engine::Engine(): _impl(nullptr) {
}

Engine::Engine(const EngineConfig& cfg): _impl(nullptr) {
    auto res = InitInternal(cfg);
    if (!res) {
        ZHLN::Panic("FATAL: Failed to initialize Engine via legacy constructor: {}", res.error().Message());
    }
}

auto Engine::HandleDeviceLost() noexcept -> std::expected<void, Error> {
    _impl->renderContext->OnDeviceLost();
    _impl->renderContext.reset();

    auto rc_res = RenderContext::Create(*_impl->windows.front(), _impl->config.render, _impl->fileSystemWatcher.get());
    if (!rc_res) {
        return std::unexpected(rc_res.error());
    }
    _impl->renderContext = std::move(rc_res.value());
    for (size_t i = 1; i < _impl->windows.size(); ++i) {
        ViewportDesc desc {};
        if (i - 1 < _impl->extraViewports.size()) {
            desc = _impl->extraViewports[i - 1];
        }
        if (auto presented = _impl->renderContext->AddViewport(*_impl->windows[i], desc); !presented) {
            ZHLN::Log("[Engine] HandleDeviceLost: extra viewport {} failed ({})", i, presented.error());
        }
    }
    CreativeWorksFactory::RebuildVulkanResources(*_impl->renderContext, _impl->registry);

    // Core has rebuilt everything it owns. Owners outside the engine now
    // re-upload against the new context, in the order they registered.
    for (const auto& callback: _impl->deviceLostCallbacks) {
        if (callback) {
            callback(*this);
        }
    }
    return {};
}

Engine::Engine(const EngineConfig& cfg, bool& outSuccess): _impl(nullptr) {
    auto res   = InitInternal(cfg);
    outSuccess = res.has_value();
    if (!res) {
        ZHLN::Log("Engine initialization failed: {}", res.error().Message());
    }
}

auto Engine::Create(const EngineConfig& cfg) -> std::expected<std::unique_ptr<Engine>, Error> {
    auto instance = std::unique_ptr<Engine>(new (std::nothrow) Engine());
    if (!instance) {
        return std::unexpected(EngineInitError::EngineAllocationFailed);
    }

    if (auto result = instance->InitInternal(cfg); !result) {
        return std::unexpected(result.error());
    }
    return std::move(instance);
}

auto Engine::InitInternal(const EngineConfig& cfg) -> std::expected<void, Error> {
    ZHLN::Fiber::InitMainThread();

    _impl                       = std::make_unique<EngineImpl>();
    _impl->config               = cfg;
    _impl->fileSystemWatcher    = std::make_unique<FileSystemWatcher>();
    _impl->scriptRunner         = std::make_unique<ScriptRunner>();

    bool use_tty = false;

    if (cfg.render.headless) {
        // True headless mode: skip GLFW entirely. No display server is required.
        ZHLN::Log("[Engine] Headless mode enabled. Skipping GLFW initialization.");
    } else {
        glfwSetErrorCallback([](int error, const char* description) -> void {
            ZHLN::Log("[GLFW Error] Code {}: {}", error, description ? description : "(null)");
        });

        if constexpr (isLinux) {
            // Detects both RenderDoc and NVIDIA Nsight Graphics (Nomad) launch environments
            if (std::getenv("ENABLE_VULKAN_RENDERDOC_CAPTURE") != nullptr || std::getenv("NOMAD_VULKAN_LAYER") != nullptr ||
                std::getenv("NGFX_INJECTION") != nullptr) {
                glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_X11);
            }
        }

        if (!AcquireGlfw()) {
            const char* desc = nullptr;
            int err = glfwGetError(&desc);
            if (desc != nullptr) {
                ZHLN::Log("[Engine] glfwInit failed: ({}) {}", err, desc);
            }
            if (TTYBackend::IsSupported()) {
                ZHLN::Log("GLFW failed to initialize. Falling back to native TTY Display Mode.");
                use_tty = true;
            } else {
                return std::unexpected(EngineInitError::WindowCreationFailed);
            }
        } else {
            _impl->glfwAcquired = true;
        }
    }

    // Keys land in InputStateComponent twice over: held state in the bitset for
    // gameplay, and a queue of presses plus typed characters for text fields.
    // The engine still does not interpret any of it as text -- it does not own a
    // GUI::Context (the caller does, see app/main.cpp:151), so it has no way to
    // know which field is focused. GUI::Context::BeginFrame drains the queue;
    // Context::PushKey/PushChar feed the same queue for hosts with no window.
    //
    // Every press is queued, not just the editing keys. Deciding which keys a
    // text field acts on is TextBuffer.hpp's business; this is only the pump.
    auto onKey = [](void* userdata, KeyCode key, bool pressed) -> void {
        auto* impl  = static_cast<EngineImpl*>(userdata);
        auto* reg   = &impl->registry;
        auto* state = &reg->GetOrEmplaceSingleton<Components::InputStateComponent>();
        state->SetKey(static_cast<uint8_t>(key), pressed);
        if (pressed) {
            state->QueueKeyPress(key);
        }
    };

    auto onMouseMove = [](void* userdata, float x, float y) -> void {
        auto* reg   = &static_cast<EngineImpl*>(userdata)->registry;
        auto* state = &reg->GetOrEmplaceSingleton<Components::InputStateComponent>();
        state->ApplyLocalMotion(x, y);
    };

    auto onMouseScroll = [](void* userdata, float delta) -> void {
        auto* reg   = &static_cast<EngineImpl*>(userdata)->registry;
        auto* state = &reg->GetOrEmplaceSingleton<Components::InputStateComponent>();
        state->ApplyWheel(delta);
    };

    auto onResize = [](void* userdata, Extent2D extent) -> void {
        auto* reg   = &static_cast<EngineImpl*>(userdata)->registry;
        auto* state = &reg->GetOrEmplaceSingleton<Components::InputStateComponent>();
        state->ApplyResize(extent);
    };

    auto onChar = [](void* userdata, unsigned int codepoint) -> void {
        auto* reg   = &static_cast<EngineImpl*>(userdata)->registry;
        auto* state = &reg->GetOrEmplaceSingleton<Components::InputStateComponent>();
        state->QueueChar(codepoint);
    };

    // userdata is the heap-allocated EngineImpl (stable for the engine's whole
    // life, unlike `this`), which owns the registry the callbacks write to.
    WindowInputReceiver receiver = {
        .userdata = _impl.get(), .onKey = onKey, .onMouseMove = onMouseMove, .onMouseScroll = onMouseScroll, .onResize = onResize,
        .onChar   = onChar
    };

    _impl->windows.push_back(std::make_unique<Window>(
        cfg.render.appName.data(), cfg.render.width, cfg.render.height, cfg.render.fullscreen, receiver, use_tty, cfg.render.headless
    ));

    // Singleton InputStateComponent must exist before the first event pump.
    _impl->registry.Create(Components::InputStateComponent {});

    if (use_tty && _impl->windows.front()->GetTTYContext() == nullptr) {
        return std::unexpected(EngineInitError::TTYInitializationFailed);
    }

    InitRenderDocAPI();

    AcquireJoltRegistration();
    _impl->joltAcquired = true;

    auto rc_res = RenderContext::Create(*_impl->windows.front(), cfg.render, _impl->fileSystemWatcher.get());
    if (!rc_res) {
        return std::unexpected(rc_res.error());
    }
    _impl->renderContext = std::move(rc_res.value());

    _impl->physicsContext = std::make_unique<PhysicsContext>(cfg.physics);
    _impl->audioContext   = std::make_unique<AudioContext>();
    _impl->assetManager   = std::make_unique<CreativeWorksManager>();
    _impl->nativeScriptModule = std::make_unique<NativeScriptModule>(*this, "scripts/gameplay");

    const auto reloadBootScript = [this](const FileWatchEvent& event) {
        if (_impl->activeGameplayDriver == GameplayDriver::Cpp || event.action == FileWatchAction::Deleted) {
            return;
        }
        _impl->scriptRunner->ReloadFile(event.path.string());
    };
    _impl->bootLuaWatch    = _impl->fileSystemWatcher->WatchFile("scripts/boot.lua", reloadBootScript);
    _impl->bootFennelWatch = _impl->fileSystemWatcher->WatchFile("scripts/boot.fnl", reloadBootScript);

    _impl->updateGraph   = std::make_unique<ECS::SystemGraph>();
    _impl->renderGraph   = std::make_unique<ECS::SystemGraph>();
    _impl->mainECB       = std::make_unique<ECS::EntityCommandBuffer>(_impl->registry);
    _impl->cullingSystem        = std::make_unique<CullingSystem>();
    _impl->articulationSystem   = std::make_unique<ArticulationSystem>();

    if (std::filesystem::exists("data/base.pak")) {
        _impl->assetManager->MountPak("data/base.pak");
    } else if (std::filesystem::exists("build/data/base.pak")) {
        _impl->assetManager->MountPak("build/data/base.pak");
    } else {
        ZHLN::Log("WARNING: Could not find 'data/base.pak' in working directory or build/ folder!");
    }

    return {};
}

Engine::~Engine() {
    // InitInternal can fail before _impl is built, and Engine::Create deletes a
    // half-built engine.
    if (_impl == nullptr) {
        return;
    }

    // The fallback preset parks entity handles in process-global storage. They
    // name entities in the registry that is about to be cleared, so they must
    // not survive into the next engine (see DefaultPreset::ReleaseFor).
    DefaultPreset::ReleaseFor(this);

    // Ragdolls retain Jolt resources outside the registry. Drain them while
    // both the components and PhysicsContext still exist. InitInternal may
    // fail before this system is created, so teardown must tolerate that path.
    if (_impl->articulationSystem != nullptr) {
        _impl->articulationSystem->Shutdown(*this);
    }
    _impl->registry.Clear();
    if (_impl->renderContext != nullptr) {
        _impl->renderContext->ReconcileEntityBuffers(_impl->registry.AliveQuery());
    }
    _impl->articulationSystem.reset();
    _impl->physicsContext.reset();
    _impl->renderContext.reset();
    _impl->nativeScriptModule.reset();
    _impl->fileSystemWatcher.reset();
    _impl->windows.clear();
    _impl->assetManager.reset();
    _impl->audioContext.reset();
    _impl->scriptRunner.reset();
    _impl->updateGraph.reset();
    _impl->renderGraph.reset();
    _impl->mainECB.reset();
    _impl->cullingSystem.reset();

    // Process-global, refcounted like Jolt: extra windows and a second engine
    // must not glfwTerminate under a window that is still open. Headless
    // engines never acquire GLFW.
    if (_impl->glfwAcquired) {
        ReleaseGlfw();
    }

    if (_impl->joltAcquired) {
        ReleaseJoltRegistration();
    }
}

auto Engine::IsRunning() const -> bool {
    return _impl->windows.front()->IsRunning();
}

void Engine::ProcessEvents() {
    ZHLN::CheckForCrashes(this);

    auto&                            reg        = _impl->registry;
    Components::InputStateComponent* inputState = reg.GetSingleton<Components::InputStateComponent>();
    if (inputState != nullptr) {
        inputState->ResetDeltas();
    }

    if (_impl->windows.front()->IsHeadless()) {
        // True headless mode: no windowing event queue to poll.
        return;
    }

    if (_impl->windows.front()->IsTTY()) {
        // TTY path uses the same WindowInputReceiver callbacks as GLFW
        TTYBackend::ProcessEvents(_impl->windows.front()->GetTTYContext(), _impl->windows.front()->GetInputReceiver());
        if (inputState != nullptr) {
            inputState->wantCaptureKeyboard = false;
            inputState->wantCaptureMouse    = false;
        }
        return;
    }

    glfwPollEvents();

    // Super+Q on any focused window ends the process. Super+W already called
    // Window::Close on that window in the key callback.
    for (const auto& window: _impl->windows) {
        if (window != nullptr && window->WantsQuitProcess()) {
            window->AcknowledgeQuitProcess();
            _impl->windows.front()->Close();
            break;
        }
    }
}

auto Engine::BeginFrame(bool& outDeviceLost) noexcept -> bool {
    outDeviceLost = false;
    auto res      = _impl->renderContext->BeginFrame();
    if (!res) {
        if (res.error() == RenderFrameResult::DeviceLost) {
            outDeviceLost = true;
            // Same contract as Steps::Present: a failed rebuild leaves no
            // RenderContext, so the window is closed to stop the host loop.
            if (auto lost_res = HandleDeviceLost(); !lost_res) {
                ZHLN::Log("[Engine] Fatal: GPU device recovery failed: {}", lost_res.error().Message());
                _impl->windows.front()->Close();
            }
        }
        return false;
    }
    return true;
}

auto Engine::EndFrame(bool& outDeviceLost) noexcept -> bool {
    outDeviceLost = false;
    auto res      = _impl->renderContext->EndFrame();
    if (!res) {
        if (res.error() == RenderFrameResult::DeviceLost) {
            outDeviceLost = true;
            if (auto lost_res = HandleDeviceLost(); !lost_res) {
                ZHLN::Log("[Engine] Fatal: GPU device recovery failed: {}", lost_res.error().Message());
                _impl->windows.front()->Close();
            }
        }
        return false;
    }
    return true;
}

auto Engine::GetCurrentFrame() const noexcept -> uint64_t {
    return _impl->frameCounter;
}

auto Engine::GetWindow() -> Window& {
    return *_impl->windows.front();
}

auto Engine::GetWindow(size_t index) -> Window& {
    if (index >= _impl->windows.size()) {
        ZHLN::Panic("Engine::GetWindow index {} out of range ({})", index, _impl->windows.size());
    }
    return *_impl->windows[index];
}

auto Engine::WindowCount() const noexcept -> size_t {
    return _impl->windows.size();
}

auto Engine::AddWindow(
    const String32& title, uint32_t width, uint32_t height, bool fullscreen, const WindowInputReceiver& receiver, ViewportMode mode, Entity camera
) -> Window* {
    if (_impl->windows.empty() || !_impl->glfwAcquired || _impl->windows.front()->IsHeadless() || _impl->windows.front()->IsTTY()) {
        ZHLN::Log("[Engine] AddWindow requires an initialized GLFW session");
        return nullptr;
    }

    auto window = std::make_unique<Window>(title, width, height, fullscreen, receiver, false, false);
    if (window->GetNativeHandle() == nullptr) {
        ZHLN::Log("[Engine] AddWindow: OS window creation failed");
        return nullptr;
    }
    Window*      raw  = window.get();
    ViewportDesc desc {.mode = mode, .camera = camera};
    _impl->windows.push_back(std::move(window));
    _impl->extraViewports.push_back(desc);
    if (_impl->renderContext != nullptr) {
        if (auto presented = _impl->renderContext->AddViewport(*raw, desc); !presented) {
            ZHLN::Log("[Engine] AddWindow: extra viewport failed ({})", presented.error());
            _impl->windows.pop_back();
            _impl->extraViewports.pop_back();
            return nullptr;
        }
    }
    return raw;
}

void Engine::RemoveWindow(Window& window) {
    if (_impl->windows.empty() || _impl->windows.front().get() == &window) {
        return;
    }
    size_t extraIdx = 0;
    for (size_t i = 1; i < _impl->windows.size(); ++i) {
        if (_impl->windows[i].get() == &window) {
            extraIdx = i - 1;
            break;
        }
    }
    if (_impl->renderContext != nullptr) {
        if (auto removed = _impl->renderContext->RemoveViewport(window); !removed) {
            ZHLN::Log("[Engine] RemoveWindow: extra viewport teardown failed ({})", removed.error());
        }
    }
    std::erase_if(_impl->windows, [&](const std::unique_ptr<Window>& owned) { return owned.get() == &window; });
    if (extraIdx < _impl->extraViewports.size()) {
        _impl->extraViewports.erase(_impl->extraViewports.begin() + static_cast<std::ptrdiff_t>(extraIdx));
    }
}

auto Engine::GetPhysicsContext() -> PhysicsContext& {
    return *_impl->physicsContext;
}
auto Engine::GetRenderContext() -> RenderContext& {
    return *_impl->renderContext;
}
auto Engine::GetCamera() -> Camera& {
    return _impl->mainCamera;
}
auto Engine::GetCreativeWorksManager() -> CreativeWorksManager& {
    return *_impl->assetManager;
}
auto Engine::GetAudioContext() -> AudioContext& {
    return *_impl->audioContext;
}
auto Engine::GetScriptRunner() -> ScriptRunner& {
    return *_impl->scriptRunner;
}
auto Engine::GetFileSystemWatcher() -> FileSystemWatcher& {
    return *_impl->fileSystemWatcher;
}
auto Engine::GetRegistry() -> ECS::Registry& {
    return _impl->registry;
}

auto Engine::GetRegistry() const -> const ECS::Registry& {
    return _impl->registry;
}

auto Engine::GetUpdateGraph() -> ECS::SystemGraph& {
    return *_impl->updateGraph;
}
auto Engine::GetRenderGraph() -> ECS::SystemGraph& {
    return *_impl->renderGraph;
}
auto Engine::GetMainECB() -> ECS::EntityCommandBuffer& {
    return *_impl->mainECB;
}
auto Engine::GetFrameScheduler() -> FrameScheduler& {
    return _impl->scheduler;
}
auto Engine::GetCullingSystem() -> CullingSystem& {
    return *_impl->cullingSystem;
}
auto Engine::GetArticulationSystem() -> ArticulationSystem& {
    return *_impl->articulationSystem;
}
auto Engine::GetVisibleEntities() -> JPH::Array<Entity>& {
    return _impl->visibleEntities;
}
auto Engine::GetVisibleShadowEntities() -> JPH::Array<Entity>& {
    return _impl->visibleShadowEntities;
}
auto Engine::GetCurrentAlpha() -> float& {
    return _impl->currentAlpha;
}

auto Engine::GetGameState() const -> void* {
    return _impl->gameState;
}
void Engine::SetGameState(void* state) {
    _impl->gameState = state;
}

void Engine::SetUICallback(UICallback callback) {
    _impl->uiCallback = std::move(callback);
}

void Engine::AddDeviceLostCallback(DeviceLostCallback callback) {
    if (callback) {
        _impl->deviceLostCallbacks.push_back(std::move(callback));
    }
}

auto Engine::DeviceLostCallbackCount() const noexcept -> size_t {
    return _impl->deviceLostCallbacks.size();
}

auto Engine::GetUICallback() const noexcept -> const UICallback* {
    return _impl->uiCallback ? &_impl->uiCallback : nullptr;
}

void Engine::ProvokeDeviceLost() {
    _impl->renderContext->ProvokeDeviceLost();
}

auto Engine::InitializeDefaultScene() -> bool {
    return DefaultPreset::InitializeDefaultScene(*this);
}

auto Engine::Tick(float dt, GameplayDriver driver) -> GameplayStatus {
    _impl->activeGameplayDriver = driver;

    // Resource contexts retain owner/handle pairs outside ECS component
    // storage. Reconcile before any phase can observe this frame's world.
    _impl->renderContext->ReconcileEntityBuffers(_impl->registry.AliveQuery());

    FrameContext ctx {.driver = driver, .status = GameplayStatus::OK, .deviceLost = false};

    // The whole frame is the scheduler's ordered step list; the two SystemGraphs
    // are steps inside it (see BuildFrameScheduler), so their hazard analysis
    // only ever orders systems within a graph, never the phases around them.
    _impl->scheduler.Execute(*this, dt, ctx);

    _impl->frameCounter++;

    return ctx.status;
}

auto Engine::Run(const CommandLineOptions& options, UICallback uiCallback) -> std::expected<void, Error> {
    Platform::Init();
    ZHLN::SetupSignalHandler();
    TaskSystem::Init();

    uint32_t w = options.fullscreen ? 0 : 1280;
    uint32_t h = options.fullscreen ? 0 : 720;

    EngineConfig config {
        .physics = {.maxBodies = 5000, .maxBodyPairs = 10000, .maxContactConstraints = 10000, .tempAllocatorSize = 64 * 1024 * 1024},
        .render  = {
            .appName        = options.launchEditor ? "Zahlen World Editor" : "Zahlen Engine",
            .width          = w,
            .height         = h,
            .vsync          = options.vsync,
            .fullscreen     = options.fullscreen,
            .validationMode = options.validationMode,
            .headless       = options.headless,
        },
    };

    auto engine_res = Engine::Create(config);
    if (!engine_res) {
        TaskSystem::Shutdown();
        return std::unexpected(engine_res.error()); // Propagate the exact Error!
    }

    auto engine = std::move(engine_res.value());
    engine->GetWindow().Focus();
    engine->InitializeDefaultScene();

    if (uiCallback) {
        engine->SetUICallback(std::move(uiCallback));
    }

    const double targetFrameTime = options.fpsLimit > 0 ? 1.0 / static_cast<double>(options.fpsLimit) : 0.0;
    auto         frameStart      = std::chrono::high_resolution_clock::now();

    while (engine->IsRunning()) {
        engine->ProcessEvents();

        auto   frameEnd = std::chrono::high_resolution_clock::now();
        double elapsed  = std::chrono::duration<double>(frameEnd - frameStart).count();
        frameStart      = std::chrono::high_resolution_clock::now();

        float rawDt = std::min(static_cast<float>(elapsed), 0.1f);

        {
            auto& r = engine->GetRegistry();
            if (auto* st = r.GetSingleton<Components::InputStateComponent>(); st != nullptr && st->needsResize) {
                engine->GetRenderContext().SetResolution(st->newSize);
                st->needsResize = false;
                continue;
            }
        }

        // Single synchronized engine tick
        GameplayStatus status = engine->Tick(rawDt, options.driver);
        if (status == GameplayStatus::RequestQuit) {
            engine->GetWindow().Close();
            break;
        }

        if (options.fpsLimit > 0) {
            auto   now          = std::chrono::high_resolution_clock::now();
            double frameElapsed = std::chrono::duration<double>(now - frameStart).count();
            if (frameElapsed < targetFrameTime) {
                double sleepTime = targetFrameTime - frameElapsed;
                if (sleepTime > 0.002) {
                    std::this_thread::sleep_for(std::chrono::microseconds(static_cast<int64_t>((sleepTime - 0.001) * 1e6)));
                }
                while (std::chrono::duration<double>(std::chrono::high_resolution_clock::now() - frameStart).count() < targetFrameTime) {
                    CPURelax();
                }
            }
        }
    }

    TaskSystem::Shutdown();
    return {}; // Success!
}

} // namespace ZHLN
