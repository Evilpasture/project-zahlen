# Optional project-light client

`zahlen_project_light_client` is a native client for the existing `project-light`
Python server. It speaks that server's current MessagePack/zlib protocol and
remains separate from `ZHLN.Network` / `ZHLN.Wire`. It is an optional extra and
requires zlib development files plus the the CharacterController and Camera extensions.

The app lives in `app/ProjectLightClientMain.cpp`, as a separate optional
`project-light-client` executable. It does **not** add client switches, macros,
or networking to the default `zahlen` entrypoint or executable. The client and
its application wiring do not change the server or the engine's `src/` and
`include/` core.

## Configure and launch

The executable reads a dotenv-style file; the URI scheme, server, port, user
identity, username, and token are all runtime settings, not compiled defaults.
For a local setup, copy the safe template into the repository root and edit it:

```sh
cp gameplay/ProjectLight/ProjectLightClient/project-light.env.example .env
$EDITOR .env
cmake --build build --target zahlen_project_light_app
./build/project-light-client
```

The loader searches the current directory and its parents for `project-light.env`
then `.env`. It also accepts `PROJECT_LIGHT_ENV_FILE=/private/path/client.env`.
Process environment variables named `PROJECT_LIGHT_SCHEME`,
`PROJECT_LIGHT_SERVER`, `PROJECT_LIGHT_PORT`, `PROJECT_LIGHT_USER_ID`,
`PROJECT_LIGHT_USERNAME`, `PROJECT_LIGHT_TOKEN`, and `PROJECT_LIGHT_PLACE`
override file values. The root `.env` is ignored by Git; keep real credentials
out of tracked files.

The launch URI is optional: the common launch path above needs no CLI flags.
When used, the scheme must match `PROJECT_LIGHT_SCHEME`; query values override
corresponding dotenv settings, while unspecified values retain the file values.
For example, with the template scheme configured:

```sh
./build/project-light-client \
  'lvyblocks://?server=127.0.0.1&port=5555&userId=42&username=Player'
```

`--launch-url 'scheme://?...'` is also accepted. Do not put a real token in shell
history or a launch URI; keep it in the private dotenv file or process
environment. For local debug servers the username is used as provided; an
authenticated deployment must use the identity/token expected by the server.

Without the optional client dependencies, CMake simply does not create the
client library or executable. The default `zahlen` target remains available and
unchanged.

## Transport and current scope

The transport implements the server's four-byte big-endian TCP frame, optional
zlib-compressed body, MessagePack maps, and raw MessagePack UDP datagrams. It
handles the TCP handshake/snapshot acknowledgement, UDP mode, and the server's
TCP realtime fallback. Replicated parts, physics updates, player-controller
states, point/directional lights, ambient lighting, and sound play/stop events
are bridged into Zahlen's ECS/render/audio services. Remote physics poses and
controller roots use a timestamped snapshot ring with a short 20 ms presentation
delay; positions are interpolated and rotations slerped each frame. The local
controller remains immediate. A local `CharacterVirtual` uses the
CharacterController extra and reports `inputs`, `controllerState`, and owned
transforms in the existing server schema.

This bridge draws the built-in primitive shapes (unsupported shapes fall back
to a box). `MeshPart` currently uses a primitive fallback instead of fetching
its `MeshId`; decals, arbitrary project-light post-processing/sky assets, and
the full Motor/Weld constraint graph are not bridged yet. Sounds play when
their `SoundId` names a file available to the local process.
