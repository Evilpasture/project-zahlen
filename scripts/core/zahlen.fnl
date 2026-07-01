;; Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
;; SPDX-License-Identifier: GPL-3.0-or-later

(require :scripts.core.ffi_cdef)
(local ffi (require :ffi))
(local ecs (require :scripts.core.ecs))

(local ull-0 (ffi.cast :uint64_t 0))

;; ============================================================================
;; Command ID Cache
;; ============================================================================
(local COMMAND_IDS {})

(fn get-cmd-id [name]
  (let [id (. COMMAND_IDS name)]
    (if id
        id
        (let [new-id (ffi.C.ZHLN_GetCommandID name)]
          (when (= new-id 4294967295)
            (error (.. "[ZHLN] Command not registered in C++: " (tostring name))))
          (tset COMMAND_IDS name new-id)
          new-id))))

;; ============================================================================
;; Math Types (vec3)
;; ============================================================================
(local vec3 {})
(set vec3.__index vec3)

(fn vec3.new [x y z]
  (ffi.new :vec3 {:x (or x 0) :y (or y 0) :z (or z 0)}))

(fn vec3.__add [a b]
  (vec3.new (+ (. a :x) (. b :x)) (+ (. a :y) (. b :y)) (+ (. a :z) (. b :z))))

(fn vec3.__sub [a b]
  (vec3.new (- (. a :x) (. b :x)) (- (. a :y) (. b :y)) (- (. a :z) (. b :z))))

(fn vec3.__unm [a]
  (vec3.new (- (. a :x)) (- (. a :y)) (- (. a :z))))

(fn vec3.__mul [a b]
  (if (= (type a) :number)
      (vec3.new (* a (. b :x)) (* a (. b :y)) (* a (. b :z)))
      (= (type b) :number)
      (vec3.new (* (. a :x) b) (* (. a :y) b) (* (. a :z) b))
      (+ (* (. a :x) (. b :x)) (* (. a :y) (. b :y)) (* (. a :z) (. b :z)))))

(fn vec3.length_sq [self]
  (+ (* (. self :x) (. self :x)) (* (. self :y) (. self :y))
     (* (. self :z) (. self :z))))

(fn vec3.length [self]
  (math.sqrt (self:length_sq)))

(fn vec3.normalized [self]
  (let [len (self:length)]
    (if (< len 0.0001)
        (vec3.new 0 0 0)
        (self:__mul (/ 1.0 len)))))

(fn vec3.cross [self b]
  (vec3.new (- (* (. self :y) (. b :z)) (* (. self :z) (. b :y)))
            (- (* (. self :z) (. b :x)) (* (. self :x) (. b :z)))
            (- (* (. self :x) (. b :y)) (* (. self :y) (. b :x)))))

(fn vec3.__tostring [self]
  (string.format "vec3(%.2f, %.2f, %.2f)" (. self :x) (. self :y) (. self :z)))

(ffi.metatype :vec3 vec3)

;; ============================================================================
;; PhysicsWorld Subsystem
;; ============================================================================
(local PhysicsWorld {})

(fn PhysicsWorld.new [engine-raw]
  (let [self-obj {:_raw engine-raw}]
    (setmetatable self-obj
                  {:__index (fn [t key]
                              (if (= key :positions)
                                  (let [view (ffi.new "ZHLN_BufferView[1]")
                                        args (ffi.new :GetBufferArgs [view])]
                                    (ffi.C.ZHLN_DispatchCommand t._raw
                                                                (get-cmd-id :GetPhysicsPositions)
                                                                args)
                                    (. view 0))
                                  (= key :velocities)
                                  (let [view (ffi.new "ZHLN_BufferView[1]")
                                        args (ffi.new :GetBufferArgs [view])]
                                    (ffi.C.ZHLN_DispatchCommand t._raw
                                                                (get-cmd-id :GetPhysicsLinearVelocities)
                                                                args)
                                    (. view 0))
                                  (= key :contacts)
                                  (let [view (ffi.new "ZHLN_BufferView[1]")
                                        args (ffi.new :GetBufferArgs [view])]
                                    (ffi.C.ZHLN_DispatchCommand t._raw
                                                                (get-cmd-id :GetPhysicsContactEvents)
                                                                args)
                                    (. view 0))
                                  (. PhysicsWorld key)))})))

