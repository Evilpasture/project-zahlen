;; Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
;; SPDX-License-Identifier: GPL-3.0-or-later

(local ffi (require :ffi))
(local zh (require :scripts.core.zahlen))

;; ============================================================================
;; Local Macros & Syntactic sugar
;; ============================================================================
(macros {:defsystem (fn [name args ...]
                      `(fn ,name ,args
                         (when _G.game_started
                           ,...)))})

;; ============================================================================
;; Magic Numbers Dictionaries (Aligned with C++ Engine)
;; ============================================================================
(local RagdollState {:STANDING 0 :RAGDOLL_FULL 1 :RAGDOLL_LIMP 2})
(local LightType {:DIRECTIONAL 0 :POINT 1 :SPOT 2 :AREA 3 :SUN 4})

;; ============================================================================
;; Mathematical & Structural Helpers
;; ============================================================================
(fn range [start end]
  (var i (- start 1))
  (fn []
    (if (< i end)
        (do
          (set i (+ i 1))
          i)
        nil)))

(fn ffi-call [cmd-name struct-name args out-field]
  (let [s (ffi.new struct-name args)]
    (if (not= (zh:dispatch cmd-name s) 0)
        (if out-field (ffi.string (. s out-field)) true)
        nil)))

;; ============================================================================
;; Animator Subsystem & Semantic Resolvers
;; ============================================================================
(local SEMANTIC_SYNONYMS {:JUMP [:JUMP :JUMPING :UP :RISE]
                          :FALL [:FALL :FALLING :DOWN :MIDAIR]
                          :LAND [:LAND :LANDING :TOUCHDOWN]
                          :RUN [:RUN :SPRINT :RUNNING :STRAFE]
                          :WALK [:WALK :WALKING :AMBLE :MARCH]
                          :IDLE [:IDLE]})

(local Animator {:unresolved_warnings {}})

(fn find-name [names predicate?]
  (accumulate [result nil i name (pairs names) &until (not= result nil)]
    (if (predicate? name) i result)))

(fn find-exact [names q]
  (let [exact? #(= $ q)]
    (find-name names exact?)))

(fn find-by-synonyms [names q]
  (let [syns (or (. SEMANTIC_SYNONYMS q) [q])]
    (accumulate [res nil _ syn (ipairs syns) &until (not= res nil)]
      (let [synonym? #(string.find $ syn)]
        (find-name names synonym?)))))

(fn find-substring [names q]
  (let [substring? #(string.find $ q)]
    (find-name names substring?)))

(fn find-locomotion-fallback [names q]
  (if (or (= q :RUN) (= q :WALK))
      (let [fallback (if (= q :RUN) :WALK :RUN)
            fallback-syns (or (. SEMANTIC_SYNONYMS fallback) [fallback])]
        (accumulate [res nil _ syn (ipairs fallback-syns) &until (not= res nil)]
          (let [locomotion? #(or (= $ fallback) (string.find $ syn))]
            (find-name names locomotion?))))
      nil))

(fn Animator.get_track_count [ent]
  (tonumber (zh:dispatch :GetAnimationTrackCount {:entityRaw ent})))

(fn Animator.get_track_names [ent]
  (let [count (Animator.get_track_count ent)]
    (collect [i (range 0 (- count 1))]
      (let [n (ffi-call :GetAnimationTrackName :GetTrackNameArgs
                        {:entityRaw ent :trackIndex i} :outName)]
        (values i (if n (string.upper n) :UNKNOWN))))))

(fn Animator.resolve_track_index [ent query]
  (if (= (type query) :number)
      query
      (let [names (Animator.get_track_names ent)
            q (string.upper query)]
        (or (find-exact names q) (find-by-synonyms names q)
            (find-substring names q) (find-locomotion-fallback names q)))))

(fn Animator.play [ent name-or-idx options]
  (let [options (or options {})
        idx (Animator.resolve_track_index ent name-or-idx)]
    (if (not idx)
        (let [track-name (tostring name-or-idx)]
          (when (not (. Animator.unresolved_warnings track-name))
            (tset Animator.unresolved_warnings track-name true))
          false)
        (let [args (ffi.new :PlayTrackArgs
                            {:entityRaw ent
                             :trackIndex idx
                             :blendDuration (or options.blend_duration 0.15)
                             :loop (if (= options.loop false) 0 1)
                             :playbackSpeed (or options.speed 1.0)})]
          (not= (zh:dispatch :PlayAnimationTrack args) 0)))))

;; ============================================================================
;; 1. CONFIGURE GRAPHICS & POST-PROCESSING ON STARTUP
;; ============================================================================
(zh:config {:giMode 2
            :aoRadius 0.8
            :aoBias 0.03
            :aoPower 2.2
            :giIntensity 1.5
            :giSamples 16
            :useLocalProbe 0
            :vignetteIntensity 1.0
            :vignettePower 1.8
            :enableSSR 0
            :enableRTR 1
            :enableTAA 1
            :taaFeedback 0.95
            :ambientExposure 25.0})

;; ============================================================================
;; 2. REQUIRE MAIN MENU & DEFINE GLOBAL BOUNDARIES
;; ============================================================================
(require :scripts.main_menu)

(set _G.game_started false)
(var pomni-parts nil)

;; ============================================================================
;; 3. DEFINE WORLD GENERATION / GAMESTART CALLBACK
;; ============================================================================
(fn _G.StartGame []
  (zh.log "[Gameplay] Spawning Circus Lobby layout and characters...")
  ;; 1. Initialize Player Capsule & Camera Controller
  (set _G.player_ent (zh:dispatch :InitPlayer))
  ;; 2. Spawn Static World Geometry
  (zh:spawn "Circus Lobby V9.glb" {:physics true :static true})
  (set pomni-parts (zh:spawn :tadc_models/POMNI.glb {:animated true}))
  ;; 3. Spawn Lights (Corrected to match C++ Engine Enums)
  (let [sun (zh:spawn_light {:type LightType.SUN
                             :rotation [-0.575 0.287 0.0 0.766]
                             :color [1.0 0.95 0.88]
                             :intensity 180.0
                             :radius 0.5
                             :range 400.0})]
    (zh.ecs:add sun :SunTagComponent))
  (zh:spawn_light {:type LightType.POINT
                   :position [50.0 110.0 34.0]
                   :rotation [0.0 0.0 0.0 1.0]
                   :color [1.0 0.95 0.88]
                   :intensity 230.0
                   :radius 200.0
                   :range 400.0})
  ;; 4. Dynamically configure Floor PBR
  (each [ent name-comp (zh.ecs:view :NameComponent)]
    (let [name-str (string.lower (ffi.string name-comp.name))]
      (when (or (string.find name-str :floor) (string.find name-str :ground)
                (string.find name-str :lobby))
        (zh.ecs:add ent :PBRComponent {:roughness 0.15 :metallic 0.95}))))
  ;; 5. Bind Ragdoll & Hierarchy
  (when (and _G.player_ent pomni-parts)
    (let [pomni-root (. pomni-parts 1)
          root-trans (zh.ecs:get pomni-root :TransformComponent)]
      (when root-trans
        (tset root-trans.position 0 0.0)
        (tset root-trans.position 1 -0.8)
        (tset root-trans.position 2 0.0))
      (zh.ecs:add pomni-root :HierarchyComponent {:parent _G.player_ent})
      (zh.ecs:add _G.player_ent :combat {:hp 100 :max_hp 100})
      (zh.physics:setup_ragdoll _G.player_ent pomni-parts)
      (zh.log "[Gameplay] Skeletal Ragdoll successfully generated and bound to player controller.")))
  (set _G.game_started true))

;; ============================================================================
;; 4. ECS SYSTEMS & GAMEPLAY PIPELINES
;; ============================================================================
(var was-r-down false)
(var total-time 0.0)

(defsystem player-input-system
  [_dt]
  (each [player-ent movement (zh.ecs:view :MovementComponent)]
    (let [yaw-rad (math.rad zh.camera.yaw)
          forward-x (math.cos yaw-rad)
          forward-z (math.sin yaw-rad)
          right-x (- (math.sin yaw-rad))
          right-z (math.cos yaw-rad)]
      ;; Map controls
      (var move-x 0)
      (var move-z 0)
      (when (zh.input:is_key_down :W) (set move-x (+ move-x forward-x))
        (set move-z (+ move-z forward-z)))
      (when (zh.input:is_key_down :S) (set move-x (- move-x forward-x))
        (set move-z (- move-z forward-z)))
      (when (zh.input:is_key_down :A) (set move-x (- move-x right-x))
        (set move-z (- move-z right-z)))
      (when (zh.input:is_key_down :D) (set move-x (+ move-x right-x))
        (set move-z (+ move-z right-z)))
      (let [len (math.sqrt (+ (* move-x move-x) (* move-z move-z)))]
        (if (> len 0.001)
            (do
              (set movement.inputX (/ move-x len))
              (set movement.inputZ (/ move-z len)))
            (do
              (set movement.inputX 0)
              (set movement.inputZ 0)))
        (set movement.isSprinting
             (and (zh.input:is_key_down :LSHIFT) (> len 0.001)))
        (when (zh.input:is_key_down :SPACE)
          (set movement.jumpRequested true)
          (zh.audio:beep 660.0 0.1 0.2))
        ;; Ragdoll Toggle on 'R'
        (let [is-r-down (zh.input:is_key_down :R)]
          (when (and is-r-down (not was-r-down))
            (let [ragdoll (zh.ecs:get player-ent :RagdollComponent)]
              (when ragdoll
                (let [pomni-root (. pomni-parts 1)]
                  (if (= ragdoll.state RagdollState.STANDING)
                      (do
                        (set ragdoll.state RagdollState.RAGDOLL_LIMP)
                        (zh.log "Player collapsed into a Limp Ragdoll!")
                        (zh.audio:beep 150.0 0.25 0.3)
                        (zh.ecs:remove pomni-root :HierarchyComponent)
                        (let [root-trans (zh.ecs:get pomni-root
                                                     :TransformComponent)]
                          (when root-trans (tset root-trans.position 1 0.0))))
                      (do
                        (set ragdoll.state RagdollState.STANDING)
                        (zh.log "Player stood back up!")
                        (let [root-trans (zh.ecs:get pomni-root
                                                     :TransformComponent)]
                          (when root-trans (tset root-trans.position 1 -0.8)))
                        (zh.ecs:add pomni-root :HierarchyComponent
                                    {:parent player-ent})))))))
          (set was-r-down is-r-down))))))

(defsystem hybrid-health-and-speed-system
  [dt]
  (each [ent movement combat (zh.ecs:view :MovementComponent :combat)]
    (if (< combat.hp 40)
        (do
          (set movement.speed 3.0)
          (when (not combat.is_poisoned)
            (zh.log (string.format "Entity %s is limping! HP: %d/100 (Speed throttled dynamically)"
                                   (tostring ent) combat.hp))
            (set combat.is_poisoned true)))
        (do
          (set movement.speed (if movement.isSprinting 8.0 4.0))
          (set combat.is_poisoned false)))
    (when (< combat.hp combat.max_hp)
      (set combat.hp (math.min combat.max_hp (+ combat.hp (* 2.0 dt)))))))

(defsystem camera-fov-system
  [_dt]
  (each [player-ent movement (zh.ecs:view :MovementComponent)]
    (each [_ cam (zh.ecs:view :TargetCameraComponent)]
      (when (= cam.target player-ent)
        (set cam.targetFov (if movement.isSprinting 55.0 45.0))))))

(defsystem visual-feedback-system
  [dt]
  (set total-time (+ total-time dt))
  (each [player-ent combat (zh.ecs:view :combat)]
    (each [_ cam (zh.ecs:view :TargetCameraComponent)]
      (when (= cam.target player-ent)
        (if (< combat.hp 40)
            (let [pulse (math.sin (* total-time 6.0))]
              (set cam.vignetteIntensity (+ 1.4 (* 0.35 pulse)))
              (set cam.vignettePower 2.0))
            (do
              (set cam.vignetteIntensity 1.1)
              (set cam.vignettePower 1.5)))))))

(var current-anim-state :IDLE)

(defsystem player-animation-system
  [_dt]
  (when (and _G.player_ent pomni-parts)
    (let [movement (zh.ecs:get _G.player_ent :MovementComponent)]
      (when movement
        (let [ragdoll (zh.ecs:get _G.player_ent :RagdollComponent)]
          (when (not (and ragdoll
                          (or (= ragdoll.state RagdollState.RAGDOLL_LIMP)
                              (= ragdoll.state RagdollState.RAGDOLL_FULL))))
            ;; 1. Evaluate physical state to determine target track
            (let [target-state (if (not movement.isGrounded)
                                   (if (> movement.currentYVel 1.0) :JUMP
                                       :FALL)
                                   (if (> movement.landingTimer 0.0)
                                       :LAND
                                       (let [vel-sq (+ (* movement.inputX
                                                          movement.inputX)
                                                       (* movement.inputZ
                                                          movement.inputZ))]
                                         (if (> vel-sq 0.01)
                                             (if movement.isSprinting :RUN
                                                 :WALK)
                                             :IDLE))))]
              ;; 2. Process State Transition
              (when (not= target-state current-anim-state)
                (set current-anim-state target-state) ; (zh.log (.. "[Animation] State Transition -> " target-state))
                (each [_ part-ent (ipairs pomni-parts)]
                  (when (zh.ecs:has part-ent :AnimatorComponent)
                    (let [options {:blend_duration 0.15 :loop true :speed 1.0}]
                      ;; Morph/One-shot overrides
                      (if (= target-state :JUMP)
                          (set options.loop false)
                          (= target-state :LAND)
                          (do
                            (set options.loop false)
                            (set options.speed 1.6)))
                      (Animator.play part-ent target-state options))))))))))))

;; --- Register Systems in the Core Scheduler ---
(zh.scheduler.register :PlayerInput 10 player-input-system)
(zh.scheduler.register :PlayerAnimations 15 player-animation-system)
(zh.scheduler.register :CombatAndSpeed 20 hybrid-health-and-speed-system)
(zh.scheduler.register :CameraFOV 30 camera-fov-system)
(zh.scheduler.register :VisualFeedback 25 visual-feedback-system)

(zh.log "[Gameplay] Systems successfully registered in dormant state.")
