;; Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
;; SPDX-License-Identifier: GPL-3.0-or-later

(local ffi (require :ffi))
(local zh (require :scripts.core.zahlen))

(local ull-0 (ffi.cast :uint64_t 0))

(local UIButtonFlags {:Hovered 1 :Pressed 2 :Clicked 4})

(local Menu {:button_stack nil
             :start_btn nil
             :quit_btn nil
             :active true
             :hovered_states {}
             :logo_entities nil
             :menu_sun nil
             :point_light_1 nil
             :point_light_2 nil
             :cam_ent nil})

(zh:on :engine.start
       (fn []
         (zh.log "[Main Menu] Building cinematic main menu scene...")
         ;; 1. Detach the Camera and Freeze Player Input
         (each [cam-ent _ (zh.ecs:view :MainCameraTagComponent)
                &until Menu.cam_ent]
           (set Menu.cam_ent cam-ent)
           (zh.ecs:remove cam-ent :TargetCameraComponent))
         (when _G.player_ent
           (zh.ecs:remove _G.player_ent :MovementComponent))
         ;; 2. Lock Camera Position
         (set zh.camera.position (zh.vec3 0.0 1.5 12.0))
         (set zh.camera.yaw -90.0)
         (set zh.camera.pitch 0.0)
         ;; 3. Spawn the Logo
         (set Menu.logo_entities
              (zh:spawn :TADCLogo.glb
                        {:position [0.0 0.0 -5.0] :physics false :static true}))
         (when Menu.logo_entities
           (each [_ ent (ipairs Menu.logo_entities)]
             (zh.ecs:add ent :PBRComponent {:roughness 0.3 :metallic 0.0})))
         ;; 4. Start Menu Music
         (set Menu.theme_music
              (zh.audio:create_instance :resources/assets/audio/theme.mp3 false))
         (when (and Menu.theme_music (not= Menu.theme_music ull-0))
           (zh.audio:play_instance Menu.theme_music))
         (var font-idx 0)
         (each [_ ui-comp (zh.ecs:view :UISettingsComponent)
                &until (not= font-idx 0)]
           (set font-idx ui-comp.defaultFontAtlasIdx))
         ;; 5. Centered Button Stack
         (set Menu.button_stack (zh.ecs:create))
         (let [stack-rect (zh.ecs:add Menu.button_stack :UIRectComponent)]
           (set stack-rect.anchorMinX 0.5)
           (set stack-rect.anchorMaxX 0.5)
           (set stack-rect.anchorMinY 0.5)
           (set stack-rect.anchorMaxY 0.5)
           (set stack-rect.x -100.0)
           (set stack-rect.y 120.0)
           (set stack-rect.width 200.0)
           (set stack-rect.height 100.0))
         (let [stack-layout (zh.ecs:add Menu.button_stack :UIStackComponent)]
           (set stack-layout.direction 1)
           (set stack-layout.spacing 15.0)
           (set stack-layout.padding 0.0))
         ;; 6. Start Button
         (set Menu.start_btn (zh.ecs:create))
         (let [start-rect (zh.ecs:add Menu.start_btn :UIRectComponent)]
           (set start-rect.parentEntity Menu.button_stack)
           (set start-rect.hierarchyDepth 2)
           (set start-rect.width 200)
           (set start-rect.height 40))
         (let [start-panel (zh.ecs:add Menu.start_btn :UIPanelComponent)]
           (tset start-panel.color 0 0.15)
           (tset start-panel.color 1 0.15)
           (tset start-panel.color 2 0.22)
           (tset start-panel.color 3 0.95)
           (set start-panel.edgeWidth 8.0))
         (zh.ecs:add Menu.start_btn :UIButtonComponent)
         (let [start-text (zh.ecs:add Menu.start_btn :TextComponent)]
           (ffi.copy start-text.text "START GAME")
           (set start-text.x 55)
           (set start-text.y 25)
           (set start-text.scale 0.8)
           (set start-text.fontIndex font-idx)
           (tset start-text.color 0 0.9)
           (tset start-text.color 1 0.9)
           (tset start-text.color 2 0.9)
           (tset start-text.color 3 1.0))
         ;; 7. Quit Button
         (set Menu.quit_btn (zh.ecs:create))
         (let [quit-rect (zh.ecs:add Menu.quit_btn :UIRectComponent)]
           (set quit-rect.parentEntity Menu.button_stack)
           (set quit-rect.hierarchyDepth 2)
           (set quit-rect.width 200)
           (set quit-rect.height 40))
         (let [quit-panel (zh.ecs:add Menu.quit_btn :UIPanelComponent)]
           (tset quit-panel.color 0 0.15)
           (tset quit-panel.color 1 0.15)
           (tset quit-panel.color 2 0.22)
           (tset quit-panel.color 3 0.95)
           (set quit-panel.edgeWidth 8.0))
         (zh.ecs:add Menu.quit_btn :UIButtonComponent)
         (let [quit-text (zh.ecs:add Menu.quit_btn :TextComponent)]
           (ffi.copy quit-text.text :QUIT)
           (set quit-text.x 80)
           (set quit-text.y 25)
           (set quit-text.scale 0.8)
           (set quit-text.fontIndex font-idx)
           (tset quit-text.color 0 0.9)
           (tset quit-text.color 1 0.9)
           (tset quit-text.color 2 0.9)
           (tset quit-text.color 3 1.0))
         (tset Menu.hovered_states Menu.start_btn false)
         (tset Menu.hovered_states Menu.quit_btn false)))

