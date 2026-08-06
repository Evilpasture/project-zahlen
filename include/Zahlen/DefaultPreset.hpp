// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Common.h>
#include <Zahlen/Entity.hpp>
#include <cstdint>
#include <string_view>

namespace ZHLN {

class Engine;

enum class FallbackReason : uint8_t { None = 0, MissingBootScript, MissingNativeModule, ScriptExecutionError };

class ZHLN_API DefaultPreset {
  public:
    static void               BuildFallbackScene(Engine& engine, FallbackReason reason, std::string_view detailMessage = "");
    static void               Update(Engine& engine, float dt);
    [[nodiscard]] static bool IsActive() noexcept;
    static void               ClearFallback() noexcept;

  private:
    static inline bool           s_IsActive       = false;
    static inline FallbackReason s_Reason         = FallbackReason::None;
    static inline char           s_DetailMsg[256] = "";

    // 3D Scene Entities
    static inline Entity s_CubeEntity = NullEntity;
    static inline Entity s_PointLight = NullEntity;

    // Native ECS UI Entities
    static inline Entity s_UIPopupBox = NullEntity;
    static inline Entity s_BtnReload  = NullEntity;
    static inline Entity s_BtnAnimate = NullEntity;
    static inline Entity s_BtnQuit    = NullEntity;

    static inline float s_AccumTime    = 0.0f;
    static inline bool  s_AnimateScene = true;
    static inline bool  s_PopupVisible = true;
};

} // namespace ZHLN
