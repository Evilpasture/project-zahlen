---@meta
-- Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
-- SPDX-License-Identifier: GPL-3.0-or-later
-- This file is used purely for IDE autocompletion and type-inference. Do not run it.

-- ============================================================================
-- 1. MATHEMATICAL & GEOMETRIC TYPES
-- ============================================================================

---@class vec3
---@field x number
---@field y number
---@field z number
local vec3 = {}

---Creates a new 3-element floating-point vector.
---@param x? number
---@param y? number
---@param z? number
---@return vec3
function vec3.new(x, y, z) end

---Returns the squared length of the vector.
---@return number
function vec3:length_sq() end

---Returns the Euclidean length of the vector.
---@return number
function vec3:length() end

---Returns a normalized copy of the vector.
---@return vec3
function vec3:normalized() end

---Returns the cross product of this vector and another.
---@param b vec3
---@return vec3
function vec3:cross(b) end

-- ============================================================================
-- 2. BUFFER VIEW PROTOCOL (Shared C/Lua memory)
-- ============================================================================

---@class ZHLN_BufferView
---@field buf lightuserdata Raw starting address of the data array.
---@field obj lightuserdata Owner sync pointer (used for RAII release).
---@field len integer Total length of the array in bytes.
---@field itemsize integer Size of a single element in bytes.
---@field format string Character format descriptor (e.g., "f", "d", "EvtF").
---@field readonly integer 1 if read-only, 0 if writable.
---@field ndim integer Number of dimensions.
---@field shape integer[] Array representing shape per dimension.
---@field strides integer[] Stride offsets per dimension.
---@field flags integer Real-estate flags (alignment, contiguity).
---@field owner_type integer Underlying C++ owner system type.
local ZHLN_BufferView = {}

---Queries a value from a multi-dimensional buffer view.
---@param i integer Index in 1st dimension.
---@param j? integer Index in 2nd dimension.
---@param k? integer Index in 3rd dimension.
---@param l? integer Index in 4th dimension.
---@return any
function ZHLN_BufferView:get(i, j, k, l) end

---Sets a value in a multi-dimensional buffer view.
---@param val any Value to write.
---@param i integer Index in 1st dimension.
---@param j? integer Index in 2nd dimension.
---@param k? integer Index in 3rd dimension.
---@param l? integer Index in 4th dimension.
function ZHLN_BufferView:set(val, i, j, k, l) end

---Explicitly releases the structural shadow lock held on this buffer's parent.
function ZHLN_BufferView:release() end

-- ============================================================================
-- 3. NATIVE & DYNAMIC ECS COMPONENTS
-- ============================================================================

---@class TransformComponent
---@field position number[] float[4] World position vector [x, y, z, w]
---@field rotation number[] float[4] Rotation quaternion [x, y, z, w]
---@field scale number[] float[4] Local scale factor [x, y, z, w]

---@class MovementComponent
---@field orientation number[] float[4] Current orientation quaternion
---@field prevOrientation number[] float[4] Prior frame orientation quaternion
---@field inputX number Horizontal input direction [-1.0 to 1.0]
---@field inputZ number Vertical input direction [-1.0 to 1.0]
---@field currentYVel number Current vertical velocity
---@field speed number Target translation movement speed
---@field jumpForce number Peak upward jump force
---@field landingTimer number Remaining duration of landing transition state
---@field jumpDelayTimer number Active jump-anticipation (coiling) delay timer
---@field jumpRequested boolean True if jump intent has been queued
---@field isGrounded boolean True if the controller is touching the floor
---@field wasGrounded boolean Grounded state of the prior frame
---@field isSprinting boolean True if sprinting button is active

---@class Mesh
---@field posBuffer integer
---@field attrBuffer integer
---@field skinBuffer integer
---@field indexBuffer integer
---@field vertexCount integer
---@field indexCount integer

---@class RagdollComponent
---@field ragdollInstance lightuserdata Native JPH::Ragdoll pointer
---@field state integer Active state (0=Inactive, 1=KeyframeMotor, 2=Limp)
---@field prevState integer Prior frame state
---@field isAddedToPhysics integer 1 if actively bound to broadphase, else 0
---@field jointOffset integer GPU skeleton matrix array offset
---@field jointCount integer Total bone count
---@field gltfSkin lightuserdata Source cgltf_skin* pointer

---@class NameComponent
---@field name string Fixed-capacity char[64] name buffer

---@class PBRComponent
---@field roughness number Material surface microfacet roughness [0.0 to 1.0]
---@field metallic number Material surface metallic/dielectric factor [0.0 to 1.0]

---@class HierarchyComponent
---@field parent integer 64-bit packed ID of parent entity

