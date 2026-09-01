;; Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
;; SPDX-License-Identifier: GPL-3.0-or-later

(local ffi (require :ffi))

(local DialogueSystem {:trees {} :blackboard {} :active nil :ui_entities []})

;; ============================================================================
;; Dialogue Database & Registration
;; ============================================================================

(fn DialogueSystem.register [self id tree]
  (tset self.trees id tree))

(fn DialogueSystem.get_variable [self key]
  (. self.blackboard key))

(fn DialogueSystem.set_variable [self key val]
  (tset self.blackboard key val))

;; ============================================================================
;; Core Session Logic & Diagnostics
;; ============================================================================

(fn DialogueSystem.start [self npc_ent dialogue_id]
  (let [tree (. self.trees dialogue_id)]
    (if (not tree)
        (do
          (_G.zh.warn (.. "Dialogue tree not found: " (tostring dialogue_id)))
          false)
        (let [start_node_id (or tree.start_node :start)]
          (set self.active {: npc_ent
                            : tree
                            :node_id start_node_id
                            :selected_choice 1
                            :variables {}})
          (_G.zh.log (.. "Starting dialogue: " dialogue_id " | Target node: "
                         start_node_id))
          (self:enter_node start_node_id)
          true))))

(fn DialogueSystem.enter_node [self node_id]
  (if (not self.active)
      (_G.zh.warn "enter_node called but session is inactive.")
      (do
        (set self.active.node_id node_id)
        (set self.active.selected_choice 1)
        (let [node (. self.active.tree.nodes node_id)]
          (if (not node)
              (do
                (_G.zh.warn (.. "enter_node failed: node '" (tostring node_id)
                                "' is nil. Terminating."))
                (self:stop))
              (do
                (_G.zh.log (.. "Entered dialogue node: '" node_id
                               "' | Speaker: " (tostring node.speaker)))
                (when node.action
                  (node.action self.active.npc_ent _G.player_ent self))
                (self:update_ui)))))))

(fn DialogueSystem.stop [self]
  (when self.active
    (_G.zh.log "Ending dialogue session.")
    (self:clear_ui)
    (set self.active nil)
    (let [movement (_G.game_ecs:get _G.player_ent :MovementComponent)]
      (when movement
        (set movement.inputX 0)
        (set movement.inputZ 0)))))

;; ============================================================================
;; Choice & Progression Navigation
;; ============================================================================

(fn DialogueSystem.get_visible_choices [self]
  (if (not self.active)
      []
      (let [node (. self.active.tree.nodes self.active.node_id)]
        (if (or (not node) (not node.choices))
            []
            (let [list []]
              (each [_ choice (ipairs node.choices)]
                (var visible true)
                (when choice.condition
                  (set visible
                       (choice.condition self.active.npc_ent _G.player_ent self)))
                (when visible
                  (table.insert list choice)))
              list)))))

(fn DialogueSystem.advance [self]
  (when self.active
    (let [node (. self.active.tree.nodes self.active.node_id)]
      (when node
        (let [choices (self:get_visible_choices)]
          (if (> (length choices) 0)
              (let [chosen (. choices self.active.selected_choice)]
                (_G.zh.log (.. "Advancing via choice selection: "
                               (tostring chosen.text)))
                (when chosen.action
                  (chosen.action self.active.npc_ent _G.player_ent self))
                (if chosen.next_node
                    (self:enter_node chosen.next_node)
                    (do
                      (_G.zh.log "Choice has no next_node. Terminating.")
                      (self:stop))))
              (do
                (_G.zh.log "Advancing via linear progression.")
                (if node.next_node
                    (self:enter_node node.next_node)
                    (do
                      (_G.zh.log "Linear node has no next_node. Terminating.")
                      (self:stop))))))))))

