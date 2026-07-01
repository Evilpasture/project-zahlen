;; Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
;; SPDX-License-Identifier: GPL-3.0-or-later

(local ffi (require :ffi))

(local unpack (or table.unpack _G.unpack unpack))

(local ANSI_RED "\027[31m")
(local ANSI_GREEN "\027[32m")
(local ANSI_YELLOW "\027[33m")
(local ANSI_CYAN "\027[36m")
(local ANSI_RESET "\027[0m")

(local ItemFile {})
(set ItemFile.__index ItemFile)

(fn ItemFile.new [name item_type content executable]
  (setmetatable {: name
                 : item_type
                 :content (or content "")
                 :executable (or executable false)} ItemFile))

(local InventoryShell {})
(set InventoryShell.__index InventoryShell)

(fn InventoryShell.new []
  ;; Virtual Filesystem Tree Layout
  (let [root {:bin {:potion.sh (ItemFile.new :potion.sh :consumable
                                             "
                                    local args = {...}
                                    local target_name = args[1]
                                    local target = player_ent
                                    if target_name then
                                        local found = game_ecs:find(target_name)
                                        if found then target = found end
                                    end
                                    local combat = game_ecs:get(target, 'combat')
                                    if combat then
                                        combat.hp = math.min(combat.max_hp, combat.hp + 20)
                                        engine:beep(600, 0.15, 0.2)
                                        local name_str = target_name or 'player'
                                        return 'Healed ' .. name_str .. ' for 20 HP! New HP: ' .. tostring(math.floor(combat.hp))
                                    else
                                        return 'Error: No combat component on target!'
                                    end
                                "
                                             true)
                    :inv (ItemFile.new :inv :executable
                                       "
                                local args = {...}
                                local is_list = false
                                local filter_type = nil
                                local equip_id = nil

                                -- 1. Parse Command Line Arguments
                                for i = 1, #args do
                                    local arg = args[i]
                                    if arg == '--list' then
                                        is_list = true
                                    elseif string.sub(arg, 1, 7) == '--type=' then
                                        filter_type = string.sub(arg, 8)
                                    elseif arg == '--equip' then
                                        equip_id = tonumber(args[i+1])
                                    end
                                end

                                -- 2. Traverse /sys/inventory/ to Discover Live .env Item Files
                                local items = {}
                                local sys_inv = shell.root.sys.inventory
                                for cat_name, cat_dir in pairs(sys_inv) do
                                    if type(cat_dir) == 'table' then
                                        for file_name, file_obj in pairs(cat_dir) do
                                            if type(file_obj) ~= 'table' and string.sub(file_name, -4) == '.env' then
                                                local meta = {}
                                                for line in string.gmatch(file_obj.content, '[^
]+') do
                                                    local k, v = string.match(line, '([^=]+)=(.*)')
                                                    if k then meta[k] = v end
                                                end
                                                meta.file_name = file_name
                                                meta.path = '/sys/inventory/' .. cat_name .. '/' .. file_name
                                                table.insert(items, meta)
                                            end
                                        end
                                    end
                                end

                                -- Stably sort items by filename so assigned ID indexes remain consistent
                                table.sort(items, function(a, b) return a.file_name < b.file_name end)

                                -- 3. Execute Command Logic
                                if is_list then
                                    local lines = {}
                                    for idx, item in ipairs(items) do
                                        if not filter_type or item.type == filter_type then
                                            local info = string.format('[%d] %s', idx, item.name)
                                            if item.dmg then info = info .. ' - dmg=' .. item.dmg end
                                            if item.weight then info = info .. ' - weight=' .. item.weight end
                                            if item.hp then info = info .. ' - hp=' .. item.hp end
                                            table.insert(lines, info)
                                        end
                                    end
                                    if #lines == 0 then return 'No items found.' end
                                    return table.concat(lines, '
')

                                elseif equip_id then
                                    local item = items[equip_id]
                                    if not item then
                                        return '\027[31mError: Item with ID ' .. tostring(equip_id) .. ' not found.\027[0m'
                                    end

                                    if item.type ~= 'weapon' then
                                        return '\027[31mError: Cannot equip ' .. item.name .. ' (not a weapon).\027[0m'
                                    end

                                    -- Mutate player's native ECS inventory state directly from the subshell
                                    local inv_comp = game_ecs:get(player_ent, 'inventory')
                                    if inv_comp then
                                        inv_comp.equipped = item.name
                                        engine:beep(330, 0.12, 0.25)
                                        return 'Equipped ' .. item.name .. '.'
                                    end
                                    return '\027[31mError: Player inventory component missing!\027[0m'
                                end

                                return 'Usage:
  inv --list [--type=<type>]
  inv --equip <id>'
                            " true)}
              :sys {:inventory {:weapons {:rusty_sword.env (ItemFile.new :rusty_sword.env
                                                                         :weapon
                                                                         "name=Rusty Sword
type=weapon
dmg=12
weight=5"
                                                                         false)
                                          :iron_dagger.env (ItemFile.new :iron_dagger.env
                                                                         :weapon
                                                                         "name=Iron Dagger
type=weapon
dmg=8
weight=2"
                                                                         false)}
                                :consumables {:potion.env (ItemFile.new :potion.env
                                                                        :consumable
                                                                        "name=Health Potion
type=consumable
hp=20"
                                                                        false)}}}
              :note.txt (ItemFile.new :note.txt :lore "Password is Swordfish"
                                      false)}]
    (setmetatable {: root
                   :current_path []
                   :current_directory root
                   :env {:PATH :/bin :USER :player}}
                  InventoryShell)))

(fn InventoryShell.resolve_path [self path_str]
  (if (or (not path_str) (= path_str ""))
      nil
      (let [start_dir self.current_directory
            steps []]
        (each [_ v (ipairs self.current_path)]
          (table.insert steps v))
        (var curr start_dir)
        (var final-steps steps)
        (var path-to-proc path_str)
        (when (= (string.sub path_str 1 1) "/")
          (set curr self.root)
          (set final-steps [])
          (set path-to-proc (string.sub path_str 2)))
        (let [path_parts []]
          (each [part (string.gmatch path-to-proc "[^/]+")]
            (table.insert path_parts part))
          (var failed false)
          (var final-name nil)
          (var found-result false)
          (each [i part (ipairs path_parts) &until (or failed found-result)]
            (if (= part ".")
                nil
                (= part "..")
                (if (> (length final-steps) 0)
                    (do
                      (table.remove final-steps)
                      (set curr self.root)
                      (each [_ step (ipairs final-steps)]
                        (set curr (. curr step))))
                    (do
                      (set curr self.root)
                      (set final-steps [])))
                (if (and (= (type curr) :table) (. curr part))
                    (if (< i (length path_parts))
                        (do
                          (set curr (. curr part))
                          (table.insert final-steps part))
                        (do
                          (set final-name part)
                          (set found-result true)))
                    (set failed true))))
          (if failed
              nil
              (values curr final-name final-steps))))))

(fn InventoryShell.execute_command [self input_string]
  (let [tokens []]
    (each [token (string.gmatch input_string "%S+")]
      (table.insert tokens token))
    (if (= (length tokens) 0)
        ""
        (let [command (. tokens 1)
              args []]
          (for [i 2 (length tokens)]
            (table.insert args (. tokens i)))
          ;; Implicit expressions return natively to form clean block output
          (if (= command :ls)
              (let [target_dir self.current_directory]
                (var final-dir target_dir)
                (var ok true)
                (var err-msg "")
                (when (. args 1)
                  (let [(resolved name _) (self:resolve_path (. args 1))]
                    (if resolved
                        (set final-dir (if name (. resolved name) resolved))
                        (do
                          (set err-msg
                               (.. ANSI_RED "ls: " (. args 1)
                                   ": No such file or directory" ANSI_RESET))
                          (set ok false)))))
                (if (not ok)
                    err-msg
                    (if (or (not= (type final-dir) :table)
                            (not= final-dir.item_type nil))
                        (.. ANSI_RED "ls: " (or (. args 1) "")
                            ": Not a directory" ANSI_RESET)
                        (let [names []]
                          (each [k v (pairs final-dir)]
                            (if (and (= (type v) :table) (= v.item_type nil))
                                (table.insert names
                                              (.. ANSI_CYAN k "/" ANSI_RESET))
                                (table.insert names k)))
                          (table.sort names)
                          (table.concat names "\n")))))
              (= command :cd)
              (let [target_path (or (. args 1) "/")]
                (let [(resolved name steps) (self:resolve_path target_path)]
                  (if resolved
                      (let [target_dir (if name (. resolved name) resolved)]
                        (if (and (= (type target_dir) :table)
                                 (= target_dir.item_type nil))
                            (do
                              (set self.current_directory target_dir)
                              (set self.current_path steps)
                              "")
                            (.. ANSI_RED "cd: " target_path ": Not a directory"
                                ANSI_RESET)))
                      (.. ANSI_RED "cd: " target_path
                          ": No such file or directory" ANSI_RESET))))
              (= command :pwd)
              (.. "/" (table.concat self.current_path "/"))
              (= command :cat)
              (let [filename (. args 1)]
                (if (not filename)
                    (.. ANSI_RED "cat: error: specify a file." ANSI_RESET)
                    (let [(resolved name _) (self:resolve_path filename)]
                      (if (and resolved name)
                          (let [file (. resolved name)]
                            (if (and file (not= (type file) :table))
                                file.content
                                (.. ANSI_RED "cat: " filename ": No such file"
                                    ANSI_RESET)))
                          (.. ANSI_RED "cat: " filename ": No such file"
                              ANSI_RESET)))))
              ;; Binary Execution
              (let [is_explicit_path (not= (string.find command "/") nil)
                    (exec_file exec_name) (if is_explicit_path
                                              (let [(resolved name _) (self:resolve_path command)]
                                                (if (and resolved name)
                                                    (values (. resolved name)
                                                            name)
                                                    (values nil nil)))
                                              (let [path_env (or self.env.PATH
                                                                 "")]
                                                (accumulate [(f n) (values nil
                                                                           nil) dir_path (string.gmatch path_env
                                                                                                                                                                           "[^:]+")
                                                             &until f]
                                                  (let [(resolved name _) (self:resolve_path (.. dir_path
                                                                                                 "/"
                                                                                                 command))]
                                                    (if (and resolved name)
                                                        (values (. resolved
                                                                   name)
                                                                name)
                                                        (values nil nil))))))]
                (if exec_file
                    (if (and (= (type exec_file) :table)
                             (not= exec_file.item_type nil))
                        (if exec_file.executable
                            (let [(chunk err) (loadstring exec_file.content)]
                              (if chunk
                                  (let [env {:game_ecs _G.game_ecs
                                             :player_ent _G.player_ent
                                             :engine _G.engine
                                             :shell self
                                             : math
                                             : string
                                             : table
                                             : tostring
                                             : tonumber
                                             : type
                                             : pairs
                                             : ipairs
                                             : next
                                             : select
                                             : print}]
                                    (setfenv chunk env)
                                    (let [(status ret) (pcall chunk
                                                              (unpack args))]
                                      (if status
                                          (.. ANSI_GREEN "Executing " exec_name
                                              "..." ANSI_RESET "\n"
                                              (or ret :Success.))
                                          (.. ANSI_RED "Runtime Error: "
                                              (tostring ret) ANSI_RESET))))
                                  (.. ANSI_RED "Compilation Error: "
                                      (tostring err) ANSI_RESET)))
                            (.. ANSI_RED "sh: permission denied: " command
                                ANSI_RESET))
                        (.. ANSI_RED "sh: " command ": Is a directory"
                            ANSI_RESET))
                    (.. ANSI_RED "sh: command not found: " command ANSI_RESET))))))))

InventoryShell