(fn PhysicsWorld.raycast [self origin direction max_dist ignore_entity]
  (let [res (ffi.new "ZHLN_RaycastResult[1]")
        args (ffi.new :RaycastArgs [(. origin :x)
                                    (. origin :y)
                                    (. origin :z)
                                    (. direction :x)
                                    (. direction :y)
                                    (. direction :z)
                                    (or max_dist 1000.0)
                                    (or ignore_entity 0)
                                    res])]
    (ffi.C.ZHLN_DispatchCommand self._raw (get-cmd-id :Raycast) args)
    (if (= (. (. res 0) :hasHit) 1)
        {:entity (. (. res 0) :entity)
         :position (vec3.new (. (. res 0) :px) (. (. res 0) :py)
                             (. (. res 0) :pz))
         :normal (vec3.new (. (. res 0) :nx) (. (. res 0) :ny)
                           (. (. res 0) :nz))
         :fraction (. (. res 0) :fraction)}
        nil)))

(fn PhysicsWorld.apply_impulse [self entity impulse]
  (let [args (ffi.new :SetCharVelArgs
                      [entity (. impulse :x) (. impulse :y) (. impulse :z)])]
    (ffi.C.ZHLN_DispatchCommand self._raw (get-cmd-id :AddImpulse) args)))

(fn PhysicsWorld.set_linear_velocity [self entity velocity]
  (let [args (ffi.new :SetCharVelArgs
                      [entity (. velocity :x) (. velocity :y) (. velocity :z)])]
    (ffi.C.ZHLN_DispatchCommand self._raw (get-cmd-id :SetLinearVelocity) args)))

(fn PhysicsWorld.set_character_velocity [self entity velocity]
  (let [args (ffi.new :SetCharVelArgs
                      [entity (. velocity :x) (. velocity :y) (. velocity :z)])]
    (ffi.C.ZHLN_DispatchCommand self._raw (get-cmd-id :SetCharacterVelocity)
                                args)))

(fn PhysicsWorld.is_grounded [self entity]
  (let [args (ffi.new :EntityOnlyArgs [entity])]
    (= (ffi.C.ZHLN_DispatchCommand self._raw (get-cmd-id :IsCharacterOnGround)
                                   args) 1)))

(fn PhysicsWorld.setup_ragdoll [self player-ent parts-table]
  (let [count (length parts-table)
        parts-arr (ffi.new "uint64_t[?]" count)]
    (for [i 1 count]
      (tset parts-arr (- i 1) (. parts-table i)))
    (let [args (ffi.new :SetupRagdollArgs [player-ent count parts-arr])]
      (ffi.C.ZHLN_DispatchCommand self._raw (get-cmd-id :SetupRagdoll) args))))

;; ============================================================================
;; Camera Subsystem
;; ============================================================================
(local Camera {})

(fn Camera.new [engine-raw]
  (let [self-obj {:_raw engine-raw}]
    (setmetatable self-obj
                  {:__index (fn [t key]
                              (if (= key :yaw)
                                  (let [fargs (ffi.new "float[1]")
                                        args (ffi.new :CameraFloatArgs [fargs])]
                                    (ffi.C.ZHLN_DispatchCommand t._raw
                                                                (get-cmd-id :GetCameraYaw)
                                                                args)
                                    (. fargs 0))
                                  (= key :fov)
                                  (let [fargs (ffi.new "float[1]")
                                        args (ffi.new :CameraFloatArgs [fargs])]
                                    (ffi.C.ZHLN_DispatchCommand t._raw
                                                                (get-cmd-id :GetCameraFOV)
                                                                args)
                                    (. fargs 0))
                                  (. Camera key)))
                   :__newindex (fn [t key value]
                                 (if (= key :fov)
                                     (let [args (ffi.new :SetCameraFOVArgs
                                                         [value])]
                                       (ffi.C.ZHLN_DispatchCommand t._raw
                                                                   (get-cmd-id :SetCameraFOV)
                                                                   args))
                                     (rawset t key value)))})))

