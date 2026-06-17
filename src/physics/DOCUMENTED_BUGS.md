[VULKAN] Skipping unsupported extension: VK_KHR_acceleration_structure
[VULKAN] Skipping unsupported extension: VK_KHR_ray_query
[RenderInit.cpp:133] [Fiber:Main] WARNING: Raytracing context failed to initialize. RTR will be disabled.
[IBLProcessor.hpp:21] [Fiber:Main] [IBL] Generating 2D BRDF Look-Up Table...
[IBLProcessor.hpp:48] [Fiber:Main] [IBL] Generating Diffuse Spherical Harmonics...
[IBLProcessor.hpp:52] [Fiber:Main] [IBL] Generating Specular Pre-filtered Cubemap Mips...
[RenderInit.cpp:352] [Fiber:Main] [IBL] Uploading Linearly Transformed Cosines (LTC) LUTs...
[AudioContext.cpp:67] [Fiber:Main] miniaudio Engine initialized successfully.
[AssetManager.cpp:86] [Fiber:Main] Mounted PAK: build/data/base.pak (406 assets)
[game_main.cpp:726] [Fiber:Main] Window active and presenting. Loading scene assets...
[gameplay.lua:227] [Fiber:Main] [LUA] Gameplay: Systems successfully initialized under the Core Scheduler.
[gameplay.lua:34] [Fiber:Main] [LUA] Scene: Spawning declarative layout...
[glTFImporter.cpp:546] [Fiber:Main] Loaded GLB Prefab: Circus Lobby V9.glb (892 unique mesh parts parsed and cached)
[glTFImporter.cpp:430] [Fiber:Main] [Diagnostics] Mesh Part 'pomni_eyelidsL' has 1 total morph targets. Engine is keeping 1.
[glTFImporter.cpp:430] [Fiber:Main] [Diagnostics] Mesh Part 'pomni_eyelidsR' has 1 total morph targets. Engine is keeping 1.
[glTFImporter.cpp:430] [Fiber:Main] [Diagnostics] Mesh Part 'pomni_eyelidsTOP' has 2 total morph targets. Engine is keeping 2.
[glTFImporter.cpp:430] [Fiber:Main] [Diagnostics] Mesh Part 'pomni_eyes' has 1 total morph targets. Engine is keeping 1.
[glTFImporter.cpp:430] [Fiber:Main] [Diagnostics] Mesh Part 'pomni_neck' has 1 total morph targets. Engine is keeping 1.
[glTFImporter.cpp:430] [Fiber:Main] [Diagnostics] Mesh Part 'pomni_pupils' has 2 total morph targets. Engine is keeping 2.
[glTFImporter.cpp:430] [Fiber:Main] [Diagnostics] Mesh Part 'pomni_teethbot' has 2 total morph targets. Engine is keeping 2.
[glTFImporter.cpp:430] [Fiber:Main] [Diagnostics] Mesh Part 'pomni_teethtop' has 2 total morph targets. Engine is keeping 2.
[glTFImporter.cpp:430] [Fiber:Main] [Diagnostics] Mesh Part 'pomni_tongue' has 1 total morph targets. Engine is keeping 1.
[glTFImporter.cpp:546] [Fiber:Main] Loaded GLB Prefab: tadc_models/POMNI.glb (24 unique mesh parts parsed and cached)
[glTFImporter.cpp:857] [Fiber:Main] Skeletal Ragdoll successfully generated and bound to player controller.
[gameplay.lua:56] [Fiber:Main] [LUA] Scene: Skeletal Ragdoll successfully generated and bound to player controller.
[gameplay.lua:60] [Fiber:Main] [LUA] Scene: Dropping dynamic physics crates...

[ZHLN] Terminal signal on Main Thread. Attempting emergency dump...

DIAGNOSTIC REPORT FOR SIGNAL: SIGSEGV (Access Violation)
Faulting Address: 0xff435263ff4352e3
┌─── STRUCT TRACE: *engine (Struct Reflection) ───
│ Source:  AssertHandler.cpp:402
├──────────────────────────────────────────────────────────────────────────────
ZHLN::Engine {
  std::unique_ptr<EngineImpl> _impl = *0x1488046a0
}
└──────────────────────────────────────────────────────────────────────────────

