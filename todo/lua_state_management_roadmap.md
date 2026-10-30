# Lua as a Pure State & Configuration Engine (`ReflectLua` Roadmap)

## Overview & Diagnosis

Before the completed Lua-state refactor, `extensions/Scripting/Lua/` (~6,034 lines across C++, C ABI shims, FFI layout generators, and Fennel core libraries, plus 3,417 lines of vendored `fennel.lua`) was architected as a **second imperative game engine** inside an engine where C++26 already wins at imperative logic:

- The former imperative script runner dispatched `ScriptRunner::CallUpdate(&engine, dt)` every frame; this path has since been removed.
- `extensions/Scripting/Lua/Scripting.cpp` (`1,332` lines) + `ScriptingABI.*` (`468` lines) + `GenFFICdef.cpp` / `check_ffi_layout.py` (`751` lines) + `scripts/core/*.fnl` (`1,753` lines) maintain a shadow Lua-side ECS (`ecs.fnl`), a shadow Lua-side system scheduler (`scheduler.fnl`), structural shadow-locked `ffi.cast` raw pointers into ECS pools, and imperative verbs (`Raycast`, `SetCharacterVelocity`, `AddImpulseAt`, `PlayOneShot3D`).
- `include/Zahlen/ScriptCommandRegistry.hpp` and `include/Zahlen/IScriptRuntime.hpp` exist in Core solely to feed that imperative jump table.
- Meanwhile, **zero gameplay `.lua` or `.fnl` scripts exist in the repository** (`boot.lua` / `boot.fnl` are absent), because writing systems in C++26 with P2996 reflection, `Module::Describe()`, and hazard-checked `SystemGraph` is strictly safer, faster, and cleaner.

### Active Dependents Audit (`df18037`)

| Consumer | What It Actually Uses Today | Needs Imperative FFI / Fennel / `TickUpdate(dt)`? |
| :--- | :--- | :--- |
| `extensions/Console/ConsoleDebugger.cpp` | `ScriptBinder` + `ScriptECSBridge` (`types`, `type <T>`, `get <id> Comp.field`, `set <id> Comp.field val`) | **No** — does not even link LuaJIT; `call` is not implemented |
| `gameplay/ProjectLight/LuaBindings` + `TestLuaDataModel.cpp` | `LuaScriptRuntime` strictly as an RAII `lua_State*` owner (`AddBindingInitializer` + `ExecuteString`) | **No** — never touches `Scripting.cpp`, `ScriptingABI`, `GenFFICdef`, or Fennel |
| `gameplay/RagdollAuthoring/RagdollAuthoring.cpp` | Contributes `"SetupRagdoll"` to `ScriptCommandRegistry` | **No** — C++ callers invoke `BuildHumanoidBipedRagdoll` directly |
| `app/main.cpp` | Instantiates `LuaScriptRuntime` only inside the `--editor` branch | **No** — no `boot.lua`/`boot.fnl` exists |

---

## Architectural Role: TOML vs. Lua vs. C++26

| Layer | Responsibility | Runs When? | Owns Canonical State? |
| :--- | :--- | :--- | :--- |
| **TOML (`plugins/toml`)** | Static literal documents requiring symmetric save/load (`SerializeTOML` $\leftrightarrow$ `Parse<T>`) | Load / Save | No |
| **Lua (`plugins/lua`)** | **Parameterized data derivation & pure state reducers** `(readonly_inputs) -> table_or_patch` | Boot, scene/asset load, `FileSystemWatcher` reload, discrete events, `GameConsole` (**never on `dt`**) | **Never** (stateless between evaluations) |
| **C++26 (`src/`, `extensions/`, `gameplay/`)** | Canonical state storage (`std::is_trivially_copyable_v` ECS components + monolithic domain contexts) & all per-frame simulation | Every frame | **100% authority** |

### Non-Negotiable Invariants

1. **ECS & Monolithic C++ Contexts Remain the Sole System of Record (`ARCHITECTURE.md` 1.1):**
   - Components remain strictly `std::is_trivially_copyable_v`, with **zero owning heap/arena containers** and **zero temporal non-owning views/pointers** (`std::span`, raw `T*`, `lua_State*`, Lua registry refs).
   - Lua never holds authoritative state across frames in Lua globals or closures; re-evaluating a `.lua` file from scratch is always idempotent.
2. **Zero Per-Frame Lua Execution:**
   - There is no `TickUpdate(dt)` hook. Lua only evaluates at discrete boundaries (startup profile evaluation, scene/prefab hydration, file hot-reload, discrete state-transition events, or interactive developer console commands).
