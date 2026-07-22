;; Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
;; SPDX-License-Identifier: GPL-3.0-or-later

(local ffi (require :ffi))
(local zh (require :scripts.core.zahlen))

;; ============================================================================
;; Local Macros
;; ============================================================================
(macros {:defsystem (fn [name args ...]
                      `(fn ,name ,args
                         (when _G.game_started
                           ,...)))})

(local RagdollState {:STANDING 0 :RAGDOLL_FULL 1 :RAGDOLL_LIMP 2})
(local LightType {:DIRECTIONAL 0 :POINT 1 :SPOT 2 :AREA 3 :SUN 4})

(set _G.game_started false)
(var pomni-parts nil)
(var g-snow-terrain nil)
(var g-campfire-light nil)
(var g-summit-light nil)
(var g-wisp-1 nil)
(var g-wisp-2 nil)
(var won-game false)
(var total-time 0.0)

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

(set _G.Animator (or _G.Animator {:unresolved_warnings {}}))
(local Animator _G.Animator)

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

(set Animator.get_track_count
     (fn [ent]
       (tonumber (zh:dispatch :GetAnimationTrackCount {:entityRaw ent}))))

(set Animator.get_track_names
     (fn [ent]
       (let [count (Animator.get_track_count ent)]
         (collect [i (range 0 (- count 1))]
           (let [n (ffi-call :GetAnimationTrackName :GetTrackNameArgs
                             {:entityRaw ent :trackIndex i} :outName)]
             (values i (if n (string.upper n) :UNKNOWN)))))))

(set Animator.resolve_track_index
     (fn [ent query]
       (if (= (type query) :number)
           query
           (let [names (Animator.get_track_names ent)
                 q (string.upper query)]
             (or (find-exact names q) (find-by-synonyms names q)
                 (find-substring names q) (find-locomotion-fallback names q))))))

(set Animator.play
     (fn [ent name-or-idx options]
       (let [options (or options {})
             idx (Animator.resolve_track_index ent name-or-idx)]
         (if (not idx)
             (let [track-name (tostring name-or-idx)]
               (when (not (. Animator.unresolved_warnings track-name))
                 (zh.warn (.. "[Animator] Failed to resolve track: " track-name))
                 (tset Animator.unresolved_warnings track-name true))
               false)
             (let [args (ffi.new :PlayTrackArgs
                                 {:entityRaw ent
                                  :trackIndex idx
                                  :blendDuration (or options.blend_duration
                                                     0.15)
                                  :loop (if (= options.loop false) 0 1)
                                  :playbackSpeed (or options.speed 1.0)})]
               (not= (zh:dispatch :PlayAnimationTrack args) 0))))))

;; ============================================================================
;; FENNEL NOISE & TERRAIN MATH GENERATOR
;; ============================================================================

(fn hash2d [x y]
  (let [n (+ (* x 1597.0) (* y 5147.0))
        h (math.fmod (* n 43758.5453) 1.0)]
    (if (< h 0.0) (+ h 1.0) h)))

(fn lerp [a b t]
  (+ a (* t (- b a))))

(fn noise2d [x y]
  (let [ix (math.floor x)
        iy (math.floor y)
        fx (- x ix)
        fy (- y iy)
        ux (* fx fx fx (+ (* fx (- (* fx 6.0) 15.0)) 10.0))
        uy (* fy fy fy (+ (* fy (- (* fy 6.0) 15.0)) 10.0))
        n00 (hash2d ix iy)
        n10 (hash2d (+ ix 1) iy)
        n01 (hash2d ix (+ iy 1))
        n11 (hash2d (+ ix 1) (+ iy 1))]
    (lerp (lerp n00 n10 ux) (lerp n01 n11 ux) uy)))

(fn compute-height [x z max-height]
  (let [peak-x -50.0
        peak-z -50.0
        dx (- x peak-x)
        dz (- z peak-z)
        dist-peak (math.sqrt (+ (* dx dx) (* dz dz)))
        mountain-mask (math.exp (/ (- (* dist-peak dist-peak)) (* 75.0 75.0)))
        ridge-dist (math.abs (+ (* x 0.6) (* z 0.8) 15.0))
        ridge-mask (math.exp (/ (- ridge-dist) 30.0))
        dist-spawn (math.sqrt (+ (* x x) (* z z)))
        spawn-clearing (math.max 0.0
                                 (math.min 1.0 (/ (- dist-spawn 12.0) 20.0)))
        tx (* x 0.015)
        tz (* z 0.015)
        warp-x (noise2d (+ tx 1.7) (+ tz 2.3))
        warp-z (noise2d (+ tx 4.1) (+ tz 8.5))
        n1 (noise2d (+ (* tx 2.0) (* warp-x 0.6)) (+ (* tz 2.0) (* warp-z 0.6)))
        n2 (* (noise2d (* tx 4.5) (* tz 4.5)) 0.5)
        n3 (* (noise2d (* tx 9.0) (* tz 9.0)) 0.25)
        sharp-ridge (let [r (- 1.0
                               (math.abs (- (* (noise2d (* tx 3.5) (* tz 3.5))
                                               2.0)
                                            1.0)))]
                      (* r r r))
        base-hills (* (+ n1 n2 n3) 5.0)
        mountain-elevation (* (+ (* mountain-mask 32.0) (* ridge-mask 16.0))
                              (+ 0.5 (* sharp-ridge 0.9)))]
    (+ (* (+ base-hills mountain-elevation) spawn-clearing)
       (* (- 1.0 spawn-clearing) 2.0))))

(fn generate-fennel-snow-mountain [sample-count world-size max-height]
  (let [total-verts (* sample-count sample-count)
        heights (ffi.new "float[?]" total-verts)
        colors (ffi.new "float[?]" (* total-verts 4))
        half-size (/ world-size 2.0)
        step (/ world-size (- sample-count 1))]
    (for [z 0 (- sample-count 1)]
      (for [x 0 (- sample-count 1)]
        (let [world-x (+ (- half-size) (* x step))
              world-z (+ (- half-size) (* z step))
              idx (+ x (* z sample-count))
              h (compute-height world-x world-z max-height)]
          (tset heights idx h))))
    (for [z 0 (- sample-count 1)]
      (for [x 0 (- sample-count 1)]
        (let [idx (+ x (* z sample-count))
              c-idx (* idx 4)
              y (. heights idx)
              x-left (math.max 0 (- x 1))
              x-right (math.min (- sample-count 1) (+ x 1))
              z-down (math.max 0 (- z 1))
              z-up (math.min (- sample-count 1) (+ z 1))
              h-L (. heights (+ x-left (* z sample-count)))
              h-R (. heights (+ x-right (* z sample-count)))
              h-D (. heights (+ x (* z-down sample-count)))
              h-U (. heights (+ x (* z-up sample-count)))
              dx-h (- h-L h-R)
              dz-h (- h-D h-U)
              dy-h (* 2.0 step)
              len (math.sqrt (+ (* dx-h dx-h) (* dy-h dy-h) (* dz-h dz-h)))
              slope (/ dy-h len)
              norm-y (/ y max-height)]
          (if (< slope 0.68)
              ;; Steep Cliff -> Dark Slate Rock
              (let [rock-n (noise2d (* x 0.1) (* z 0.1))
                    r (+ 0.11 (* rock-n 0.04))
                    g (+ 0.13 (* rock-n 0.04))
                    b (+ 0.16 (* rock-n 0.05))]
                (tset colors (+ c-idx 0) r)
                (tset colors (+ c-idx 1) g)
                (tset colors (+ c-idx 2) b)
                (tset colors (+ c-idx 3) 1.0))
              (< slope 0.82)
              ;; Transition -> Light Snow Dusting on Rock
              (let [t (/ (- slope 0.68) 0.14)
                    r (lerp 0.12 0.88 t)
                    g (lerp 0.14 0.92 t)
                    b (lerp 0.17 0.98 t)]
                (tset colors (+ c-idx 0) r)
                (tset colors (+ c-idx 1) g)
                (tset colors (+ c-idx 2) b)
                (tset colors (+ c-idx 3) 1.0))
              (> norm-y 0.65)
              ;; High Snow Cap
              (do
                (tset colors (+ c-idx 0) 0.97)
                (tset colors (+ c-idx 1) 0.98)
                (tset colors (+ c-idx 2) 1.0)
                (tset colors (+ c-idx 3) 1.0))
              (< norm-y 0.12)
              ;; Glacial Ice Tint
              (do
                (tset colors (+ c-idx 0) 0.78)
                (tset colors (+ c-idx 1) 0.88)
                (tset colors (+ c-idx 2) 0.95)
                (tset colors (+ c-idx 3) 1.0))
              ;; Powder Snow
              (let [snow-v (+ 0.9 (* 0.05 (math.sin (* y 0.4))))]
                (tset colors (+ c-idx 0) (* snow-v 0.94))
                (tset colors (+ c-idx 1) (* snow-v 0.97))
                (tset colors (+ c-idx 2) snow-v)
                (tset colors (+ c-idx 3) 1.0))))))
    (values heights colors)))

;; ============================================================================
;; 1. CONFIGURE GRAPHICS & ATMOSPHERIC BLIZZARD POST-PROCESSING
;; ============================================================================
(zh:config {:giMode 2
            :aoRadius 1.4
            :aoBias 0.02
            :aoPower 2.2
            :giIntensity 2.2
            :giSamples 24
            :useLocalProbe 0
            :vignetteIntensity 1.25
            :vignettePower 1.8
            :enableSSR 1
            :enableTAA 1
            :taaFeedback 0.96
            :ambientExposure 22.0})

(require :scripts.main_menu)

;; ============================================================================
;; 2. PLAYER RESPAWN
;; ============================================================================
(fn RespawnPlayer []
  (when _G.player_ent
    (when pomni-parts
      (each [_ part-ent (ipairs pomni-parts)]
        (zh:dispatch :DestroyEntity {:entityRaw part-ent})))
    (zh:dispatch :DestroyEntity {:entityRaw _G.player_ent})
    (set _G.player_ent (zh:dispatch :InitPlayer))
    (set pomni-parts (zh:spawn :tadc_models/POMNI.glb {:animated true}))
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
        (zh.log "[Snow Scene] Player successfully respawned!")))))

;; ============================================================================
;; 3. GAMESTART CALLBACK
;; ============================================================================
(fn _G.StartGame []
  (zh.log "[Snow Scene] Generating Volumetric Blizzard Environment...")
  (set won-game false)
  (set total-time 0.0)
  ;; 1. Initialize Player Capsule safely above ground at (0, 8, 0)
  (set _G.player_ent (zh:dispatch :InitPlayer))
  ;; 2. Calculate heightmap and vertex colors entirely in Fennel
  (let [sample-count 128
        world-size 280.0
        max-height 35.0
        (heights colors) (generate-fennel-snow-mountain sample-count world-size
                                                        max-height)]
    ;; 3. Spawn terrain
    (set g-snow-terrain
         (zh:spawn_terrain {: heights
                            : colors
                            :sample_count sample-count
                            :world_size world-size
                            :max_height max-height
                            :roughness 0.85
                            :metallic 0.05})))
  ;; 4. Sunlight (Low-angle cool winter sun penetrating the blizzard)
  (let [sun (zh:spawn_light {:type LightType.SUN
                             :rotation [-0.55 0.35 0.1 0.76]
                             :color [0.85 0.92 1.0]
                             :intensity 210.0
                             :radius 0.6
                             :range 450.0})]
    (zh.ecs:add sun :SunTagComponent))
  ;; 5. Cozy Warm Campfire Point Light at starting clearing (Warm volumetric shaft)
  (set g-campfire-light (zh:spawn_light {:type LightType.POINT
                                         :position (zh.vec3 4.0 3.5 4.0)
                                         :color [1.0 0.5 0.12]
                                         :intensity 320.0
                                         :radius 0.6
                                         :range 30.0}))
  ;; 6. Fast Flying Glacial Wind Wisps
  (set g-wisp-1 (zh:spawn_light {:type LightType.POINT
                                 :position (zh.vec3 -15.0 12.0 -15.0)
                                 :color [0.15 0.85 1.0]
                                 :intensity 240.0
                                 :radius 0.4
                                 :range 25.0}))
  (set g-wisp-2 (zh:spawn_light {:type LightType.POINT
                                 :position (zh.vec3 20.0 18.0 -30.0)
                                 :color [0.4 0.7 1.0]
                                 :intensity 220.0
                                 :radius 0.4
                                 :range 25.0}))
  ;; 7. Golden Beacon Light on mountain summit at (-50, 36, -50)
  (set g-summit-light (zh:spawn_light {:type LightType.POINT
                                       :position (zh.vec3 -50.0 38.0 -50.0)
                                       :color [1.0 0.85 0.2]
                                       :intensity 380.0
                                       :radius 0.8
                                       :range 45.0}))
  ;; 8. Spawn Character
  (set pomni-parts (zh:spawn :tadc_models/POMNI.glb {:animated true}))
  ;; 9. Bind Hierarchy & Ragdoll
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
      (zh.log "[Snow Scene] Character bound to snow world controller.")))
  (set _G.game_started true))

;; ============================================================================
;; 4. ECS SYSTEMS
;; ============================================================================
(var was-r-down false)

(defsystem player-input-system
  [_dt]
  (each [player-ent movement (zh.ecs:view :MovementComponent)]
    (let [yaw-rad (math.rad zh.camera.yaw)
          forward-x (math.cos yaw-rad)
          forward-z (math.sin yaw-rad)
          right-x (- (math.sin yaw-rad))
          right-z (math.cos yaw-rad)]
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
        ;; Ragdoll Toggle
        (let [is-r-down (zh.input:is_key_down :R)]
          (when (and is-r-down (not was-r-down))
            (let [ragdoll (zh.ecs:get player-ent :RagdollComponent)]
              (when ragdoll
                (let [pomni-root (. pomni-parts 1)]
                  (if (= ragdoll.state RagdollState.STANDING)
                      (do
                        (set ragdoll.state RagdollState.RAGDOLL_LIMP)
                        (zh.log "Player collapsed into the blizzard!")
                        (zh.audio:beep 150.0 0.25 0.3)
                        (zh.ecs:remove pomni-root :HierarchyComponent)
                        (let [root-trans (zh.ecs:get pomni-root
                                                     :TransformComponent)]
                          (when root-trans (tset root-trans.position 1 0.0))))
                      (do
                        (set ragdoll.state RagdollState.STANDING)
                        (zh.log "Player stood up in the blizzard!")
                        (let [root-trans (zh.ecs:get pomni-root
                                                     :TransformComponent)]
                          (when root-trans (tset root-trans.position 1 -0.8)))
                        (zh.ecs:add pomni-root :HierarchyComponent
                                    {:parent player-ent})))))))
          (set was-r-down is-r-down))))))

;; Wind & Campfire Flickering System
(defsystem blizzard-wind-system
  [dt]
  (set total-time (+ total-time dt))
  ;; 1. Fire Gust Flickering
  (let [fire-light (zh.ecs:get g-campfire-light :LightComponent)]
    (when fire-light
      (let [gust (+ 300.0 (* 55.0 (math.sin (* total-time 14.0)))
                    (* 30.0 (math.cos (* total-time 28.0))))]
        (set fire-light.intensity gust))))
  ;; 2. Blowing Glacial Wisps across the snow
  (let [w1-trans (zh.ecs:get g-wisp-1 :TransformComponent)]
    (when w1-trans
      (let [radius 25.0
            angle (* total-time 0.8)]
        (tset w1-trans.position 0 (+ -20.0 (* (math.cos angle) radius)))
        (tset w1-trans.position 1
              (+ 14.0 (* (math.sin (* total-time 2.1)) 4.0)))
        (tset w1-trans.position 2 (+ -20.0 (* (math.sin angle) radius))))))
  (let [w2-trans (zh.ecs:get g-wisp-2 :TransformComponent)]
    (when w2-trans
      (let [radius 32.0
            angle (+ (* total-time -0.6) 1.57)]
        (tset w2-trans.position 0 (+ 15.0 (* (math.cos angle) radius)))
        (tset w2-trans.position 1
              (+ 20.0 (* (math.cos (* total-time 1.7)) 5.0)))
        (tset w2-trans.position 2 (+ -30.0 (* (math.sin angle) radius)))))))

(defsystem camera-fov-system
  [_dt]
  (each [player-ent movement (zh.ecs:view :MovementComponent)]
    (each [_ cam (zh.ecs:view :TargetCameraComponent)]
      (when (= cam.target player-ent)
        (set cam.targetFov (if movement.isSprinting 55.0 45.0))))))

(defsystem visual-feedback-system
  [dt]
  (each [player-ent combat (zh.ecs:view :combat)]
    (each [_ cam (zh.ecs:view :TargetCameraComponent)]
      (when (= cam.target player-ent)
        (if (< combat.hp 40)
            (let [pulse (math.sin (* total-time 6.0))]
              (set cam.vignetteIntensity (+ 1.4 (* 0.35 pulse)))
              (set cam.vignettePower 2.0))
            (do
              (set cam.vignetteIntensity 1.15)
              (set cam.vignettePower 1.6)))))))

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
              (when (not= target-state current-anim-state)
                (set current-anim-state target-state)
                (each [_ part-ent (ipairs pomni-parts)]
                  (when (zh.ecs:has part-ent :AnimatorComponent)
                    (let [options {:blend_duration 0.15 :loop true :speed 1.0}]
                      (if (= target-state :JUMP)
                          (set options.loop false)
                          (= target-state :LAND)
                          (do
                            (set options.loop false)
                            (set options.speed 1.6)))
                      (Animator.play part-ent target-state options))))))))))))

(defsystem check-fall-system
  []
  (when _G.player_ent
    (let [state (zh.ecs:get _G.player_ent :PhysicsStateComponent)]
      (when (and state (< (. state.currPosition 1) -10.0))
        (zh.log "[Snow Scene] Player fell off the mountain! Respawning...")
        (RespawnPlayer)))))

(defsystem summit-victory-system
  []
  (when (and _G.player_ent (not won-game))
    (let [state (zh.ecs:get _G.player_ent :PhysicsStateComponent)]
      (when state
        ;; Summit beacon target coordinates: [-50.0, 35.0, -50.0]
        (let [dx (- (. state.currPosition 0) -50.0)
              dy (- (. state.currPosition 1) 35.0)
              dz (- (. state.currPosition 2) -50.0)
              dist (math.sqrt (+ (* dx dx) (* dy dy) (* dz dz)))]
          (when (< dist 8.0)
            (set won-game true)
            (zh.log "[Snow Scene] VICTORY! You climbed through the blizzard to the Summit Beacon!")
            (zh.audio:beep 880 0.25 0.3)
            (zh.audio:beep 1100 0.25 0.3)
            (zh.audio:beep 1320 0.45 0.3)))))))

;; --- Register Systems ---
(zh.scheduler.register :PlayerInput 10 player-input-system)
(zh.scheduler.register :BlizzardWind 12 blizzard-wind-system)
(zh.scheduler.register :PlayerAnimations 15 player-animation-system)
(zh.scheduler.register :CameraFOV 30 camera-fov-system)
(zh.scheduler.register :VisualFeedback 25 visual-feedback-system)
(zh.scheduler.register :CheckFall 35 check-fall-system)
(zh.scheduler.register :SummitVictory 40 summit-victory-system)

(zh.log "[Snow Scene] Volumetric blizzard systems successfully registered.")
