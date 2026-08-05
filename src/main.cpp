// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// File: src/main.cpp
#include "engine/Platform.hpp"
#include "physics/PhysicsWorld.hpp"
#include <GLFW/glfw3.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Zahlen/Audio.hpp>
#include <Zahlen/Camera.hpp>
#include <Zahlen/Clock.hpp>
#include <Zahlen/CommandLine.hpp>
#include <Zahlen/Components.hpp>
#include <Zahlen/Console.hpp>
#include <Zahlen/CreativeWorksFactory.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Entity.hpp>
#include <Zahlen/Input.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Math3D.hpp>
#include <Zahlen/Profiler.hpp>
#include <Zahlen/Render.hpp>
#include <Zahlen/Scripting.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <Zahlen/Window.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <Zahlen/physics/Physics.hpp>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <format>
#include <imgui.h>
#include <print>

extern std::vector<std::string> s_InvShellLog;
extern bool                     s_InvScrollToBottom;

namespace {

// ============================================================================
// CONSOLE & SHELL UI
// ============================================================================

void TextAnsi(const std::string& line) {
    ImVec4 color      = ImVec4(0.9f, 0.9f, 0.9f, 1.0f);
    size_t pos        = 0;
    bool   hasPrinted = false;

    while (pos < line.size()) {
        size_t esc = line.find("\x1b[", pos);
        if (esc == std::string::npos) {
            if (hasPrinted) {
                ImGui::SameLine(0.0f, 0.0f);
            }
            ImGui::TextColored(color, "%s", line.substr(pos).c_str());
            break;
        }

        if (esc > pos) {
            if (hasPrinted) {
                ImGui::SameLine(0.0f, 0.0f);
            }
            ImGui::TextColored(color, "%s", line.substr(pos, esc - pos).c_str());
            hasPrinted = true;
        }

        size_t m = line.find('m', esc);
        if (m == std::string::npos) {
            if (hasPrinted) {
                ImGui::SameLine(0.0f, 0.0f);
            }
            ImGui::TextColored(color, "%s", line.substr(esc).c_str());
            break;
        }

        std::string code = line.substr(esc + 2, m - (esc + 2));
        if (code == "0" || code == "39") {
            color = ImVec4(0.9f, 0.9f, 0.9f, 1.0f);
        } else if (code == "31") {
            color = ImVec4(1.0f, 0.3f, 0.3f, 1.0f);
        } else if (code == "32") {
            color = ImVec4(0.3f, 1.0f, 0.3f, 1.0f);
        } else if (code == "33") {
            color = ImVec4(1.0f, 0.8f, 0.3f, 1.0f);
        } else if (code == "36") {
            color = ImVec4(0.3f, 0.8f, 1.0f, 1.0f);
        }

        pos = m + 1;
    }
}

int ConsoleInputCallback(ImGuiInputTextCallbackData* data) {
    if (data->EventFlag == ImGuiInputTextFlags_CallbackHistory) {
        size_t historyCount = ZHLN::GameConsole::GetHistoryCount();
        int&   pos          = ZHLN::GameConsole::HistoryPos();
        int    prev_pos     = pos;

        if (data->EventKey == ImGuiKey_UpArrow) {
            if (pos == -1) {
                pos = (int) historyCount - 1;
            } else if (pos > 0) {
                pos--;
            }
        } else if (data->EventKey == ImGuiKey_DownArrow) {
            if (pos != -1) {
                if (++pos >= (int) historyCount) {
                    pos = -1;
                }
            }
        }

        if (prev_pos != pos) {
            std::string_view history_str = (pos >= 0) ? ZHLN::GameConsole::GetHistoryItem(pos) : "";
            data->DeleteChars(0, data->BufTextLen);
            data->InsertChars(0, history_str.data(), history_str.data() + history_str.size());
        }
    }
    return 0;
}

void DrawConsole(ZHLN::Engine& engine) {
    ImGui::SetNextWindowSize({520, 400}, ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Lua Console")) {
        ImGui::End();
        return;
    }

    if (ImGui::Button("Provoke GPU Hang")) {
        engine.ProvokeDeviceLost();
    }
    ImGui::SameLine();

    const float footer_height = ImGui::GetStyle().ItemSpacing.y + ImGui::GetFrameHeightWithSpacing();
    ImGui::BeginChild("ScrollingRegion", ImVec2(0, -footer_height), false);

    size_t entryCount = ZHLN::GameConsole::GetEntryCount();
    for (size_t i = 0; i < entryCount; ++i) {
        std::string_view text;
        float            r, g, b, a;
        ZHLN::GameConsole::GetEntry(i, text, r, g, b, a);
        ImGui::TextColored(ImVec4(r, g, b, a), "%.*s", (int) text.size(), text.data());
    }

    if (ZHLN::GameConsole::ConsumeScroll()) {
        ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();
    ImGui::Separator();

    static char InputBuf[256] = "";
    if (ImGui::InputText(
            "##ConsoleInput", InputBuf, IM_ARRAYSIZE(InputBuf), ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackHistory, ConsoleInputCallback
        )) {
        std::string cmd = InputBuf;
        if (!cmd.empty()) {
            ZHLN::GameConsole::Log("> " + cmd, {.r = 1.0f, .g = 1.0f, .b = 1.0f, .a = 1.0f});
            ZHLN::GameConsole::AddHistory(cmd);
            engine.GetScriptRunner().ExecuteString(cmd);
        }
        InputBuf[0] = '\0';
        ImGui::SetKeyboardFocusHere(-1);
    }

    ImGui::End();
}

void DrawInventoryShell(ZHLN::Engine& engine) {
    ImGui::SetNextWindowSize({500, 350}, ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Inventory Terminal (Subshell)")) {
        ImGui::End();
        return;
    }

    if (s_InvShellLog.empty()) {
        s_InvShellLog.emplace_back("Inventory Subshell v1.0.0");
        s_InvShellLog.emplace_back("Type 'ls' to view files, 'cd <dir>' to navigate, 'cat <file>' to read.");
        s_InvShellLog.emplace_back("------------------------------------------------------------------");
    }

    const float footer_height = ImGui::GetStyle().ItemSpacing.y + ImGui::GetFrameHeightWithSpacing();
    ImGui::BeginChild("InvScrollingRegion", ImVec2(0, -footer_height), false);

    for (const auto& line: s_InvShellLog) {
        if (line.starts_with("$ ")) {
            ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "%s", line.c_str());
        } else {
            TextAnsi(line);
        }
    }

    if (s_InvScrollToBottom) {
        ImGui::SetScrollHereY(1.0f);
        s_InvScrollToBottom = false;
    }
    ImGui::EndChild();
    ImGui::Separator();

    static char InputBuf[256] = "";
    if (ImGui::InputText("##InvInput", InputBuf, IM_ARRAYSIZE(InputBuf), ImGuiInputTextFlags_EnterReturnsTrue)) {
        std::string cmd = InputBuf;
        if (!cmd.empty()) {
            s_InvShellLog.push_back("$ " + cmd);
            s_InvScrollToBottom = true;

            std::println(stdout, "[InvShell Input] $ {}", cmd);
            std::fflush(stdout);

            std::string luaCall = "run_inventory_command('" + cmd + "')";
            engine.GetScriptRunner().ExecuteString(luaCall);
        }
        InputBuf[0] = '\0';
        ImGui::SetKeyboardFocusHere(-1);
    }
    ImGui::End();
}

// ============================================================================
// PROFILER UI
// ============================================================================

void DrawProfiler(ZHLN::Engine& engine) {
    if (ImGui::Begin("Zahlen Profiler")) {
        if (ImGui::CollapsingHeader("Performance Timings", ImGuiTreeNodeFlags_DefaultOpen)) {
            ZHLN::CPUProfiler::IterateMetrics(
                [](const char* name, float cpuTimeMS, float rollingAverageMS, const float* history, size_t historyCount, void*) {
                    bool isGpu = std::string_view(name).starts_with("[GPU]");
                    if (isGpu) {
                        ImGui::TextColored(ImVec4(0.3f, 0.8f, 1.0f, 1.0f), "%-25s: %.3f ms (Avg: %.3f)", name, cpuTimeMS, rollingAverageMS);
                    } else {
                        ImGui::Text("%-25s: %.3f ms (Avg: %.3f)", name, cpuTimeMS, rollingAverageMS);
                    }

                    std::string label = "##" + std::string(name);
                    if (historyCount > 0) {
                        ImGui::PlotLines(label.c_str(), history, (int) historyCount, 0, nullptr, 0.0f, 16.0f, ImVec2(0, 30));
                    }
                },
                nullptr
            );
        }

        if (ImGui::CollapsingHeader("Physics Stats")) {
            auto& pc = engine.GetPhysicsContext();
            ImGui::BulletText("Active Bodies: %u", pc.GetActiveBodyCount());
            ImGui::BulletText("Total Bodies: %zu", pc.GetWorld().count.load());
        }

        if (ImGui::CollapsingHeader("Memory Usage")) {
            size_t physicsMem = engine.GetPhysicsContext().GetMemoryUsage();
            float  mb         = physicsMem / (1024.0f * 1024.0f);
            ImGui::Text("Physics Temp Allocator: %.2f MB", mb);
            ImGui::Text("ECS Entities: %zu", engine.GetRegistry().GetEntitiesWith<ZHLN::Components::PhysicsComponent>().size());
        }

        if (ImGui::CollapsingHeader("Vulkan Info")) {
            auto& rc = engine.GetRenderContext();
            ImGui::Text("GPU: %s", rc.GetGPUName());
            ImGui::Text("API: %s", rc.GetRendererName());
            auto size = engine.GetWindow().GetSize();
            ImGui::Text("Resolution: %ux%u", size.width, size.height);
        }

        if (ImGui::CollapsingHeader("Frustum Culling", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Checkbox("Enable Culling", &ZHLN::CullingStats::EnableCulling);
            ImGui::Checkbox("Freeze Frustum", &ZHLN::CullingStats::FreezeFrustum);

            ImGui::Text("Total Objects:    %u", ZHLN::CullingStats::TotalObjects);
            ImGui::Text("Objects Rendered: %u", ZHLN::CullingStats::TotalObjects - ZHLN::CullingStats::CulledObjects);
            ImGui::Text("Objects Culled:   %u", ZHLN::CullingStats::CulledObjects);

            ImGui::Separator();

            ImGui::Text("Total Triangles:    %u", ZHLN::CullingStats::TotalTriangles);
            ImGui::Text("Rendered Triangles: %u", ZHLN::CullingStats::RenderedTriangles);

            float culledRatio = (ZHLN::CullingStats::TotalObjects > 0) ? (float) ZHLN::CullingStats::CulledObjects / (float) ZHLN::CullingStats::TotalObjects :
                                                                         0.0f;

            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.2f, 0.7f, 0.2f, 1.0f));
            ImGui::ProgressBar(culledRatio, ImVec2(-1, 0), std::format("{:.1f}%% Culled", culledRatio * 100.0f).c_str());
            ImGui::PopStyleColor();
        }

        if (ImGui::CollapsingHeader("Anti-Aliasing", ImGuiTreeNodeFlags_DefaultOpen)) {
            auto aaEnts = engine.GetRegistry().GetEntitiesWith<ZHLN::Components::AASettingsComponent>();
            if (!aaEnts.empty()) {
                auto* aaSettings = engine.GetRegistry().Get<ZHLN::Components::AASettingsComponent>(aaEnts[0]);

                const char* aaModesList[]  = {"Disabled", "FXAA (Fast Approximate)", "MLAA (Morphological)", "TAA (Temporal)", "SMAA (Subpixel Morphological)"};
                int         currentModeIdx = static_cast<int>(aaSettings->state.mode);

                if (ImGui::Combo("AA Method", &currentModeIdx, aaModesList, IM_ARRAYSIZE(aaModesList))) {
                    aaSettings->state.mode = static_cast<ZHLN::AAMode>(currentModeIdx);
                }

                if (aaSettings->state.mode == ZHLN::AAMode::TAA) {
                    ImGui::SliderFloat("TAA Blend", &aaSettings->state.taaFeedback, 0.80f, 0.99f, "%.2f");
                } else if (aaSettings->state.mode == ZHLN::AAMode::FXAA) {
                    ImGui::SliderFloat("FXAA Subpixel Blend", &aaSettings->state.fxaaSubpix, 0.0f, 1.0f, "%.2f");
                    ImGui::SliderFloat("FXAA Edge Threshold", &aaSettings->state.fxaaEdgeThreshold, 0.063f, 0.333f, "%.3f");
                } else if (aaSettings->state.mode == ZHLN::AAMode::MLAA) {
                    ImGui::SliderFloat("MLAA Edge Threshold", &aaSettings->state.mlaaThreshold, 0.05f, 0.25f, "%.3f");
                    int steps = static_cast<int>(aaSettings->state.mlaaMaxSearchSteps);
                    if (ImGui::SliderInt("MLAA Max Search Steps", &steps, 4, 32)) {
                        aaSettings->state.mlaaMaxSearchSteps = static_cast<uint32_t>(steps);
                    }
                }
            }
        }

        struct BudgetPayload {
            float cpuTotal = 0.0f;
            float gpuTotal = 0.0f;
        } totals;

        ZHLN::CPUProfiler::IterateMetrics(
            [](const char* name, float cpuTimeMS, float, const float*, size_t, void* userData) {
                auto* b = static_cast<BudgetPayload*>(userData);
                if (std::string_view(name).starts_with("[GPU]")) {
                    b->gpuTotal += cpuTimeMS;
                } else {
                    b->cpuTotal += cpuTimeMS;
                }
            },
            &totals
        );

        ImGui::SeparatorText("Performance Overview");

        float fps       = ImGui::GetIO().Framerate;
        float frameTime = 1000.0f / fps;

        ImGui::Text("FPS: %.1f", fps);
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "(%.2f ms/frame)", frameTime);

