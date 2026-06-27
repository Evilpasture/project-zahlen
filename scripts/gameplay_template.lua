-- scripts/gameplay_template.lua

local zh = require("scripts.core.zahlen")
local ffi = require("ffi")
local bit = require("bit")

-- ============================================================================
-- 1. AMBIENT & POST-PROCESSING CONFIGURATION
-- ============================================================================
zh:config({
    giMode = 2,
    aoRadius = 0.8,
    aoBias = 0.03,
    aoPower = 2.2,
    giIntensity = 1.5,
    giSamples = 16,
    useLocalProbe = 0,
    vignetteIntensity = 1.0,
    vignettePower = 1.8,
    enableSSR = 1,
    enableTAA = 1,
    taaFeedback = 0.95,
    ambientExposure = 5.0,
})

local UIButtonFlags = {
    Hovered = 1, -- 1 << 0
    Pressed = 2, -- 1 << 1
    Clicked = 4  -- 1 << 2
}

-- Declare file-scoped references so they can be accessed across systems
local parent = nil
local closeBtn = nil

-- ============================================================================
-- 2. SCENE INITIALIZATION (Baseplate, Lights & Physical Props)
-- ============================================================================
zh:on("engine.start", function()
    zh.log("[Sandbox] Initializing lightweight lighting & physics playground...")

    -- A. Spawn a Large Static Baseplate
    local baseplate = zh:spawn_entity({
        type = "box",
        size = zh.vec3(100.0, 1.0, 100.0),
        position = zh.vec3(0.0, -0.5, 0.0),
        color = { 0.5, 0.5, 0.5, 1.0 },
        static = true
    })

    zh.ecs:add(baseplate, "PBRComponent", { roughness = 0.15, metallic = 0.85 })
    zh.ecs:add(baseplate, "NameComponent", { name = "Baseplate" })

    -- B. Drop Dynamic Physical Props (Stacked test crates)
    zh.log("[Sandbox] Spawning dynamic physical crates...")
    local colors = {
        { 0.8, 0.2, 0.2, 1.0 },
        { 0.2, 0.8, 0.2, 1.0 },
        { 0.2, 0.2, 0.8, 1.0 },
        { 0.8, 0.8, 0.2, 1.0 },
        { 0.8, 0.2, 0.8, 1.0 }
    }

    for i = 1, 5 do
        local box = zh:spawn_entity({
            type = "box",
            size = zh.vec3(1.0, 1.0, 1.0),
            position = zh.vec3(0.0, 4.0 + (i * 2.2), -3.0),
            color = colors[(i % 5) + 1],
            static = false
        })
        zh.ecs:add(box, "NameComponent", { name = "TestCrate_" .. tostring(i) })
        zh.ecs:add(box, "PBRComponent", {
            roughness = 0.15 * i,
            metallic = 0.2 * (5 - i)
        })
    end

    -- C. ADDED: Semi-Transparent Interactive Glass Box
    zh.log("[Sandbox] Spawning semi-transparent glass box...")
    local glassBox = zh:spawn_entity({
        type = "box",
        size = zh.vec3(1.5, 3.0, 1.5),
        position = zh.vec3(4.0, 1.5, -3.0),
        color = { 0.4, 0.7, 1.0, 0.4 },
        static = true
    })
    zh.ecs:add(glassBox, "NameComponent", { name = "GlassObstacle" })
    zh.ecs:add(glassBox, "PBRComponent", {
        roughness = 0.05,
        metallic = 0.10
    })

    -- D. Inject Lighting Workspace
    zh.log("[Sandbox] Injecting high-contrast light setup...")

    local sun = zh:spawn_light({
        type = 0,
        rotation = { -0.575, 0.287, 0.0, 0.766 },
        color = { 1.0, 0.95, 0.88 },
        intensity = 180.0,
        radius = 0.5,
        range = 400.0
    })
    zh.ecs:add(sun, "SunTagComponent")

    -- ============================================================================
    -- 4. NESTED UI HIERARCHY DEMONSTRATION
    -- ============================================================================
    zh.log("[UI] Spawning nested window frame...")

    -- A. The Master Window (Parent Panel - Centered on screen)
    parent = zh.ecs:create()
    local p_rect = zh.ecs:add(parent, "UIRectComponent")
    p_rect.anchorMinX, p_rect.anchorMaxX = 0.5, 0.5
    p_rect.anchorMinY, p_rect.anchorMaxY = 0.5, 0.5
    p_rect.x, p_rect.y = -250, -150
    p_rect.width, p_rect.height = 500, 300
    p_rect.hierarchyDepth = 0

    local p_panel = zh.ecs:add(parent, "UIPanelComponent")
    p_panel.color[0] = 0.08
    p_panel.color[1] = 0.08
    p_panel.color[2] = 0.12
    p_panel.color[3] = 0.95

    -- B. The Header Bar
    local header = zh.ecs:create()
    local h_rect = zh.ecs:add(header, "UIRectComponent")
    h_rect.parentEntity = parent
    h_rect.hierarchyDepth = 1
    h_rect.anchorMinX, h_rect.anchorMaxX = 0.0, 1.0
    h_rect.anchorMinY, h_rect.anchorMaxY = 0.0, 0.0
    h_rect.x, h_rect.y = 0, 0
    h_rect.width, h_rect.height = 0, 35

    local h_panel = zh.ecs:add(header, "UIPanelComponent")
    h_panel.color[0] = 0.15
    h_panel.color[1] = 0.15
    h_panel.color[2] = 0.22
    h_panel.color[3] = 1.0

    -- Make the header bar click-reactive and link its drag-target to the master window parent
    zh.ecs:add(header, "UIButtonComponent")
    zh.ecs:add(header, "UIDragComponent", {
        targetEntity = parent
    })

    -- C. The Close Button (Child Panel - Anchored to Top-Right corner of the parent)
    closeBtn = zh.ecs:create()
    local b_rect = zh.ecs:add(closeBtn, "UIRectComponent")
    b_rect.parentEntity = header -- Parented directly to the header bar
    b_rect.hierarchyDepth = 2    -- Increased depth ensures it sorts first
    b_rect.anchorMinX, b_rect.anchorMaxX = 1.0, 1.0
    b_rect.anchorMinY, b_rect.anchorMaxY = 0.0, 0.0
    b_rect.x, b_rect.y = -40, 7
    b_rect.width, b_rect.height = 32, 20

    local b_panel = zh.ecs:add(closeBtn, "UIPanelComponent")
    b_panel.color[0] = 0.85
    b_panel.color[1] = 0.25
    b_panel.color[2] = 0.25
    b_panel.color[3] = 1.0

    -- Attach the dynamic button interaction component to the Close Button
    zh.ecs:add(closeBtn, "UIButtonComponent")

    -- D. Window Title Text (Child Text - Parented to the header bar)
    local title = zh.ecs:create()
    local t_rect = zh.ecs:add(title, "UIRectComponent")
    t_rect.parentEntity = header
    t_rect.hierarchyDepth = 2
    t_rect.anchorMinX, t_rect.anchorMaxX = 0.0, 0.0
    t_rect.anchorMinY, t_rect.anchorMaxY = 0.0, 0.0
    t_rect.x = 15
    t_rect.y = 25

    local font_idx = 0
    for _, ui_comp in zh.ecs:view("UISettingsComponent") do
        font_idx = ui_comp.defaultFontAtlasIdx
        break
    end

    local text_comp = zh.ecs:add(title, "TextComponent")
    ffi.copy(text_comp.text, "Sandbox Workspace Controller")
    text_comp.scale = 0.8
    text_comp.fontIndex = font_idx
    text_comp.color[0] = 0.9
    text_comp.color[1] = 0.9
    text_comp.color[2] = 0.9
    text_comp.color[3] = 1.0
end)

