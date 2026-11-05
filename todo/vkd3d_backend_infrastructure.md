# Vulkan / Direct3D 12 backend boundary plan

## Goal

Introduce the address-model and build-target seams for the future `zahlen_direct3d` and `zahlen_vkd3d` backends without migrating the renderer yet. `src/render` remains Vulkan-specific and continues to use `zahlen_vulkan` directly during this phase.

## Landed in this infrastructure patch

- [x] Add `ZHLN::D3D::BufferAddress<T>` in `src/d3d12/include/d3d12/memory/BufferAddress.hpp` as a header-only SM 6.6 descriptor-slot plus byte-offset address. It has no Windows SDK dependency, so its type and target can be configured on non-Windows hosts.
- [x] Make `ZHLN::Vk::DeviceAddress<T>` independently includable through Vulkan/volk headers, while preserving the existing Vulkan address API and pointer-arithmetic behavior.
- [x] Add joint-core enum spellings: `Vk::` in `src/vulkan/include/vulkan/Enums.hpp` and neutral `VkD3D::` in `src/vkd3d/include/vkd3d/Enums.hpp` (pure vocabulary, no backend values). Translation lives with the side that owns the constants: neutral<->Vk:: in `src/vkd3d/include/vkd3d/VulkanConversions.hpp`, neutral->D3D12 in `src/vkd3d/D3D12Conversions.hpp` against the real DirectX headers (no SDK-free mirror enum, no hand-typed SDK integers). Raw `VK_IMAGE_LAYOUT_*` / `VK_FORMAT_*` / `VK_PIPELINE_STAGE_2_*` / `VK_ACCESS_2_*` / `VK_IMAGE_ASPECT_*` and the pipeline-state families are swept to the `Vk::` spellings in src/vulkan and src/render (render stays on Vk::, no VkD3D migration yet); BarrierStage/BarrierAccess are now aliases of Vk::PipelineStage/Vk::Access. Not yet swept: VK_DESCRIPTOR_TYPE_*, VK_PRESENT_MODE_*, swapchain surface families.
- [x] Add `ZHLN::VkD3D::DeviceAddress<T>` in the private `src/vkd3d/GpuAddress.hpp` as the 64-bit shared wire contract. Backend conversions are concentrated in this type; no GPU address wrapper is installed or exposed through `include/Zahlen`, and the current renderer has not been switched to it.
- [x] Add concrete build targets `zahlen_d3d12`, `zahlen_direct3d`, and `zahlen_vkd3d` (`Zahlen::D3D12` / `Zahlen::Direct3D` / `Zahlen::VkD3D` aliases). Their internal include paths and backend link dependencies are not propagated as a public engine API.
- [x] Add `resources/shaders/gpu_memory.slang` as the single shader-side location for SPIR-V physical-BDA versus DXIL descriptor-heap lowering. Register it as a shared shader input for dependency tracking.
- [x] Migrate every shader off `gpu_buffer.slang`: all `GPUBuffer<T>` uses are `DeviceAddress<T>` from `gpu_memory.slang` (loads, stores, uint atomics, `isValid()` — the API is a 1:1 match), the two dead imports (`rcgi_common.slang`, `reflection.slang`) are gone, and `gpu_buffer.slang` itself is deleted. Raw pointer casts now exist only inside `gpu_memory.slang` (plus the intentional `hang_gpu.slang` diagnostic).
- [x] Formulate the D3D12/DXIL root-signature convention: space0 `b0` = root constants (pass payload, 64-DWORD budget matching `GpuAbi::kScenePassPayloadBytes`), space0 `b1` = CBV over `DescriptorHeapPushData`, SM 6.6 bindless heaps implicit (no descriptor tables), profile `sm_6_6`. Stated once in `cmake/ShaderCompilation.cmake` (`ZHLN_DXIL_PROFILE` / `ZHLN_DXIL_ROOT_SIGNATURE`), mirrored in `resources/shaders/descriptor_heap_layout.slang`, and summarized in `resources/shaders/SHADER.md`. Passes keep declaring `[vk::push_constant]`.
- [x] Root-constants refactor: the 64-DWORD budget is now law host-side -- `GpuAbi::kRootConstantMaxDwords` / `kRootConstantMaxBytes`, a `static_assert` that `ScenePassPushConstants` fits it, and the `RootConstantPayload` concept that `ScenePassPayload` builds on. The DXIL cook exists: `compile_dxil` in `cmake/ShaderCompilation.cmake` (mirrors `compile_slang`, `-target dxil`, per-stage `sm_6_6` profile derived from `ZHLN_DXIL_PROFILE`, `-D__TARGET_DXIL__=1`, no catalog reflection records -- the zshader replay stays SPIR-V-only), with the opt-in `zahlen_shaders_dxil` target smoke-cooking the skinning compute entry (push payload + `DeviceAddress<T>` loads/stores/atomics). Needs a DXIL-capable slangc (host/SDK, or vendored with `-DSLANG_ENABLE_DXIL=ON`); not part of the default build.
- [x] Keep all current `src/render` sources, target links, and draw-command address usage unchanged.