        float  cpuPercent = totals.cpuTotal / 16.66f;
        ImVec4 cpuColor   = cpuPercent > 0.9f ? ImVec4(1, 0.3f, 0.3f, 1) : ImVec4(0.3f, 1, 0.3f, 1);
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, cpuColor);
        ImGui::ProgressBar(cpuPercent, ImVec2(-1, 20), std::format("CPU Profiled: {:.2f} ms / 16.6ms", totals.cpuTotal).c_str());
        ImGui::PopStyleColor();

        float  gpuPercent = totals.gpuTotal / 16.66f;
        ImVec4 gpuColor   = gpuPercent > 0.9f ? ImVec4(1, 0.3f, 0.3f, 1) : ImVec4(0.3f, 0.8f, 1.0f, 1);
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, gpuColor);
        ImGui::ProgressBar(gpuPercent, ImVec2(-1, 20), std::format("GPU Profiled: {:.2f} ms / 16.6ms", totals.gpuTotal).c_str());
        ImGui::PopStyleColor();

        static float fps_history[100] = {};
        static int   offset           = 0;
        fps_history[offset]           = fps;
        offset                        = (offset + 1) % 100;

        ImGui::PlotHistogram("##FPS", fps_history, 100, offset, nullptr, 0.0f, 250.0f, ImVec2(-1, 40));

        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Wall-clock time (%.2fms) includes driver overhead and VSync wait.", frameTime);
        }
    }
    ImGui::End();
}

