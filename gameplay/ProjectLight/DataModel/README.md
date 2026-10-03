# DataModel Instance graph

`zahlen_datamodel` is the native project-light object model. It owns one
canonical `Instance` tree with stable 64-bit instance IDs; typed C++ wrappers
provide the script-facing semantics, while unknown instance classes stay in the
tree as `OpaqueInstance` instead of being dropped. Project-light services that
are not yet typed wrappers remain queryable as `OpaqueService`. This target is
independent of LuaJIT and does not create another scripting runtime.

The root starts locally as ID `0` and can take its authoritative server ID when
a snapshot is decoded. Ordinary IDs are never reused, including after an
instance is destroyed. Services are intentionally not created automatically:
the snapshot is the authority for both their IDs and their presence.

Instances derive from Jolt's `JPH::RefTarget` and public strong handles use
`JPH::Ref`, whose count is atomic. Parentage and semantic cross-Instance links
store stable IDs, while each parent's child list owns its children. The ID index
is non-owning and keeps tombstones after final release, so IDs cannot be reused;
these one-way ownership rules avoid intrusive-reference cycles. Parenting
rejects cycles and cross-DataModel references. Tree, property, and lifecycle
events use `DataModelSignal`; its connection state also uses Jolt intrusive
references, and its RAII tokens invalidate cleanly when the signal is destroyed.
Signals are synchronous and main-thread only. Tree mutation and ID resolution
remain owner-thread operations; atomic reference counts protect handle lifetime,
not concurrent access to the graph or ID index. Private child links, callback
slots, and traversal scratch use Zahlen's inline-capacity `Array`, while the
public tree snapshot methods keep their `std::vector` return types. Instance IDs
are indexed with Zahlen's `HashMap`; names remain owning, unrestricted
`std::string` values.

The `Entity` binding on `Instance` is optional and generation-safe. It is an
adapter hook into the ECS, not a second identity system: scripts and replication
continue to address instances by their stable `InstanceId`. The DataModel does
not synchronize or destroy ECS entities itself; a separate adapter owns that
lifecycle and maps the canonical Instance graph to the registry.

Part and spawn-point rotations stay in the DataModel's protocol-facing Euler
representation. The adapter converts them at the physics/ECS boundary with
`ZHLN::Math::EulerDegreesToQuat` and `ZHLN::Math::QuatToEulerDegrees`; it should
not add a second transform cache to `Instance`.

```cpp
#include <DataModel/DataModel.hpp>

using namespace ZHLN::ProjectLight;

DataModel game;
auto workspace = game.Create<WorkspaceService>();
auto part = game.Create<Part>("SpawnPlatform");
if (workspace && part) {
    const auto workspaceParent = (*workspace)->SetParent(game.Root());
    const auto partParent = (*part)->SetParent(*workspace);
    if (workspaceParent && partParent) {
        (*part)->SetPosition(JPH::Vec3(0.0f, 4.0f, 0.0f));
    }
}
```

`DataModel::CreateInstanceWithId` is the protocol-facing factory. Implemented
project-light classes produce typed wrappers; the other built-in service names
produce `OpaqueService` objects, and unrecognized instance classes produce an
`OpaqueInstance`, all retaining the supplied ID and class name. Typed wrappers
cover core tree behavior, services, players, models, humanoids, parts, mesh
parts, spawn points, decals, sounds, and motors.

LuaJIT integration lives in `extensions/Scripting/Lua`; it registers against the
existing `LuaScriptRuntime` through `AddBindingInitializer`, so scripts and
C++ continue to use this same Instance graph. Instance userdata retain
`JPH::Ref<Instance>` handles and are weakly cached by their underlying C++ object
pointer to preserve Lua identity without creating another object world. The
Lua bridge's shared ownership for callback/connection records is separate from
Instance ownership and remains unchanged. Sound `Play`/`Stop` currently update
model state only; audio-backend integration is not part of this binding layer.
The Instance-to-ECS synchronization adapter remains a separate consumer of the
DataModel target.