--- CAMERA DEEP STATE ---
  Position:  (0.000000, 5.081417, 4.431634)
  Direction: Yaw: -90.000000, Pitch: -10.000000

--- FRUSTUM PLANE EQUATIONS (SIMD DECODED) ---
  Plane Left  : [0.805233x -0.102966y -0.583949z] offset: 3.111063
  Plane Right : [-0.805233x -0.102966y -0.583949z] offset: 3.111063
  Plane Top   : [-0.000000x 0.843391y -0.537299z] offset: -1.904507
  Plane Bottom: [-0.000000x -0.976296y -0.216439z] offset: 5.920148
  Plane Near  : [-0.000000x -0.173648y -0.984807z] offset: 5.146687
  Plane Far   : [0.000000x 0.173666y 0.984804z] offset: 994.389282
┌─── DUMP: cam.frustum (Manual Dump) ───
│ Source:  AssertHandler.cpp:429
│ Address: 0x148804730 (128 bytes)
├──────────────────┬───────────────────────────────────────────────────────┬──────────────────┬─────────────────────────┤
│     Address      │ Hex Data                                              │ ASCII            │ Interpretation          │
├──────────────────┼───────────────────────────────────────────────────────┼──────────────────┼─────────────────────────┤
│ 0000000148804730 │ CA 23 4E 3F  CA 23 4E BF  84 BE C9 B2  56 89 22 B2    │ .#N?.#N.....V.". │ flt: 0.805233           │
│ 0000000148804740 │ 04 E3 38 B3  A3 D9 38 33  00 00 00 00  00 00 00 00    │ ..8...83........ │ int: -1288117500        │
│ 0000000148804750 │ E8 DF D2 BD  E8 DF D2 BD  7F E8 57 3F  8A EE 79 BF    │ ..........W?..y. │ flt: -0.102966          │
│ 0000000148804760 │ E0 D0 31 BE  9A D5 31 3E  00 00 00 00  00 00 00 00    │ ..1...1>........ │ flt: -0.173648          │
│ 0000000148804770 │ B4 7D 15 BF  B3 7D 15 BF  7B 8C 09 BF  4E A2 5D BE    │ .}...}..{...N.]. │ flt: -0.583949          │
│ 0000000148804780 │ 5C 1C 7C BF  27 1C 7C 3F  00 00 00 00  00 00 00 00    │ \.|.'.|?........ │ flt: -0.984807          │
│ 0000000148804790 │ AC 1B 47 40  AC 1B 47 40  E5 C6 F3 BF  DB 71 BD 40    │ ..G@..G@.....q.@ │ flt: 3.111063           │
│ 00000001488047A0 │ AA B1 A4 40  EA 98 78 44  F9 02 15 50  F9 02 15 50    │ ...@..xD...P...P │ flt: 5.146687           │
└──────────────────┴───────────────────────────────────────────────────────┴──────────────────┴─────────────────────────┘
┌─── STRUCT TRACE: engine->GetPhysicsContext().GetWorld() (Struct Reflection) ───
│ Source:  AssertHandler.cpp:432
├──────────────────────────────────────────────────────────────────────────────
ZHLN::Physics::PhysicsWorld {
  BufferSync sync = {
    ZHLN::Atomic<int> viewExportCount = {
      int value = 0
    }
    ZHLN::Mutex shadowLock = *0x1365ec984
  }
  JPH::PhysicsSystem * system = 0x1365ea200
  JPH::BodyInterface * bodyInterface = 0x1365ea3a0
  JPH::JobSystem * jobSystem = 0x1365ea700
  JPH::BroadPhaseLayerInterface * bpInterface = 0x0
  JPH::ObjectLayerPairFilter * pairFilter = 0x0
  JPH::ObjectVsBroadPhaseLayerFilter * bpFilter = 0x0
  JPH::ContactListener * contactListener = 0x0
  JPH::TempAllocator * tempAllocator = 0x135bafb60
  uint32_t maxJoltBodies = 5000
  double time = 0.000000
  ZHLN::Atomic<size_t> count = {
    unsigned long value = 899
  }
  size_t capacity = %zu
  size_t slotCapacity = %zu
  ZHLN::Atomic<size_t> freeCount = {
    unsigned long value = 4101
  }
  JPH::Real * positions = 0x1380c8000
  JPH::Real * prevPositions = 0x1380f0000
  float * rotations = 0x138290000
  float * prevRotations = 0x138118000
  float * linearVelocities = 0x138130000
  float * angularVelocities = 0x138148000
  JPH::Array<JPH::BodyID> bodyIDs = *0x1365eca98
  JPH::Array<uint32_t> materialIDs = *0x1365ecab0
  JPH::Array<uint64_t> userData = *0x1365ecac8
  ZHLN::Atomic<bool> isStepping = {
    bool value = 1
  }
  JPH::Array<Command> commandQueue = *0x1365ecb08
  JPH::Array<Command> commandQueueSpare = *0x1365ecb20
  size_t commandCount = %zu
  size_t commandCapacity = %zu
  JPH::Array<const void *> joltBodyPtrs = *0x1365ecb80
  JPH::Array<ZHLN::Atomic<uint64_t>> idToHandleMap = *0x1365ecb98
  JPH::Array<uint32_t> slotToDense = *0x1365ecbb0
  JPH::Array<uint32_t> denseToSlot = *0x1365ecbc8
  JPH::Array<uint32_t> freeSlots = *0x1365ecbe0
  JPH::Array<uint32_t> categories = *0x1365ecbf8
  JPH::Array<uint32_t> masks = *0x1365ecc10
  JPH::Array<ZHLN::Atomic<uint8_t>> slotStates = *0x1365ecc28
  JPH::Array<ZHLN::Atomic<uint32_t>> generations = *0x1365ecc40
  JPH::Array<ContactEvent> contactBuffer = *0x1365ecc80
  ZHLN::Atomic<size_t> contactCount = {
    unsigned long value = 0
  }
  size_t contactCapacity = %zu
  JPH::Array<MaterialData> materials = *0x1365eccc0
  size_t materialCount = %zu
  size_t materialCapacity = %zu
  JPH::Array<JPH::Constraint *> constraints = *0x1365ecd00
  JPH::Array<ZHLN::Atomic<uint32_t>> constraintGenerations = *0x1365ecd18
  JPH::Array<uint8_t> constraintStates = *0x1365ecd30
  JPH::Array<uint32_t> freeConstraintSlots = *0x1365ecd48
  size_t constraintCount = %zu
  size_t constraintCapacity = %zu
  size_t freeConstraintCount = %zu
}
└──────────────────────────────────────────────────────────────────────────────