void DrawECSProfiler() {
    if (ImGui::Begin("Zahlen ECS Profiler")) {
        ImGui::SeparatorText("ECS Systems CPU Execution");

        ZHLN::CPUProfiler::IterateMetrics(
            [](const char* name, float cpuTimeMS, float rollingAverageMS, const float* history, size_t historyCount, void*) {
                std::string_view metricName(name);
                if (metricName.starts_with("ECS System:")) {
                    metricName.remove_prefix(12);

                    ImGui::Text("%-30.*s: %.3f ms (Avg: %.3f)", (int) metricName.size(), metricName.data(), cpuTimeMS, rollingAverageMS);

                    std::string label = "##ECS_" + std::string(metricName);
                    if (historyCount > 0) {
                        ImGui::PlotLines(label.c_str(), history, (int) historyCount, 0, nullptr, 0.0f, 4.0f, ImVec2(-1, 30));
                    }
                }
            },
            nullptr
        );
    }
    ImGui::End();
}

// ============================================================================
// COMPASS / GIZMO UI
// ============================================================================

void DrawOrientationGizmo(const ZHLN::Camera& cam) {
    ImGui::SetNextWindowSize({110, 110}, ImGuiCond_Always);
    ImGuiWindowFlags windowFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoBackground;

    if (!ImGui::Begin("Direction Compass", nullptr, windowFlags)) {
        ImGui::End();
        return;
    }

    ImDrawList* drawList  = ImGui::GetWindowDrawList();
    ImVec2      windowPos = ImGui::GetWindowPos();
    ImVec2      size      = ImGui::GetWindowSize();

    ImVec2 center(windowPos.x + size.x * 0.5f, windowPos.y + size.y * 0.5f);
    float  radius = size.x * 0.35f;

    drawList->AddCircle(center, radius, IM_COL32(100, 100, 100, 100), 32, 1.0f);

    JPH::Mat44 view = cam.GetViewMatrix();

    JPH::Vec4 rawX = view * JPH::Vec4(1.0f, 0.0f, 0.0f, 0.0f);
    JPH::Vec4 rawY = view * JPH::Vec4(0.0f, 1.0f, 0.0f, 0.0f);
    JPH::Vec4 rawZ = view * JPH::Vec4(0.0f, 0.0f, 1.0f, 0.0f);

    JPH::Vec3 rotatedX(rawX.GetX(), rawX.GetY(), rawX.GetZ());
    JPH::Vec3 rotatedY(rawY.GetX(), rawY.GetY(), rawY.GetZ());
    JPH::Vec3 rotatedZ(rawZ.GetX(), rawZ.GetY(), rawZ.GetZ());

    ImVec2 ptX(center.x + rotatedX.GetX() * radius, center.y - rotatedX.GetY() * radius);
    ImVec2 ptY(center.x + rotatedY.GetX() * radius, center.y - rotatedY.GetY() * radius);
    ImVec2 ptZ(center.x + rotatedZ.GetX() * radius, center.y - rotatedZ.GetY() * radius);

    drawList->AddLine(center, ptX, IM_COL32(255, 75, 75, 255), 2.5f);
    drawList->AddLine(center, ptY, IM_COL32(75, 255, 75, 255), 2.5f);
    drawList->AddLine(center, ptZ, IM_COL32(75, 75, 255, 255), 2.5f);

    drawList->AddText(ImVec2(ptX.x + 3, ptX.y - 6), IM_COL32(255, 120, 120, 255), "X");
    drawList->AddText(ImVec2(ptY.x + 3, ptY.y - 6), IM_COL32(120, 255, 120, 255), "Y");
    drawList->AddText(ImVec2(ptZ.x + 3, ptZ.y - 6), IM_COL32(120, 120, 255, 255), "Z");

    ImGui::End();
}

