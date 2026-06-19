-- Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
-- SPDX-License-Identifier: GPL-3.0-or-later
--
--
-- For dialogue testing.

local dialogue_db = {}

dialogue_db["pomni_intro"] = {
    start_node = "start",
    nodes = {
        ["start"] = {
            speaker = "Pomni",
            text = "Uh... Hello? Are you another person stuck in this digital circus?",
            choices = {
                {
                    text = "Stuck? What is this place?",
                    next_node = "explain_circus"
                },
                {
                    text = "I don't know, I just found this rusty sword.",
                    next_node = "sword_response",
                    condition = function(npc, player, sys)
                        -- Only show this option if player has discovered the sword note
                        return sys:get_variable("has_sword") == true
                    end
                },
                {
                    text = "[Leave] I don't have time for this.",
                    next_node = nil
                }
            }
        },
        ["explain_circus"] = {
            speaker = "Pomni",
            text = "It's a digital world run by a wacky ringmaster named Caine. You can't escape!",
            choices = {
                {
                    text = "We'll find a way out.",
                    next_node = "hopeful"
                },
                {
                    text = "Sounds like a nightmare.",
                    next_node = "despair"
                }
            }
        },
        ["sword_response"] = {
            speaker = "Pomni",
            text = "A sword? Oh, Caine dropped that. Be careful with it, the physics here are weird!",
            action = function(npc, player, sys)
                -- Mutate state on interaction
                sys:set_variable("pomni_warned", true)
            end,
            next_node = "start"
        },
        ["hopeful"] = {
            speaker = "Pomni",
            text = "I hope so... Let me know if you find anything strange. Good luck!",
            next_node = nil
        },
        ["despair"] = {
            speaker = "Pomni",
            text = "It is... I've been losing my mind trying to find an exit. Please leave me be.",
            next_node = nil
        }
    }
}

return dialogue_db
