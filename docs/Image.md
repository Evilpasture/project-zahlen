# Engine-independent image codecs

`zahlen_image` / installed `Zahlen::Image` is an unconditional static consumer
library. Its header is `<Image/Image.hpp>`. It depends only on the standard
library; no Engine, ErrorCode registry, Log, reflection, GUI, Vulkan or module
runtime. C++26, PIC, `-fno-exceptions` and `-fno-rtti` are part of its target
contract. The build-time `zshader` tool can link it without introducing the
shader-catalog -> renderer -> engine cycle.

```cpp
#include <Image/Image.hpp>
auto image = ZHLN::Image::ReadRGBA8("source.png");
if (image) {
    auto written = ZHLN::Image::WritePNG("copy.png", image->pixels,
                                        image->width, image->height);
}
```

The API exposes owned vectors and `std::expected<T, ImageError>`:
- Inspect: source dimensions/channel count without allocating decoded pixels.
- IsHDR: true for a recognized HDR input, false for recognized LDR, an error
  for empty/unrecognized input. Not a claim that arbitrary compressed data is
  fully valid; Decode performs the actual decode.
- DecodeRGBA8 / ReadRGBA8: tightly packed, top-to-bottom RGBA8, alpha filled
  with 255 when absent. No vertical flip or color-profile processing.
- DecodeFloatRGBA: tightly packed RGBA float, HDR values retained. STB's default
  LDR-to-float gamma conversion is retained; callers needing exact sRGB use the
  byte decoder and apply their own transfer function (AssetCooking does).
- EncodePNGToMemory / WritePNG: 1..4 channels, optional byte stride (zero means
  packed). Both validate dimensions, signed codec length limits, row/stride
  arithmetic and supplied span size before calling the encoder. Paths are
  string_view slices, copied through filesystem paths; empty/embedded-NUL
  names fail. File open/read/write/close failures are reported.

There is no global flip/gamma setter, borrowed failure-reason pointer, raw
allocation/free API, third-party header, or C out-parameter in the public
interface. STB read/write implementations are static symbols in Image.cpp
only. Temporary decode allocations are RAII-owned and copied into vectors;
this intentional allocation/copy boundary keeps C allocator ownership private.
As with other no-exception vector APIs in the repository, process allocation
failure is not a recoverable ImageError contract.

## Consumers and Core boundary

AssetCooking, Fonts, glTF (embedded, data URI and external images), BSP fallback
images, Screenshot, zcook GLB emission, fidelity, zshader and test PNG helpers
use the capability. Importer downsampling now owns vector buffers too. No raw
image include directory escapes Image and no Core target links Image. The
unused PrefabFactory image include and GUI STB search root are removed.
`src/engine/stbi_impl.c`, `zahlen_stb_image`, duplicate PNG implementation TUs
and `ZHLN_TEST_IMAGE_WRITE_IMPL` are removed.

`zcook font` is a separate, existing offline outline-font parser, not an image
codec. Its sole stb_truetype implementation remains in FontBake.cpp, isolated
in `zcook_font_bake` with a PRIVATE search root. This does not reintroduce
runtime font parsing or expose STB to the rest of the cooker.

`configure/check_image_boundary.py` is part of the normal governance run and
rejects escaped raw image includes/calls, old target links, Core image includes
and image engine link dependencies.

## Installed packages

`zhln_toolchain_package(Image ...)` exports the static archive and header with
no package dependencies. Fonts, GLTF and Screenshot explicitly depend on it.
Image-only consumers are not required to support reflection; finding Image
first must not suppress a later Engine package's reflection probe.

Enabling all exports also exposed missing exports from prior COM migrations:
Scene is exported as its own package, the GLTF package carries GLTFAssets, and
all optional COM extension/plugin packages ship as relocatable source/header
packages compiled by the consumer's toolchain against `Zahlen::Engine`, with
zero prebuilt `.a` archives and zero native compatibility targets.