---@class TargetCameraComponent
---@field target integer Entity being tracked by camera
---@field distance number Current distance to target
---@field targetDistance number Desired zoom distance
---@field yaw number Horizontal panning angle
---@field pitch number Vertical tilt angle
---@field targetOffset number[] Aligned float[4] look-at offset vector
---@field stiffness number Elastic spring constant for smooth transitions
---@field vignetteIntensity number Post-processing vignette strength
---@field vignettePower number Vignette falloff exponent
---@field fov number Current field of view
---@field targetFov number Target field of view
---@field smoothTargetPos number[] Aligned float[4] low-frequency target position
---@field hasInitSmoothTarget integer 1 if initialized, else 0

---@class TextComponent
---@field text string Fixed-capacity char[256] text buffer
---@field text_len integer String length
---@field x number Screen horizontal coordinate
---@field y number Screen vertical coordinate
---@field scale number Size multiplier
---@field color number[] float[4] RGBA text color
---@field fontIndex integer Font index in bindless array
---@field mesh Mesh Generated geometry mesh metadata
---@field lastDrawX number Cached render position
---@field lastDrawY number Cached render position

---@class UISettingsComponent
---@field defaultFontAtlasIdx integer Default font index in bindless array

---@class UIRectComponent
---@field parentEntity integer Parent UI element entity
---@field x number Horizontal coordinate offset
---@field y number Vertical coordinate offset
---@field width number Element width
---@field height number Element height
---@field anchorMinX number Normalized anchor min [0.0 to 1.0]
---@field anchorMinY number Normalized anchor min [0.0 to 1.0]
---@field anchorMaxX number Normalized anchor max [0.0 to 1.0]
---@field anchorMaxY number Normalized anchor max [0.0 to 1.0]
---@field computedAbsMinX number Output absolute screen-space coordinate
---@field computedAbsMinY number Output absolute screen-space coordinate
---@field computedAbsMaxX number Output absolute screen-space coordinate
---@field computedAbsMaxY number Output absolute screen-space coordinate
---@field hierarchyDepth integer Element z-depth (sorting priority)
---@field clipChildren boolean True if children are clipped to bounds

---@class UIPanelComponent
---@field color number[] float[4] Panel color
---@field borderRadius number[] float[4] Border radius [TL, TR, BR, BL]
---@field textureIndex integer Bindless texture array index
---@field isDirty boolean True if mesh rebuild is queued
---@field mesh Mesh Underlying quad mesh metadata
---@field edgeWidth number Screen-space 9-slice margin
---@field uvLeft number Texture-space 9-slice boundary [0.0 to 1.0]
---@field uvRight number Texture-space 9-slice boundary [0.0 to 1.0]
---@field uvTop number Texture-space 9-slice boundary [0.0 to 1.0]
---@field uvBottom number Texture-space 9-slice boundary [0.0 to 1.0]

---@class UIButtonComponent
---@field flags integer Button states (0=None, 1=Hovered, 2=Pressed, 4=Clicked)

---@class UIDragComponent
---@field targetEntity integer Element translation target
---@field isDragging boolean True if actively dragged by mouse

---@class UIStackComponent
---@field spacing number Spacer gap size
---@field padding number Margin container padding
---@field direction integer Layout flow (0=Horizontal, 1=Vertical)

---@class UITextInputComponent
---@field text string Fixed-capacity char[256] text buffer
---@field cursorIndex integer Caret index in string
---@field isFocused boolean True if receiving keyboard input focus


-- ============================================================================
-- 4. SUBSYSTEMS & RUNTIMES
-- ============================================================================

---@class Registry
local Registry = {}

---Registers a custom Lua table layout into a native C++ sparse set.
---@param comp_name string FFI structure type-name
function Registry:register_dynamic(comp_name) end

---Instantiates a new entity in the sparse set pool.
---@param ent? integer Optional 64-bit packed handle to restore
---@return integer entity 64-bit packed entity identifier
function Registry:create(ent) end

---Removes an entity and unregisters all associated components.
---@param ent integer 64-bit packed entity identifier
function Registry:destroy(ent) end

---Checks if an entity exists and matches current generation index.
---@param ent integer 64-bit packed entity identifier
---@return boolean alive
function Registry:is_alive(ent) end

---Adds or retrieves a component on an entity.
---@generic T : string
---@param ent integer 64-bit packed entity identifier
---@param comp_name T Component type name (e.g. "TransformComponent")
---@param data? table Optional values to write/overwrite
---@return any component FFI pointer or Lua table reference corresponding to the component type
function Registry:add(ent, comp_name, data) end

---Gets a component on an entity, or nil if missing.
---@generic T : string
---@param ent integer 64-bit packed entity identifier
---@param comp_name T Component type name
---@return any? component FFI pointer or Lua table reference
function Registry:get(ent, comp_name) end