;; ============================================================================
;; Input Subsystem
;; ============================================================================
(local Input {})
(set Input.__index Input)

(local KEY_MAP {:W 1
                :A 2
                :S 3
                :D 4
                :LSHIFT 5
                :SHIFT 5
                :RBUTTON 6
                :SPACE 7
                :ESCAPE 8
                :R 9
                :E 10
                :LBUTTON 11})

(fn Input.new [engine-raw]
  (setmetatable {:_raw engine-raw} Input))

(fn Input.is_key_down [self key_name]
  (let [code (. KEY_MAP (string.upper key_name))]
    (if (not code)
        false
        (let [args (ffi.new :IsKeyDownArgs [code])]
          (= (ffi.C.ZHLN_DispatchCommand self._raw (get-cmd-id :IsKeyDown) args)
             1)))))

(fn Input.get_mouse_delta [self]
  (let [x (ffi.new "float[1]")
        y (ffi.new "float[1]")
        args (ffi.new :GetMouseDeltaArgs [x y])]
    (ffi.C.ZHLN_DispatchCommand self._raw (get-cmd-id :GetMouseDelta) args)
    (values (. x 0) (. y 0))))

;; ============================================================================
;; Audio Subsystem
;; ============================================================================
(local Audio {})
(set Audio.__index Audio)

(fn Audio.new [engine-raw]
  (setmetatable {:_raw engine-raw} Audio))

(fn Audio.play [self filepath volume]
  (let [args (ffi.new :PlayOneShotArgs [filepath (or volume 1.0)])]
    (ffi.C.ZHLN_DispatchCommand self._raw (get-cmd-id :PlayOneShot) args)))

(fn Audio.play_3d [self filepath pos volume]
  (let [args (ffi.new :PlayOneShot3DArgs
                      [filepath
                       (. pos :x)
                       (. pos :y)
                       (. pos :z)
                       (or volume 1.0)])]
    (ffi.C.ZHLN_DispatchCommand self._raw (get-cmd-id :PlayOneShot3D) args)))

(fn Audio.beep [self frequency duration volume]
  (let [args (ffi.new :PlayProceduralBeepArgs
                      [(or frequency 440.0)
                       (or duration 0.15)
                       (or volume 0.25)])]
    (ffi.C.ZHLN_DispatchCommand self._raw (get-cmd-id :PlayProceduralBeep) args)))

(fn Audio.create_instance [self filepath spatialized]
  (let [args (ffi.new :CreateSoundInstanceArgs [filepath (if spatialized 1 0)])]
    (ffi.C.ZHLN_DispatchCommand self._raw (get-cmd-id :CreateSoundInstance)
                                args)))

(fn Audio.play_instance [self handle]
  (when (and handle (not= handle ull-0))
    (let [args (ffi.new :SoundInstanceArgs [handle])]
      (ffi.C.ZHLN_DispatchCommand self._raw (get-cmd-id :PlaySoundInstance)
                                  args))))

(fn Audio.stop_instance [self handle]
  (when (and handle (not= handle ull-0))
    (let [args (ffi.new :SoundInstanceArgs [handle])]
      (ffi.C.ZHLN_DispatchCommand self._raw (get-cmd-id :StopSoundInstance)
                                  args))))

(fn Audio.destroy_instance [self handle]
  (when (and handle (not= handle ull-0))
    (let [args (ffi.new :SoundInstanceArgs [handle])]
      (ffi.C.ZHLN_DispatchCommand self._raw (get-cmd-id :DestroySoundInstance)
                                  args))))

;; ============================================================================
;; Engine Root Object
;; ============================================================================
(local Engine {})
(set Engine.__index Engine)

