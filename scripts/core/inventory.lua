local ffi = require("ffi")
local _ = require("scripts.core.zahlen")

local ANSI_RED = "\27[31m"
local ANSI_GREEN = "\27[32m"
local ANSI_YELLOW = "\27[33m"
local ANSI_CYAN = "\27[36m"
local ANSI_RESET = "\27[0m"

local ItemFile = {}
ItemFile.__index = ItemFile

function ItemFile.new(name, item_type, content, executable)
    return setmetatable({
        name = name,
        item_type = item_type,
        content = content or "",
        executable = executable or false
    }, ItemFile)
end

local InventoryShell = {}
InventoryShell.__index = InventoryShell

function InventoryShell.new()
    -- Virtual Filesystem Tree Layout
    local root = {
        ["bin"] = {
            -- High-Level Consumable Action Binary
            ["potion.sh"] = ItemFile.new(
                "potion.sh",
                "consumable",
                [[
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
                ]],
                true
            ),

            -- Monolithic Proprietary Inventory Manager [3]
            ["inv"] = ItemFile.new(
                "inv",
                "executable",
                [[
                    local args = {...}
                    local is_list = false
                    local filter_type = nil
                    local equip_id = nil

                    -- 1. Parse Command Line Arguments
                    for i = 1, #args do
                        local arg = args[i]
                        if arg == "--list" then
                            is_list = true
                        elseif string.sub(arg, 1, 7) == "--type=" then
                            filter_type = string.sub(arg, 8)
                        elseif arg == "--equip" then
                            equip_id = tonumber(args[i+1])
                        end
                    end

                    -- 2. Traverse /sys/inventory/ to Discover Live .env Item Files
                    local items = {}
                    local sys_inv = shell.root.sys.inventory
                    for cat_name, cat_dir in pairs(sys_inv) do
                        if type(cat_dir) == "table" then
                            for file_name, file_obj in pairs(cat_dir) do
                                if type(file_obj) ~= "table" and string.sub(file_name, -4) == ".env" then
                                    local meta = {}
                                    for line in string.gmatch(file_obj.content, "[^\n]+") do
                                        local k, v = string.match(line, "([^=]+)=(.*)")
                                        if k then meta[k] = v end
                                    end
                                    meta.file_name = file_name
                                    meta.path = "/sys/inventory/" .. cat_name .. "/" .. file_name
                                    table.insert(items, meta)
                                end
                            end
                        end
                    end

                    -- Stably sort items by filename so assigned ID indexes remain consistent [3]
                    table.sort(items, function(a, b) return a.file_name < b.file_name end)

                    -- 3. Execute Command Logic
                    if is_list then
                        local lines = {}
                        for idx, item in ipairs(items) do
                            if not filter_type or item.type == filter_type then
                                local info = string.format("[%d] %s", idx, item.name)
                                if item.dmg then info = info .. " - dmg=" .. item.dmg end
                                if item.weight then info = info .. " - weight=" .. item.weight end
                                if item.hp then info = info .. " - hp=" .. item.hp end
                                table.insert(lines, info)
                            end
                        end
                        if #lines == 0 then return "No items found." end
                        return table.concat(lines, "\n")

                    elseif equip_id then
                        local item = items[equip_id]
                        if not item then
                            return "\27[31mError: Item with ID " .. tostring(equip_id) .. " not found.\27[0m"
                        end

                        if item.type ~= "weapon" then
                            return "\27[31mError: Cannot equip " .. item.name .. " (not a weapon).\27[0m"
                        end

                        -- Mutate player's native ECS inventory state directly from the subshell [3]
                        local inv_comp = game_ecs:get(player_ent, "inventory")
                        if inv_comp then
                            inv_comp.equipped = item.name
                            engine:beep(330, 0.12, 0.25) -- Mechanized equipment beep sound
                            return "Equipped " .. item.name .. "."
                        end
                        return "\27[31mError: Player inventory component missing!\27[0m"
                    end

                    return "Usage:\n  inv --list [--type=<type>]\n  inv --equip <id>"
                ]],
                true
            )
        },
        ["sys"] = {
            ["inventory"] = {
                ["weapons"] = {
                    ["rusty_sword.env"] = ItemFile.new("rusty_sword.env", "weapon",
                        "name=Rusty Sword\ntype=weapon\ndmg=12\nweight=5", false),
                    ["iron_dagger.env"] = ItemFile.new("iron_dagger.env", "weapon",
                        "name=Iron Dagger\ntype=weapon\ndmg=8\nweight=2", false)
                },
                ["consumables"] = {
                    ["potion.env"] = ItemFile.new("potion.env", "consumable",
                        "name=Health Potion\ntype=consumable\nhp=20", false)
                }
            }
        },
        ["note.txt"] = ItemFile.new("note.txt", "lore", "Password is Swordfish", false)
    }

    return setmetatable({
        root = root,
        current_path = {},
        current_directory = root,
        env = {
            PATH = "/bin",
            USER = "player"
        }
    }, InventoryShell)
end

-- Hierarchical Path Resolver
function InventoryShell:resolve_path(path_str)
    if not path_str or path_str == "" then
        return nil
    end

    local start_dir = self.current_directory
    local steps = {}
    for _, v in ipairs(self.current_path) do
        table.insert(steps, v)
    end

    if string.sub(path_str, 1, 1) == "/" then
        start_dir = self.root
        steps = {}
        path_str = string.sub(path_str, 2)
    end

    local path_parts = {}
    for part in string.gmatch(path_str, "[^/]+") do
        table.insert(path_parts, part)
    end

    local curr = start_dir
    for i, part in ipairs(path_parts) do
        if part == "." then
            -- stay in current directory
        elseif part == ".." then
            if #steps > 0 then
                table.remove(steps)
                curr = self.root
                for _, step in ipairs(steps) do
                    curr = curr[step]
                end
            else
                curr = self.root
                steps = {}
            end
        else
            if type(curr) == "table" and curr[part] then
                if i < #path_parts then
                    curr = curr[part]
                    table.insert(steps, part)
                else
                    return curr, part, steps
                end
            else
                return nil
            end
        end
    end

    return curr, nil, steps
