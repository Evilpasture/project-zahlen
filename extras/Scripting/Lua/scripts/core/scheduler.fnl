;; Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
;; SPDX-License-Identifier: GPL-3.0-or-later

(local Scheduler {:systems []})

(fn Scheduler.register [self name priority f]
  (table.insert self.systems {: name
                              :priority (or priority 100)
                              :fn f
                              :enabled true})
  (table.sort self.systems (fn [a b] (< a.priority b.priority))))

(fn Scheduler.set_system_enabled [self name enabled]
  (each [_ sys (ipairs self.systems) &until (= (. sys :name) name)]
    (set sys.enabled enabled)))

(fn Scheduler.update [self registry dt]
  (each [_ sys (ipairs self.systems)]
    (when (. sys :enabled)
      ((. sys :fn) registry dt))))

Scheduler

