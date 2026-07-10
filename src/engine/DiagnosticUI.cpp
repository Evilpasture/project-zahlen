// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include <Zahlen/Camera.hpp>
#include <Zahlen/Components.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Input.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Render.hpp>
#include <Zahlen/Scripting.hpp>
#include <algorithm>
#include <ecs/ECS.hpp>
#include <imgui.h>

namespace ZHLN {
void DrawConsole(ScriptRunner& runner);
void DrawProfiler(Engine& engine);
void DrawOrientationGizmo(const ZHLN::Camera& cam);
void DrawInventoryShell(ScriptRunner& runner);
void DrawECSProfiler();
} // namespace ZHLN

namespace ZHLN {

void UISystem(Engine& engine, ScriptRunner& scriptRunner) {
    if (engine.GetWindow().IsTTY()) {
        return;
    }

    DrawConsole(scriptRunner);
    DrawInventoryShell(scriptRunner);
    DrawProfiler(engine);
    DrawOrientationGizmo(engine.GetCamera());
    DrawECSProfiler();

    auto& reg = engine.GetRegistry();

    // 1. Retrieve the Shadow Settings Component safely
    auto                                 shadowSettingsEntities = reg.GetEntitiesWith<Components::ShadowSettingsComponent>();
    Components::ShadowSettingsComponent* shadowSettings         = nullptr;
    if (!shadowSettingsEntities.empty()) {
        shadowSettings = reg.Get<Components::ShadowSettingsComponent>(shadowSettingsEntities[0]);
    }

    if (shadowSettings != nullptr) {
        ImGui::Begin("Lighting Workspace Controller");
        ImGui::SeparatorText("Global Shadow Settings");

        // --- Shadow Width Control ---
        ImGui::DragFloat("Shadow Width", &shadowSettings->shadowWidth, 1.0f, 10.0f, 500.0f, "%.1f m");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Orthographic width of the directional cascade volume.");
        }

        // --- Shadow Resolution Control ---
        const char* resolutions[] = {"512", "1024", "2048", "4096"};
        int         currentResIdx = 2; // Default to 2048
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

            // Trigger depth texture allocation changes
            if (auto res = engine.GetRenderContext().SetShadowResolution(newRes); !res) {
                ZHLN::Log("ERROR: Failed to update shadow resolution: {}", res.error());
            }
        }
        ImGui::End();
    }

    // ============================================================================
    // semi-transparent corner HUD overlay for coordinate tracking
    // ============================================================================
    ImGui::SetNextWindowBgAlpha(0.35f);
    ImGuiWindowFlags hudFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                                ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;

    ImGui::SetNextWindowPos({10.0f, 50.0f}, ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Coordinates HUD", nullptr, hudFlags)) {
        // 1. Fetch player entity position
        Entity playerEnt = NullEntity;
        for (Entity e: reg.GetEntitiesWith<Components::MovementComponent>()) {
            playerEnt = e;
            break;
        }

        if (playerEnt != NullEntity) {
            if (auto* trans = reg.Get<Components::TransformComponent>(playerEnt)) {
                ImGui::Text("Player Pos:  X: %.2f, Y: %.2f, Z: %.2f", trans->position.GetX(), trans->position.GetY(), trans->position.GetZ());
            }
        } else {
            ImGui::Text("Player Pos:  [Not Found]");
        }

        // 2. Fetch camera position & orientation
        auto& cam = engine.GetCamera();
        ImGui::Text("Camera Pos:  X: %.2f, Y: %.2f, Z: %.2f", cam.position.GetX(), cam.position.GetY(), cam.position.GetZ());
        ImGui::Text("Camera Rot:  Yaw: %.1f, Pitch: %.1f", cam.yaw, cam.pitch);
    }
    ImGui::End();

    auto settingsEntities = reg.GetEntitiesWith<Components::GlobalSettingsTagComponent>();
    if (settingsEntities.empty()) {
        return;
    }

    Entity settingsEnt = settingsEntities[0];
    auto*  pp          = reg.Get<Components::PostProcessSettingsComponent>(settingsEnt);
    auto*  dbg         = reg.Get<Components::DebugSettingsComponent>(settingsEnt);

    if ((pp == nullptr) || (dbg == nullptr)) {
        return;
    }

    ImGui::Begin("Lighting Workspace Controller");

    ImGui::SeparatorText("Punctual Shadows (Raster Fallback)");
    ImGui::SliderInt("Max Punctual Shadows", &shadowSettings->maxPunctualShadows, 0, 4);
    if (shadowSettings->maxPunctualShadows > 0) {
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "  [Rendering %d shadow-casting light(s)]", shadowSettings->maxPunctualShadows);
    } else {
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "  [Punctual shadows disabled (0ms overhead)]");
    }

    auto camEnts = reg.GetEntitiesWith<Components::MainCameraTagComponent>();
    if (!camEnts.empty()) {
        Entity camEnt    = camEnts[0];
        bool   isFreeCam = (reg.Get<Components::FreeCamTagComponent>(camEnt) != nullptr);

        ImGui::SeparatorText("Camera Controls");
        if (ImGui::Checkbox("Free Cam Mode (Fly)", &isFreeCam)) {
            if (isFreeCam) {
                reg.Add(camEnt, Components::FreeCamTagComponent {});
            } else {
                reg.Remove<Components::FreeCamTagComponent>(camEnt);
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

    Components::PBRComponent* floorPbr = nullptr;
    for (Entity e: reg.GetEntitiesWith<Components::PBRComponent>()) {
        if (auto* nameComp = reg.Get<Components::NameComponent>(e)) {
            std::string nameLower(nameComp->name.c_str());
            std::ranges::transform(nameLower, nameLower.begin(), ::tolower);
            if (nameLower.contains("floor") || nameLower.contains("ground") || nameLower.contains("lobby")) {
                floorPbr = reg.Get<Components::PBRComponent>(e);
                break;
            }
        }
    }

    if (floorPbr != nullptr) {
        ImGui::SliderFloat("Floor Roughness", &floorPbr->roughness, 0.0f, 1.0f);
        ImGui::SliderFloat("Floor Metallic", &floorPbr->metallic, 0.0f, 1.0f);
    }

    int lightIdx = 1;
    for (Entity e: reg.GetEntitiesWith<Components::LightComponent>()) {
        if (auto* light = reg.Get<Components::LightComponent>(e)) {
            if (light->type == LightType::Point) {
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

} // namespace ZHLN