(fn Engine.new [raw-ptr]
  (let [self-obj (setmetatable {:_raw raw-ptr
                                :physics (PhysicsWorld.new raw-ptr)
                                :camera (Camera.new raw-ptr)
                                :input (Input.new raw-ptr)
                                :audio (Audio.new raw-ptr)
                                :ecs (ecs.new raw-ptr)
                                :settings {:pp nil :aa nil}
                                :dialogue (require :scripts.core.dialogue)
                                :_events {}
                                :_tracked_views []
                                :log (if _G.zahlen _G.zahlen.log print)
                                :warn (if _G.zahlen _G.zahlen.warn print)}
                               Engine)]
    self-obj))

(fn Engine.track_view [self v]
  (table.insert (. self :_tracked_views) v)
  v)

(fn Engine.on [self event_name callback]
  (when (not (. self._events event_name))
    (tset self._events event_name []))
  (table.insert (. self._events event_name) callback))

(fn Engine.trigger [self event_name ...]
  (let [listeners (. self._events event_name)]
    (when listeners
      (each [_ cb (ipairs listeners)]
        (cb ...)))))

;; ============================================================================
;; COORDINATE HELPERS
;; ============================================================================
(fn get-xyz [v def-x def-y def-z]
  (if (not v)
      (values (or def-x 0) (or def-y 0) (or def-z 0))
      (= (type v) :cdata)
      (values (. v :x) (. v :y) (. v :z))
      (values (or (. v 1) (or (. v :x) (or def-x 0)))
              (or (. v 2) (or (. v :y) (or def-y 0)))
              (or (. v 3) (or (. v :z) (or def-z 0))))))

(fn get-xyzw [v def-x def-y def-z def-w]
  (if (not v)
      (values (or def-x 0) (or def-y 0) (or def-z 0) (or def-w 1))
      (= (type v) :cdata)
      (values (or (. v :x) 0) (or (. v :y) 0) (or (. v :z) 0) (or (. v :w) 1))
      (values (or (. v 1) (or (. v :x) (or def-x 0)))
              (or (. v 2) (or (. v :y) (or def-y 0)))
              (or (. v 3) (or (. v :z) (or def-z 0)))
              (or (. v 4) (or (. v :w) (or def-w 1))))))

(fn get-rgba [v def-r def-g def-b def-a]
  (if (not v)
      (values (or def-r 1) (or def-g 1) (or def-b 1) (or def-a 1))
      (values (or (. v 1) (or (. v :r) (or def-r 1)))
              (or (. v 2) (or (. v :g) (or def-g 1)))
              (or (. v 3) (or (. v :b) (or def-b 1)))
              (or (. v 4) (or (. v :a) (or def-a 1))))))

(local SHAPE_TYPES {:box 0 :sphere 1 :capsule 2 :cylinder 3 :plane 4})

