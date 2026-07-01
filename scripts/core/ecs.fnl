;; Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
;; SPDX-License-Identifier: GPL-3.0-or-later

(local ffi (require :ffi))

(local unpack (or table.unpack _G.unpack unpack))

(local ull-0 (ffi.cast :uint64_t 0))

(local Registry {})
(set Registry.__index Registry)

(local NATIVE_COMPONENTS {:TransformComponent true
                          :HierarchyComponent true
                          :MovementComponent true
                          :MeshComponent true
                          :PhysicsComponent true
                          :ALifeComponent true
                          :RagdollComponent true
                          :NameComponent true
                          :TargetCameraComponent true
                          :PhysicsStateComponent true
                          :PBRComponent true
                          :TextComponent true
                          :UISettingsComponent true
                          :SunTagComponent true
                          :UIRectComponent true
                          :UIPanelComponent true
                          :UIButtonComponent true
                          :UIDragComponent true
                          :UIStackComponent true
                          :UITextInputComponent true
                          :AnimatorComponent true})

(local DYNAMIC_COMPONENTS {})

(local CMD_IDS {})

(fn get-cmd [name]
  (let [id (. CMD_IDS name)]
    (if id
        id
        (let [new-id (ffi.C.ZHLN_GetCommandID name)]
          (when (= new-id 4294967295)
            (error (.. "[ZHLN] Command not registered in C++: " (tostring name))))
          (tset CMD_IDS name new-id)
          new-id))))

(fn Registry.register_dynamic [self comp-name]
  (when (not (. DYNAMIC_COMPONENTS comp-name))
    (let [size (ffi.sizeof comp-name)
          align (ffi.alignof comp-name)
          args (ffi.new :RegisterDynamicComponentArgs [comp-name size align])
          family-id (ffi.C.ZHLN_DispatchCommand self.engine
                                                (get-cmd :RegisterDynamicComponent)
                                                args)]
      (when (= family-id 4294967295)
        (error (.. "Failed to register dynamic component: " comp-name)))
      (tset DYNAMIC_COMPONENTS comp-name true))))

(fn to-key [ent]
  (if (= (type ent) :number) (tostring (ffi.cast :uint64_t ent))
      (= (type ent) :cdata) (tostring ent)
      (tostring ent)))

(fn Registry.new [engine-raw]
  (setmetatable {:engine engine-raw
                 :pools {}
                 :sizes {}
                 :entities {}
                 :next_id 1} Registry))

(fn Registry.is_native [self comp-name]
  (or (= (. NATIVE_COMPONENTS comp-name) true)
      (= (. DYNAMIC_COMPONENTS comp-name) true)))

(fn Registry.create [self ent]
  (let [id (if (not ent)
               (ffi.C.ZHLN_DispatchCommand self.engine (get-cmd :CreateEntity)
                                           nil)
               ent)]
    (tset self.entities (to-key id) true)
    id))

(fn Registry.destroy [self ent]
  (let [key (to-key ent)]
    (when (. self.entities key)
      (let [args (ffi.new :EntityOnlyArgs [ent])]
        (ffi.C.ZHLN_DispatchCommand self.engine (get-cmd :DestroyEntity) args)
        (each [name pool (pairs self.pools)]
          (when (not= (. pool key) nil)
            (tset pool key nil)
            (tset self.sizes name (- (. self.sizes name) 1))))
        (tset self.entities key nil)))))

(fn Registry.is_alive [self ent]
  (= (. self.entities (to-key ent)) true))

(fn Registry.add [self ent comp-name data]
  (tset self.entities (to-key ent) true)
  (if (self:is_native comp-name)
      (let [args (ffi.new :GetComponentArgs [ent comp-name])
            ptr-int-get (ffi.C.ZHLN_DispatchCommand self.engine
                                                    (get-cmd :GetComponent) args)
            ptr-int (if (= ptr-int-get ull-0)
                        (ffi.C.ZHLN_DispatchCommand self.engine
                                                    (get-cmd :AddComponent) args)
                        ptr-int-get)]
        (when (= ptr-int ull-0)
          (error (.. "Cannot add native component '" comp-name "'.")))
        (let [comp (ffi.cast (.. comp-name "*") ptr-int)]
          (when data
            (each [k v (pairs data)]
              (tset comp k v)))
          comp))
      (let [pool (or (. self.pools comp-name)
                     (let [new-pool {}]
                       (tset self.pools comp-name new-pool)
                       (tset self.sizes comp-name 0)
                       new-pool))
            key (to-key ent)]
        (when (= (. pool key) nil)
          (tset self.sizes comp-name (+ (. self.sizes comp-name) 1)))
        (let [val (if (= (type data) :function) (data) (or data {}))]
          (tset pool key val)
          val))))

