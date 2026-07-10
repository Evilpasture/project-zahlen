// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Zahlen/Camera.hpp"
#include <imgui.h>
namespace ZHLN {
void DrawOrientationGizmo(const ZHLN::Camera& cam) {
    // 1. Create a small, semi-transparent floating debug window
    ImGui::SetNextWindowSize({110, 110}, ImGuiCond_Always);
    ImGuiWindowFlags windowFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoBackground;

    if (!ImGui::Begin("Direction Compass", nullptr, windowFlags)) {
        ImGui::End();
        return;
    }

    ImDrawList* drawList  = ImGui::GetWindowDrawList();
    ImVec2      windowPos = ImGui::GetWindowPos();
    ImVec2      size      = ImGui::GetWindowSize();

    // Determine the center and radius of our compass ring
    ImVec2 center(windowPos.x + size.x * 0.5f, windowPos.y + size.y * 0.5f);
    float  radius = size.x * 0.35f;

    // Draw background boundary ring
    drawList->AddCircle(center, radius, IM_COL32(100, 100, 100, 100), 32, 1.0f);

    // 2. Retrieve the camera's view matrix and extract the rotation
    JPH::Mat44 view = cam.GetViewMatrix();

    // Rotate the standard unit axes by the camera's view matrix (ignoring translation)
    JPH::Vec4 rawX = view * JPH::Vec4(1.0f, 0.0f, 0.0f, 0.0f);
    JPH::Vec4 rawY = view * JPH::Vec4(0.0f, 1.0f, 0.0f, 0.0f);
    JPH::Vec4 rawZ = view * JPH::Vec4(0.0f, 0.0f, 1.0f, 0.0f);

    JPH::Vec3 rotatedX(rawX.GetX(), rawX.GetY(), rawX.GetZ());
    JPH::Vec3 rotatedY(rawY.GetX(), rawY.GetY(), rawY.GetZ());
    JPH::Vec3 rotatedZ(rawZ.GetX(), rawZ.GetY(), rawZ.GetZ());

    // 3. Project 3D vectors onto the 2D screen plane
    // (Note: Y is negated because ImGui Y-coordinates grow downward)
    ImVec2 ptX(center.x + rotatedX.GetX() * radius, center.y - rotatedX.GetY() * radius);
    ImVec2 ptY(center.x + rotatedY.GetX() * radius, center.y - rotatedY.GetY() * radius);
    ImVec2 ptZ(center.x + rotatedZ.GetX() * radius, center.y - rotatedZ.GetY() * radius);

    // 4. Draw Axis Lines (Red = X, Green = Y, Blue = Z)
    drawList->AddLine(center, ptX, IM_COL32(255, 75, 75, 255), 2.5f); // +X Axis (Right)
    drawList->AddLine(center, ptY, IM_COL32(75, 255, 75, 255), 2.5f); // +Y Axis (Up)
    drawList->AddLine(center, ptZ, IM_COL32(75, 75, 255, 255), 2.5f); // +Z Axis (Backward)

    // 5. Add Text Labels
    drawList->AddText(ImVec2(ptX.x + 3, ptX.y - 6), IM_COL32(255, 120, 120, 255), "X");
    drawList->AddText(ImVec2(ptY.x + 3, ptY.y - 6), IM_COL32(120, 255, 120, 255), "Y");
    drawList->AddText(ImVec2(ptZ.x + 3, ptZ.y - 6), IM_COL32(120, 120, 255, 255), "Z");

    ImGui::End();
}
} // namespace ZHLN