// ============================================================================
// MAIN DIAGNOSTIC UI SYSTEM
// ============================================================================

void UISystem(ZHLN::Engine& engine) {
    if (engine.GetWindow().IsTTY()) {
        return;
    }

    DrawConsole(engine);
    DrawInventoryShell(engine);
    DrawProfiler(engine);
    DrawOrientationGizmo(engine.GetCamera());
    DrawECSProfiler();

    auto& reg = engine.GetRegistry();

    auto                                       shadowSettingsEntities = reg.GetEntitiesWith<ZHLN::Components::ShadowSettingsComponent>();
    ZHLN::Components::ShadowSettingsComponent* shadowSettings         = nullptr;
    if (!shadowSettingsEntities.empty()) {
        shadowSettings = reg.Get<ZHLN::Components::ShadowSettingsComponent>(shadowSettingsEntities[0]);
    }

    if (shadowSettings != nullptr) {
        ImGui::Begin("Lighting Workspace Controller");
        ImGui::SeparatorText("Global Shadow Settings");

        ImGui::DragFloat("Shadow Width", &shadowSettings->shadowWidth, 1.0f, 10.0f, 500.0f, "%.1f m");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Orthographic width of the directional cascade volume.");
        }

        const char* resolutions[] = {"512", "1024", "2048", "4096"};
        int         currentResIdx = 2;
        if (shadowSettings->shadowResolution == 512) {
            currentResIdx = 0;
        } else if (shadowSettings->shadowResolution == 1024) {
            currentResIdx = 1;
        } else if (shadowSettings->shadowResolution == 2048) {
            currentResIdx = 2;
        } else if (shadowSettings->shadowResolution == 4096) {
            currentResIdx = 3;
        }

        if (ImGui::Combo("Shadow Map Resolution", &currentResIdx, resolutions, IM_ARRAYSIZE(resolutions))) {
            int newRes = 2048;
            if (currentResIdx == 0) {
                newRes = 512;
            } else if (currentResIdx == 1) {
                newRes = 1024;
            } else if (currentResIdx == 2) {
                newRes = 2048;
            } else if (currentResIdx == 3) {
                newRes = 4096;
            }

            shadowSettings->shadowResolution = newRes;

            if (auto res = engine.GetRenderContext().SetShadowResolution(newRes); !res) {
                ZHLN::Log("ERROR: Failed to update shadow resolution: {}", res.error().Message());
            }
        }

        ImGui::DragFloat("Raytraced Sun Softness", &shadowSettings->sunSize, 0.005f, 0.001f, 0.05f, "%.3f");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Angular size of the sun for ray-traced penumbra softness.");
        }
        ImGui::End();
    }

    ImGui::SetNextWindowBgAlpha(0.35f);
    ImGuiWindowFlags hudFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                                ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;

    ImGui::SetNextWindowPos({10.0f, 50.0f}, ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Coordinates HUD", nullptr, hudFlags)) {
        ZHLN::Entity playerEnt = ZHLN::NullEntity;
        for (ZHLN::Entity e: reg.GetEntitiesWith<ZHLN::Components::MovementComponent>()) {
            playerEnt = e;
            break;
        }

        if (playerEnt != ZHLN::NullEntity) {
            if (auto* trans = reg.Get<ZHLN::Components::TransformComponent>(playerEnt)) {
                ImGui::Text("Player Pos:  X: %.2f, Y: %.2f, Z: %.2f", trans->position.GetX(), trans->position.GetY(), trans->position.GetZ());
            }
        } else {
            ImGui::Text("Player Pos:  [Not Found]");
        }

        auto& cam = engine.GetCamera();
        ImGui::Text("Camera Pos:  X: %.2f, Y: %.2f, Z: %.2f", cam.position.GetX(), cam.position.GetY(), cam.position.GetZ());
        ImGui::Text("Camera Rot:  Yaw: %.1f, Pitch: %.1f", cam.yaw, cam.pitch);
    }
    ImGui::End();

    auto settingsEntities = reg.GetEntitiesWith<ZHLN::Components::GlobalSettingsTagComponent>();
    if (settingsEntities.empty()) {
        return;
    }

    ZHLN::Entity settingsEnt = settingsEntities[0];
    auto*        pp          = reg.Get<ZHLN::Components::PostProcessSettingsComponent>(settingsEnt);
    auto*        dbg         = reg.Get<ZHLN::Components::DebugSettingsComponent>(settingsEnt);

    if ((pp == nullptr) || (dbg == nullptr)) {
        return;
    }

    ImGui::Begin("Lighting Workspace Controller");

    ImGui::SeparatorText("Punctual Shadows (Raster Fallback)");
    if (shadowSettings) {
        ImGui::SliderInt("Max Punctual Shadows", &shadowSettings->maxPunctualShadows, 0, 4);
        if (shadowSettings->maxPunctualShadows > 0) {
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "  [Rendering %d shadow-casting light(s)]", shadowSettings->maxPunctualShadows);
        } else {
            ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "  [Punctual shadows disabled (0ms overhead)]");
        }
    }

    ImGui::SeparatorText("Dynamic Sky Gradient");
    std::array<float, 3> zenith  = {pp->skyZenith.GetX(), pp->skyZenith.GetY(), pp->skyZenith.GetZ()};
    std::array<float, 3> horizon = {pp->skyHorizon.GetX(), pp->skyHorizon.GetY(), pp->skyHorizon.GetZ()};
    std::array<float, 3> ground  = {pp->skyGround.GetX(), pp->skyGround.GetY(), pp->skyGround.GetZ()};

    if (ImGui::ColorEdit3("Sky Zenith (Top)", zenith.data())) {
        pp->skyZenith = JPH::Vec4(zenith[0], zenith[1], zenith[2], 1.0f);
    }
    if (ImGui::ColorEdit3("Sky Horizon (Middle)", horizon.data())) {
        pp->skyHorizon = JPH::Vec4(horizon[0], horizon[1], horizon[2], 1.0f);
    }
    if (ImGui::ColorEdit3("Sky Ground (Bottom)", ground.data())) {
        pp->skyGround = JPH::Vec4(ground[0], ground[1], ground[2], 1.0f);
    }

    auto camEnts = reg.GetEntitiesWith<ZHLN::Components::MainCameraTagComponent>();
    if (!camEnts.empty()) {
        ZHLN::Entity camEnt    = camEnts[0];
        bool         isFreeCam = (reg.Get<ZHLN::Components::FreeCamTagComponent>(camEnt) != nullptr);

        ImGui::SeparatorText("Camera Controls");
        if (ImGui::Checkbox("Free Cam Mode (Fly)", &isFreeCam)) {
            if (isFreeCam) {
                reg.Add(camEnt, ZHLN::Components::FreeCamTagComponent {});
            } else {
                reg.Remove<ZHLN::Components::FreeCamTagComponent>(camEnt);
            }
        }
        if (isFreeCam) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1.0f), "  [Hold Right-Click + WASD to fly]");
        }
    }

    ImGui::SeparatorText("Physics Debug");
    ImGui::RadioButton("Hidden", &dbg->physicsDrawMode, 0);
    ImGui::SameLine();
    ImGui::RadioButton("Wireframe", &dbg->physicsDrawMode, 1);
    ImGui::SameLine();
    ImGui::RadioButton("Solid", &dbg->physicsDrawMode, 2);
    ImGui::Text("PBR Materials & Lights Controller");
    ImGui::Separator();

    ZHLN::Components::PBRComponent* floorPbr = nullptr;
    for (ZHLN::Entity e: reg.GetEntitiesWith<ZHLN::Components::PBRComponent>()) {
        if (auto* nameComp = reg.Get<ZHLN::Components::NameComponent>(e)) {
            std::string nameLower(nameComp->name.c_str());
            std::ranges::transform(nameLower, nameLower.begin(), ::tolower);
            if (nameLower.contains("floor") || nameLower.contains("ground") || nameLower.contains("lobby")) {
                floorPbr = reg.Get<ZHLN::Components::PBRComponent>(e);
                break;
            }
        }
    }

    if (floorPbr != nullptr) {
        ImGui::SliderFloat("Floor Roughness", &floorPbr->roughness, 0.0f, 1.0f);
        ImGui::SliderFloat("Floor Metallic", &floorPbr->metallic, 0.0f, 1.0f);
    }

    int lightIdx = 1;
    for (ZHLN::Entity e: reg.GetEntitiesWith<ZHLN::Components::LightComponent>()) {
        if (auto* light = reg.Get<ZHLN::Components::LightComponent>(e)) {
            if (light->type == ZHLN::LightType::Point) {
                std::string labelInt = std::format("Point Light {} Intensity", lightIdx);
                std::string labelRad = std::format("Point Light {} Radius", lightIdx);
                ImGui::SliderFloat(labelInt.c_str(), &light->intensity, 0.0f, 500.0f);
                ImGui::SliderFloat(labelRad.c_str(), &light->radius, 0.0f, 5.0f);
                lightIdx++;
            }
        }
    }

    ImGui::SeparatorText("Parallax-Corrected Local Reflection Probe");
    bool useProbe = pp->useLocalProbe != 0;
    if (ImGui::Checkbox("Enable Box Projection", &useProbe)) {
        pp->useLocalProbe = useProbe ? 1 : 0;
    }
    if (pp->useLocalProbe != 0) {
        std::array<float, 3> minArr = {pp->probeMin.GetX(), pp->probeMin.GetY(), pp->probeMin.GetZ()};
        std::array<float, 3> maxArr = {pp->probeMax.GetX(), pp->probeMax.GetY(), pp->probeMax.GetZ()};
        std::array<float, 3> posArr = {pp->probePos.GetX(), pp->probePos.GetY(), pp->probePos.GetZ()};

        if (ImGui::DragFloat3("Box Min", minArr.data(), 0.1f, -100.0f, 100.0f, "%.1fm")) {
            pp->probeMin = JPH::Vec3(minArr[0], minArr[1], minArr[2]);
        }
        if (ImGui::DragFloat3("Box Max", maxArr.data(), 0.1f, -100.0f, 100.0f, "%.1fm")) {
            pp->probeMax = JPH::Vec3(maxArr[0], maxArr[1], maxArr[2]);
        }
        if (ImGui::DragFloat3("Probe Position", posArr.data(), 0.1f, -100.0f, 100.0f, "%.1fm")) {
            pp->probePos = JPH::Vec3(posArr[0], posArr[1], posArr[2]);
        }
    }

    ImGui::SeparatorText("Ambient Occlusion & Global Illumination");
    constexpr std::array<const char*, 5> giModesList = {
        "Off", "SSAO (Ambient Occlusion)", "SSGI (Screen Space GI)", "HBAO (Horizon-Based AO)", "GTAO (Ground Truth AO)"
    };
    ImGui::Combo("GI Mode", &pp->giMode, giModesList.data(), static_cast<int>(giModesList.size()));

    if (pp->giMode == 1) {
        ImGui::SliderFloat("AO Radius", &pp->aoRadius, 0.05f, 2.5f, "%.2fm");
        ImGui::SliderFloat("AO Bias", &pp->aoBias, 0.001f, 0.2f, "%.3f");
        ImGui::SliderFloat("AO Contrast", &pp->aoPower, 0.5f, 5.0f, "%.1fx");
        ImGui::SliderInt("AO Samples", &pp->giSamples, 2, 32);
    } else if (pp->giMode == 2) {
        ImGui::SliderFloat("Bounce Radius", &pp->aoRadius, 0.05f, 2.5f, "%.2fm");
        ImGui::SliderFloat("Bounce Bias", &pp->aoBias, 0.001f, 0.2f, "%.3f");
        ImGui::SliderFloat("GI Bounce Intensity", &pp->giIntensity, 0.1f, 5.0f, "%.1fx");
        ImGui::SliderInt("GI Samples", &pp->giSamples, 2, 32);
    } else if (pp->giMode == 3 || pp->giMode == 4) {
        ImGui::SliderFloat("Search Radius", &pp->aoRadius, 0.05f, 3.0f, "%.2fm");
        ImGui::SliderFloat("Acne Bias", &pp->aoBias, 0.001f, 0.2f, "%.3f");
        ImGui::SliderFloat("Shadow Contrast", &pp->aoPower, 0.5f, 6.0f, "%.1fx");
        ImGui::SliderInt("Search Steps", &pp->giSamples, 4, 32);
    }

    ImGui::SeparatorText("Camera Vignette");
    ImGui::SliderFloat("Vignette Intensity", &pp->vignetteIntensity, 0.0f, 2.5f, "%.2f");
    if (pp->vignetteIntensity > 0.0f) {
        ImGui::SliderFloat("Vignette Power", &pp->vignettePower, 0.1f, 6.0f, "%.2f");
    }

    bool useSsr = pp->enableSSR != 0;
    if (ImGui::Checkbox("Enable SSR", &useSsr)) {
        pp->enableSSR = useSsr ? 1 : 0;
    }

    bool useRtr = pp->enableRTR != 0;
    if (ImGui::Checkbox("Enable Hardware RTR", &useRtr)) {
        pp->enableRTR = useRtr ? 1 : 0;
    }

    bool useFullBright = pp->fullBright != 0;
    if (ImGui::Checkbox("Fullbright Mode (Disable Lighting/Shadows)", &useFullBright)) {
        pp->fullBright = useFullBright ? 1 : 0;
    }

    ImGui::End();
}