(fn Registry.get [self ent comp-name]
  (if (self:is_native comp-name)
      (let [args (ffi.new :GetComponentArgs [ent comp-name])
            ptr-int (ffi.C.ZHLN_DispatchCommand self.engine
                                                (get-cmd :GetComponent) args)]
        (if (= ptr-int ull-0) nil (ffi.cast (.. comp-name "*") ptr-int)))
      (let [pool (. self.pools comp-name)]
        (if pool (. pool (to-key ent)) nil))))

(fn Registry.has [self ent comp-name]
  (if (self:is_native comp-name)
      (let [args (ffi.new :GetComponentArgs [ent comp-name])]
        (not= (ffi.C.ZHLN_DispatchCommand self.engine (get-cmd :GetComponent)
                                          args) ull-0))
      (let [pool (. self.pools comp-name)]
        (if pool (not= (. pool (to-key ent)) nil) false))))

(fn Registry.remove [self ent comp-name]
  (when (not (self:is_native comp-name))
    (let [pool (. self.pools comp-name)
          key (to-key ent)]
      (when (and pool (not= (. pool key) nil))
        (tset pool key nil)
        (tset self.sizes comp-name (- (. self.sizes comp-name) 1))))))

(fn Registry.pure_lua_view [self ...]
  (let [comps [...]
        n (length comps)]
    (if (= n 0)
        (fn [])
        (let [(smallest-name smallest-size) (do
                                              (var s-name (. comps 1))
                                              (var s-size
                                                   (or (. self.sizes s-name) 0))
                                              (for [i 2 n]
                                                (let [name (. comps i)
                                                      size (or (. self.sizes
                                                                  name)
                                                               0)]
                                                  (when (< size s-size)
                                                    (set s-size size)
                                                    (set s-name name))))
                                              (values s-name s-size))]
          (if (= smallest-size 0)
              (fn [])
              (let [smallest-pool (. self.pools smallest-name)
                    other-pools []]
                (for [i 1 n]
                  (let [name (. comps i)]
                    (when (not= name smallest-name)
                      (table.insert other-pools (or (. self.pools name) {})))))
                (var ent nil)
                (var val nil)
                (fn []
                  (var done false)
                  (var out-ent nil)
                  (var out-results nil)
                  (while (not done)
                    (let [(next-ent next-val) (next smallest-pool ent)]
                      (set ent next-ent)
                      (set val next-val)
                      (if (not ent)
                          (set done true)
                          (let [matches (accumulate [is-match true _ pool (ipairs other-pools)
                                                     &until (not is-match)]
                                          (not= (. pool ent) nil))]
                            (when matches
                              (let [results []]
                                (for [i 1 n]
                                  (tset results i
                                        (. (. self.pools (. comps i)) ent)))
                                (set out-ent ent)
                                (set out-results results)
                                (set done true)))))))
                  (if out-ent
                      (values out-ent (unpack out-results 1 n))
                      nil))))))))

(fn Registry.view [self ...]
  (let [comps [...]
        n (length comps)]
    (if (= n 0)
        (fn [])
        (let [(has-native primary-native) (accumulate [(has-nat prim-nat) (values false
                                                                                  nil) _ comp (ipairs comps)
                                                       &until has-nat]
                                            (if (self:is_native comp)
                                                (values true comp)
                                                (values false nil)))]
          (if (not has-native)
              (self:pure_lua_view (unpack comps 1 n))
              (let [view-buf (ffi.new "ZHLN_BufferView[1]")
                    args (ffi.new :GetECSBufferArgs [primary-native view-buf])
                    _ (ffi.C.ZHLN_DispatchCommand self.engine
                                                  (get-cmd :GetECSEntities) args)
                    entities-view (. view-buf 0)
                    count (tonumber (. entities-view.shape 0))
                    entity-array (ffi.cast :uint64_t* entities-view.buf)
                    entities []]
                (for [i 0 (- count 1)]
                  (tset entities (+ i 1) (. entity-array i)))
                (let [rel-args (ffi.new :ReleaseBufferArgs [entities-view.obj])]
                  (ffi.C.ZHLN_DispatchCommand nil (get-cmd :ReleaseBuffer)
                                              rel-args))
                (var index 1)
                (fn []
                  (var out-ent nil)
                  (var out-results nil)
                  (while (and (<= index count) (not out-ent))
                    (let [ent (. entities index)]
                      (set index (+ index 1))
                      (var matches true)
                      (local results [])
                      (each [i comp-name (ipairs comps) &until (not matches)]
                        (let [comp (self:get ent comp-name)]
                          (if (= comp nil)
                              (set matches false)
                              (tset results i comp))))
                      (when matches
                        (set out-ent ent)
                        (set out-results results))))
                  (if out-ent
                      (values out-ent (unpack out-results 1 n))
                      nil))))))))

(fn Registry.find [self name]
  (var found nil)
  (each [ent name-comp (self:view :NameComponent) &until found]
    (when (= (ffi.string name-comp.name) name)
      (set found ent)))
  found)

Registry

