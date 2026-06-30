;; Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
;; SPDX-License-Identifier: GPL-3.0-or-later

(local dialogue-db {})

(set dialogue-db.pomni_intro {:nodes {:despair {:next_node nil
                                                :speaker :Pomni
                                                :text "It is... I've been losing my mind trying to find an exit. Please leave me be."}
                                      :explain_circus {:choices [{:next_node :hopeful
                                                                  :text "We'll find a way out."}
                                                                 {:next_node :despair
                                                                  :text "Sounds like a nightmare."}]
                                                       :speaker :Pomni
                                                       :text "It's a digital world run by a wacky ringmaster named Caine. You can't escape!"}
                                      :hopeful {:next_node nil
                                                :speaker :Pomni
                                                :text "I hope so... Let me know if you find anything strange. Good luck!"}
                                      :start {:choices [{:next_node :explain_circus
                                                         :text "Stuck? What is this place?"}
                                                        {:condition (fn [npc
                                                                         player
                                                                         sys]
                                                                      (= (sys:get_variable :has_sword)
                                                                         true))
                                                         :next_node :sword_response
                                                         :text "I don't know, I just found this rusty sword."}
                                                        {:next_node nil
                                                         :text "[Leave] I don't have time for this."}]
                                              :speaker :Pomni
                                              :text "Uh... Hello? Are you another person stuck in this digital circus?"}
                                      :sword_response {:action (fn [npc
                                                                    player
                                                                    sys]
                                                                 (sys:set_variable :pomni_warned
                                                                                   true))
                                                       :next_node :start
                                                       :speaker :Pomni
                                                       :text "A sword? Oh, Caine dropped that. Be careful with it, the physics here are weird!"}}
                              :start_node :start})

dialogue-db