-- ============================================================================
-- 3. MOVEMENT & INTERACTION SYSTEM GRAPH
-- ============================================================================
local function player_input_system(dt)
    for player_ent, movement in zh.ecs:view("MovementComponent") do
        local yaw_rad = math.rad(zh.camera.yaw)
        local move_x, move_z = 0, 0
        local forward_x = math.cos(yaw_rad)
        local forward_z = math.sin(yaw_rad)
        local right_x = -math.sin(yaw_rad)
        local right_z = math.cos(yaw_rad)

        if zh.input:is_key_down("W") then
            move_x = move_x + forward_x; move_z = move_z + forward_z
        end
        if zh.input:is_key_down("S") then
            move_x = move_x - forward_x; move_z = move_z - forward_z
        end
        if zh.input:is_key_down("A") then
            move_x = move_x - right_x; move_z = move_z - right_z
        end
        if zh.input:is_key_down("D") then
            move_x = move_x + right_x; move_z = move_z + right_z
        end

        local len = math.sqrt(move_x * move_x + move_z * move_z)
        if len > 0.001 then
            move_x = move_x / len
            move_z = move_z / len
        end

        movement.inputX = move_x
        movement.inputZ = move_z
        movement.isSprinting = zh.input:is_key_down("LSHIFT") and (len > 0.001)

        if zh.input:is_key_down("SPACE") then
            movement.jumpRequested = true
            zh.audio:beep(660.0, 0.1, 0.2)
        end
    end
end

-- --- UI Controller System to handle Close actions ---
local function ui_controller_system(dt)
    if not closeBtn then return end

    local btn = zh.ecs:get(closeBtn, "UIButtonComponent")
    if btn then
        local is_clicked = bit.band(btn.flags, UIButtonFlags.Clicked) ~= 0
        if is_clicked then
            zh.log("[UI] Close button clicked. Destroying layout...")
            zh.audio:beep(500, 0.08, 0.15)

            -- Destroying the parent master window cleans up all child elements
            zh.ecs:destroy(parent)

            -- Nil out the references so this update logic stops polling
            parent = nil
            closeBtn = nil
        end
    end
end

zh.scheduler.register("PlayerInput", 10, player_input_system)
zh.scheduler.register("UIController", 20, ui_controller_system)
zh.log("[Sandbox] Test Sandbox initialization complete.")
