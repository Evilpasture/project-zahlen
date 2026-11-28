# Requirements

Applies to the native Linux x86_64 packages in release v0.7.0. Use the
requirements at the tag of the installed release.

## Build tools

| Task | Requirements |
| --- | --- |
| Build a game against installed packages | GCC with C++26 `-freflection` support (GNU >= 16.1.1; release compiler: GCC 16.2.0), CMake >= 3.28, a build tool (Ninja recommended), and the needed packages installed with `zpkg`. |
| Build Zahlen from source | GCC 16.2.0 for the release workflow, CMake >= 3.28, a build tool, and the dependencies provisioned by the [development containers](../docker/README.md). |
| Run a compiled game | No build tools. See runtime and Vulkan requirements below. |

The installed CMake packages check compiler family, architecture, libstdc++
ABI, and the required reflection construct (GCC PR c++/125280). Imported
CMake targets set the C++26/reflection, no-exceptions, and no-RTTI options.
See [CONSUMING.md](CONSUMING.md) for a consumer build and
[DEVELOPMENT.md](DEVELOPMENT.md) for source builds.

## Runtime platform and libraries

| Component | Requirement |
| --- | --- |
| Platform | Linux x86_64. |
| glibc | >= 2.35 for the prebuilt engine. Check separately built games for their own glibc floor with `readelf --version-info`. |
| GNU runtime | `libstdc++.so.6` providing `GLIBCXX_3.4.36` and `libgcc_s.so.1` providing `GCC_13.0.0`; use the runtime accompanying the compatible GCC 16 toolchain. |
| System libraries | Vulkan loader (`libvulkan.so.1`), Vulkan ICD/driver, `libzstd.so.1`, and standard system libraries. GLFW is statically included in `libzahlen_engine.so`; no separate `libglfw3` is needed. |
| Windowed application | Wayland session and Wayland/xkbcommon client libraries. The release was built with Wayland enabled and X11 disabled. |
| Headless application | Vulkan loader, ICD, and supported device. No display session is needed. |

A game or optional package can add dependencies. For a deployed executable,
inspect its linked libraries with `ldd` and glibc symbol versions with
`readelf --version-info`.

## Vulkan driver and hardware

The selected Vulkan device must expose `VK_EXT_descriptor_heap`,
`VK_KHR_shader_untyped_pointers`, and `VK_KHR_device_address_commands` and
support their required feature bits. Engine initialization checks device
capabilities. No minimum GPU model, VRAM size, or CPU core count is specified
for this release.

| Driver | Version / support |
| --- | --- |
| NVIDIA proprietary Linux | **610 series or newer** for the general-release descriptor-heap support documented by [NVIDIA](https://developer.nvidia.com/blog/streamlining-resource-binding-with-end-to-end-support-for-vulkan-descriptor-heaps/). Earlier developer beta drivers may expose the extension; inspect the selected device's capabilities. |
| Mesa lavapipe | Mesa **26.2+** for software Vulkan with descriptor-heap support. |
| Mesa RADV / ANV | Mesa **26.2+** enables descriptor heaps by default on AMD/Intel drivers ([release notes](https://docs.mesa3d.org/relnotes/26.2.0.html)); device features vary by GPU. |

To inspect a selected ICD and its reported extensions and features, install
`vulkaninfo` (usually provided by `vulkan-tools`):

```sh
vulkaninfo | grep -E 'deviceName|VK_EXT_descriptor_heap|VK_KHR_shader_untyped_pointers|VK_KHR_device_address_commands|descriptorHeap[[:space:]]*='
```

For lavapipe, select the installed ICD JSON before running `vulkaninfo` or
a headless game (the path below is distribution-dependent):

```sh
export VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.json
vulkaninfo | grep -E 'deviceName|VK_EXT_descriptor_heap|VK_KHR_shader_untyped_pointers|VK_KHR_device_address_commands|descriptorHeap[[:space:]]*='
```
