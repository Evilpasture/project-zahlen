# COM Console and scripting inspection

ConsoleDebugger accepts COM::IEngine, COM::IEntities or a host-owned
ScriptComponentAccess. ScriptECSBridge accepts COM::IEntities or that same
host-owned access table. There is no native Engine, ECS::Inspection, Registry,
RenderContext or Jolt dependency in either consumer. The capability remains
reference-counted. Release bridges and retained boxes before the host ProcessScope
retires its task scheduler. Caller phase isolation remains mandatory: no graph or
structural mutation may overlap a property operation.

## What is inspected

The host reflection tables resolve bare property/type names to canonical COM
component names. Core component bindings are offered only when IEntities
publishes their POD ABI. A host can explicitly register its own POD component
manifest. Registering a manifest does not attach components or allocate a
parallel engine registry: the host attaches data through IEntities as usual.
Non-POD roots (std::string/vector owners, opaque native runtime state) cannot be
registered as COM components. They must remain engine-owned or host-owned; the
generic ScriptBinder still reflects host-owned STL values locally. This is an
intentional boundary, not a native inspection compatibility bridge. Metadata
`types`/`type` can still describe a reflected type without granting storage access.

GetRaw now enforces its documented **dynamic-only** contract. A guessed native
family name cannot reveal a typed engine component behind the publication table.
Published POD components are still reached by the normal GetEngineRaw path.
LogicWorld adds the data-only PBR component to its explicit publication list so
debugger commands can be exercised without initializing graphics/audio/windows.

The shared data-only ComponentPolicy identifies external-resource owners. Those
components can be read, but reflective set/element/nested mutations are
refused; Lua patching is no longer part of Console. Replacing physics/audio/GPU handles must go through their owning
COM capability's lifecycle operations, not overwrite resource fields in place.
The policy used by engine SceneResources and the debugger is one definition.

## Commands and host behavior

help/?, types, type, entity/inspect, get, set, patch, sym/addr and find remain.
Numeric entity/address tokens now require complete parsing (no trailing junk).
The `patch` command always reports that Lua patching is unavailable: scripting
lives in `zahlen_plugin_lua` and the standalone Lua sandbox host, never in the
Console translation unit. Zahlen value/matrix/quat Lua conversions are kept with
the Lua implementation in ValueTypes.hpp; Scene.hpp keeps the existing native
Jolt adapters for unported callers.

The real document/fallback/terrain scene host accepts repeatable startup commands:

```
zahlen-scene-host --headless --frames 3 --raster-only \
  --console-command 'find FallbackGround' \
  --console-command 'type PBRComponent'
```

Commands run after scene creation, at the host's isolated startup phase. Results
are recorded in its application-owned GameConsole/history and printed to host
logs. The engine owns neither the console nor a UI callback/context. A GUI host
may display those entries in its own frame; Extras_ConsoleHost does so through
HostUI, submits the real console output and reads back the rendered frame.
If Console was not built, a supplied command fails explicitly rather than being
silently ignored. Existing document, fallback, terrain and graphics watching
behavior otherwise stays unchanged.

## Boxed values, arrays and lifetime

Entity-rooted boxes own their component/property names and a complete path,
including nested struct fields, indexed struct arrays and nested arrays. They
retain their originating capability owner through a host-side lifetime token,
and store **no cached ECS address**. Every get/set resolves entity generation,
component membership and each path/index again. A sparse-set move or growth is
safe; removed components and dead entities fail; reattaching on the same live
entity resolves the new component. An identical entity ID in another world is
not interchangeable: cross-world boxes are refused.

Array snapshots recursively stamp contained boxes, including nested arrays.
Assignment resolves boxed RHS values recursively before invoking typed setters,
so arrays of structs can be copied without stale pointers. Host-owned raw boxes
remain local borrowed values whose lifetime the caller must manage. They never
turn a non-POD host object into engine storage. FixedString fields cross the
host binder as strings; over-capacity assignment fails instead of truncating.

The public ResolveBoxedPointer helper returns a synchronously borrowed POD
interior pointer for immediate host-side reflection only, never a registry or
native resource object. Do not cache that result across operations/mutations.

## Packages and verification

Zahlen::Scripting and Zahlen::Console are independently exported and neither
links Zahlen::Lua. Lua is delivered as the `zahlen_plugin_lua` shared library
(or linked explicitly by the Lua sandbox host/tests); installed Lua requires a
system LuaJIT library, with no Lua headers exposed to Console consumers.

- Existing Console command tests use real renderer-free COM worlds, including
  the unavailable-Lua patch diagnostic, find/entity dumps, matrix ranges and errors.
- Script bridge test uses a POD manifest, fixed strings and fixed arrays through
  COM, preserving scalar/nested/indexed mutation and sparse-set move/removal cases.
- Five extra lifetime cases cover multi-level boxes, nested arrays, growth/moves,
  reattachment/destruction, retained owner lifetime, cross-world rejection,
  recursive boxed assignments, fixed strings and resource-owner write refusal.
- A real Engine + Scene + Console + HostUI GPU test checks get/set/find, dynamic
  raw isolation, body retention, submitted console output and nonblank capture.
- The installed C++26 Console consumer links/runs the debugger and property edits.

No engine scripting behavior, arbitrary native registry inspection or bulk ECS
mapping is introduced. Native compatibility targets elsewhere remain explicitly
unported. Option B Animator/CharacterMotor is the remaining migration slice.