;; ============================================================================
;; SPECIALIZED FFI HANDLERS
;; ============================================================================
(local SPECIALIZED_HANDLERS
       {:SpawnPrefab (fn [self args]
                       (let [max-count (or (. args :max_entities) 2048)
                             ent-buffer (ffi.new "uint64_t[?]" max-count)
                             path-c (ffi.new "char[256]")]
                         (ffi.copy path-c (. args :path))
                         (let [(px py pz) (get-xyz (. args :position))
                               ffi-args (ffi.new :SpawnPrefabArgs
                                                 [path-c
                                                  px
                                                  py
                                                  pz
                                                  (if (. args :physics) 1 0)
                                                  (if (or (= (. args :static)
                                                             nil)
                                                          (= (. args :static)
                                                             true))
                                                      1
                                                      0)
                                                  (if (. args :animated) 1 0)
                                                  max-count
                                                  ent-buffer])
                               count (tonumber (ffi.C.ZHLN_DispatchCommand self._raw
                                                                           (get-cmd-id :SpawnPrefab)
                                                                           ffi-args))
                               entities []]
                           (for [i 0 (- count 1)]
                             (table.insert entities (. ent-buffer i)))
                           entities)))
        :SpawnEntity (fn [self args]
                       (let [shape-type (or (. SHAPE_TYPES
                                               (string.lower (or (. args :type)
                                                                 :box)))
                                            0)]
                         (var p1 1)
                         (var p2 1)
                         (var p3 1)
                         (if (= (. args :type) :box)
                             (let [size (or (. args :size)
                                            (or (. args :extents)
                                                (vec3.new 1 1 1)))]
                               (set p1 (. size :x))
                               (set p2 (. size :y))
                               (set p3 (. size :z)))
                             (= (. args :type) :sphere)
                             (set p1 (or (. args :radius) 0.5))
                             (or (= (. args :type) :capsule)
                                 (= (. args :type) :cylinder))
                             (do
                               (set p1 (or (. args :radius) 0.5))
                               (set p2 (or (. args :half_height) 1.0)))
                             (= (. args :type) :plane)
                             (set p1 (or (. args :extent) 10.0)))
                         (let [(px py pz) (get-xyz (. args :position))
                               (rx ry rz rw) (get-xyzw (. args :rotation) 0 0 0
                                                       1)
                               (r g b a) (get-rgba (. args :color) 1 1 1 1)
                               is-static (if (= (. args :static) nil) true
                                             (. args :static))
                               ffi-args (ffi.new :SpawnEntityArgs
                                                 {:shapeType shape-type
                                                  : p1
                                                  : p2
                                                  : p3
                                                  : px
                                                  : py
                                                  : pz
                                                  : rx
                                                  : ry
                                                  : rz
                                                  : rw
                                                  : r
                                                  : g
                                                  : b
                                                  : a
                                                  :isStatic (if is-static 1 0)})]
                           (ffi.C.ZHLN_DispatchCommand self._raw
                                                       (get-cmd-id :SpawnEntity)
                                                       ffi-args))))
        :SpawnLight (fn [self args]
                      (let [(px py pz) (get-xyz (. args :position))
                            (rx ry rz rw) (get-xyzw (. args :rotation) 0 0 0 1)
                            (r g b) (get-rgba (. args :color) 1 1 1 1)
                            (dx dy dz) (get-xyz (. args :direction) 0 -1 0)
                            ffi-args (ffi.new :SpawnLightArgs
                                              [px
                                               py
                                               pz
                                               rx
                                               ry
                                               rz
                                               rw
                                               r
                                               g
                                               b
                                               (or (. args :intensity) 100.0)
                                               (or (. args :radius) 0.1)
                                               dx
                                               dy
                                               dz
                                               (or (. args :range) 10.0)
                                               (or (. args :type) 1)
                                               (if (. args :twoSided) 1 0)])]
                        (ffi.C.ZHLN_DispatchCommand self._raw
                                                    (get-cmd-id :SpawnLight)
                                                    ffi-args)))
        :Raycast (fn [self args]
                   (let [res (ffi.new "ZHLN_RaycastResult[1]")
                         (ox oy oz) (get-xyz (. args :origin))
                         (dx dy dz) (get-xyz (. args :direction) 0 -1 0)
                         ffi-args (ffi.new :RaycastArgs
                                           [ox
                                            oy
                                            oz
                                            dx
                                            dy
                                            dz
                                            (or (. args :max_dist) 1000.0)
                                            (or (. args :ignore_entity) 0)
                                            res])]
                     (ffi.C.ZHLN_DispatchCommand self._raw
                                                 (get-cmd-id :Raycast) ffi-args)
                     (if (= (. (. res 0) :hasHit) 1)
                         {:entity (. (. res 0) :entity)
                          :position (vec3.new (. (. res 0) :px)
                                              (. (. res 0) :py)
                                              (. (. res 0) :pz))
                          :normal (vec3.new (. (. res 0) :nx) (. (. res 0) :ny)
                                            (. (. res 0) :nz))
                          :fraction (. (. res 0) :fraction)}
                         nil)))
        :ProvokeDeviceLost (fn [self _]
                             (ffi.C.ZHLN_DispatchCommand self._raw
                                                         (get-cmd-id :ProvokeDeviceLost)
                                                         nil))
        :InitPlayer (fn [self _]
                      (ffi.C.ZHLN_DispatchCommand self._raw
                                                  (get-cmd-id :InitPlayer) nil))})