---Checks if an entity owns a component.
---@param ent integer 64-bit packed entity identifier
---@param comp_name string Component type name
---@return boolean owned
function Registry:has(ent, comp_name) end

---Unregisters a specific component on an entity.
---@param ent integer 64-bit packed entity identifier
---@param comp_name string Component type name
function Registry:remove(ent, comp_name) end

---Yields a fast iterator over all entities containing specific components.
---@param ... string Component names list (e.g. "TransformComponent", "MovementComponent")
---@return fun(): integer, ... Iterator function returning (entity, comp1, comp2, ...)
function Registry:view(...) end

---Finds the first entity containing NameComponent matching value.
---@param name string Name to query
---@return integer? entity 64-bit packed entity identifier, or nil
function Registry:find(name) end

---@class PhysicsWorld
local PhysicsWorld = {}

---Contains the world-space positions SoA float array.
---@type ZHLN_BufferView
PhysicsWorld.positions = nil

---Contains the world-space velocities SoA float array.
---@type ZHLN_BufferView
PhysicsWorld.velocities = nil

---Contains the frame collision events.
---@type ZHLN_BufferView
PhysicsWorld.contacts = nil

---@class RaycastResult
---@field entity integer 64-bit packed entity identifier of hit object
---@field position vec3 World space point of intersection
---@field normal vec3 Surface normal at contact point
---@field fraction number Distance fraction [0.0 to 1.0]

---Launches a physical raycast query against Jolt shapes.
---@param origin vec3 Starting world coordinates
---@param direction vec3 Normalized direction vector
---@param max_dist? number Max search range (defaults to 1000m)
---@param ignore_entity? integer Entity handle to ignore
---@return RaycastResult? result Hit info table, or nil if no hit
function PhysicsWorld:raycast(origin, direction, max_dist, ignore_entity) end

---Applies an instantaneous force impulse directly to a rigid body.
---@param entity integer 64-bit packed entity identifier
---@param impulse vec3 Force vector to apply
function PhysicsWorld:apply_impulse(entity, impulse) end

---Applies a continuous linear velocity to a dynamic rigid body.
---@param entity integer 64-bit packed entity identifier
---@param velocity vec3 Target velocity vector
function PhysicsWorld:set_linear_velocity(entity, velocity) end

---Applies a movement velocity specifically to a Kinematic Character Virtual.
---@param entity integer 64-bit packed entity identifier
---@param velocity vec3 Target velocity vector
function PhysicsWorld:set_character_velocity(entity, velocity) end

---Checks if the character virtual controller is touching the floor.
---@param entity integer 64-bit packed entity identifier
---@return boolean grounded
function PhysicsWorld:is_grounded(entity) end

---Compiles a Jolt skeletal constraint mapping from visual mesh joints.
---@param player_ent integer Master player entity to bind physics
---@param parts_table integer[] Packed entity IDs representing bone meshes
function PhysicsWorld:setup_ragdoll(player_ent, parts_table) end

---@class Camera
---@field yaw number Horizontal view angle
---@field fov number Camera field of view
local Camera = {}


---@class Input
local Input = {}

---Checks if a keyboard key is currently pressed.
---@param key_name string Case-insensitive key string (e.g. "W", "A", "SPACE", "LSHIFT")
---@return boolean down
function Input:is_key_down(key_name) end

---Returns the raw pixel motion of the mouse pointer this frame.
---@return number dx, number dy
function Input:get_mouse_delta() end

---@class Audio
local Audio = {}

---Plays a simple 2D fire-and-forget sound effect.
---@param filepath string Path inside mounted PAK (e.g. "audio/theme.mp3")
---@param volume? number Volume level (defaults to 1.0)
function Audio:play(filepath, volume) end

---Plays a 3D spatialized sound effect at a static world point.
---@param filepath string Path inside mounted PAK
---@param pos vec3 Position vector
---@param volume? number Volume level (defaults to 1.0)
function Audio:play_3d(filepath, pos, volume) end

---Generates a simple, procedural raw sine wave sound effect on the fly.
---@param frequency? number Pitch in Hz (defaults to 440)
---@param duration? number Length in seconds (defaults to 0.15)
---@param volume? number Volume level (defaults to 0.25)
function Audio:beep(frequency, duration, volume) end

---Loads and compiles an audio asset as a managed sound handle.
---@param filepath string Path inside mounted PAK
---@param spatialized boolean True to update positional 3D panning, false for flat 2D
---@return integer handle 64-bit audio instance pointer
function Audio:create_instance(filepath, spatialized) end

---Triggers playback on a compiled sound instance.
---@param handle integer 64-bit audio instance pointer
function Audio:play_instance(handle) end