// ============================================================================
// WORLD EDITOR
// ============================================================================

struct EditorState {
    bool         simulationRunning = false;
    ZHLN::Entity selectedEntity    = ZHLN::NullEntity;
    bool         freeCamActive     = true;
    float        freeCamSpeed      = 25.0f;
};

static EditorState s_EditorState;

void UpdateEditorCamera(ZHLN::Camera& cam, const ZHLN::InputContext& input, float dt) {
    const float sensitivity = 0.15f;

    if (input.IsMouseButtonDown(ZHLN::KeyCode::RButton)) {
        cam.yaw += input.GetMouse().deltaX * sensitivity;
        cam.pitch = std::clamp(cam.pitch - (input.GetMouse().deltaY * sensitivity), -89.0f, 89.0f);
    }

    float yawRad   = JPH::DegreesToRadians(cam.yaw);
    float pitchRad = JPH::DegreesToRadians(cam.pitch);

    JPH::Vec3 forward(JPH::Cos(yawRad) * JPH::Cos(pitchRad), JPH::Sin(pitchRad), JPH::Sin(yawRad) * JPH::Cos(pitchRad));
    forward         = forward.Normalized();
    JPH::Vec3 right = forward.Cross(JPH::Vec3::sAxisY()).Normalized();

    float moveSpeed = input.IsKeyDown(ZHLN::KeyCode::LShift) ? (s_EditorState.freeCamSpeed * 2.5f) : s_EditorState.freeCamSpeed;

    JPH::Vec3 moveDirection = JPH::Vec3::sZero();
    if (input.IsKeyDown(ZHLN::KeyCode::W)) {
        moveDirection += forward;
    }
    if (input.IsKeyDown(ZHLN::KeyCode::S)) {
        moveDirection -= forward;
    }
    if (input.IsKeyDown(ZHLN::KeyCode::A)) {
        moveDirection -= right;
    }
    if (input.IsKeyDown(ZHLN::KeyCode::D)) {
        moveDirection += right;
    }

    if (moveDirection.LengthSq() > 0.0f) {
        cam.position += moveDirection.Normalized() * moveSpeed * dt;
    }
}