(fn main_menu_update_system [dt]
  (when Menu.active
    ;; Lock camera position firmly in place
    (set zh.camera.position (zh.vec3 0.0 1.5 12.0))
    (set zh.camera.yaw -90.0)
    (set zh.camera.pitch 0.0)

    (fn process-button [btn-ent click-callback]
      (let [btn (zh.ecs:get btn-ent :UIButtonComponent)
            panel (zh.ecs:get btn-ent :UIPanelComponent)]
        (when (and btn panel)
          (let [is-hovered (not= (band btn.flags UIButtonFlags.Hovered) 0)
                is-pressed (not= (band btn.flags UIButtonFlags.Pressed) 0)
                is-clicked (not= (band btn.flags UIButtonFlags.Clicked) 0)]
            (if is-hovered
                (do
                  (when (not (. Menu.hovered_states btn-ent))
                    (zh.audio:beep 440 0.05 0.15)
                    (tset Menu.hovered_states btn-ent true))
                  (if is-pressed
                      (do
                        (tset panel.color 0 0.1)
                        (tset panel.color 1 0.1)
                        (tset panel.color 2 0.15))
                      (do
                        (tset panel.color 0 0.22)
                        (tset panel.color 1 0.22)
                        (tset panel.color 2 0.32))))
                (do
                  (tset panel.color 0 0.15)
                  (tset panel.color 1 0.15)
                  (tset panel.color 2 0.22)
                  (tset Menu.hovered_states btn-ent false)))
            (when is-clicked
              (click-callback))))))

    (process-button Menu.start_btn
                    (fn []
                      (zh.log "[Main Menu] Start Clicked. Transitioning to Gameplay...")
                      (zh.audio:beep 660 0.15 0.25)
                      (when (and Menu.theme_music (not= Menu.theme_music ull-0))
                        (zh.audio:stop_instance Menu.theme_music)
                        (zh.audio:destroy_instance Menu.theme_music)
                        (set Menu.theme_music nil))
                      ;; Destruct 3D Elements
                      (when Menu.logo_entities
                        (each [_ ent (ipairs Menu.logo_entities)]
                          (zh.ecs:destroy ent))
                        (set Menu.logo_entities nil))
                      (when Menu.menu_sun (zh.ecs:destroy Menu.menu_sun))
                      (when Menu.point_light_1
                        (zh.ecs:destroy Menu.point_light_1))
                      (when Menu.point_light_2
                        (zh.ecs:destroy Menu.point_light_2))
                      ;; Destruct UI
                      (zh.ecs:destroy Menu.button_stack)
                      (zh.ecs:destroy Menu.start_btn)
                      (zh.ecs:destroy Menu.quit_btn)
                      (set Menu.active false)
                      ;; Restore Player physics/movement
                      (when _G.player_ent
                        (zh.ecs:add _G.player_ent :MovementComponent))
                      ;; Re-attach Camera to Player
                      (when Menu.cam_ent
                        (let [target-cam (zh.ecs:add Menu.cam_ent
                                                     :TargetCameraComponent)]
                          (set target-cam.target _G.player_ent)
                          (set target-cam.distance 4.5)
                          (set target-cam.targetDistance 4.5)
                          (set target-cam.yaw -90.0)
                          (set target-cam.pitch -10.0)
                          (set target-cam.stiffness 15.0)
                          (set target-cam.vignetteIntensity 1.1)
                          (set target-cam.vignettePower 1.5)
                          (set target-cam.fov 45.0)
                          (set target-cam.targetFov 45.0)
                          (tset target-cam.targetOffset 0 0.0)
                          (tset target-cam.targetOffset 1 1.3)
                          (tset target-cam.targetOffset 2 0.0)
                          (tset target-cam.targetOffset 3 0.0)))
                      ;; Launch Gameplay
                      (when (= (type _G.StartGame) :function)
                        (_G.StartGame))))
    (process-button Menu.quit_btn
                    (fn []
                      (os.exit 0)))))

(zh.scheduler.register :MainMenuUpdate 10 main_menu_update_system)