Stack Trace:
0   libzahlen_engine.dylib              0x0000000102a31e58 ZHLN::GetPoorMansStacktrace() + 72
1   libzahlen_engine.dylib              0x0000000102a37de4 ZHLN::PerformDiagnosticDump(int, void*, ZHLN::Engine*) + 1484
2   libzahlen_engine.dylib              0x0000000102a380ac ZHLN::PosixCrashHandler(int, __siginfo*, void*) + 224
3   libsystem_platform.dylib            0x0000000187093624 _sigtramp + 56
4   libzahlen_engine.dylib              0x0000000102ad88dc ZHLN::PhysicsContext::Step(float) + 764
5   libzahlen_engine.dylib              0x0000000102ad88dc ZHLN::PhysicsContext::Step(float) + 764
6   zahlen                              0x000000010236fa28 ZHLN::UpdateGame(ZHLN::Engine&, float, float&, ZHLN::ScriptRunner&, ZHLN::FileWatcher&, ZHLN::InputSystem&, ZHLN::AnimationSystem&, ZHLN::ArticulationSystem&, ZHLN::TransformSystem&) + 328
7   zahlen                              0x0000000102370434 RunGame(ZHLN::CommandLineOptions const&) + 1480
8   zahlen                              0x000000010235fc14 main + 340
9   dyld                                0x0000000186cbab4c start + 6000