## Follow-up implementation phases

### 1. Validate the shared address contract

- [x] Add compile-time checks for conversions, 64-bit layout, slot-zero validity, null handling, and typed positive/negative arithmetic.
- [x] Use one-based encoded slot IDs (`logical slot + 1`) so logical slot zero / offset zero is non-null while raw zero remains the null sentinel. Decode only at the D3D12 descriptor-heap access boundary.
- [x] Reject pointer steps that overflow or underflow the 32-bit D3D12 byte offset by producing null; never allow carry/borrow into the slot field.
- Add runtime tests once resource allocation exists to verify descriptor lifetime and report invalid/out-of-range pointer steps.

### 2. Compile and validate the Slang module

- Cook a minimal test entry point for both `spirv_1_5`/physical-storage-buffer and DXIL Shader Model 6.6.
- Confirm the emitted SPIR-V uses physical-storage-buffer loads/stores and the DXIL uses descriptor-heap raw-buffer operations.
- Cover aggregate loads/stores, read-only versus read-write resources, atomics, non-uniform descriptor indexing, and null checks. Keep all backend conditionals in `gpu_memory.slang`.
- [x] Import the module from the existing shader passes (done with the `gpu_buffer.slang` retirement above; both target paths passed their compiler checks per the validation log).

### 3. Build native Direct3D 12 backend

Target shape: SM 6.6+ bindless (`ResourceDescriptorHeap`), Enhanced Barriers (`D3D12_BARRIER_*`), and compile-time zero-overhead synthesis of driver structs on the stack — no heavyweight virtual interfaces, mirroring how `src/vulkan` works today.