;; ============================================================================
;; UNIFIED COMMAND DISPATCHER (Hyprland-style IPC)
;; ============================================================================
(local COMMAND_STRUCTS
       {:IsKeyDown :IsKeyDownArgs
        :GetMouseDelta :GetMouseDeltaArgs
        :GetCameraYaw :CameraFloatArgs
        :GetCameraFOV :CameraFloatArgs
        :SetCameraFOV :SetCameraFOVArgs
        :PlayOneShot :PlayOneShotArgs
        :PlayOneShot3D :PlayOneShot3DArgs
        :PlayProceduralBeep :PlayProceduralBeepArgs
        :CreateSoundInstance :CreateSoundInstanceArgs
        :PlaySoundInstance :SoundInstanceArgs
        :StopSoundInstance :SoundInstanceArgs
        :DestroySoundInstance :SoundInstanceArgs
        :SetCharacterVelocity :SetCharVelArgs
        :SetLinearVelocity :SetCharVelArgs
        :AddImpulse :SetCharVelArgs
        :AddImpulseAt :AddImpulseAtArgs
        :SetMovementInput :SetMoveInputArgs
        :LogInventoryShell :LogInventoryArgs
        :SetJumpIntent :EntityOnlyArgs
        :DestroyEntity :EntityOnlyArgs
        :IsCharacterOnGround :EntityOnlyArgs
        :PlayAnimationTrack :PlayTrackArgs
        :GetAnimationTrackCount :EntityOnlyArgs
        :GetAnimationTrackName :GetTrackNameArgs})

(fn Engine.dispatch [self cmd_name args]
  (let [specialized (. SPECIALIZED_HANDLERS cmd_name)]
    (if specialized
        (specialized self args)
        (let [struct_name (. COMMAND_STRUCTS cmd_name)]
          (when (not struct_name)
            (error (.. "[ZHLN] Unknown command: " (tostring cmd_name))))
          (let [ffi_arg (if (= (type args) :cdata)
                            args
                            (ffi.new struct_name args))]
            (ffi.C.ZHLN_DispatchCommand self._raw (get-cmd-id cmd_name) ffi_arg))))))

;; Backward compatibility aliases
(fn Engine.spawn [self path options]
  (let [opts (or options {})]
    (set opts.path path)
    (self:dispatch :SpawnPrefab opts)))

(fn Engine.spawn_entity [self options]
  (self:dispatch :SpawnEntity options))

(fn Engine.spawn_light [self options]
  (self:dispatch :SpawnLight options))

(fn Engine.provoke_device_lost [self]
  (self:dispatch :ProvokeDeviceLost nil))

(fn Engine.create_material [self color]
  (let [col (or color [1 1 1 1])
        out_pipeline (ffi.new "uint64_t[1]")
        out_albedo (ffi.new "uint32_t[1]")
        args (ffi.new :CreateMaterialArgs
                      {:r (. col 1)
                       :g (. col 2)
                       :b (. col 3)
                       :a (. col 4)
                       :outPipeline out_pipeline
                       :outAlbedo out_albedo})]
    (ffi.C.ZHLN_DispatchCommand self._raw (get-cmd-id :CreateBasicMaterial)
                                args)
    (values (. out_pipeline 0) (. out_albedo 0))))

