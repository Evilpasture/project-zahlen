// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// ----------------------------------------------------------------------------
// 1. GLOBAL MODULE FRAGMENT (Traditional headers go here)
// ----------------------------------------------------------------------------
module;

#include <Jolt/Jolt.h>
#include <Jolt/Math/Vec3.h>
#include <Jolt/Math/Vec4.h>
#include <Zahlen/Audio.hpp>
#include <Zahlen/Components.hpp>
#include <Zahlen/CreativeWorksFactory.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Entity.hpp>
#include <Zahlen/Log.hpp>
#include <algorithm>
#include <Zahlen/ecs/ECS.hpp>
#include <functional>
#include <string>
#include <vector>

// ----------------------------------------------------------------------------
// 2. MODULE DECLARATION & EXPORTS
// ----------------------------------------------------------------------------
export module ZHLN.MainMenu;

namespace ZHLN {

export struct MenuButtonDesc {
    std::string                  text;
    std::function<void(Engine*)> onClick;

    JPH::Vec4 normalColor  = JPH::Vec4(0.15f, 0.15f, 0.22f, 0.95f);
    JPH::Vec4 hoverColor   = JPH::Vec4(0.22f, 0.22f, 0.32f, 0.95f);
    JPH::Vec4 pressedColor = JPH::Vec4(0.10f, 0.10f, 0.15f, 0.95f);
    JPH::Vec4 textColor    = JPH::Vec4(0.90f, 0.90f, 0.90f, 1.0f);

    float width     = 200.0f;
    float height    = 40.0f;
    float textX     = 55.0f;
    float textY     = 25.0f;
    float textScale = 0.8f;
    float edgeWidth = 8.0f;
};

export struct MenuConfig {
    std::string titleLogoPrefab = "";
    JPH::RVec3  logoPosition    = JPH::RVec3(0.0f, 0.0f, -5.0f);
    std::string themeMusicPath  = "";

    JPH::Vec3 cameraPosition = JPH::Vec3(0.0f, 1.5f, 12.0f);
    float     cameraYaw      = -90.0f;
    float     cameraPitch    = 0.0f;

    float stackWidth    = 200.0f;
    float stackHeight   = 100.0f;
    float stackYOffset  = 120.0f;
    float buttonSpacing = 15.0f;

    std::vector<MenuButtonDesc> buttons;
};

export class MainMenu {
  public:
    MainMenu()  = default;
    ~MainMenu() = default;

    void Build(Engine* engine, const MenuConfig& config);
    void Update(Engine* engine, float dt);
    void Destroy(Engine* engine);

    [[nodiscard]] bool IsActive() const noexcept {
        return m_active;
    }

  private:
    struct InternalButton {
        Entity         entity = NullEntity;
        MenuButtonDesc desc;
        bool           wasHovered = false;
    };

    MenuConfig m_config;
    bool       m_active = false;

