-- Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
-- SPDX-License-Identifier: GPL-3.0-or-later

local ffi = require("ffi")
local zh = require("scripts.core.zahlen")

local UIButtonFlags = {
    Hovered = 1,
    Pressed = 2,
    Clicked = 4
}

local Menu = {
    button_stack = nil,
    start_btn = nil,
    quit_btn = nil,
    active = true,
    hovered_states = {},

    -- 3D Scene Elements
    logo_entities = nil,
    menu_sun = nil,
    point_light_1 = nil,
    point_light_2 = nil,
    cam_ent = nil
}

zh:on("engine.start", function()
    zh.log("[Main Menu] Building cinematic main menu scene...")

    -- 1. Completely Detach the Camera and Freeze Player Input
    for cam_ent, _ in zh.ecs:view("MainCameraTagComponent") do
        Menu.cam_ent = cam_ent
        zh.ecs:remove(cam_ent, "TargetCameraComponent") -- Stops all camera tracking/freecam
        break
    end

    if _G.player_ent then
        zh.ecs:remove(_G.player_ent, "MovementComponent") -- Stops player from moving
    end

    -- 2. Lock Camera Position to frame the logo perfectly (Moved back to Z=12.0)
    zh.camera.position = zh.vec3(0.0, 1.5, 12.0)
    zh.camera.yaw = -90.0
    zh.camera.pitch = 0.0

    -- 3. Spawn the Logo
    Menu.logo_entities = zh:spawn("TADCLogo.glb", {
        position = { 0.0, 0.0, -5.0 },
        physics = false,
        static = true
    })

    -- Force PBR materials to bright non-metallic plastic
    if Menu.logo_entities then
        for _, ent in ipairs(Menu.logo_entities) do
            zh.ecs:add(ent, "PBRComponent", { roughness = 0.3, metallic = 0.0 })
        end
    end

    -- 4. Spawn Studio Lighting (Overpowers the dark void GI)
    Menu.menu_sun = zh:spawn_light({
        type = 0,                        -- Directional Light
        direction = { 0.0, -0.2, -1.0 }, -- Pointing straight at the logo
        color = { 1.0, 1.0, 1.0 },
        intensity = 500.0,
    })
    zh.ecs:add(Menu.menu_sun, "SunTagComponent")

    Menu.point_light_1 = zh:spawn_light({
        type = 1,                  -- Point Light
        position = { -4.0, 2.0, 4.0 },
        color = { 1.0, 0.9, 0.8 }, -- Warm Left
        intensity = 1500.0,
        radius = 0.5,
        range = 50.0
    })

    Menu.point_light_2 = zh:spawn_light({
        type = 1,                  -- Point Light
        position = { 4.0, 1.0, 4.0 },
        color = { 0.8, 0.9, 1.0 }, -- Cool Right
        intensity = 1500.0,
        radius = 0.5,
        range = 50.0
    })

    -- Start Menu Music
    Menu.theme_music = zh.audio:create_instance("resources/assets/audio/theme.mp3", false)
    if Menu.theme_music and Menu.theme_music ~= 0ULL then
        zh.audio:play_instance(Menu.theme_music)
    end

    local font_idx = 0
    for _, ui_comp in zh.ecs:view("UISettingsComponent") do
        font_idx = ui_comp.defaultFontAtlasIdx
        break
    end

    -- 5. Centered Button Stack (No parentEntity, so it anchors to the raw window screen!)
    Menu.button_stack = zh.ecs:create()
    local stack_rect = zh.ecs:add(Menu.button_stack, "UIRectComponent")
    stack_rect.anchorMinX, stack_rect.anchorMaxX = 0.5, 0.5
    stack_rect.anchorMinY, stack_rect.anchorMaxY = 0.5, 0.5
    stack_rect.x = -100.0
    stack_rect.y = 120.0
    stack_rect.width = 200.0
    stack_rect.height = 100.0

    local stack_layout = zh.ecs:add(Menu.button_stack, "UIStackComponent")
    stack_layout.direction = 1
    stack_layout.spacing = 15.0
    stack_layout.padding = 0.0

    -- 6. Start Button
    Menu.start_btn = zh.ecs:create()
    local start_rect = zh.ecs:add(Menu.start_btn, "UIRectComponent")
    start_rect.parentEntity = Menu.button_stack
    start_rect.hierarchyDepth = 2
    start_rect.width = 200
    start_rect.height = 40

    local start_panel = zh.ecs:add(Menu.start_btn, "UIPanelComponent")
    start_panel.color[0], start_panel.color[1], start_panel.color[2], start_panel.color[3] = 0.15, 0.15, 0.22, 0.95
    start_panel.edgeWidth = 8.0

    zh.ecs:add(Menu.start_btn, "UIButtonComponent")

    local start_text = zh.ecs:add(Menu.start_btn, "TextComponent")
    ffi.copy(start_text.text, "START GAME")
    start_text.x = 55
    start_text.y = 25
    start_text.scale = 0.8
    start_text.fontIndex = font_idx
    start_text.color[0], start_text.color[1], start_text.color[2], start_text.color[3] = 0.9, 0.9, 0.9, 1.0

    -- 7. Quit Button
    Menu.quit_btn = zh.ecs:create()
    local quit_rect = zh.ecs:add(Menu.quit_btn, "UIRectComponent")
    quit_rect.parentEntity = Menu.button_stack
    quit_rect.hierarchyDepth = 2
    quit_rect.width = 200
    quit_rect.height = 40

    local quit_panel = zh.ecs:add(Menu.quit_btn, "UIPanelComponent")
    quit_panel.color[0], quit_panel.color[1], quit_panel.color[2], quit_panel.color[3] = 0.15, 0.15, 0.22, 0.95
    quit_panel.edgeWidth = 8.0

    zh.ecs:add(Menu.quit_btn, "UIButtonComponent")

    local quit_text = zh.ecs:add(Menu.quit_btn, "TextComponent")
    ffi.copy(quit_text.text, "QUIT")
    quit_text.x = 80
    quit_text.y = 25
    quit_text.scale = 0.8
    quit_text.fontIndex = font_idx
    quit_text.color[0], quit_text.color[1], quit_text.color[2], quit_text.color[3] = 0.9, 0.9, 0.9, 1.0

    Menu.hovered_states[Menu.start_btn] = false
    Menu.hovered_states[Menu.quit_btn] = false
end)