(fn DialogueSystem.navigate [self dir]
  (when self.active
    (let [choices (self:get_visible_choices)]
      (when (not= (length choices) 0)
        (var next_idx (+ self.active.selected_choice dir))
        (if (< next_idx 1) (set next_idx (length choices))
            (> next_idx (length choices)) (set next_idx 1))
        (set self.active.selected_choice next_idx)
        (_G.engine.audio:beep 440 0.05 0.15)
        (self:update_ui)))))

;; ============================================================================
;; UI Render Layer (Native TextComponent Instances)
;; ============================================================================

(fn DialogueSystem.clear_ui [self]
  (each [_ ent (ipairs self.ui_entities)]
    (_G.game_ecs:destroy ent))
  (set self.ui_entities []))

(fn DialogueSystem.update_ui [self]
  (self:clear_ui)
  (when self.active
    (let [node (. self.active.tree.nodes self.active.node_id)]
      (when node
        (let [screen_w 1280
              screen_h 720]
          (var font_idx 0)
          (each [_ ui_comp (_G.game_ecs:view :UISettingsComponent)
                 &until (not= font_idx 0)]
            (set font_idx ui_comp.defaultFontAtlasIdx))
          ;; 1. Render Speaker Name
          (let [speaker_ent (_G.game_ecs:create)
                s_comp (_G.game_ecs:add speaker_ent :TextComponent)
                s_text (or node.speaker "???")]
            (ffi.copy s_comp.text s_text)
            (set s_comp.text_len (length s_text))
            (set s_comp.x (* screen_w 0.15))
            (set s_comp.y (* screen_h 0.7))
            (set s_comp.scale 2.0)
            (tset s_comp.color 0 1.0)
            (tset s_comp.color 1 0.9)
            (tset s_comp.color 2 0.3)
            (tset s_comp.color 3 1.0)
            (set s_comp.fontIndex font_idx)
            (table.insert self.ui_entities speaker_ent))
          ;; 2. Render Dialogue Text
          (let [text_ent (_G.game_ecs:create)
                t_comp (_G.game_ecs:add text_ent :TextComponent)
                t_text (or node.text "")]
            (ffi.copy t_comp.text t_text)
            (set t_comp.text_len (length t_text))
            (set t_comp.x (* screen_w 0.15))
            (set t_comp.y (* screen_h 0.75))
            (set t_comp.scale 1.6)
            (tset t_comp.color 0 1.0)
            (tset t_comp.color 1 1.0)
            (tset t_comp.color 2 1.0)
            (tset t_comp.color 3 1.0)
            (set t_comp.fontIndex font_idx)
            (table.insert self.ui_entities text_ent))
          ;; 3. Render Choice Lists
          (let [choices (self:get_visible_choices)]
            (if (> (length choices) 0)
                (each [idx choice (ipairs choices)]
                  (when (<= idx 4)
                    (let [choice_ent (_G.game_ecs:create)
                          c_comp (_G.game_ecs:add choice_ent :TextComponent)
                          is_selected (= idx self.active.selected_choice)
                          txt (.. (if is_selected "> " "  ") choice.text)]
                      (ffi.copy c_comp.text txt)
                      (set c_comp.text_len (length txt))
                      (set c_comp.x (* screen_w 0.17))
                      (set c_comp.y (+ (* screen_h 0.81) (* (- idx 1) 25.0)))
                      (set c_comp.scale 1.3)
                      (if is_selected
                          (do
                            (tset c_comp.color 0 0.3)
                            (tset c_comp.color 1 1.0)
                            (tset c_comp.color 2 0.3)
                            (tset c_comp.color 3 1.0))
                          (do
                            (tset c_comp.color 0 0.8)
                            (tset c_comp.color 1 0.8)
                            (tset c_comp.color 2 0.8)
                            (tset c_comp.color 3 1.0)))
                      (set c_comp.fontIndex font_idx)
                      (table.insert self.ui_entities choice_ent))))
                ;; Prompt to advance
                (let [prompt_ent (_G.game_ecs:create)
                      p_comp (_G.game_ecs:add prompt_ent :TextComponent)
                      p_text "[Press SPACE to continue]"]
                  (ffi.copy p_comp.text p_text)
                  (set p_comp.text_len (length p_text))
                  (set p_comp.x (* screen_w 0.15))
                  (set p_comp.y (* screen_h 0.84))
                  (set p_comp.scale 1.2)
                  (tset p_comp.color 0 0.5)
                  (tset p_comp.color 1 0.5)
                  (tset p_comp.color 2 0.5)
                  (tset p_comp.color 3 1.0)
                  (set p_comp.fontIndex font_idx)
                  (table.insert self.ui_entities prompt_ent)))))))))

