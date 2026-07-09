# Project Zahlen

A **simple** project that integrates Vulkan, Jolt Physics and ImGUI for hardware raytracing, asset pipelining and game development.

## Build Requirements
* **CMake (>= 3.25)**: Build automation tool.
* **C++26 Compiler with -freflection**: Supporting C++26 standard features (GCC 16.1.1 or later).
* **C23 Compiler**: Supporting C23 standard features (such as `#embed` support).
* **Python**: Used during the asset building phase to scan level assets and configure the parallel build rules.

## Build Instructions

You can do it the hard way, or the easy way.

### The Hard Way
1. Clone the repository and its submodules:
   ```sh
   git clone --recurse-submodules https://github.com/Evilpasture/project-zahlen.git
   ```
   
2. Create a build directory and configure the project:
   ```sh
   cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Release 
   ```
   
3. Build the project:
   ```sh
   cmake --build build
   ```

4. Run the project:
   ```sh
   ./build/zahlen
   ```  

### The Easy Way

1. Clone the repository and its submodules:
   ```sh
   git clone --recurse-submodules https://github.com/Evilpasture/project-zahlen.git
   ```
   
2. Run this script:
   ```sh
   ./tools/build.sh --gcc
   ```

3. Run the project:
   ```sh
   ./build/zahlen
   ```

## System & Platform Dependencies

The project is primarily developed on Linux and macOS, but should also work on Windows as originally planned.

These packages are expected to be installed on the host operating system:

* **Vulkan SDK (>= 1.3)**: Core graphics API, validation layers, and the DirectX Shader Compiler (`dxc`) used for SPIR-V shader compilation.
* **zstd (Zstandard)**: Compression algorithm used to build and decompress custom `.pak` assets.
* **Fontconfig** *(Linux & macOS)*: System font customization and configuration library (used to locate standard system fonts for font atlas generation).
* **libevdev** *(Linux only)*: Kernel-level input device wrapper used by the native TTY/KMS fallback backend.
* **libseat** *(Linux only)*: Shared session management library used to acquire input and graphics permissions in TTY mode without root access.
* **X11 / Xlib** *(Linux only)*: Legacy windowing library used for windowing demo test utilities.
* **Cocoa / QuartzCore** *(macOS only)*: Apple frameworks used to spin up native macOS swapchain presentation surfaces.

## Bundled / External Libraries
These are located in the `extern/` and `third_party/` directories:

### Physics & Math
* **Jolt Physics**: A multi-core 3D physics simulation and collision engine.
* **VulkanMemoryAllocator (VMA)**: Vulkan memory management utility.

### Scripting & Audio
* **LuaJIT**: JIT runtime for the Lua scripting language.
* **miniaudio**: Single-file C audio playback and mixing library.

### Graphics & Tooling
* **Dear ImGui**: Immediate-mode graphical user interface for debug overlays and controllers.
* **GLFW**: Multi-platform window, GL/Vulkan context, and input handling.
* **SPIRV-Reflect**: Lightweight reflection library for SPIR-V shader bytecode.
* **RenderDoc**: Integrated in-app graphics debugger hook.

### Utilities & Formats
* **cgltf**: Lightweight glTF 2.0 parser and loader.
* **stb**: Single-file public domain libraries (including `stb_image` for texture loading and `stb_truetype` for font atlas rendering).

## LICENSE

This project is licensed under the GNU General Public License version 3.0 or later versions. See the [LICENSE](LICENSE.md) file for more details.