---Suspends playback on a compiled sound instance.
---@param handle integer 64-bit audio instance pointer
function Audio:stop_instance(handle) end

---Frees all memory associated with a compiled sound instance.
---@param handle integer 64-bit audio instance pointer
function Audio:destroy_instance(handle) end

---@class DialogueSystem
local DialogueSystem = {}

---Registers a custom dialogue tree structure into the global database.
---@param id string Dialogue registration key
---@param tree table Table representing dialogue nodes, choices, and conditions
function DialogueSystem:register(id, tree) end

---Launches an interactive dialogue session.
---@param npc_ent integer 64-bit packed NPC entity handle
---@param dialogue_id string Dialogue registration key
---@return boolean success
function DialogueSystem:start(npc_ent, dialogue_id) end

---Terminates the active dialogue session and releases input locks.
function DialogueSystem:stop() end

---Tick update hook processing spatial proximity checks.
---@param dt number Delta time in seconds
function DialogueSystem:update(dt) end

---@class Scheduler
local Scheduler = {}

---Registers a systems function to run in the main thread scheduler.
---@param name string System registration key
---@param priority integer Running priority order (lower runs first)
---@param fn fun(dt: number) Function callback
function Scheduler.register(name, priority, fn) end

-- ============================================================================
-- 5. GLOBALS & BOOTSTRAPS
-- ============================================================================

---@class Engine
---@field physics PhysicsWorld Physics world subsystem
---@field camera Camera Viewport transform controller
---@field input Input Peripheral keyboard and mouse tracker
---@field audio Audio Miniaudio playback controller
---@field ecs Registry Sparse set entity registry
---@field dialogue DialogueSystem Cinematic dialogue system
---@field scheduler Scheduler Main thread task scheduler
local Engine = {}

---Dispatches a structured FFI command over the engine IPC boundary.
---@param cmd_name string Command identifier key
---@param args? any FFI struct pointer containing arguments
---@return integer result Output payload returned by C++ handler
function Engine:dispatch(cmd_name, args) end

---Asynchronously spawns a glTF model asset.
---@param path string Filename inside mounted PAK
---@param options? {position?: number[], physics?: boolean, static?: boolean, animated?: boolean, max_entities?: integer}
---@return integer[] entities List of newly spawned entity handles
function Engine:spawn(path, options) end

---Spawns a basic geometric rigid body.
---@param options {type?: "box"|"sphere"|"capsule"|"cylinder"|"plane", size?: vec3, radius?: number, half_height?: number, extent?: number, position?: vec3, rotation?: number[], color?: number[], static?: boolean}
---@return integer entity 64-bit packed entity identifier
function Engine:spawn_entity(options) end

---Spawns an analytical light source.
---@param options {type: integer, position?: number[], rotation?: number[], direction?: number[], color: number[], intensity: number, radius?: number, range?: number, twoSided?: boolean}
---@return integer entity 64-bit packed entity identifier
function Engine:spawn_light(options) end

---Provokes a Vulkan pipeline page fault to force-test device recovery.
function Engine:provoke_device_lost() end

---Bakes a simple flat color material on the fly.
---@param color? number[] RGBA material color factor (defaults to white)
---@return integer pipeline, integer albedoIdx Returns pipeline and albedo index
function Engine:create_material(color) end

---Updates global engine post-processing configuration.
---@param cfg {giMode?: integer, aoRadius?: number, aoBias?: number, aoPower?: number, giIntensity?: number, giSamples?: integer, useLocalProbe?: integer, ambientExposure?: number, probeMin?: vec3, probeMax?: vec3, probePos?: vec3, vignetteIntensity?: number, vignettePower?: number, enableSSR?: integer, enableRTR?: integer, fullBright?: integer, enableTAA?: integer, taaFeedback?: number}
function Engine:config(cfg) end

---Registers a callback for core engine lifecycle events.
---@param event_name "engine.start"|"engine.tick"
---@param callback fun(...) Callback function
function Engine:on(event_name, callback) end

---Manually triggers an engine event.
---@param event_name string Event key
---@param ... any Arguments payload
function Engine:trigger(event_name, ...) end

---@type Engine
_G.zh = nil

---@type Engine
_G.engine = nil

---@type Registry
_G.game_ecs = nil

---@type PhysicsWorld
_G.world = nil

---@type integer 64-bit packed player entity handle, or nil
_G.player_ent = nil

---@type table Virtual file system controller for the inventory subshell
_G.inventory_shell = nil

---The main engine loop tick hook called once per frame.
---@param ptr lightuserdata Raw engine handle
---@param dt number Delta time in seconds
_G.update = function(ptr, dt) end

---The main entry point for the inventory subshell.
---@param cmd string Terminal command string to execute
_G.run_inventory_command = function(cmd) end
