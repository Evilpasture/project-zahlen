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
    static void               SetDisabled(bool disabled) noexcept {
        s_Disabled = disabled;
    }
    [[nodiscard]] static bool IsDisabled() noexcept {
        return s_Disabled;
    }

  private:
    static inline bool           s_IsActive       = false;
    static inline bool           s_Disabled       = false;
    static inline FallbackReason s_Reason         = FallbackReason::None;
    static inline char           s_DetailMsg[256] = "";

    // 3D Scene Entities
    static inline Entity s_CubeEntity = Entity::Null();
    static inline Entity s_PointLight = Entity::Null();

    // Native ECS UI Entities
    static inline Entity s_UIPopupBox = Entity::Null();
    static inline Entity s_BtnReload  = Entity::Null();
    static inline Entity s_BtnAnimate = Entity::Null();
    static inline Entity s_BtnQuit    = Entity::Null();

    static inline float s_AccumTime    = 0.0f;
    static inline bool  s_AnimateScene = true;
    static inline bool  s_PopupVisible = true;
};

} // namespace ZHLN