ZHLN::Physics::RaycastResult CastPickingRay(ZHLN::Engine& engine, const ZHLN::Camera& cam) {
    const auto& input   = engine.GetInput();
    auto        mouse   = input.GetMouse();
    auto        winSize = engine.GetWindow().GetSize();

    if (winSize.width == 0 || winSize.height == 0) {
        return {};
    }

    float ndcX   = (2.0f * mouse.x) / (float) winSize.width - 1.0f;
    float ndcY   = 1.0f - (2.0f * mouse.y) / (float) winSize.height;
    float aspect = (float) winSize.width / (float) winSize.height;

    JPH::Mat44 invVP = (cam.GetProjectionMatrix(aspect) * cam.GetViewMatrix()).Inversed();

    JPH::Vec4 nearWorld = invVP * JPH::Vec4(ndcX, ndcY, 0.0f, 1.0f);
    JPH::Vec4 farWorld  = invVP * JPH::Vec4(ndcX, ndcY, 1.0f, 1.0f);

    JPH::Vec3 pNear = JPH::Vec3(nearWorld.GetX() / nearWorld.GetW(), nearWorld.GetY() / nearWorld.GetW(), nearWorld.GetZ() / nearWorld.GetW());
    JPH::Vec3 pFar  = JPH::Vec3(farWorld.GetX() / farWorld.GetW(), farWorld.GetY() / farWorld.GetW(), farWorld.GetZ() / farWorld.GetW());
    JPH::Vec3 dir   = (pFar - pNear).Normalized();

    return ZHLN::Physics::Raycast(engine.GetPhysicsContext(), JPH::RVec3(pNear), dir, 1000.0f);
}

