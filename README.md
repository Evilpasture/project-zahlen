# Project Zahlen

A **simple** project that integrates Vulkan, Jolt Physics and ImGUI for hardware raytracing, fast and straightforward creative work conversions and game development with Fennel/Lua.

## Build Requirements
* **CMake (>= 3.25)**: Build automation tool.
* **C++26 Compiler with -freflection**: Supporting C++26 standard features (GCC 16.1.1 or later).
* **C23 Compiler**: Supporting C23 standard features (such as `#embed` support).
* **Python**: Used during the asset building phase to scan level assets and configure the parallel build rules.

## Build Instructions

For creative works, the build system expects Blender files in `./blender/`

You can do it the hard way, or the easy way.

### Manual Build
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

### Convenient Build

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

## Hardware expectations

The project is designed to run on a system with a discrete GPU and a dedicated CPU. Tested on NVIDIA RTX 3050 6GB in Arch Linux.
Should work on macOS too with MoltenVK if you manage to compile it, but expect MoltenVK overhead.

## System & Platform Dependencies

The project is primarily developed on Linux and macOS, but should also work on Windows as originally planned. 
That is true, if your compiler supports standard C++26 features. Otherwise, tough luck. Compile GCC 16.1.1+ yourself.

See [here](https://en.cppreference.com/cpp/compiler_support/26) for compiler support information.

These packages are expected to be installed on the host operating system:

* **Vulkan SDK (>= 1.3)**: Core graphics API, validation layers, and the DirectX Shader Compiler (`dxc`) used for SPIR-V shader compilation.
* **zstd (Zstandard)**: Compression algorithm used to build and decompress custom `.pak` assets.
* **Windows SDK** *(Windows only)*: Windows API headers and libraries. It's expected that you should have the SDK installed on your system.
* **Visual Studio Build Tools** *(Windows only)*: Required to build the project with Clang/LLVM when targeting Windows.
* **Fontconfig** *(Linux & macOS)*: System font customization and configuration library (used to locate standard system fonts for font atlas generation).
* **libevdev** *(Linux only)*: Kernel-level input device wrapper used by the native TTY/KMS fallback backend.
* **libseat** *(Linux only)*: Shared session management library used to acquire input and graphics permissions in TTY mode without root access.
* **X11 / Xlib** *(Linux only)*: Legacy windowing library used for windowing demo test utilities.
* **Cocoa / QuartzCore** *(macOS only)*: Apple frameworks used to spin up native macOS swapchain presentation surfaces.

## Bundled / External Libraries
These are located in the `extern/` and `third_party/` directories:

### Physics & Math
* **Jolt Physics**: A multi-core 3D physics simulation and collision engine. Also used for fast SIMD math.
* **VulkanMemoryAllocator (VMA)**: Vulkan memory management utility.

### Scripting & Audio
* **LuaJIT**: JIT runtime for the Lua scripting language.
* **Fennel Compiler**: Used to transpile Fennel scripts to Lua.
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