    Entity                      m_cameraEntity = NullEntity;
    Entity                      m_buttonStack  = NullEntity;
    std::vector<Entity>         m_logoEntities;
    std::vector<InternalButton> m_buttons;
    void*                       m_themeMusicHandle = nullptr;
};

// ----------------------------------------------------------------------------
// 3. MODULE IMPLEMENTATION
// ----------------------------------------------------------------------------

void MainMenu::Build(Engine* engine, const MenuConfig& config) {
    if (!engine || m_active)
        return;

    m_config  = config;
    auto& reg = engine->GetRegistry();
    auto& cam = engine->GetCamera();

    ZHLN::Log("[MainMenu Module] Constructing native C++ main menu scene...");

    // 1. Detach Camera & Freeze Player
    for (Entity camEnt: reg.GetEntitiesWith<Components::MainCameraTagComponent>()) {
        m_cameraEntity = camEnt;
        reg.Remove<Components::TargetCameraComponent>(camEnt);
        break;
    }

    for (Entity playerEnt: reg.GetEntitiesWith<Components::PlayerTagComponent>()) {
        reg.Remove<Components::MovementComponent>(playerEnt);
    }

    // 2. Lock Camera Position & Orientation
    cam.position = m_config.cameraPosition;
    cam.yaw      = m_config.cameraYaw;
    cam.pitch    = m_config.cameraPitch;

    // 3. Optional 3D Title Logo Spawn
    if (!m_config.titleLogoPrefab.empty()) {
        auto* prefab = CreativeWorksFactory::LoadModelPrefab(engine->GetRenderContext(), engine->GetCreativeWorksManager(), m_config.titleLogoPrefab);

        if (prefab != nullptr) {
            CreativeWorksFactory::SpawnParams params;
            params.position        = m_config.logoPosition;
            params.createPhysics   = false;
            params.isStaticPhysics = true;

            m_logoEntities.resize(32);
            uint32_t count = CreativeWorksFactory::InstantiatePrefab(
                engine->GetRenderContext(), reg, engine->GetPhysicsContext(), *prefab, params, m_logoEntities.data(), 32
            );
            m_logoEntities.resize(count);

            for (Entity ent: m_logoEntities) {
                reg.Add(ent, Components::PBRComponent {.roughness = 0.3f, .metallic = 0.0f});
            }
        }
    }

    // 4. Play Theme Audio
    if (!m_config.themeMusicPath.empty()) {
        m_themeMusicHandle = engine->GetAudioContext().CreateSoundInstance(m_config.themeMusicPath, false);
        if (m_themeMusicHandle != nullptr) {
            engine->GetAudioContext().PlaySoundInstance(m_themeMusicHandle);
        }
    }

    // 5. Query Active Font Atlas
    uint32_t fontIdx = 0;
    for (Entity uiEnt: reg.GetEntitiesWith<Components::UISettingsComponent>()) {
        if (auto* uiSettings = reg.Get<Components::UISettingsComponent>(uiEnt)) {
            fontIdx = uiSettings->defaultFontAtlasIdx;
            break;
        }
    }

    // 6. UI Button Stack Container
    m_buttonStack        = reg.Create();
    auto& stackRect      = reg.Add(m_buttonStack, Components::UIRectComponent {});
    stackRect.anchorMinX = 0.5f;
    stackRect.anchorMaxX = 0.5f;
    stackRect.anchorMinY = 0.5f;
    stackRect.anchorMaxY = 0.5f;
    stackRect.x          = -m_config.stackWidth * 0.5f;
    stackRect.y          = m_config.stackYOffset;
    stackRect.width      = m_config.stackWidth;
    stackRect.height     = m_config.stackHeight;

    auto& stackLayout     = reg.Add(m_buttonStack, Components::UIStackComponent {});
    stackLayout.direction = StackDirection::Vertical;
    stackLayout.spacing   = m_config.buttonSpacing;
    stackLayout.padding   = 0.0f;

    // 7. Instantiate Menu Buttons
    m_buttons.clear();
    for (const auto& btnDesc: m_config.buttons) {
        Entity btnEnt = reg.Create();

        auto& bRect          = reg.Add(btnEnt, Components::UIRectComponent {});
        bRect.parentEntity   = m_buttonStack;
        bRect.hierarchyDepth = 2;
        bRect.width          = btnDesc.width;
        bRect.height         = btnDesc.height;

        auto& bPanel     = reg.Add(btnEnt, Components::UIPanelComponent {});
        bPanel.color     = btnDesc.normalColor;
        bPanel.edgeWidth = btnDesc.edgeWidth;

        reg.Add(btnEnt, Components::UIButtonComponent {});

        auto& bText = reg.Add(btnEnt, Components::TextComponent {});
        bText.text.assign(btnDesc.text);
        bText.x         = btnDesc.textX;
        bText.y         = btnDesc.textY;
        bText.scale     = btnDesc.textScale;
        bText.fontIndex = fontIdx;
        bText.color     = btnDesc.textColor;

        m_buttons.push_back(InternalButton {.entity = btnEnt, .desc = btnDesc, .wasHovered = false});
    }

    m_active = true;
}

void MainMenu::Update(Engine* engine, float /*dt*/) {
    if (!m_active || !engine)
        return;

    auto& reg   = engine->GetRegistry();
    auto& cam   = engine->GetCamera();
    auto& audio = engine->GetAudioContext();

    // Lock camera in fixed pose
    cam.position = m_config.cameraPosition;
    cam.yaw      = m_config.cameraYaw;
    cam.pitch    = m_config.cameraPitch;

    // Process button interactions
    for (auto& btn: m_buttons) {
        auto* buttonComp = reg.Get<Components::UIButtonComponent>(btn.entity);
        auto* panelComp  = reg.Get<Components::UIPanelComponent>(btn.entity);

        if (!buttonComp || !panelComp)
            continue;

        bool isHovered = buttonComp->Has(UIButton::Hovered);
        bool isPressed = buttonComp->Has(UIButton::Pressed);
        bool isClicked = buttonComp->Has(UIButton::Clicked);

        if (isHovered) {
            if (!btn.wasHovered) {
                audio.PlayProceduralBeep(440.0f, 0.05f, 0.15f);
                btn.wasHovered = true;
            }
            panelComp->color = isPressed ? btn.desc.pressedColor : btn.desc.hoverColor;
        } else {
            panelComp->color = btn.desc.normalColor;
            btn.wasHovered   = false;
        }

        if (isClicked) {
            audio.PlayProceduralBeep(660.0f, 0.15f, 0.25f);
            if (btn.desc.onClick) {
                btn.desc.onClick(engine);
            }
        }
    }
}

void MainMenu::Destroy(Engine* engine) {
    if (!m_active || !engine)
        return;

    auto& reg   = engine->GetRegistry();
    auto& audio = engine->GetAudioContext();

    // 1. Stop audio
    if (m_themeMusicHandle != nullptr) {
        audio.StopSoundInstance(m_themeMusicHandle);
        audio.DestroySoundInstance(m_themeMusicHandle);
        m_themeMusicHandle = nullptr;
    }

    // 2. Clean up 3D logo entities
    for (Entity e: m_logoEntities) {
        reg.Destroy(e);
    }
    m_logoEntities.clear();

    // 3. Clean up UI elements
    for (auto& btn: m_buttons) {
        reg.Destroy(btn.entity);
    }
    m_buttons.clear();

    if (m_buttonStack != NullEntity) {
        reg.Destroy(m_buttonStack);
        m_buttonStack = NullEntity;
    }

    // 4. Restore camera tracking and player movement
    Entity playerEnt = NullEntity;
    for (Entity pEnt: reg.GetEntitiesWith<Components::PlayerTagComponent>()) {
        playerEnt = pEnt;
        reg.Add(playerEnt, Components::MovementComponent {});
        break;
    }

    if (m_cameraEntity != NullEntity && playerEnt != NullEntity) {
        auto& targetCam             = reg.Add(m_cameraEntity, Components::TargetCameraComponent {});
        targetCam.target            = playerEnt;
        targetCam.distance          = 4.5f;
        targetCam.targetDistance    = 4.5f;
        targetCam.yaw               = -90.0f;
        targetCam.pitch             = -10.0f;
        targetCam.stiffness         = 15.0f;
        targetCam.vignetteIntensity = 1.1f;
        targetCam.vignettePower     = 1.5f;
        targetCam.fov               = 45.0f;
        targetCam.targetFov         = 45.0f;
        targetCam.targetOffset      = JPH::Vec3(0.0f, 1.3f, 0.0f);
    }

    m_active = false;
    ZHLN::Log("[MainMenu Module] Menu destroyed and gameplay controls restored.");
}

} // namespace ZHLN