void DrawEditorPanels(ZHLN::Engine& engine, const ZHLN::CommandLineOptions& options) {
    auto&       reg   = engine.GetRegistry();
    auto&       pc    = engine.GetPhysicsContext();
    const auto& world = pc.GetWorld();

    auto                                            settingsEntities = reg.GetEntitiesWith<ZHLN::Components::GlobalSettingsTagComponent>();
    ZHLN::Components::PostProcessSettingsComponent* pp               = nullptr;

    if (!settingsEntities.empty()) {
        pp = reg.Get<ZHLN::Components::PostProcessSettingsComponent>(settingsEntities[0]);
    }

    ImGui::Begin("Toolbar", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove);
    ImGui::SetWindowPos({0, 0});
    ImGui::SetWindowSize({(float) engine.GetWindow().GetSize().width, 42.0f});

    if (ImGui::Button(s_EditorState.simulationRunning ? "PAUSE" : "PLAY")) {
        s_EditorState.simulationRunning = !s_EditorState.simulationRunning;
    }
    ImGui::SameLine();
    if (ImGui::Button("Step Frame")) {
        engine.Tick(1.0f / 60.0f, options.driver);
    }
    ImGui::SameLine();

    if (pp != nullptr) {
        bool fullBright = (pp->fullBright != 0);
        if (ImGui::Checkbox("Fullbright Mode", &fullBright)) {
            pp->fullBright = fullBright ? 1 : 0;
        }
    }

    ImGui::SameLine();
    if (ImGui::Button("+ Create Empty Entity")) {
        ZHLN::Entity e = reg.Create();
        reg.Add(e, ZHLN::Components::NameComponent {.name = ZHLN::String64(std::format("New Entity {}", e.index))});
        reg.Add(e, ZHLN::Components::TransformComponent {});
        s_EditorState.selectedEntity = e;
    }
    ImGui::End();

    ImGui::Begin("Scene Hierarchy");
    for (ZHLN::Entity e: reg.GetEntitiesWith<ZHLN::Components::NameComponent>()) {
        auto*       nameComp   = reg.Get<ZHLN::Components::NameComponent>(e);
        std::string label      = std::format("{} [ID: {}, Gen: {}]", nameComp->name.c_str(), e.index, e.generation);
        bool        isSelected = (s_EditorState.selectedEntity == e);
        if (ImGui::Selectable(label.c_str(), isSelected)) {
            s_EditorState.selectedEntity = e;
        }
    }
    ImGui::End();

    ImGui::Begin("Component Inspector");
    if (s_EditorState.selectedEntity != ZHLN::NullEntity && reg.IsAlive(s_EditorState.selectedEntity)) {
        ZHLN::Entity e = s_EditorState.selectedEntity;
        ImGui::TextUnformatted(std::format("Active Entity ID: {} (Gen: {})", e.index, e.generation).c_str());
        ImGui::SameLine();
        if (ImGui::Button("Delete Entity")) {
            reg.Destroy(e);
            s_EditorState.selectedEntity = ZHLN::NullEntity;
            ImGui::End();
            return;
        }
        ImGui::Separator();

        if (auto* name = reg.Get<ZHLN::Components::NameComponent>(e)) {
            if (ImGui::CollapsingHeader("Name Component", ImGuiTreeNodeFlags_DefaultOpen)) {
                char buf[64];
                name->name.copy_to(buf);
                if (ImGui::InputText("Name", buf, sizeof(buf))) {
                    name->name.assign(buf);
                }
            }
        }

        if (auto* trans = reg.Get<ZHLN::Components::TransformComponent>(e)) {
            if (ImGui::CollapsingHeader("Transform Component", ImGuiTreeNodeFlags_DefaultOpen)) {
                std::array<float, 3> pos   = {trans->position.GetX(), trans->position.GetY(), trans->position.GetZ()};
                JPH::Vec3            euler = ZHLN::Math::QuatToEulerDegrees(trans->rotation);
                std::array<float, 3> rot   = {euler.GetX(), euler.GetY(), euler.GetZ()};
                std::array<float, 3> scale = {trans->scale.GetX(), trans->scale.GetY(), trans->scale.GetZ()};

                bool posMod   = ImGui::DragFloat3("Position", pos.data(), 0.05f);
                bool rotMod   = ImGui::DragFloat3("Rotation", rot.data(), 0.2f);
                bool scaleMod = ImGui::DragFloat3("Scale", scale.data(), 0.02f);

                if (posMod || rotMod || scaleMod) {
                    trans->position = JPH::Vec3(pos[0], pos[1], pos[2]);
                    trans->rotation = ZHLN::Math::EulerDegreesToQuat(JPH::Vec3(rot[0], rot[1], rot[2]));
                    trans->scale    = JPH::Vec3(scale[0], scale[1], scale[2]);

                    if (auto* phys = reg.Get<ZHLN::Components::PhysicsComponent>(e)) {
                        JPH::BodyID bid = ZHLN::Physics::GetBodyID(world, phys->physicsHandle);
                        if (!bid.IsInvalid()) {
                            world.bodyInterface->SetPositionAndRotation(bid, JPH::RVec3(trans->position), trans->rotation, JPH::EActivation::Activate);
                        }
                    }
                }
            }
        }

        if (auto* pbr = reg.Get<ZHLN::Components::PBRComponent>(e)) {
            if (ImGui::CollapsingHeader("PBR Component", ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::SliderFloat("Roughness", &pbr->roughness, 0.0f, 1.0f);
                ImGui::SliderFloat("Metallic", &pbr->metallic, 0.0f, 1.0f);
            }
        }

        if (auto* light = reg.Get<ZHLN::Components::LightComponent>(e)) {
            if (ImGui::CollapsingHeader("Light Component", ImGuiTreeNodeFlags_DefaultOpen)) {
                std::array<float, 3> color = {light->color.GetX(), light->color.GetY(), light->color.GetZ()};
                if (ImGui::ColorEdit3("Color", color.data())) {
                    light->color = JPH::Vec3(color[0], color[1], color[2]);
                }
                ImGui::SliderFloat("Intensity", &light->intensity, 0.0f, 2000.0f);
                ImGui::SliderFloat("Range", &light->range, 0.5f, 500.0f);
            }
        }
    } else {
        ImGui::TextUnformatted("No entity selected. Click an element in the hierarchy or viewport.");
    }
    ImGui::End();
}

int RunWorldEditor(ZHLN::Engine& engine, const ZHLN::CommandLineOptions& options) {
    ZHLN::Clock clock;
    auto&       cam = engine.GetCamera();

    cam.position = {0.0f, 20.0f, 40.0f};
    cam.yaw      = -90.0f;
    cam.pitch    = -20.0f;

    const double targetFrameTime = options.fpsLimit > 0 ? 1.0 / static_cast<double>(options.fpsLimit) : 0.0;
    auto         frameStart      = std::chrono::high_resolution_clock::now();

    ZHLN::Log("[WorldEditor] Editor session launched.");

    while (engine.IsRunning()) {
        float frameTime = clock.GetDeltaTime();
        engine.ProcessEvents();

        if (engine.GetInput().IsKeyDown(ZHLN::KeyCode::Escape)) {
            engine.GetWindow().Close();
            break;
        }

        if (!engine.GetInput().IsKeyDown(ZHLN::KeyCode::Unknown) && !engine.GetInput().IsMouseButtonDown(ZHLN::KeyCode::RButton) &&
            !ImGui::GetIO().WantCaptureMouse) {
            static bool wasMouseDown = false;
            bool        isMouseDown  = glfwGetMouseButton(static_cast<GLFWwindow*>(engine.GetWindow().GetNativeHandle()), GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;

            if (isMouseDown && !wasMouseDown) {
                auto hit                     = CastPickingRay(engine, cam);
                s_EditorState.selectedEntity = hit.hasHit ? hit.handle : ZHLN::NullEntity;
            }
            wasMouseDown = isMouseDown;
        }

        DrawEditorPanels(engine, options);

        if (engine.GetInput().NeedsResize()) {
            engine.GetRenderContext().SetResolution(engine.GetInput().GetNewSize());
            engine.GetInput().ClearResizeFlag();
            ImGui::EndFrame();
            continue;
        }

        if (s_EditorState.simulationRunning) {
            ZHLN::GameplayStatus status = engine.Tick(frameTime, options.driver);
            if (status == ZHLN::GameplayStatus::RequestQuit) {
                engine.GetWindow().Close();
                break;
            }
        } else {
            UpdateEditorCamera(cam, engine.GetInput(), frameTime);

            ZHLN::GameplayStatus status = engine.Tick(0.0f, options.driver);
            if (status == ZHLN::GameplayStatus::RequestQuit) {
                engine.GetWindow().Close();
                break;
            }
        }

        if (options.fpsLimit > 0) {
            auto   frameEnd = std::chrono::high_resolution_clock::now();
            double elapsed  = std::chrono::duration<double>(frameEnd - frameStart).count();
            if (elapsed < targetFrameTime) {
                double sleepTime = targetFrameTime - elapsed;
                if (sleepTime > 0.002) {
                    std::this_thread::sleep_for(std::chrono::microseconds(static_cast<int64_t>((sleepTime - 0.001) * 1e6)));
                }
                while (std::chrono::duration<double>(std::chrono::high_resolution_clock::now() - frameStart).count() < targetFrameTime) {
                    ZHLN::CPURelax();
                }
            }
        }
        frameStart = std::chrono::high_resolution_clock::now();
    }

    ZHLN::Log("[WorldEditor] Editor session closed.");
    return EXIT_SUCCESS;
}

} // namespace

int main(int argc, char* argv[]) {
    return ZHLN::HandleCommandLine(std::span(argv, static_cast<size_t>(argc)))
        .transform_error([](const ZHLN::Error& err) -> int {
            ZHLN::Log("CommandLine Error: {}", err.Message());
            return EXIT_FAILURE;
        })
        .and_then([](const ZHLN::CommandLineOptions& options) -> std::expected<int, int> {
            if (options.helpRequested || options.versionRequested || options.printGraphRequested) {
                return 0; // Clean exit for CLI queries
            }

            ZHLN::SetLogLevel(options.logLevel);

            if (options.launchEditor) {
                ZHLN::Platform::Init();
                ZHLN::SetupSignalHandler();
                ZHLN::TaskSystem::Init();

                uint32_t w = options.fullscreen ? 0 : 1280;
                uint32_t h = options.fullscreen ? 0 : 720;

                ZHLN::EngineConfig config {
                    .physics = {.maxBodies = 5000, .maxBodyPairs = 10000, .maxContactConstraints = 10000, .tempAllocatorSize = 64 * 1024 * 1024},
                    .render  = {
                        .appName          = "Zahlen World Editor",
                        .width            = w,
                        .height           = h,
                        .vsync            = options.vsync,
                        .fullscreen       = options.fullscreen,
                        .enableValidation = options.enableValidation,
                    },
                };

                auto engine_res = ZHLN::Engine::Create(config);
                if (!engine_res) {
                    ZHLN::Log("Error initializing Engine: {}", engine_res.error().Message());
                    return EXIT_FAILURE;
                }

                auto engine = std::move(engine_res.value());
                engine->GetWindow().Focus();
                engine->InitializeDefaultScene();

                int ret = RunWorldEditor(*engine, options);

                ZHLN::TaskSystem::Shutdown();
                return ret;
            } else {
                return ZHLN::Engine::Run(options, UISystem);
            }
        })
        .or_else([](int errorCode) -> std::expected<int, int> { return errorCode; })
        .value();
}