(fn Engine.config [self cfg]
  (let [pp (. (. self :settings) :pp)
        aa (. (. self :settings) :aa)]
    (when (and (. cfg :giMode) pp) (tset (. pp 0) :giMode (. cfg :giMode)))
    (when (and (. cfg :aoRadius) pp)
      (tset (. pp 0) :aoRadius (. cfg :aoRadius)))
    (when (and (. cfg :aoBias) pp) (tset (. pp 0) :aoBias (. cfg :aoBias)))
    (when (and (. cfg :aoPower) pp) (tset (. pp 0) :aoPower (. cfg :aoPower)))
    (when (and (. cfg :giIntensity) pp)
      (tset (. pp 0) :giIntensity (. cfg :giIntensity)))
    (when (and (. cfg :giSamples) pp)
      (tset (. pp 0) :giSamples (. cfg :giSamples)))
    (when (and (. cfg :useLocalProbe) pp)
      (tset (. pp 0) :useLocalProbe (. cfg :useLocalProbe)))
    (when (and (. cfg :ambientExposure) pp)
      (tset (. pp 0) :ambientExposure (. cfg :ambientExposure)))
    (when (and (. cfg :probeMin) pp)
      (tset (. (. pp 0) :probeMin) 0
            (or (. cfg :probeMin :x) (or (. (. cfg :probeMin) 1) 0)))
      (tset (. (. pp 0) :probeMin) 1
            (or (. cfg :probeMin :y) (or (. (. cfg :probeMin) 2) 0)))
      (tset (. (. pp 0) :probeMin) 2
            (or (. cfg :probeMin :z) (or (. (. cfg :probeMin) 3) 0))))
    (when (and (. cfg :probeMax) pp)
      (tset (. (. pp 0) :probeMax) 0
            (or (. cfg :probeMax :x) (or (. (. cfg :probeMax) 1) 0)))
      (tset (. (. pp 0) :probeMax) 1
            (or (. cfg :probeMax :y) (or (. (. cfg :probeMax) 2) 0)))
      (tset (. (. pp 0) :probeMax) 2
            (or (. cfg :probeMax :z) (or (. (. cfg :probeMax) 3) 0))))
    (when (and (. cfg :probePos) pp)
      (tset (. (. pp 0) :probePos) 0
            (or (. cfg :probePos :x) (or (. (. cfg :probePos) 1) 0)))
      (tset (. (. pp 0) :probePos) 1
            (or (. cfg :probePos :y) (or (. (. cfg :probePos) 2) 0)))
      (tset (. (. pp 0) :probePos) 2
            (or (. cfg :probePos :z) (or (. (. cfg :probePos) 3) 0))))
    (when (and (. cfg :vignetteIntensity) pp)
      (tset (. pp 0) :vignetteIntensity (. cfg :vignetteIntensity)))
    (when (and (. cfg :vignettePower) pp)
      (tset (. pp 0) :vignettePower (. cfg :vignettePower)))
    (when (and (not= (. cfg :enableSSR) nil) pp)
      (tset (. pp 0) :enableSSR (. cfg :enableSSR)))
    (when (and (not= (. cfg :enableRTR) nil) pp)
      (tset (. pp 0) :enableRTR (. cfg :enableRTR)))
    (when (and (not= (. cfg :fullBright) nil) pp)
      (tset (. pp 0) :fullBright (. cfg :fullBright)))
    (when (and (not= (. cfg :enableTAA) nil) aa)
      (tset (. (. aa 0) :state) :mode (if (= (. cfg :enableTAA) 1) 2 0)))
    (when (and (. cfg :taaFeedback) aa)
      (tset (. (. aa 0) :state) :taaFeedback (. cfg :taaFeedback)))))

;; ============================================================================
;; Threading Task Scheduler
;; ============================================================================
(local scheduler {:systems []})

(fn scheduler.register [name priority f]
  (for [i (length scheduler.systems) 1 -1]
    (when (= (. (. scheduler.systems i) :name) name)
      (table.remove scheduler.systems i)))
  (table.insert scheduler.systems
                {: name :priority (or priority 100) :fn f :enabled true})
  (table.sort scheduler.systems (fn [a b] (< a.priority b.priority))))

;; ============================================================================
;; Thread Cleanup (Memory Release Hook)
;; ============================================================================
(fn Engine.cleanup []
  (let [self _G.zh]
    (each [i v (ipairs (. self :_tracked_views))]
      (v:release)
      (tset (. self :_tracked_views) i nil))
    (set (. self :_tracked_views) [])))

;; ============================================================================
;; Global Host Hooks & Initialization (LSP Static Declaration)
;; ============================================================================
(local engine_ptr (ffi.C.ZHLN_GetEngineContext))

