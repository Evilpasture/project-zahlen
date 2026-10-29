# Discord Rich Presence (local RPC)

`zahlen_discord_rpc` is an optional local Rich Presence client built on `ZHLN::TaskSystem` and `zahlen_socket`'s local-stream API: Unix-domain sockets on POSIX and named pipes on Windows. It does not make an Internet connection. The separate `zahlen_discord_engine` target provides `Discord/EngineIntegration.hpp` helpers that store the client in `Engine::Services` (not ECS); Engine teardown requests worker shutdown and joins it before the service state is destroyed. Applications can use the RPC target without the Engine integration if they manage both the client and `TaskSystem` lifetimes themselves.

```cpp
#include <Discord/EngineIntegration.hpp>

if (auto started = ZHLN::Discord::StartPresence(engine, applicationId); !started) {
    // InvalidApplicationId, AlreadyStarted, TaskSystemUnavailable, or TaskCapacityExhausted.
}

// Later, when game state changes. This only enqueues the value; IPC runs on
// the client's worker thread and does not block the frame.
if (auto updated = ZHLN::Discord::SetPresence(engine, {
        .details = "Exploring Hanoi",
        .state = "Chapter 4"
    }); !updated) {
    // Presence service was not started.
}

// Optional; this only signals the worker. Engine::Services joins it when
// Engine is destroyed.
ZHLN::Discord::StopPresence(engine);
```

The application ID is supplied by the embedding application and must be a decimal Discord Application ID. `Start()` queues a background TaskSystem job and fails fast if the scheduler is unavailable or its fiber capacity is exhausted; it never runs the persistent worker inline on the caller. `SetActivity` and `ClearActivity` only update a coalesced value; IPC and reconnect work run on a background worker. `Stop()` signals cancellation and returns without waiting; client destruction waits for the worker to finish, so destroy clients before shutting down `TaskSystem`. `RichPresence::startTimestamp` is Unix epoch seconds. The desktop Discord client must be running; on POSIX, discovery checks `XDG_RUNTIME_DIR`, `TMPDIR`, `TMP`, `TEMP`, and `/tmp`, using `discord-ipc-0` through `discord-ipc-9`. Windows discovery checks `\\?\pipe\discord-ipc-0` through `discord-ipc-9`.

`PresenceState::Stopping` is reported after cancellation is requested and until the worker exits; `Stopped` means it has finished. `PresenceState::RpcRejected` means Discord returned an RPC `ERROR` event. This matters because the general RPC reference says commands require authentication and reports error 4006, while the Social SDK Rich Presence guide separately documents direct Rich Presence without calling `Connect`. This client performs the version-1 IPC handshake and `SET_ACTIVITY` only; it does not implement OAuth authorization. If the running Discord client rejects the command, the state is surfaced rather than treating the raw IPC route as unrestricted. See the [RPC reference](https://discord.com/developers/docs/topics/rpc) and [Social SDK Rich Presence guide](https://docs.discord.com/developers/discord-social-sdk/development-guides/setting-rich-presence).
