-- Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
-- SPDX-License-Identifier: GPL-3.0-or-later

local ffi = require("ffi")
local zh = require("scripts.core.zahlen")

local UIButtonFlags = {
    Hovered = 1, -- 1 << 0
    Pressed = 2, -- 1 << 1
    Clicked = 4  -- 1 << 2
}

local Menu = {
    root_canvas = nil,
    button_stack = nil,
    start_btn = nil,
    quit_btn = nil,
    active = true,
    hovered_states = {}
}

zh:on("engine.start", function()
    zh.log("[Main Menu] Building baseline canvas and interactable buttons...")

    -- Start menu music (non-spatialized)
    Menu.theme_music = zh.audio:create_instance("resources/assets/audio/theme.mp3", false)
    if Menu.theme_music and Menu.theme_music ~= 0ULL then
        zh.audio:play_instance(Menu.theme_music)
    end

    local font_idx = 0
    for _, ui_comp in zh.ecs:view("UISettingsComponent") do
        font_idx = ui_comp.defaultFontAtlasIdx
        break
    end

    -- Fullscreen Background Canvas
    Menu.root_canvas = zh.ecs:create()
    local canvas_rect = zh.ecs:add(Menu.root_canvas, "UIRectComponent")
    canvas_rect.anchorMinX, canvas_rect.anchorMaxX = 0.0, 1.0
    canvas_rect.anchorMinY, canvas_rect.anchorMaxY = 0.0, 1.0
    canvas_rect.x, canvas_rect.y = 0, 0
    canvas_rect.width, canvas_rect.height = 0, 0
    canvas_rect.hierarchyDepth = 0

    local canvas_panel = zh.ecs:add(Menu.root_canvas, "UIPanelComponent")
    canvas_panel.color[0], canvas_panel.color[1], canvas_panel.color[2], canvas_panel.color[3] = 0.05, 0.05, 0.08, 0.90

    -- Centered Button Stack
    Menu.button_stack = zh.ecs:create()
    local stack_rect = zh.ecs:add(Menu.button_stack, "UIRectComponent")
    stack_rect.parentEntity = Menu.root_canvas
    stack_rect.hierarchyDepth = 1
    stack_rect.anchorMinX, stack_rect.anchorMaxX = 0.5, 0.5
    stack_rect.anchorMinY, stack_rect.anchorMaxY = 0.5, 0.5
    stack_rect.x, stack_rect.y = -110, -50
    stack_rect.width, stack_rect.height = 220, 100

    local stack_layout = zh.ecs:add(Menu.button_stack, "UIStackComponent")
    stack_layout.direction = 1
    stack_layout.spacing = 15.0
    stack_layout.padding = 10.0

    -- Start Button
    Menu.start_btn = zh.ecs:create()
    local start_rect = zh.ecs:add(Menu.start_btn, "UIRectComponent")
    start_rect.parentEntity = Menu.button_stack
    start_rect.hierarchyDepth = 2
    start_rect.width = 200
    start_rect.height = 40

    local start_panel = zh.ecs:add(Menu.start_btn, "UIPanelComponent")
    start_panel.color[0], start_panel.color[1], start_panel.color[2], start_panel.color[3] = 0.15, 0.15, 0.22, 1.0
    start_panel.edgeWidth = 8.0

    zh.ecs:add(Menu.start_btn, "UIButtonComponent")

    local start_text = zh.ecs:add(Menu.start_btn, "TextComponent")
    ffi.copy(start_text.text, "START GAME")
    start_text.x = 55
    start_text.y = 25
    start_text.scale = 0.8
    start_text.fontIndex = font_idx
    start_text.color[0], start_text.color[1], start_text.color[2], start_text.color[3] = 0.9, 0.9, 0.9, 1.0

    -- Quit Button
    Menu.quit_btn = zh.ecs:create()
    local quit_rect = zh.ecs:add(Menu.quit_btn, "UIRectComponent")
    quit_rect.parentEntity = Menu.button_stack
    quit_rect.hierarchyDepth = 2
    quit_rect.width = 200
    quit_rect.height = 40

    local quit_panel = zh.ecs:add(Menu.quit_btn, "UIPanelComponent")
    quit_panel.color[0], quit_panel.color[1], quit_panel.color[2], quit_panel.color[3] = 0.15, 0.15, 0.22, 1.0
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
    if not Menu.active or not Menu.start_btn or not Menu.quit_btn then
        return
    end

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
        zh.log("[Main Menu] Start Clicked. Destroying UI and loading world...")
        zh.audio:beep(660, 0.15, 0.25)

        -- Stop and destroy the menu music
        if Menu.theme_music and Menu.theme_music ~= 0ULL then
            zh.audio:stop_instance(Menu.theme_music)
            zh.audio:destroy_instance(Menu.theme_music)
            Menu.theme_music = nil
        end


        -- Clean up menu UI
        zh.ecs:destroy(Menu.root_canvas)

        Menu.active = false
        Menu.root_canvas = nil
        Menu.start_btn = nil
        Menu.quit_btn = nil

        -- Trigger the global game-world initialization defined in gameplay_template.lua
        if type(_G.StartGame) == "function" then
            _G.StartGame()
        else
            zh.warn("[Main Menu] Warning: _G.StartGame function is not defined.")
        end
    end)

    process_button(Menu.quit_btn, function()
        zh.log("[Main Menu] Quit Clicked. Exiting process...")
        zh.audio:beep(220, 0.20, 0.25)
        os.exit(0)
    end)
end

zh.scheduler.register("MainMenuUpdate", 10, main_menu_update_system)
