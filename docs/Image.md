# Engine-independent image codecs

Use `find_package(ZahlenImage CONFIG REQUIRED)` and link `Zahlen::Image`.
Include `<Image/Image.hpp>`. The library requires C++26, disables exceptions and
RTTI, and does not require Engine or reflection.

```cpp
#include <Image/Image.hpp>
auto image = ZHLN::Image::ReadRGBA8("source.png");
if (image) {
    auto written = ZHLN::Image::WritePNG("copy.png", image->pixels,
                                        image->width, image->height);
}
```

The API exposes owned vectors and `std::expected<T, Error>`:
- Inspect: source dimensions/channel count without allocating decoded pixels.
- IsHDR: true for a recognized HDR input, false for recognized LDR, an error
  for empty/unrecognized input. Use Decode to validate the image data.
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

Decoded pixels are owned vectors. There is no global flip/gamma setter or
caller-managed codec allocation. Process allocation failure is not reported as
a recoverable `Error`.