3. **The C++26 Reflected Type *Is* the Schema (`ReflectLua`):**
   - Mirrors `plugins/toml/TOML.hpp` (`ReflectTOML`) and `plugins/json/JSONSchema.hpp`: field names, types, defaults, and enums come directly from `ZHLN::Reflect::ForEachFieldAccessor<T>` and `ZHLN::Reflect::EnumNames<E>` / `StringToEnum<E>`.
   - **Missing keys** leave C++ default member initializers (or existing live values during `ApplyPatch`) untouched.
   - **Unknown keys** fail or warn with `lua_Debug` source/line provenance so typos (`giIntensty = 1.5`) never drop silently.
   - **Transactional commits:** `ApplyPatch(T& target, ...)` hydrates into a stack copy of `T` first and assigns to `target` only when every field in the table validates cleanly; a bad edit during hot-reload leaves live engine state completely untouched.
4. **Hermetic, Bounded Sandbox (`LuaVm`):**
   - Opens only pure data libraries: `base` (with `dofile`, `loadfile`, `collectgarbage` stripped), `math`, `string`, `table`, and `bit`.
   - **Never opens `ffi`, `io`, `os`, `package`, or `debug`** in state-evaluation VMs.
   - Enforces a deterministic instruction ceiling via `lua_sethook(..., LUA_MASKCOUNT, maxInstructions)` and an optional arena/byte ceiling via custom `lua_Alloc`, returning `LuaError::InstructionBudgetExceeded` / `LuaError::MemoryBudgetExceeded` instead of hanging or OOM-ing the process.
5. **Monadic C++26 APIs:**
   - Pure `std::expected<T, ErrorCode>` and `ZHLN::Optional<T>` value-oriented signatures throughout — zero C# `bool TryRead(..., T& out)` out-parameter idioms and zero `(void)` cast noise.
6. **Baseline vs. Non-Destructive Delta Discipline (`ARCHITECTURE.md` 1.1 Rule 5):**
   - Base configuration files hydrate baseline structs/components; runtime state transitions emit partial table patches applied against baseline state rather than clobbering global base values.

---

## Target Surface (`plugins/lua/`)

Placed under `plugins/lua/` (`zahlen_lua`) alongside `plugins/toml/` (`zahlen_toml`) and `plugins/json/` (`zahlen_json`), respecting `configure/check_core_layer_boundary.py` and `configure/check_layer_dag.py`:

```cpp
namespace ZHLN {

enum class LuaError : uint8_t {
    AllocationFailed = 1,
    FileReadFailed,
    SyntaxError,
    RuntimeError,
    InstructionBudgetExceeded,
    MemoryBudgetExceeded,
    NotATable,
    NotAFunction,
    TypeMismatch,
    InvalidEnumString,
    UnknownField,
    OutOfRange,
};

struct LuaBudget {
    uint32_t maxInstructions = 1'000'000;
    size_t   maxMemoryBytes  = 16 * 1024 * 1024;
    bool     rejectUnknownFields = true;
};

namespace ReflectLua {

// Opt-in vector trait (mirrors ReflectTOML::TOMLVector in plugins/toml/SceneTOML.hpp)
// Accepts both positional `{ 1.0, 2.0, 3.0 }` and named `{ x = 1.0, y = 2.0, z = 3.0 }`.
template <typename T>
struct LuaVector : std::false_type {};

class LuaVm {
  public:
    [[nodiscard]] static auto Create(LuaBudget budget = {}) noexcept -> std::expected<LuaVm, ErrorCode>;

    // Escape hatch for isolated host adapters (e.g. ProjectLight DataModelBindings).
    [[nodiscard]] auto RawState() noexcept -> lua_State*;
};

// 1. Evaluate a Lua chunk/file returning a table -> construct T from defaults + table overrides
template <typename T>
[[nodiscard]] auto EvaluateString(std::string_view chunk, std::string_view chunkName = "chunk", LuaBudget budget = {})
    -> std::expected<T, ErrorCode>;

template <typename T>
[[nodiscard]] auto EvaluateFile(const std::filesystem::path& path, LuaBudget budget = {})
    -> std::expected<T, ErrorCode>;

// 2. Evaluate a pure Lua function `return function(in1, in2, ...) -> table` with reflected C++ inputs
template <typename OutT, typename... InArgs>
[[nodiscard]] auto EvaluateCall(std::string_view chunk, std::string_view chunkName, LuaBudget budget, const InArgs&... inputs)
    -> std::expected<OutT, ErrorCode>;

// 3. Transactional partial patch onto an existing C++ struct / ECS component
template <typename T>
[[nodiscard]] auto ApplyPatchString(T& target, std::string_view chunk, std::string_view chunkName = "patch", LuaBudget budget = {})
    -> std::expected<void, ErrorCode>;

template <typename T>
[[nodiscard]] auto ApplyPatchFile(T& target, const std::filesystem::path& path, LuaBudget budget = {})
    -> std::expected<void, ErrorCode>;

} // namespace ReflectLua
} // namespace ZHLN
```

---

