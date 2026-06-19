-- Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
-- SPDX-License-Identifier: GPL-3.0-or-later

local ffi = require("ffi")

local DialogueSystem = {
    trees = {},
    blackboard = {}, -- Global blackboard variables (quests, flags, etc.)
    active = nil,    -- Active dialogue session: { npc_ent, tree, node_id, selected_choice }
    ui_entities = {} -- Instanced text UI entities
}

-- ============================================================================
-- Dialogue Database & Registration
-- ============================================================================

function DialogueSystem:register(id, tree)
    self.trees[id] = tree
end

function DialogueSystem:get_variable(key)
    return self.blackboard[key]
end

function DialogueSystem:set_variable(key, val)
    self.blackboard[key] = val
end

-- ============================================================================
-- Core Session Logic & Diagnostics
-- ============================================================================

function DialogueSystem:start(npc_ent, dialogue_id)
    local tree = self.trees[dialogue_id]
    if not tree then
        _G.zh.warn("Dialogue tree not found: " .. tostring(dialogue_id))
        return false
    end

    local start_node_id = tree.start_node or "start"

    self.active = {
        npc_ent = npc_ent,
        tree = tree,
        node_id = start_node_id,
        selected_choice = 1,
        variables = {}
    }

    _G.zh.log("Starting dialogue: " .. dialogue_id .. " | Target node: " .. start_node_id)
    self:enter_node(start_node_id)
    return true
end

function DialogueSystem:enter_node(node_id)
    if not self.active then
        _G.zh.warn("enter_node called but session is inactive.")
        return
    end

    self.active.node_id = node_id
    self.active.selected_choice = 1

    local node = self.active.tree.nodes[node_id]
    if not node then
        _G.zh.warn("enter_node failed: node '" .. tostring(node_id) .. "' is nil. Terminating.")
        self:stop()
        return
    end

    _G.zh.log("Entered dialogue node: '" .. node_id .. "' | Speaker: " .. tostring(node.speaker))

    if node.action then
        node.action(self.active.npc_ent, _G.player_ent, self)
    end

    self:update_ui()
end

function DialogueSystem:stop()
    if not self.active then return end
    _G.zh.log("Ending dialogue session.")

    self:clear_ui()
    self.active = nil

    local movement = _G.game_ecs:get(_G.player_ent, "MovementComponent")
    if movement then
        movement.inputX = 0
        movement.inputZ = 0
    end
end

-- ============================================================================
-- Choice & Progression Navigation
-- ============================================================================

function DialogueSystem:get_visible_choices()
    if not self.active then return {} end
    local node = self.active.tree.nodes[self.active.node_id]
    if not node or not node.choices then return {} end

    local list = {}
    for i = 1, #node.choices do
        local choice = node.choices[i]
        local visible = true
        if choice.condition then
            visible = choice.condition(self.active.npc_ent, _G.player_ent, self)
        end
        if visible then
            table.insert(list, choice)
        end
    end
    return list
end

function DialogueSystem:advance()
    if not self.active then return end
    local node = self.active.tree.nodes[self.active.node_id]
    if not node then return end

    local choices = self:get_visible_choices()
    if #choices > 0 then
        local chosen = choices[self.active.selected_choice]
        _G.zh.log("Advancing via choice selection: " .. tostring(chosen.text))
        if chosen.action then
            chosen.action(self.active.npc_ent, _G.player_ent, self)
        end
        if chosen.next_node then
            self:enter_node(chosen.next_node)
        else
            _G.zh.log("Choice has no next_node. Terminating.")
            self:stop()
        end
    else
        _G.zh.log("Advancing via linear progression.")
        if node.next_node then
            self:enter_node(node.next_node)
        else
            _G.zh.log("Linear node has no next_node. Terminating.")
            self:stop()
        end
    end
end

function DialogueSystem:navigate(dir)
    if not self.active then return end
    local choices = self:get_visible_choices()
    if #choices == 0 then return end

    local next_idx = self.active.selected_choice + dir
    if next_idx < 1 then
        next_idx = #choices
    elseif next_idx > #choices then
        next_idx = 1
    end
    self.active.selected_choice = next_idx
    _G.engine.audio:beep(440, 0.05, 0.15)
    self:update_ui()
end

-- ============================================================================
-- UI Render Layer (Native TextComponent Instances)
-- ============================================================================

function DialogueSystem:clear_ui()
    for _, ent in ipairs(self.ui_entities) do
        _G.game_ecs:destroy(ent)
    end
    self.ui_entities = {}
end