end

function InventoryShell:execute_command(input_string)
    local tokens = {}
    for token in string.gmatch(input_string, "%S+") do
        table.insert(tokens, token)
    end

    if #tokens == 0 then return "" end

    local command = tokens[1]
    local args = {}
    for i = 2, #tokens do
        table.insert(args, tokens[i])
    end

    local stdout = ""
    local stderr = ""

    -- ====================================================================
    -- BUILT-IN COMMANDS
    -- ====================================================================

    if command == "ls" then
        local target_dir = self.current_directory
        if args[1] then
            local resolved, name, _ = self:resolve_path(args[1])
            if resolved then
                target_dir = name and resolved[name] or resolved
            else
                stderr = ANSI_RED .. "ls: " .. args[1] .. ": No such file or directory" .. ANSI_RESET
                return stderr
            end
        end

        -- Check if target_dir is a file instead of a directory [3]
        if type(target_dir) ~= "table" or target_dir.item_type ~= nil then
            stderr = ANSI_RED .. "ls: " .. (args[1] or "") .. ": Not a directory" .. ANSI_RESET
            return stderr
        end

        local names = {}
        for k, v in pairs(target_dir) do
            -- Stably differentiate nested directory tables from ItemFile class objects [3]
            if type(v) == "table" and v.item_type == nil then
                table.insert(names, ANSI_CYAN .. k .. "/" .. ANSI_RESET) -- Folders in Cyan
            else
                table.insert(names, k)                                   -- Standard files
            end
        end
        table.sort(names)
        stdout = table.concat(names, "\n")
        return stdout
    elseif command == "cd" then
        local target_path = args[1] or "/"
        local resolved, name, steps = self:resolve_path(target_path)
        if resolved then
            local target_dir = name and resolved[name] or resolved
            -- Only allow navigation if target is a subdirectory (not an ItemFile) [3]
            if type(target_dir) == "table" and target_dir.item_type == nil then
                self.current_directory = target_dir
                self.current_path = steps
                return ""
            else
                stderr = ANSI_RED .. "cd: " .. target_path .. ": Not a directory" .. ANSI_RESET
                return stderr
            end
        else
            stderr = ANSI_RED .. "cd: " .. target_path .. ": No such file or directory" .. ANSI_RESET
            return stderr
        end
    elseif command == "pwd" then
        stdout = "/" .. table.concat(self.current_path, "/")
        return stdout
    elseif command == "cat" then
        local filename = args[1]
        if not filename then
            stderr = ANSI_RED .. "cat: error: specify a file." .. ANSI_RESET
            return stderr
        end

        local resolved, name, _ = self:resolve_path(filename)
        if resolved and name then
            local file = resolved[name]
            if file and type(file) ~= "table" then
                stdout = file.content
                return stdout
            end
        end
        stderr = ANSI_RED .. "cat: " .. filename .. ": No such file" .. ANSI_RESET
        return stderr
    end

    -- ====================================================================
    -- BINARY RESOLUTION & PATH EXECUTION
    -- ====================================================================

    local is_explicit_path = string.find(command, "/") ~= nil
    local exec_file = nil
    local exec_name = nil

    if is_explicit_path then
        local resolved, name, _ = self:resolve_path(command)
        if resolved and name then
            exec_file = resolved[name]
            exec_name = name
        end
    else
        local path_env = self.env.PATH or ""
        for dir_path in string.gmatch(path_env, "[^:]+") do
            local resolved, name, _ = self:resolve_path(dir_path .. "/" .. command)
            if resolved and name then
                exec_file = resolved[name]
                exec_name = name
                break
            end
        end
    end

    if exec_file then
        -- Verify it is a valid ItemFile object (not a directory table) [3]
        if type(exec_file) == "table" and exec_file.item_type ~= nil then
            if exec_file.executable then
                local chunk, err = loadstring(exec_file.content)
                if chunk then
                    local env = {
                        game_ecs = _G.game_ecs,
                        player_ent = _G.player_ent,
                        engine = _G.engine,
                        shell = self,

                        math = math,
                        string = string,
                        table = table,
                        tostring = tostring,
                        tonumber = tonumber,
                        type = type,
                        pairs = pairs,
                        ipairs = ipairs,
                        next = next,
                        select = select,
                        print = print
                    }
                    setfenv(chunk, env)

                    local status, ret = pcall(chunk, unpack(args))
                    if status then
                        stdout = ANSI_GREEN ..
                        "Executing " .. exec_name .. "..." .. ANSI_RESET .. "\n" .. (ret or "Success.")
                        return stdout
                    else
                        stderr = ANSI_RED .. "Runtime Error: " .. tostring(ret) .. ANSI_RESET
                        return stderr
                    end
                else
                    stderr = ANSI_RED .. "Compilation Error: " .. tostring(err) .. ANSI_RESET
                    return stderr
                end
            else
                stderr = ANSI_RED .. "sh: permission denied: " .. command .. ANSI_RESET
                return stderr
            end
        else
            stderr = ANSI_RED .. "sh: " .. command .. ": Is a directory" .. ANSI_RESET
            return stderr
        end
    end
    stderr = ANSI_RED .. "sh: command not found: " .. command .. ANSI_RESET
    return stderr
end

return InventoryShell
