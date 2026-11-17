#include <Image/Image.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <string>
#include <thread>
using namespace ZHLN::Image;
int main() {
    int  failures = 0;
    auto check    = [&](bool ok) {
        if (!ok) {
            std::fprintf(stderr, "Image test failure\n");
            ++failures;
        }
    };
    const std::array<uint8_t, 16> rgba {255, 0, 0, 255, 0, 255, 0, 128, 0, 0, 255, 0, 1, 2, 3, 4};
    auto                          png = EncodePNGToMemory(rgba, 2, 2);
    check(png.has_value());
    if (!png)
        return 1;
    auto image = DecodeRGBA8(*png);
    check(image && image->width == 2 && image->height == 2 && std::ranges::equal(image->pixels, rgba));
    auto info = Inspect(*png);
    check(info && info->channels == 4);
    auto hdr = IsHDR(*png);
    check(hdr && !*hdr);
    auto floats = DecodeFloatRGBA(*png);
    check(floats && floats->pixels[0] == 1.0f && floats->pixels[3] == 1.0f);
    const std::array<uint8_t, 8> gray {11, 22, 0, 0, 33, 44, 0, 0};
    auto                         padded  = EncodePNGToMemory(gray, 2, 2, 1, 4);
    auto                         decoded = padded ? DecodeRGBA8(*padded) : std::expected<RGBA8, ImageError>(std::unexpected(ImageError::EncodeFailed));
    check(
        decoded && decoded->pixels[0] == 11 && decoded->pixels[4] == 22 && decoded->pixels[8] == 33 && decoded->pixels[12] == 44 && decoded->pixels[3] == 255
    );
    check(!DecodeRGBA8({}));
    check(!DecodeFloatRGBA({}));
    check(!Inspect({}));
    check(!IsHDR(rgba));
    check(!DecodeRGBA8(rgba));
    check(!DecodeRGBA8(std::span(*png).first(12)));
    check(!EncodePNGToMemory(rgba, 0, 1));
    check(!EncodePNGToMemory(rgba, 1, 1, 5));
    check(!EncodePNGToMemory(rgba, 2, 2, 4, 4));
    check(!EncodePNGToMemory(rgba, 3, 3));
    check(!EncodePNGToMemory(rgba, UINT32_MAX, UINT32_MAX));
    check(!ReadRGBA8(""));
    check(!WritePNG("", rgba, 2, 2));
    check(!WritePNG(std::string_view("a\0b", 3), rgba, 2, 2));
    auto path = std::filesystem::temp_directory_path() /
                ("zahlen-image-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".png");
    check(WritePNG(path.string(), rgba, 2, 2).has_value());
    auto read = ReadRGBA8(path.string());
    check(read && read->pixels == image->pixels);
    std::filesystem::remove(path);
    check(!ReadRGBA8(path.string()));
    check(!WritePNG((path / "missing.png").string(), rgba, 2, 2));
    // Minimal Radiance RGBE, 1x1 flat scanline: red 1, green .5, blue .25.
    const std::string    header = "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 1 +X 1\n";
    std::vector<uint8_t> rgbe(header.begin(), header.end());
    rgbe.insert(rgbe.end(), {128, 64, 32, 129});
    auto yesHDR   = IsHDR(rgbe);
    auto radiance = DecodeFloatRGBA(rgbe);
    check(yesHDR && *yesHDR && radiance && radiance->pixels == std::vector<float>({1.0f, .5f, .25f, 1.0f}));
    std::atomic<bool> concurrent {true};
    auto              work = [&] {
        for (int i = 0; i < 12; ++i) {
            auto d = DecodeRGBA8(*png);
            auto e = EncodePNGToMemory(rgba, 2, 2);
            if (!d || !e || !std::ranges::equal(d->pixels, rgba))
                concurrent = false;
        }
    };
    std::thread a(work), b(work);
    a.join();
    b.join();
    check(concurrent);
    std::printf("Image codec checks: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