local function main_menu_update_system(dt)
    if not Menu.active then
        return
    end

    -- Keep camera locked firmly in place every frame
    zh.camera.position = zh.vec3(0.0, 1.5, 12.0)
    zh.camera.yaw = -90.0
    zh.camera.pitch = 0.0

    local function process_button(btn_ent, click_callback)
        local btn = zh.ecs:get(btn_ent, "UIButtonComponent")
        local panel = zh.ecs:get(btn_ent, "UIPanelComponent")

        if btn and panel then
            local is_hovered = bit.band(btn.flags, UIButtonFlags.Hovered) ~= 0
            local is_pressed = bit.band(btn.flags, UIButtonFlags.Pressed) ~= 0
            local is_clicked = bit.band(btn.flags, UIButtonFlags.Clicked) ~= 0

            if is_hovered then
                if not Menu.hovered_states[btn_ent] then
                    zh.audio:beep(440, 0.05, 0.15)
                    Menu.hovered_states[btn_ent] = true
                end

                if is_pressed then
                    panel.color[0], panel.color[1], panel.color[2] = 0.10, 0.10, 0.15
                else
                    panel.color[0], panel.color[1], panel.color[2] = 0.22, 0.22, 0.32
                end
            else
                panel.color[0], panel.color[1], panel.color[2] = 0.15, 0.15, 0.22
                Menu.hovered_states[btn_ent] = false
            end

            if is_clicked then
                click_callback()
            end
        end
    end

    process_button(Menu.start_btn, function()
        zh.log("[Main Menu] Start Clicked. Transitioning to Gameplay...")
        zh.audio:beep(660, 0.15, 0.25)

        if Menu.theme_music and Menu.theme_music ~= 0ULL then
            zh.audio:stop_instance(Menu.theme_music)
            zh.audio:destroy_instance(Menu.theme_music)
            Menu.theme_music = nil
        end

        -- Destruct 3D Elements
        if Menu.logo_entities then
            for _, ent in ipairs(Menu.logo_entities) do
                zh.ecs:destroy(ent)
            end
            Menu.logo_entities = nil
        end
        if Menu.menu_sun then zh.ecs:destroy(Menu.menu_sun) end
        if Menu.point_light_1 then zh.ecs:destroy(Menu.point_light_1) end
        if Menu.point_light_2 then zh.ecs:destroy(Menu.point_light_2) end

        -- Destruct UI
        zh.ecs:destroy(Menu.button_stack)
        zh.ecs:destroy(Menu.start_btn)
        zh.ecs:destroy(Menu.quit_btn)

        Menu.active = false

        -- Restore Player physics/movement
        if _G.player_ent then
            zh.ecs:add(_G.player_ent, "MovementComponent")
        end

        -- Re-attach Camera to Player
        if Menu.cam_ent then
            local target_cam = zh.ecs:add(Menu.cam_ent, "TargetCameraComponent")
            target_cam.target = _G.player_ent
            target_cam.distance = 4.5
            target_cam.targetDistance = 4.5
            target_cam.yaw = -90.0
            target_cam.pitch = -10.0
            target_cam.stiffness = 15.0
            target_cam.vignetteIntensity = 1.10
            target_cam.vignettePower = 1.50
            target_cam.fov = 45.0
            target_cam.targetFov = 45.0
            target_cam.targetOffset[0] = 0.0
            target_cam.targetOffset[1] = 1.3
            target_cam.targetOffset[2] = 0.0
            target_cam.targetOffset[3] = 0.0
        end

        -- Launch Gameplay (Calls StartGame inside gameplay_template.lua)
        if type(_G.StartGame) == "function" then
            _G.StartGame()
        end
    end)

    process_button(Menu.quit_btn, function()
        os.exit(0)
    end)
end

zh.scheduler.register("MainMenuUpdate", 10, main_menu_update_system)
