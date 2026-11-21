# Descriptor heaps

Backend reference for `VK_EXT_descriptor_heap`. For shader declarations and
scene payloads, see the [shader guide](../../../resources/shaders/SHADER.md)
and [renderer data contract](../../render/README.md).

## Binding model

`HeapManager` in [DescriptorHeap.hpp](DescriptorHeap.hpp) owns resource and
sampler heaps backed by device-addressable buffers. Host descriptor writes use
`vkWriteResourceDescriptorsEXT` and `vkWriteSamplerDescriptorsEXT`; command
streams bind the heaps and supply per-pass data with `vkCmdPushDataEXT`.

Pipelines use `VK_PIPELINE_CREATE_2_DESCRIPTOR_HEAP_BIT_EXT` and a null pipeline
layout. Shader set/binding declarations are mapped to heap locations at pipeline
creation. Heap/push-data commands and descriptor-set/push-constant commands
invalidate each other's state within a command buffer; do not mix the models.

## Command-stream state

Establish heap bindings and frame addresses when beginning each command stream,
including async compute and secondary command buffers. `RenderFrameContext`
provides `BeginStream`; secondary recording establishes its heap inheritance
and frame-address data separately. Passes write their payloads, not stream-wide
heap state.

The scene push layout separates the pass-payload prefix from frame addresses.
Use reflected offsets and the supplied `HeapPushDataLayout`; do not duplicate
byte offsets in passes or backend code.

## Allocation and lifetime

Resource heaps contain static bindings, the bindless texture region, frame
transient partitions, an immediate-work partition, and driver-reserved space.
Resource slot stride must accommodate descriptor sizes and alignments reported
by the device.

- Frame scopes select the transient partition for the frame being recorded.
- Immediate scopes nest in LIFO order and restore their allocation cursor on
  exit. GPU work must complete before those slots are reused.
- Bindless texture slots are retired before reuse so in-flight frames retain
  valid descriptors. Descriptor lifetime and backing-image lifetime must agree.

## Registration and mappings

| Operation | API examples | Purpose |
|---|---|---|
| Register | `RegisterSampler`, `RegisterImage`, `RegisterStorageBuffer` | Allocate a typed handle and write its descriptor |
| Reserve | `ReserveSampler`, `ReserveImage`, `ReserveImageRange` | Allocate a handle before its resource is available |
| Bind | `BindSampler`, `BindImage`, `BindStorageBuffer` | Attach or refresh a reserved/existing handle |

Mapping tables describe semantic bindings. `HeapManager::Materialize` converts
these into driver mappings using the heap's offsets and strides. Keep driver
mapping structs out of persistent scene state.

## Tests

[Descriptor heap tests](../../../tests/render/TestDescriptorHeaps.cpp) exercise
bindless textures and frame-address data. The
[parallel recording tests](../../../tests/render/TestDescriptorHeapsParallel.cpp)
exercise secondary command buffers. See the [test guide](../../../tests/README.md)
for running tests.