- `D3D12Context`: `CreateDXGIFactory2` (`DXGI_CREATE_FACTORY_DEBUG` when validation is on); adapter enumeration and scoring (discrete vs. integrated, cf. `SelectPhysicalDevice`); verify `D3D12_FEATURE_DATA_D3D12_OPTIONS12::EnhancedBarriersSupported` and Shader Model 6.6 (`D3D12_FEATURE_DATA_SHADER_MODEL`); create `ID3D12Device` / `ID3D12Device10`.
- Timeline synchronization: `ID3D12Fence` is the 1:1 analogue of the Vulkan timeline semaphore (`vkWaitSemaphores` / `VkSemaphoreSubmitInfo`). Implement a D3D12 `FrameSync`: graphics + compute timeline fences, `Signal(value)` on queue submission, `SetEventOnCompletion` on CPU wait and `ID3D12CommandQueue::Wait` on GPU queues.
- Global descriptor heaps (no classic descriptor tables): one shader-visible `CBV_SRV_UAV` heap sized 500,000–1,000,000 slots, one shader-visible `SAMPLER` heap (max 2048). On resource creation, write the descriptor into `ResourceDescriptorHeap[slot]` and mint `D3D::BufferAddress<T>(slot, byteOffset)` for the CPU wire format.
- Heap allocator: a basic D3D12 heap allocator exposing `BufferAddress<T>` (slot + offset) to feed shader-side `DeviceAddress<T>`.
- Preserve descriptor view kind in buffer allocation: any address used by `store()` or `atomicAdd()` must resolve to a UAV descriptor; read-only `load()` addresses may use SRVs. Never reinterpret one descriptor as the other at shader access time.
- `D3D12BarrierRecorder`: [x] synthesis landed in the joint layer (`src/vkd3d/D3D12Barriers.hpp` + `D3D12Conversions.hpp`: neutral vocabulary -> `D3D12_GLOBAL_BARRIER` / `D3D12_BUFFER_BARRIER` / `D3D12_TEXTURE_BARRIER`, submitted through `ID3D12GraphicsCommandList7::Barrier()`, consteval-verified in `src/vkd3d/D3D12Conversions.cpp`); the backend's own native recording is `src/d3d12/Barriers.hpp`. Remaining: wire recording into command-list execution once Phase 3 passes record barriers.
- Dynamic-rendering equivalent: `OMSetRenderTargets` plus Enhanced Barriers (`D3D12_BARRIER_LAYOUT_RENDER_TARGET` / `DEPTH_STENCIL_WRITE`) is the direct analogue of `vkCmdBeginRendering` / `DynamicPass` — no classic `BeginRenderPass`. The D3D12 pass execution transitions layouts, binds RTVs/DSVs, sets viewport/scissor, and runs the callback.
- Parallel recording: prefer separate `D3D12_COMMAND_LIST_TYPE_DIRECT` lists executed together via `ExecuteCommandLists` (fewer state-inheritance quirks) over `D3D12_COMMAND_LIST_TYPE_BUNDLE`; bundles remain the analogue of `SecondaryPassContext` where inheritance is actually wanted.
- `DXGISwapchainPresenter`: `CreateSwapChainForHwnd` with `DXGI_SWAP_EFFECT_FLIP_DISCARD`, query `IDXGISwapChain4`, acquire via `GetCurrentBackBufferIndex()`, `Present(1, 0)`.
- DXIL shader cook: [x] `compile_dxil` + `zahlen_shaders_dxil` smoke target landed with the root-constants refactor (profile half of the convention consumed). Remaining: create the `ID3D12RootSignature` from `ZHLN_DXIL_ROOT_SIGNATURE` on the C++ side so cook and runtime cannot drift; reflect the DXIL register assignments at pipeline creation and pin them (explicit `register()` clauses) only where auto-assignment disagrees with the convention; decide the `SLANG_ENABLE_DXIL` flip for the vendored build (OFF today; the cook accepts any DXIL-capable slangc).
- Keep platform/API headers and implementation confined to `src/d3d12`; expose only backend-neutral types to the joint contract.
- Add Windows CI with a GNU-style toolchain compatible with the repository's C++26 requirements, Windows SDK, and Slang DXIL support.

### 4. Expand the neutral seam (`src/vkd3d`)

The joint layer grows from `GpuAddress.hpp` + `Enums.hpp` into the full neutral vocabulary `src/render` can talk to without naming a backend:

- API-neutral barrier value types: [x] landed, with all translation owned by the joint layer. `VkD3D::{MemoryBarrier, BufferBarrier, ImageBarrier}` (`src/vkd3d/include/vkd3d/Barriers.hpp`) store `VkD3D::PipelineStage`, `VkD3D::Access`, `VkD3D::ImageLayout`, `VkD3D::ImageAspect` plus an opaque 64-bit resource word (the VkBuffer/VkImage value on the Vulkan path; D3D12 synthesis takes its `ID3D12Resource*` separately and ignores the word; queue-family fields are Vulkan-only). The hazard factories and the layout->sync derivation (`GetLayoutSyncInfo` / `LayoutTraits`) moved with them -- that derivation is graph theory, and the frame graph will consume it from here. Both synthesis directions live in `src/vkd3d` next to the conversions (`VulkanBarriers.hpp`: `AsVkBarrier` synthesis plus the `Record()` member definitions over the neutral values; `D3D12Barriers.hpp`: `MakeTextureBarrier` / `MakeBufferBarrier` / `MakeGlobalBarrier`, incl. subresource/plane lowering and `FLAG_DISCARD` for transitions out of `Undefined`), and `D3D12Conversions.hpp` moved from `src/d3d12` into the joint layer with them. Neither backend names a VkD3D type: `src/vulkan/execution/Barriers.hpp` keeps its original self-contained native barrier classes, and `src/d3d12/Barriers.hpp` is native-SDK-typed barrier synthesis. `src/render` barrier call sites have since adopted VkD3D as the Tell-Don't-Ask layer (`VkD3D::*Barrier::Transition/DiscardTo` factories taking view/slice domain objects whole, `Record()` member verbs defined for `VkCommandBuffer` in `src/vkd3d/include/vkd3d/VulkanBarriers.hpp`); the frame graph adopting them, and retiring the Vulkan-native classes once `src/vulkan` internals migrate, rides on Phase 5 Steps 1-2.
- Neutral resource handles: `RenderContext.hpp` / `Types.hpp` keep exposing `BufferHandle` and `TextureHandle`; internally `ImageSlice` and `BufferSlice` become API-neutral by holding a 64-bit resource ID/address plus `VkD3D::Format` and `VkD3D::ImageLayout`.
- Neutral sync structures: `SemaphoreSubmit` holds either a `VkSemaphore` or an `ID3D12Fence*` + 64-bit timeline value + `VkD3D::PipelineStage` (the union lives in the joint layer; neither backend header leaks through it).