;; ============================================================================
;; Proximity Scanner (Trigger System)
;; ============================================================================

(var was_interact_down false)
(var was_nav_up_down false)
(var was_nav_down_down false)

(fn DialogueSystem.update [self dt]
  (when _G.player_ent
    ;; Handle input capture and override standard movement when dialogue is active
    (if self.active
        (let [up (_G.engine.input:is_key_down :W)
              down (_G.engine.input:is_key_down :S)
              select (_G.engine.input:is_key_down :SPACE)]
          (when (and up (not was_nav_up_down))
            (self:navigate -1))
          (when (and down (not was_nav_down_down))
            (self:navigate 1))
          (when (and select (not was_interact_down))
            (_G.engine.audio:beep 520 0.08 0.2)
            (self:advance))
          (set was_nav_up_down up)
          (set was_nav_down_down down)
          (set was_interact_down select))
        (let [player_trans (_G.game_ecs:get _G.player_ent :TransformComponent)]
          (when player_trans
            (let [p_pos player_trans.position
                  interact_pressed (_G.engine.input:is_key_down :SPACE)]
              (var nearest_npc nil)
              (var min_dist 3.5)
              (each [ent dialogue_comp trans (_G.game_ecs:view :DialogueComponent
                                                               :TransformComponent)]
                (let [dx (- (. trans.position 0) (. p_pos 0))
                      dy (- (. trans.position 1) (. p_pos 1))
                      dz (- (. trans.position 2) (. p_pos 2))
                      dist (math.sqrt (+ (* dx dx) (* dy dy) (* dz dz)))]
                  (when (< dist min_dist)
                    (set nearest_npc ent)
                    (set min_dist dist))))
              (if nearest_npc
                  (do
                    ;; Render contextual interaction prompt
                    (when (not self.prompt_entity)
                      (var font_idx 0)
                      (each [_ ui_comp (_G.game_ecs:view :UISettingsComponent)
                             &until (not= font_idx 0)]
                        (set font_idx ui_comp.defaultFontAtlasIdx))
                      (set self.prompt_entity (_G.game_ecs:create))
                      (let [prompt_comp (_G.game_ecs:add self.prompt_entity
                                                         :TextComponent)
                            prompt_text "[Press SPACE to talk]"]
                        (ffi.copy prompt_comp.text prompt_text)
                        (set prompt_comp.text_len (length prompt_text))
                        (set prompt_comp.x 515.0)
                        (set prompt_comp.y 450.0)
                        (set prompt_comp.scale 1.5)
                        (tset prompt_comp.color 0 1.0)
                        (tset prompt_comp.color 1 1.0)
                        (tset prompt_comp.color 2 1.0)
                        (tset prompt_comp.color 3 1.0)
                        (set prompt_comp.fontIndex font_idx)))
                    (when (and interact_pressed (not was_interact_down))
                      (let [comp (_G.game_ecs:get nearest_npc
                                                  :DialogueComponent)]
                        (when comp
                          (when self.prompt_entity
                            (_G.game_ecs:destroy self.prompt_entity)
                            (set self.prompt_entity nil))
                          (self:start nearest_npc comp.dialogue_id)))))
                  (when self.prompt_entity
                    (_G.game_ecs:destroy self.prompt_entity)
                    (set self.prompt_entity nil)))
              (set was_interact_down interact_pressed)))))))

DialogueSystem

