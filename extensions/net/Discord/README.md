# Discord Rich Presence (local RPC)

`zahlen_discord_rpc` is an optional local Rich Presence client built on `zahlen_socket`'s local-stream API: Unix-domain sockets on POSIX and named pipes on Windows. It does not make an Internet connection, and applications own the `PresenceClient` lifetime directly in host code without coupling to engine internals.

```cpp
#include <Discord/PresenceClient.hpp>

ZHLN::Discord::PresenceClient presence;
if (auto started = presence.Start(applicationId); !started) {
    // InvalidApplicationId, AlreadyStarted, or worker launch failure.
}

// Later, when game state changes. This only enqueues the value; IPC runs on
// the client's worker and does not block the frame.
presence.SetActivity({
    .details = "Exploring Hanoi",
    .state = "Chapter 4",
});

// Optional; Stop() requests cancellation and ~PresenceClient joins the worker.
presence.Stop();
```

The application ID is supplied by the embedding application and must be a decimal Discord Application ID. `SetActivity` and `ClearActivity` only update a coalesced value; IPC and reconnect work run on a background worker. `Stop()` signals cancellation and returns without waiting; client destruction waits for the worker to finish. `RichPresence::startTimestamp` is Unix epoch seconds. The desktop Discord client must be running; on POSIX, discovery checks `XDG_RUNTIME_DIR`, `TMPDIR`, `TMP`, `TEMP`, and `/tmp`, using `discord-ipc-0` through `discord-ipc-9`. Windows discovery checks `\\?\pipe\discord-ipc-0` through `discord-ipc-9`.

`PresenceState::Stopping` is reported after cancellation is requested and until the worker exits; `Stopped` means it has finished. `PresenceState::RpcRejected` means Discord returned an RPC `ERROR` event. This client performs the version-1 IPC handshake and `SET_ACTIVITY` only; it does not implement OAuth authorization. See the [RPC reference](https://discord.com/developers/docs/topics/rpc) and [Social SDK Rich Presence guide](https://docs.discord.com/developers/discord-social-sdk/development-guides/setting-rich-presence).
