# Vulkan / Direct3D 12 backend boundary plan

## Goal

Introduce the address-model and build-target seams for the future `zahlen_direct3d` and `zahlen_vkd3d` backends without migrating the renderer yet. `src/render` remains Vulkan-specific and continues to use `zahlen_vulkan` directly during this phase.

## Landed in this infrastructure patch

- [x] Add `ZHLN::D3D::BufferAddress<T>` in `src/d3d12/include/d3d12/memory/BufferAddress.hpp` as a header-only SM 6.6 descriptor-slot plus byte-offset address. It has no Windows SDK dependency, so its type and target can be configured on non-Windows hosts.
- [x] Make `ZHLN::Vk::DeviceAddress<T>` independently includable through Vulkan/volk headers, while preserving the existing Vulkan address API and pointer-arithmetic behavior.
- [x] Add `ZHLN::VkD3D::DeviceAddress<T>` in the private `src/vkd3d/GpuAddress.hpp` as the 64-bit shared wire contract. Backend conversions are concentrated in this type; no GPU address wrapper is installed or exposed through `include/Zahlen`, and the current renderer has not been switched to it.
- [x] Add concrete build targets `zahlen_d3d12`, `zahlen_direct3d`, and `zahlen_vkd3d` (`Zahlen::D3D12` / `Zahlen::Direct3D` / `Zahlen::VkD3D` aliases). Their internal include paths and backend link dependencies are not propagated as a public engine API.
- [x] Add `resources/shaders/gpu_memory.slang` as the single shader-side location for SPIR-V physical-BDA versus DXIL descriptor-heap lowering. Register it as a shared shader input for dependency tracking; no existing shader imports it yet.
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
- Import the module from the existing shader passes only after both target paths pass their compiler checks.

### 3. Build native Direct3D 12 backend

- Add D3D12 device, queue, descriptor heap, buffer allocation, synchronization, and resource-state implementations under `src/d3d12/`.
- Preserve descriptor view kind in buffer allocation: any address used by `store()` or `atomicAdd()` must resolve to a UAV descriptor; read-only `load()` addresses may use SRVs. Never reinterpret one descriptor as the other at shader access time.
- Keep platform/API headers and implementation confined to `src/d3d12`; expose only backend-neutral types to the joint contract.
- Add Windows CI with a GNU-style toolchain compatible with the repository's C++26 requirements, Windows SDK, and Slang DXIL support.

### 4. Integrate the renderer behind the boundary

- Migrate renderer-owned GPU address fields and push-constant payloads from `Vk::DeviceAddress<T>` to `VkD3D::DeviceAddress<T>` in a dedicated change.
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