### 5. Integrate the renderer behind the boundary

Migration order is chosen so the working Vulkan renderer stays green at every step; `src/render` talks to `src/vkd3d`, which dispatches to `src/vulkan` or `src/d3d12`:

- Migrate renderer-owned GPU address fields and push-constant payloads from `Vk::DeviceAddress<T>` to `VkD3D::DeviceAddress<T>` in a dedicated change.
- Step 1 — neutral spellings in graph declarations: replace raw `Vk::` enums in `RenderTargetTags.hpp` (`Res_SceneColor`, `Res_Depth`, ...) with `VkD3D::Format` / `VkD3D::ImageAspect`, and update `RenderPassDecl.hpp` (`ColorWrite`, `DepthWrite`, `ShaderRead`, ...) to take `VkD3D::*` enums.
- Step 2 — dual backend in `CompileTimeFrameGraph`: the `StateTable` already performs constexpr layout/hazard analysis over `Usage::layout` and `Usage::stage`; once those are `VkD3D::*`, the same simulation generates both Vulkan barrier arrays and D3D12 barrier arrays with no second hazard model.
- Step 3 — CMake backend switch: add a `ZHLN_RENDER_BACKEND` option (`VULKAN` | `D3D12`). With `D3D12`, `Renderer` wraps `D3D12Context`, the D3D12 allocator, and `DXGISwapchainPresenter`, compiling `src/render` against `src/d3d12`; the Vulkan path remains the default.
- Implement backend-specific allocation/recording behind `zahlen_vulkan` and `zahlen_direct3d`; do not add API conditionals to render passes, draw-command consumers, or shader entry points.
- Add an explicit joint renderer/selection layer only after the current Vulkan path remains green and the D3D12 backend reaches parity for required features.

## Validation completed for this stage

- Configured natively with GCC 16.2.0 and SlangPy's Slang 2026.12 C API shim.
- Built `zahlen_vulkan`, `zahlen_direct3d`, `zahlen_d3d12`, and `zahlen_vkd3d` successfully; no `zahlen_render` build was requested.
- Compiled the repository's 62 Slang stage variants through `scripts/compile_shaders_check.py`: 62/62 passed.
- Compiled a `gpu_memory` load/store/atomic smoke shader via `scripts/slangc_slangpy.py` to SPIR-V and validated it for Vulkan 1.3. Disassembly contains `PhysicalStorageBufferAddresses`, `PhysicalStorageBuffer64`, and `OpAtomicIAdd`.
- Compiled the same smoke shader to DXIL with Slang 2026.20 + DXC 1.9.2609 (downloaded via the repository's toolchain installer / upstream release), since the current PyPI `slangpy` wheel bundles Slang 2026.12, which predates direct `ResourceDescriptorHeap` syntax. The smoke case exercises dynamic slot decoding and negative typed stepping; DXIL disassembly contains `dx.op.createHandleFromHeap`, raw-buffer load/store, and atomic operations.
- Compile-time checks cover logical slot zero at offset zero, negative offset stepping without slot borrow, and offset underflow/overflow becoming null.
- `git diff --check` passes; the incremental patch applies cleanly on top of the previous infrastructure patch.

## Acceptance checks for this stage

- CMake exposes both requested target names without changing the default renderer backend.
- The focused Vulkan and D3D address backend targets compile independently of `zahlen_render`.
- Existing Vulkan shader stages still compile with the SlangPy shim.
- The address headers enforce typed assignment and preserve a 64-bit shared representation.
- Shader backend-specific lowering is isolated in one Slang module; no render-pass or shader-pass migration occurs yet.