## Staged Execution Plan

### Phase 1: Introduce `plugins/lua/` (`zahlen_lua`) & Unit Tests
- [x] Create `plugins/lua/CMakeLists.txt` (`zahlen_lua`, linking `zahlen_engine` + `LuaJIT`, `zahlen_enable_reflection(zahlen_lua)`), guarded by `ZHLN_BUILD_LUA` (default `ON` when `ZHLN_BUILD_EXTRAS=ON`), with hermetic VM isolation (`luaopen_ffi` / `luaopen_package` never opened).
- [x] Implement `plugins/lua/ReflectLua.hpp` and `plugins/lua/ReflectLua.cpp`:
  - Custom `lua_Alloc` wrapper tracking live bytes against `LuaBudget::maxMemoryBytes` + `LUA_MASKCOUNT` hook enforcing `LuaBudget::maxInstructions` (with JIT trace compilation disabled in interpreter state VMs so `LUA_MASKCOUNT` triggers deterministically).
  - Sandboxed global environment (`base` whitelist, `math`, `string`, `table`, `bit`; no `ffi`, `io`, `os`, `package`, `debug`; text-only `"t"` loader rejecting binary bytecode).
  - P2996 reflection hydration (`ForEachFieldAccessor<T>`) and serialization (`PushValue<T>` for passing read-only C++ context structs into Lua pure functions):
    - `bool`, integral types (with integer/range checks), floating-point types, `std::string`, `std::string_view`, fixed `char[N]`, fixed `T[N]` C arrays, `FixedString<N>` component fields, reflected enums (exact + case-insensitive `Reflect::StringToEnum<E>`), `std::optional<U>` / `ZHLN::Optional<U>`, `std::array<U, N>`, `std::vector<U>`, string-keyed maps, nested reflected structs, and `LuaVector<T>` / `LuaValueTrait<T>` specializations.
    - Transactional `ApplyPatch*` & `ApplyCallPatchString`: clone `target` on stack, overlay present table keys, reject unknown keys on demand, commit only on full success.
- [x] Create `plugins/lua/SceneLua.hpp` (mirroring `plugins/toml/SceneTOML.hpp`) specializing `JPH::Float3`, `JPH::Float4`, `JPH::Vec3`, `JPH::Vec4`, and `JPH::Quat`.
- [x] Add `tests/extras/TestReflectLua.cpp` (`ReflectLuaSuite`: `6/6` passed) covering:
  - Default-preserving hydration, nested structs, enums, `JPH::Float3/4/Vec3/Vec4/Quat` array & `{x,y,z}` forms, fixed arrays, and `Optional`.
  - Procedural `ZHLN::Scene::Scene` recipe generation (`TestProceduralSceneRecipe`).
  - Pure function evaluation `EvaluateCallString<GraphicsSettings>` + shipped `config/graphics.lua` & `config/ragdoll_humanoid.lua` (`TestShippedConfigProfiles`).
  - Transactional `ApplyPatchString` (verifying `target` is unchanged when a deep field fails validation).
  - Sandbox enforcement (`while true do end` -> `LuaError::InstructionBudgetExceeded`, exponential string allocation -> `LuaError::MemoryBudgetExceeded`, binary bytecode -> `LuaError::SyntaxError`, `io`/`os`/`ffi`/`package`/`debug`/`jit` -> `nil`).

### Phase 2: Decouple `ConsoleDebugger` & Migrate `ProjectLight/LuaBindings`
- [x] Trim `extensions/Scripting/` into a lean, Lua-free reflection property inspector (`ScriptBinder` + `ScriptECSBridge`) used by `extensions/Console/ConsoleDebugger.cpp`:
  - Removed imperative `ScriptMethod` / PMF method invocation and heap-allocated `OwnedObject` (`std::shared_ptr<void>`), keeping only reflected field get/set/element access over trivially copyable components and value types (`TestScripting`: `3/3` passed).
  - Removed the unimplemented `call` line from `ConsoleDebugger`'s help text and added transactional `patch <entity_id> <Component> <lua_table_expr>` (when `zahlen_lua` is linked) across both `ZHLN::Components` and `ZHLN::GUI` (`ConsoleDebuggerTestSuite`: `9/9` passed).
- [x] Migrated `gameplay/ProjectLight/LuaBindings` and `tests/extras/TestLuaDataModel.cpp` onto `ZHLN::ReflectLua::LuaVm` (`TestLuaDataModelSuite`: `1/1` passed).