function DialogueSystem:update_ui()
    self:clear_ui()
    if not self.active then return end

    local node = self.active.tree.nodes[self.active.node_id]
    if not node then return end

    local screen_w = 1280
    local screen_h = 720

    -- Find the default font index from active UI components
    local font_idx = 0
    for _, ui_comp in _G.game_ecs:view("UISettingsComponent") do
        font_idx = ui_comp.defaultFontAtlasIdx
        break
    end

    -- 1. Render Speaker Name
    local speaker_ent = _G.game_ecs:create()
    local s_comp = _G.game_ecs:add(speaker_ent, "TextComponent")
    local s_text = node.speaker or "???"
    ffi.copy(s_comp.text, s_text)
    s_comp.text_len = #s_text
    s_comp.x = screen_w * 0.15
    s_comp.y = screen_h * 0.70
    s_comp.scale = 2.0
    s_comp.color[0] = 1.0
    s_comp.color[1] = 0.9
    s_comp.color[2] = 0.3
    s_comp.color[3] = 1.0
    s_comp.fontIndex = font_idx
    table.insert(self.ui_entities, speaker_ent)

    -- 2. Render Dialogue Text
    local text_ent = _G.game_ecs:create()
    local t_comp = _G.game_ecs:add(text_ent, "TextComponent")
    local t_text = node.text or ""
    ffi.copy(t_comp.text, t_text)
    t_comp.text_len = #t_text
    t_comp.x = screen_w * 0.15
    t_comp.y = screen_h * 0.75
    t_comp.scale = 1.6
    t_comp.color[0] = 1.0
    t_comp.color[1] = 1.0
    t_comp.color[2] = 1.0
    t_comp.color[3] = 1.0
    t_comp.fontIndex = font_idx
    table.insert(self.ui_entities, text_ent)

    -- 3. Render Choice Lists
    local choices = self:get_visible_choices()
    if #choices > 0 then
        for idx, choice in ipairs(choices) do
            if idx <= 4 then
                local choice_ent = _G.game_ecs:create()
                local c_comp = _G.game_ecs:add(choice_ent, "TextComponent")

                local is_selected = (idx == self.active.selected_choice)
                local txt = (is_selected and "> " or "  ") .. choice.text
                ffi.copy(c_comp.text, txt)
                c_comp.text_len = #txt

                c_comp.x = screen_w * 0.17
                c_comp.y = screen_h * 0.81 + (idx - 1) * 25.0
                c_comp.scale = 1.3

                if is_selected then
                    c_comp.color[0], c_comp.color[1], c_comp.color[2], c_comp.color[3] = 0.3, 1.0, 0.3, 1.0
                else
                    c_comp.color[0], c_comp.color[1], c_comp.color[2], c_comp.color[3] = 0.8, 0.8, 0.8, 1.0
                end

                c_comp.fontIndex = font_idx
                table.insert(self.ui_entities, choice_ent)
            end
        end
    else
        -- Prompt to advance
        local prompt_ent = _G.game_ecs:create()
        local p_comp = _G.game_ecs:add(prompt_ent, "TextComponent")
        local p_text = "[Press SPACE to continue]"
        ffi.copy(p_comp.text, p_text)
        p_comp.text_len = #p_text
        p_comp.x = screen_w * 0.15
        p_comp.y = screen_h * 0.84
        p_comp.scale = 1.2
        p_comp.color[0], p_comp.color[1], p_comp.color[2], p_comp.color[3] = 0.5, 0.5, 0.5, 1.0
        p_comp.fontIndex = font_idx
        table.insert(self.ui_entities, prompt_ent)
    end
end

-- ============================================================================
-- Proximity Scanner (Trigger System)
-- ============================================================================

local was_interact_down = false
local was_nav_up_down = false
local was_nav_down_down = false

function DialogueSystem:update(dt)
    if not _G.player_ent then return end

    -- Handle input capture and override standard movement when dialogue is active
    if self.active then
        local up = _G.engine.input:is_key_down("W")
        local down = _G.engine.input:is_key_down("S")
        local select = _G.engine.input:is_key_down("SPACE")

        if up and not was_nav_up_down then
            self:navigate(-1)
        end
        if down and not was_nav_down_down then
            self:navigate(1)
        end
        if select and not was_interact_down then
            _G.engine.audio:beep(520, 0.08, 0.2)
            self:advance()
        end

        was_nav_up_down = up
        was_nav_down_down = down
        was_interact_down = select
        return
    end

    -- Process world scan for nearest speakable NPC when dialogue is inactive
    local player_trans = _G.game_ecs:get(_G.player_ent, "TransformComponent")
    if not player_trans then return end

    local p_pos = player_trans.position

    local interact_pressed = _G.engine.input:is_key_down("SPACE")
    local nearest_npc = nil
    local min_dist = 3.5 -- Active chat trigger range (meters)

    for ent, dialogue_comp, trans in _G.game_ecs:view("DialogueComponent", "TransformComponent") do
        local dx = trans.position[0] - p_pos[0]
        local dy = trans.position[1] - p_pos[1]
        local dz = trans.position[2] - p_pos[2]
        local dist = math.sqrt(dx * dx + dy * dy + dz * dz)

        if dist < min_dist then
            nearest_npc = ent
            min_dist = dist
        end
    end

    if nearest_npc then
        -- Render contextual interaction prompt
        if not self.prompt_entity then
            local font_idx = 0
            for _, ui_comp in _G.game_ecs:view("UISettingsComponent") do
                font_idx = ui_comp.defaultFontAtlasIdx
                break
            end
            self.prompt_entity = _G.game_ecs:create()
            local prompt_comp = _G.game_ecs:add(self.prompt_entity, "TextComponent")
            local prompt_text = "[Press SPACE to talk]"
            ffi.copy(prompt_comp.text, prompt_text)
            prompt_comp.text_len = #prompt_text
            prompt_comp.x = 515.0
            prompt_comp.y = 450.0
            prompt_comp.scale = 1.5
            prompt_comp.color[0], prompt_comp.color[1], prompt_comp.color[2], prompt_comp.color[3] = 1.0, 1.0, 1.0, 1.0
            prompt_comp.fontIndex = font_idx
        end

        if interact_pressed and not was_interact_down then
            local comp = _G.game_ecs:get(nearest_npc, "DialogueComponent")
            if comp then
                if self.prompt_entity then
                    _G.game_ecs:destroy(self.prompt_entity)
                    self.prompt_entity = nil
                end
                self:start(nearest_npc, comp.dialogue_id)
            end
        end
    else
        if self.prompt_entity then
            _G.game_ecs:destroy(self.prompt_entity)
            self.prompt_entity = nil
        end
    end

    was_interact_down = interact_pressed
end

return DialogueSystem