;; Define 'zh' as a static table literal so the LSP can read its fields
(local zh {:physics (PhysicsWorld.new engine_ptr)
           :camera (Camera.new engine_ptr)
           :input (Input.new engine_ptr)
           :audio (Audio.new engine_ptr)
           :ecs (ecs.new engine_ptr)
           :settings {:pp nil :aa nil}
           :dialogue (require :scripts.core.dialogue)
           :log (if _G.zahlen _G.zahlen.log print)
           :warn (if _G.zahlen _G.zahlen.warn print)
           :vec3 vec3.new
           : scheduler
           :cleanup Engine.cleanup
           :dispatch Engine.dispatch
           :spawn Engine.spawn
           :spawn_entity Engine.spawn_entity
           :spawn_light Engine.spawn_light
           :provoke_device_lost Engine.provoke_device_lost
           :create_material Engine.create_material
           :config Engine.config
           :_raw engine_ptr
           :_events {}
           :_tracked_views []})

(setmetatable zh Engine)

(set _G.zh zh)

(when (not= engine_ptr nil)
  (set _G.engine zh)
  (set _G.game_ecs (. zh :ecs))
  (set _G.world (. zh :physics))
  (let [db (require :scripts.dialogue_db)]
    (each [id tree (pairs db)]
      ((. (. zh :dialogue) :register) (. zh :dialogue) id tree)))
  (let [InventoryShell (require :scripts.core.inventory)]
    (set _G.inventory_shell (InventoryShell.new)))
  ;; Pull PostProcessSettings Component from ECS
  (let [pp_view (ffi.new "ZHLN_BufferView[1]")]
    (ffi.C.ZHLN_DispatchCommand engine_ptr (get-cmd-id :GetECSBuffer)
                                (ffi.new :GetECSBufferArgs
                                         {:componentName :PostProcessSettingsComponent
                                          :outView pp_view}))
    (when (not= (. (. pp_view 0) :buf) nil)
      (tset (. zh :settings) :pp
            (ffi.cast :PostProcessSettingsComponent* (. (. pp_view 0) :buf))))
    ;; Immediately release structural shadow lock
    (when (not= (. (. pp_view 0) :obj) nil)
      (let [rel_args (ffi.new :ReleaseBufferArgs [(. (. pp_view 0) :obj)])]
        (ffi.C.ZHLN_DispatchCommand nil (get-cmd-id :ReleaseBuffer) rel_args))))
  ;; Pull AASettings Component from ECS
  (let [aa_view (ffi.new "ZHLN_BufferView[1]")]
    (ffi.C.ZHLN_DispatchCommand engine_ptr (get-cmd-id :GetECSBuffer)
                                (ffi.new :GetECSBufferArgs
                                         {:componentName :AASettingsComponent
                                          :outView aa_view}))
    (when (not= (. (. aa_view 0) :buf) nil)
      (tset (. zh :settings) :aa
            (ffi.cast :AASettingsComponent* (. (. aa_view 0) :buf))))
    ;; Immediately release structural shadow lock
    (when (not= (. (. aa_view 0) :obj) nil)
      (let [rel_args (ffi.new :ReleaseBufferArgs [(. (. aa_view 0) :obj)])]
        (ffi.C.ZHLN_DispatchCommand nil (get-cmd-id :ReleaseBuffer) rel_args))))
  (each [ent _ (_G.game_ecs:view :MovementComponent) &until _G.player_ent]
    (set _G.player_ent ent)))

(set _G.update (fn [_ dt]
                 (when (not _G.engine_started)
                   (set _G.engine_started true)
                   (zh:trigger :engine.start))
                 (zh:trigger :engine.tick dt)
                 (let [d (. zh :dialogue)]
                   (d:update dt))
                 (each [_ sys (ipairs (. scheduler :systems))]
                   (when (. sys :enabled)
                     ((. sys :fn) dt)))))

(set _G.run_inventory_command
     (fn [cmd]
       (when _G.inventory_shell
         (let [out (_G.inventory_shell:execute_command cmd)]
           (when (not= out "")
             (let [args (ffi.new :LogInventoryArgs {:msg out})]
               (ffi.C.ZHLN_DispatchCommand engine_ptr
                                           (get-cmd-id :LogInventoryShell) args)))))))

zh