### Phase 3: Retire the Imperative FFI / Fennel / Per-Frame Scripting Stack (~10,137 Net LoC Reduction)
- [x] Deleted 24 imperative Lua/Fennel runtime & FFI layout files (`76 files changed, 3,220 insertions(+), 13,357 deletions(-)`):
  - `extensions/Scripting/Lua/Scripting.cpp` (`1,332` lines)
  - `extensions/Scripting/Lua/ScriptingABI.h` & `ScriptingABI.cpp` (`468` lines)
  - `extensions/Scripting/Lua/LuaScriptRuntime.hpp` & `LuaScriptRuntime.cpp` (`363` lines)
  - `extensions/Scripting/Lua/tools/GenFFICdef.cpp`, `FFICTypeMap.hpp`, `check_ffi_layout.py` (`751` lines)
  - `extensions/Scripting/Lua/scripts/core/{ecs,ffi_cdef,memoryview,scheduler,zahlen}.fnl` & `fennel.lua` (`5,170` lines)
  - `cmake/FennelCompiler.cmake` & `tools/fennelc.lua` (`142` lines)
- [x] Simplified Core (`include/Zahlen/`, `src/engine/`, `modules/zahlen.cppm`):
  - Removed `include/Zahlen/ScriptCommandRegistry.hpp` and `src/engine/ScriptCommandRegistry.cpp`.
  - Removed `include/Zahlen/IScriptRuntime.hpp`, `include/Zahlen/Scripting.hpp`, `modules/zahlen-scripting.cppm`, and `src/engine/ScriptRunner.cpp`.
  - Removed per-frame `ScriptRunner::CallUpdate` branches from `src/engine/SystemWiring.cpp` (renaming `"ScriptAndShaderReload"` -> `"StateAndShaderReload"`) and `bootScriptWatches` from `src/engine/Engine.cpp`, leaving C++ gameplay in the application executable (`CommandLineTestSuite`: `3/3` passed).
- [x] Cleaned up build & repo references across `CMakeLists.txt`, `cmake/ThirdParty.cmake`, `Dockerfile`, `README.md`, `include/ARCHITECTURE.md`, `gameplay/FallbackScene/`, `scripts/snapshot.py`, `utils/ecsc/src/codegen.rs`, `configure/error_ordinal_allowlist.json`, and `configure/check_layer_dag.py`.

### Phase 4: Wire State-Only Lua Domains
- [x] **1. Hardware-Adaptive Graphics Profile (`config/graphics.lua`):**
  - Added Core-pure `ApplyGraphicsSettings(Engine&, const GraphicsSettings&)` in `src/engine/system/GraphicsSettingsSync.{hpp,cpp}` and wired `config/graphics.lua` (`ZHLN::RenderInfo` -> `ZHLN::GraphicsSettings` overlay + preset expansion + `FileSystemWatcher` hot-reload in `FramePhase::HotReload` after `"StateAndShaderReload"`) in `app/main.cpp` (`--graphics-config <file.lua>`).
- [x] **2. Parametric Scene Recipes (`--scene <file.toml|file.lua>`):**
  - Added `.lua` and `.toml` scene document loading in `app/main.cpp` (`ReflectLua::EvaluateFile<ZHLN::Scene::Scene>(path)` -> `ZHLN::Scene::Instantiate(engine, *scene)`).
- [x] **3. Data-Driven Ragdoll Authoring Policy (`gameplay/RagdollAuthoring` + `config/ragdoll_humanoid.lua`):**
  - Replaced `SetupRagdollArgs` / `ScriptCommandRegistry` with reflected `RagdollRecipe` (`RagdollJointRule`, `RagdollPartSpec`, `RagdollShapeKind`), `LoadRecipeFromLuaString` / `LoadRecipeFromLuaFile`, and shipped `config/ragdoll_humanoid.lua`.

### Phase 5: Verification & Governance Sign-Off
- [x] `python3 configure/run_governance_checks.py --no-cache` + `check_include_governance.py`: **100% pass** across Core/layer boundary, Layer DAG, `tests/` public-API boundary, Subsystem include boundary, PIMPL encapsulation, Reflection boundary, Macro governance (0 banned, 0 allowlist additions), Namespace governance, Module linkage, Error ordinals, and Include provenance (834 source files).
- [x] GCC 16.2 (`-std=c++26 -freflection -fno-exceptions -fno-rtti -Wall -Wextra -Werror`) non-renderer suite & syntax verification:
  - `ReflectLuaSuite`: `6/6` passed (including shipped `config/graphics.lua` + `config/ragdoll_humanoid.lua`).
  - `ConsoleDebuggerTestSuite`: `9/9` passed (including transactional Lua `patch` & rollback).
  - `TestScripting`: `3/3` passed.
  - `TestLuaDataModelSuite`: `1/1` passed.
  - `CommandLineTestSuite`: `3/3` passed.
  - `-fsyntax-only` across `Engine.cpp`, `SystemWiring.cpp`, `GraphicsSettingsSync.cpp`, `CharacterController.cpp`, `ProjectLightClient.cpp`, `FallbackScene.cpp`, and `app/main.cpp`: **0 warnings, 0 errors**.
