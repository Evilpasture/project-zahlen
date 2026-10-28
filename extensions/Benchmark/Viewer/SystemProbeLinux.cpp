// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// extensions/Benchmark/Viewer/SystemProbeLinux.cpp -- INTERNAL backend.
//
// Linux's hardware counters, and the only file in this extension that touches a
// syscall, a /proc file, a /sys file or a vendor library. Everything above it
// (SystemProbe.hpp, SystemTelemetry.hpp and their callers) is platform-free by
// construction.
//
// CPU, from procfs:
//
//   * /proc/stat's per-core lines are jiffie counters -- user, nice, system,
//     idle, iowait, irq, softirq, steal -- so usage is the busy fraction of the
//     delta between two reads. Cores are kept under the kernel's own number, the
//     one cpufreq and /proc/cpuinfo use, so a core that is offline in one read
//     and back in the next is labelled correctly rather than shifting its
//     neighbours; a core with no previous read has no delta and says so.
//   * Clocks come from cpufreq's scaling_cur_freq (kHz) where the kernel exposes
//     it. On a machine without cpufreq -- a VM, or a kernel booted with it off --
//     /proc/cpuinfo's "cpu MHz" is the fallback; it is a reported frequency
//     rather than a live one, so it is used only when the live path is missing.
//
// GPU, in order of preference:
//
//   * NVML, loaded at runtime through the engine's own ZHLN::SharedLibrary -- the
//     repo's sanctioned dlopen wrapper, so this file includes no <dlfcn.h> and no
//     <nvml.h>: NVIDIA's ABI is declared here, from the handful of prototypes
//     used, and a machine without the driver simply fails to load and falls
//     through. The device is matched to the renderer's Vulkan device name, so a
//     two-GPU machine reports the one drawing the frames.
//   * DRM sysfs, for AMD (amdgpu) and Intel (i915): gpu_busy_percent for usage,
//     pp_dpm_sclk (amdgpu) or gt_cur_freq_mhz (i915) for the clock. The card is
//     chosen by matching the vendor to the renderer's device name, then by
//     boot_vga, then by whichever card exposes a counter at all.
//
// Neither path is required: a machine with no GPU counters reports CPU numbers
// and a reason, which is what the panel draws.
#if defined(__linux__)

#include "SystemProbe.hpp"
#include <Zahlen/Core/SharedLibrary.hpp>
#include <Zahlen/Error.hpp>
#include <Zahlen/Log.hpp>
#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

namespace ZHLN::Bench::Platform {

namespace {

namespace fs = std::filesystem;

inline constexpr std::string_view kProcStat    = "/proc/stat";
inline constexpr std::string_view kProcCpuInfo = "/proc/cpuinfo";
inline constexpr std::string_view kCpuRoot     = "/sys/devices/system/cpu";
inline constexpr std::string_view kDrmRoot     = "/sys/class/drm";

// A sane ceiling on the core number /proc/stat may report, so a garbled read
// cannot ask for a multi-gigabyte resize.
inline constexpr size_t kMaxCoreIndex = 4096;

// What the kernel says about one CPU: everything it has spent time on, and how
// much of that was idle (idle + iowait). Usage is the fraction of the delta that
// is not idle, which is the definition top and htop use.
struct CpuJiffies {
    uint64_t total = 0;
    uint64_t idle  = 0;
};

// One /proc/stat read: the aggregate line's counters, and one entry per CPU
// under the kernel's own core number, which is the vector's position. A core
// missing from this read -- hotplug, or a CPU taken offline -- is an empty
// optional in the middle, and it keeps its slot so a core that comes back does
// not inherit its neighbour's history.
struct ProcStat {
    CpuJiffies                             aggregate {};
    std::vector<std::optional<CpuJiffies>> cores;
};

// Reads a whole file. Small files only -- /proc/stat is a few kilobytes -- and an
// empty string on any failure, which every caller treats as "not there": for the
// single-number files this probe reads, an empty file and a missing one are the
// same answer.
[[nodiscard]] auto ReadWholeFile(const fs::path& path) -> std::string {
    std::ifstream file(path, std::ios::in | std::ios::binary);
    if (!file) {
        return {};
    }
    std::string text;
    text.reserve(4096);
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

// The lines of a text, newline kept out. A view over `text`, so the string has
// to outlive it -- and the trailing newline leaves it with a short final line
// rather than a matched one.
[[nodiscard]] constexpr auto Lines(std::string_view text) noexcept {
    return std::views::split(text, '\n') | std::views::transform([](auto&& chunk) noexcept -> std::string_view {
               return std::string_view {std::ranges::data(chunk), std::ranges::size(chunk)};
           });
}

// The first integer on a line, or nothing: used for the one-number sysfs files
// (gpu_busy_percent, gt_cur_freq_mhz) and for /proc/cpuinfo's processor index.
[[nodiscard]] auto FirstInteger(std::string_view text) -> std::optional<uint64_t> {
    const size_t at = text.find_first_of("0123456789");
    if (at == std::string_view::npos) {
        return std::nullopt;
    }
    uint64_t    value  = 0;
    const char* begin  = text.data() + at;
    const auto  parsed = std::from_chars(begin, text.data() + text.size(), value);
    if (parsed.ec != std::errc {}) {
        return std::nullopt;
    }
    return value;
}

// One /proc/stat CPU line as the counters on it: every field through steal summed
// into `total`, with idle + iowait as `idle`. `skip` is where the fields start,
// past the "cpu" or "cpuN" label the caller has already matched.
//
// The guest fields are deliberately left out of the total: the kernel already
// counts a guest's time inside user and nice, so summing the whole line counts it
// twice -- and it moves the delta of the two reads in the same direction, which
// is how a usage figure goes quietly wrong on a host with VMs.
[[nodiscard]] auto ParseCpuJiffies(std::string_view line, size_t skip) -> std::optional<CpuJiffies> {
    size_t   at     = std::min(skip, line.size());
    uint64_t total  = 0;
    uint64_t idle   = 0;
    size_t   fields = 0;

    while (at < line.size()) {
        while (at < line.size() && line[at] == ' ') {
            ++at;
        }
        if (at >= line.size()) {
            break;
        }

        uint64_t    value  = 0;
        const char* begin  = line.data() + at;
        const auto  parsed = std::from_chars(begin, line.data() + line.size(), value);
        if (parsed.ec != std::errc {} || parsed.ptr == begin) {
            break;
        }

        if (fields < 8) {
            total += value;
        }
        if (fields == 3 || fields == 4) {
            idle += value; // idle and iowait: waiting is not busy
        }
        ++fields;
        at = static_cast<size_t>(parsed.ptr - line.data());
    }

    if (fields < 5) {
        return std::nullopt; // not a CPU line: a truncated or older /proc/stat entry
    }
    return CpuJiffies {.total = total, .idle = idle};
}

// The parse against the file. The aggregate line is "cpu" with no digits; the
// cores follow as "cpuN" in the kernel's numbering.
[[nodiscard]] auto ReadProcStat() -> std::optional<ProcStat> {
    const std::string text = ReadWholeFile(fs::path(kProcStat));
    if (text.empty()) {
        return std::nullopt;
    }

    ProcStat stat;
    size_t   seen = 0;

    for (const std::string_view line: Lines(text)) {
        if (!line.starts_with("cpu")) {
            if (seen > 0) {
                break; // the cpu lines are contiguous, and they are done
            }
            continue;
        }

        size_t digits = 3;
        while (digits < line.size() && line[digits] >= '0' && line[digits] <= '9') {
            ++digits;
        }

        const std::optional<CpuJiffies> jiffies = ParseCpuJiffies(line, digits);
        if (!jiffies) {
            return std::nullopt;
        }
        ++seen;

        if (digits == 3) {
            stat.aggregate = *jiffies;
            continue;
        }

        uint64_t core = 0;
        if (const char* first = line.data() + 3; std::from_chars(first, line.data() + digits, core).ec != std::errc {} || core > kMaxCoreIndex) {
            continue;
        }
        if (stat.cores.size() <= core) {
            stat.cores.resize(core + 1);
        }
        stat.cores[core] = *jiffies;
    }

    if (seen == 0) {
        return std::nullopt;
    }
    return stat;
}

// How busy one core was between two reads, or nothing when it was not in both of
// them -- its first sample, or one the kernel skipped. A core with no delta at
// all reports nothing rather than a zero.
[[nodiscard]] auto UsageBetween(const std::optional<CpuJiffies>& before, const std::optional<CpuJiffies>& now) noexcept -> std::optional<float> {
    if (!before || !now) {
        return std::nullopt;
    }

    const uint64_t totalDelta = now->total > before->total ? now->total - before->total : 0;
    const uint64_t idleDelta  = now->idle > before->idle ? now->idle - before->idle : 0;
    if (totalDelta == 0) {
        return std::nullopt;
    }

    const uint64_t busy = totalDelta - std::min(idleDelta, totalDelta);
    return static_cast<float>((static_cast<double>(busy) / static_cast<double>(totalDelta)) * 100.0);
}

// cpufreq's live frequency for one core, in MHz. Nothing when the kernel does
// not expose cpufreq for it.
[[nodiscard]] auto ReadCpufreqMHz(unsigned core) -> std::optional<float> {
    const fs::path    path = fs::path(kCpuRoot) / ("cpu" + std::to_string(core)) / "cpufreq" / "scaling_cur_freq";
    const std::string text = ReadWholeFile(path);
    if (text.empty()) {
        return std::nullopt;
    }
    if (const auto khz = FirstInteger(text)) {
        return static_cast<float>(*khz) / 1000.0f;
    }
    return std::nullopt;
}

// /proc/cpuinfo's reported frequency per processor, indexed by "processor". The
// fallback for a machine with no cpufreq; reported rather than live, which is
// why the live path is preferred when it exists.
//
// Nothing when the file cannot be read, and an empty table when it has no "cpu
// MHz" line -- a distinction the start-up log line makes ("none exposed" against
// "/proc/cpuinfo"), which is the whole reason this is not just a vector.
[[nodiscard]] auto ReadCpuInfoMHz() -> std::optional<std::vector<float>> {
    const std::string text = ReadWholeFile(fs::path(kProcCpuInfo));
    if (text.empty()) {
        return std::nullopt;
    }

    std::vector<float> table;
    size_t             current = 0;

    for (const std::string_view line: Lines(text)) {
        if (line.starts_with("processor")) {
            if (const size_t colon = line.find(':'); colon != std::string_view::npos) {
                current = static_cast<size_t>(FirstInteger(line.substr(colon)).value_or(0));
            }
            continue;
        }
        if (!line.starts_with("cpu MHz")) {
            continue;
        }

        const size_t colon = line.find(':');
        if (colon == std::string_view::npos) {
            continue;
        }

        // "cpu MHz : 3599.998" -- a fractional value, so it is read as one.
        std::string_view value = line.substr(colon + 1);
        while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
            value.remove_prefix(1);
        }

        float      mhz    = 0.0f;
        const auto parsed = std::from_chars(value.data(), value.data() + value.size(), mhz, std::chars_format::general);
        if (parsed.ec != std::errc {} || mhz <= 0.0f || current > kMaxCoreIndex) {
            continue;
        }
        if (table.size() <= current) {
            table.resize(current + 1, 0.0f);
        }
        table[current] = mhz;
    }

    return table;
}

[[nodiscard]] constexpr auto AsciiLower(char c) noexcept -> char {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

// Does `text` contain `needle`, ignoring ASCII case?
//
// A fold over the two ranges rather than a lowercased copy of the name: this runs
// against a device name of a few dozen characters, and there is nothing to
// allocate for it.
[[nodiscard]] auto ContainsNoCase(std::string_view text, std::string_view needle) noexcept -> bool {
    if (needle.empty()) {
        return true;
    }
    const auto compare = [](char a, char b) noexcept -> bool { return AsciiLower(a) == AsciiLower(b); };
    return !std::ranges::search(text, needle, compare).empty();
}

// The PCI vendor id a renderer device name implies, for matching a sysfs card.
[[nodiscard]] auto VendorForName(std::string_view name) noexcept -> uint32_t {
    if (ContainsNoCase(name, "nvidia") || ContainsNoCase(name, "geforce") || ContainsNoCase(name, "quadro")) {
        return 0x10DE;
    }
    if (ContainsNoCase(name, "amd") || ContainsNoCase(name, "radeon") || ContainsNoCase(name, "ati ")) {
        return 0x1002;
    }
    if (ContainsNoCase(name, "intel") || ContainsNoCase(name, "arc")) {
        return 0x8086;
    }
    return 0;
}

// One card under /sys/class/drm that answers at least one of the counters.
struct DrmCard {
    fs::path path; // .../cardN/device
    uint32_t vendor  = 0;
    bool     bootVga = false;
    bool     hasUsage = false;
    bool     hasClock = false;
};

// The counters of one card under /sys/class/drm, or nothing when the directory
// itself cannot be enumerated -- a kernel with no DRM, a locked-down /sys. An
// empty list is the other answer: the directory is there and no card in it
// exposes a counter.
[[nodiscard]] auto ReadDrmCards() -> std::optional<std::vector<DrmCard>> {
    std::error_code ec;
    fs::directory_iterator directory {fs::path(kDrmRoot), ec};
    if (ec) {
        return std::nullopt;
    }

    std::vector<DrmCard> cards;
    for (const fs::directory_entry& entry: directory) {
        const std::string name = entry.path().filename().string();
        if (!name.starts_with("card") || name.size() <= 4) {
            continue;
        }
        if (!std::ranges::all_of(name.substr(4), [](char c) noexcept -> bool { return c >= '0' && c <= '9'; })) {
            continue; // card0-DP-1 and friends: a connector, not a device
        }

        DrmCard card;
        card.path = entry.path() / "device";
        if (!fs::exists(card.path, ec)) {
            continue;
        }

        // vendor is a hex string ("0x1002"); the other one-number files are
        // decimal, so this one is read as hex.
        if (const std::string text = ReadWholeFile(card.path / "vendor"); !text.empty()) {
            if (const size_t at = text.find("0x"); at != std::string::npos) {
                uint32_t    value  = 0;
                const char* first  = text.data() + at + 2;
                const auto  parsed = std::from_chars(first, text.data() + text.size(), value, 16);
                if (parsed.ec == std::errc {}) {
                    card.vendor = value;
                }
            }
        }
        if (const std::string text = ReadWholeFile(card.path / "boot_vga"); !text.empty()) {
            card.bootVga = FirstInteger(text).value_or(0) != 0;
        }

        card.hasUsage = fs::exists(card.path / "gpu_busy_percent", ec);
        card.hasClock = fs::exists(card.path / "pp_dpm_sclk", ec) || fs::exists(card.path / "gt_cur_freq_mhz", ec);
        if (card.hasUsage || card.hasClock) {
            cards.push_back(std::move(card));
        }
    }
    return cards;
}

// How well a card answers "is this the one drawing the frames": the vendor the
// renderer's own device name implies beats the firmware's boot display, which
// beats merely having a counter to read.
[[nodiscard]] constexpr auto CardScore(const DrmCard& card, uint32_t wantedVendor) noexcept -> int {
    return (wantedVendor != 0 && card.vendor == wantedVendor ? 4 : 0) + (card.bootVga ? 2 : 0) + (card.hasUsage ? 1 : 0);
}

// amdgpu's pp_dpm_sclk lists every DPM level as "1: 2200Mhz" with the active one
// marked by a trailing '*'.
[[nodiscard]] auto ReadAmdgpuSclkMHz(const fs::path& device) -> std::optional<float> {
    const std::string text = ReadWholeFile(device / "pp_dpm_sclk");
    if (text.empty()) {
        return std::nullopt;
    }

    for (const std::string_view line: Lines(text)) {
        const size_t colon = line.find(':');
        if (!line.contains('*') || colon == std::string_view::npos) {
            continue;
        }
        if (const auto mhz = FirstInteger(line.substr(colon))) {
            return static_cast<float>(*mhz);
        }
    }
    return std::nullopt;
}

// i915's current GT frequency, in MHz: the requested frequency first, the actual
// one where the kernel only exposes that.
[[nodiscard]] auto ReadI915MHz(const fs::path& device) -> std::optional<float> {
    for (const std::string_view name: {"gt_cur_freq_mhz", "gt_act_freq_mhz"}) {
        if (const auto mhz = FirstInteger(ReadWholeFile(device / name))) {
            return static_cast<float>(*mhz);
        }
    }
    return std::nullopt;
}

// The handful of NVML entry points this probe uses, declared here rather than
// pulled from <nvml.h>: the ABI (an integer status, opaque device handles, one
// small struct) is frozen, and a header that has to exist at build time on a
// machine that may have no NVIDIA driver at all is worse than these lines.
//
// nvmlReturn_t is an int and NVML_SUCCESS is 0; the only enumerators used are
// NVML_CLOCK_SM (1) and NVML_CLOCK_GRAPHICS (0).
struct NvmlUtilization {
    unsigned int gpu    = 0;
    unsigned int memory = 0;
};

inline constexpr int kNvmlSuccess       = 0;
inline constexpr int kNvmlClockGraphics = 0;
inline constexpr int kNvmlClockSm       = 1;

// One GPU reading from whichever source answered: a counter the driver did not
// give is an empty optional, and a reading with both empty is a driver that
// answers but exposes nothing.
struct GpuReading {
    std::optional<float> usagePct;
    std::optional<float> clockMHz;
};

// Why NVML is not answering. Five ways to fail behind one call, so the failure
// carries which one it was rather than a boolean: the log line names it, and a
// machine with no NVIDIA driver at all -- the common case -- is told apart from a
// driver that is there and half-broken.
enum class NvmlFailure : uint8_t {
    DriverLibraryMissing ZHLN_ANNOTATION(ZHLN::Description<"the NVIDIA driver library is not installed"> {})       = 1,
    EntryPointsMissing   ZHLN_ANNOTATION(ZHLN::Description<"the driver library is loaded but missing entry points"> {}) = 2,
    InitFailed           ZHLN_ANNOTATION(ZHLN::Description<"the driver refused to initialize"> {})                   = 3,
    NoDevices            ZHLN_ANNOTATION(ZHLN::Description<"the driver reports no devices"> {})                      = 4,
    NoDeviceHandle       ZHLN_ANNOTATION(ZHLN::Description<"no device could be opened"> {})                          = 5,
};

// The NVIDIA driver, bound at runtime. A machine with no NVML fails at the first
// enumerator above, which is the common case and is not a fault.
class NvmlProbe {
  public:
    NvmlProbe() = default;
    ~NvmlProbe() noexcept {
        if (_initialized && _shutdown != nullptr) {
            _shutdown();
        }
    }

    NvmlProbe(const NvmlProbe&)                    = delete;
    auto operator=(const NvmlProbe&) -> NvmlProbe& = delete;
    NvmlProbe(NvmlProbe&&)                         = delete;
    auto operator=(NvmlProbe&&) -> NvmlProbe&      = delete;

    [[nodiscard]] auto Init(std::string_view gpuNameHint) -> std::expected<void, ErrorCode>;

    // One reading. Never fails: a driver that answers with nothing gives a
    // reading with nothing in it, which is a fact about the device rather than an
    // error about the call.
    [[nodiscard]] auto Sample() const noexcept -> GpuReading;

    [[nodiscard]] auto DeviceName() const noexcept -> std::string_view {
        return _name;
    }

  private:
    using InitFn             = int (*)();
    using ShutdownFn         = int (*)();
    using GetCountFn         = int (*)(unsigned int*);
    using GetHandleByIndexFn = int (*)(unsigned int, void**);
    using GetNameFn          = int (*)(void*, char*, unsigned int);
    using GetUtilizationFn   = int (*)(void*, NvmlUtilization*);
    using GetClockInfoFn     = int (*)(void*, int, unsigned int*);

    // One nvmlDeviceGetClockInfo call, with the status folded into the value: a
    // clock of zero is the absence of an answer, which is what the caller stores.
    [[nodiscard]] auto ClockInfo(int domain) const noexcept -> unsigned int {
        if (_getClockInfo == nullptr) {
            return 0;
        }
        unsigned int mhz = 0;
        return _getClockInfo(_device, domain, &mhz) == kNvmlSuccess ? mhz : 0;
    }

    ZHLN::SharedLibrary _library;
    ShutdownFn          _shutdown       = nullptr;
    GetUtilizationFn    _getUtilization = nullptr;
    GetClockInfoFn      _getClockInfo   = nullptr;

    void*       _device      = nullptr;
    bool        _initialized = false;
    std::string _name;
};

auto NvmlProbe::Init(std::string_view gpuNameHint) -> std::expected<void, ErrorCode> {
    // libnvidia-ml.so.1 is the soname the driver installs; the unversioned alias
    // exists on some distributions.
    if (!_library.OpenAny({"libnvidia-ml.so.1", "libnvidia-ml.so"})) {
        return std::unexpected(NvmlFailure::DriverLibraryMissing);
    }

    InitFn init = _library.Symbol<InitFn>("nvmlInit_v2");
    if (init == nullptr) {
        init = _library.Symbol<InitFn>("nvmlInit");
    }
    _shutdown       = _library.Symbol<ShutdownFn>("nvmlShutdown");
    _getUtilization = _library.Symbol<GetUtilizationFn>("nvmlDeviceGetUtilizationRates");
    _getClockInfo   = _library.Symbol<GetClockInfoFn>("nvmlDeviceGetClockInfo");

    const auto getCount  = _library.Symbol<GetCountFn>("nvmlDeviceGetCount_v2");
    const auto getHandle = _library.Symbol<GetHandleByIndexFn>("nvmlDeviceGetHandleByIndex_v2");
    const auto getName   = _library.Symbol<GetNameFn>("nvmlDeviceGetName");
    if (init == nullptr || _shutdown == nullptr || getCount == nullptr || getHandle == nullptr) {
        return std::unexpected(NvmlFailure::EntryPointsMissing);
    }

    if (init() != kNvmlSuccess) {
        return std::unexpected(NvmlFailure::InitFailed);
    }
    _initialized = true;

    unsigned int count = 0;
    if (getCount(&count) != kNvmlSuccess || count == 0) {
        return std::unexpected(NvmlFailure::NoDevices);
    }

    // The renderer's device name is the truth about which GPU is drawing: NVML
    // reports the same string Vulkan does for NVIDIA devices, so an exact match
    // picks the right one on a machine with two, and the first device is the
    // fallback anywhere else.
    std::optional<std::pair<void*, std::string>> chosen;
    for (const unsigned int index: std::views::iota(0u, count)) {
        void* candidate = nullptr;
        if (getHandle(index, &candidate) != kNvmlSuccess || candidate == nullptr) {
            continue;
        }

        std::string          candidateName;
        std::array<char, 96> buffer {};
        if (getName != nullptr && getName(candidate, buffer.data(), static_cast<unsigned int>(buffer.size())) == kNvmlSuccess) {
            candidateName = buffer.data();
        }

        const bool matches = !gpuNameHint.empty() && candidateName == gpuNameHint;
        if (!chosen || matches) {
            chosen = std::pair {candidate, std::move(candidateName)};
        }
        if (matches) {
            break;
        }
    }

    if (!chosen) {
        return std::unexpected(NvmlFailure::NoDeviceHandle);
    }

    _device = chosen->first;
    _name   = std::move(chosen->second);
    if (!gpuNameHint.empty() && !_name.empty() && _name != gpuNameHint) {
        ZHLN::Log("[Bench] system telemetry: NVML reports '{}' while the renderer draws on '{}'.", _name, gpuNameHint);
    }
    return {};
}

auto NvmlProbe::Sample() const noexcept -> GpuReading {
    GpuReading reading;
    if (_device == nullptr) {
        return reading;
    }

    if (_getUtilization != nullptr) {
        NvmlUtilization utilization;
        if (_getUtilization(_device, &utilization) == kNvmlSuccess) {
            reading.usagePct = static_cast<float>(utilization.gpu);
        }
    }

    // The SM clock is the core clock for rendering and compute; the graphics
    // domain carries the same number on the parts this has been checked against
    // and is the fallback where it is the only one that answers.
    if (const unsigned int sm = ClockInfo(kNvmlClockSm)) {
        reading.clockMHz = static_cast<float>(sm);
    } else if (const unsigned int graphics = ClockInfo(kNvmlClockGraphics)) {
        reading.clockMHz = static_cast<float>(graphics);
    }

    return reading;
}

// The two halves of a backend name -- "procfs" and "NVML" -- as one label, with
// the half that is missing left out rather than a branch per combination.
[[nodiscard]] auto BackendLabel(std::string_view cpu, std::string_view gpu) -> std::string {
    if (cpu.empty()) {
        return std::string(gpu);
    }
    if (gpu.empty()) {
        return std::string(cpu);
    }
    return std::format("{} + {}", cpu, gpu);
}

// The backend itself: procfs for the CPU, then NVML or DRM for the GPU.
class LinuxProbe final: public SystemProbe {
  public:
    explicit LinuxProbe(std::string_view gpuNameHint) {
        // One read at construction: it fixes the core set, and it is the base a
        // first sample's delta is taken from. The first sample therefore covers
        // the time since the probe started rather than kSampleInterval, which is
        // a stronger average of a longer interval, not a wrong number.
        if (std::optional<ProcStat> first = ReadProcStat()) {
            _cores    = first->cores.size();
            _previous = std::move(*first);
        } else {
            _reason = "/proc/stat is unreadable, so no CPU counters are available.";
        }

        _cpufreqLive = ReadCpufreqMHz(0).has_value();
        if (!_cpufreqLive) {
            _cpuInfoMHz = ReadCpuInfoMHz();
        }

        InitGpu(gpuNameHint);

        _backend = BackendLabel(_cores > 0 ? "procfs" : "", GpuSourceLabel());

        ZHLN::Log(
            "[Bench] system telemetry: {} CPU cores ({} clocks, {} usage), GPU counters: {}.", _cores,
            _cpufreqLive ? "cpufreq" : (_cpuInfoMHz ? "/proc/cpuinfo" : "none exposed"), _cores > 0 ? "procfs" : "none exposed",
            GpuSourceLabel().empty() ? "none exposed" : GpuSourceLabel()
        );
        if (const std::string_view name = GpuName(); !name.empty()) {
            ZHLN::Log("[Bench] system telemetry: GPU '{}'.", name);
        }
    }

    [[nodiscard]] auto Sample() noexcept -> std::optional<ProbeSample> override {
        // The CPU is the floor: a probe that could read neither /proc/stat nor
        // any GPU counter has nothing to report, and says so once through
        // Unavailable() rather than returning an empty sample every four
        // hundred milliseconds.
        if (_cores == 0) {
            return std::nullopt;
        }

        std::optional<ProcStat> current = ReadProcStat();
        if (!current) {
            return std::nullopt;
        }

        ProbeSample sample;
        sample.cores.resize(_cores);

        double usageSum  = 0.0;
        double clockSum  = 0.0;
        size_t usageSeen = 0;
        size_t clockSeen = 0;

        for (size_t core = 0; core < _cores; ++core) {
            ProbeCore& entry = sample.cores[core];

            // A core the kernel stopped reporting has no previous counter to take
            // a delta from, so it carries no usage rather than a zero.
            const std::optional<CpuJiffies>& before = core < _previous.cores.size() ? _previous.cores[core] : std::nullopt;
            const std::optional<CpuJiffies>& now    = core < current->cores.size() ? current->cores[core] : std::nullopt;
            entry.usagePct                          = UsageBetween(before, now);
            if (entry.usagePct) {
                usageSum += static_cast<double>(*entry.usagePct);
                ++usageSeen;
            }

            entry.clockMHz = CoreClockMHz(static_cast<unsigned>(core));
            if (entry.clockMHz) {
                clockSum += static_cast<double>(*entry.clockMHz);
                ++clockSeen;
            }
        }

        _previous = std::move(*current);

        if (usageSeen > 0) {
            sample.cpuUsagePct = static_cast<float>(usageSum / static_cast<double>(usageSeen));
        }
        if (clockSeen > 0) {
            sample.cpuClockMHz = static_cast<float>(clockSum / static_cast<double>(clockSeen));
        }

        const GpuReading gpu = ReadGpu();
        sample.gpuUsagePct   = gpu.usagePct;
        sample.gpuClockMHz   = gpu.clockMHz;

        return sample;
    }

    [[nodiscard]] auto BackendName() const noexcept -> std::string_view override {
        return _backend;
    }

    [[nodiscard]] auto Unavailable() const noexcept -> std::string_view override {
        return _reason;
    }

    [[nodiscard]] auto GpuUnavailable() const noexcept -> std::string_view override {
        if (_nvml) {
            // The library is loaded and a device was opened, so an empty reading
            // is the driver's answer about the device rather than about the
            // machine.
            return "the NVIDIA driver answered but exposes neither a utilization nor a clock counter";
        }
        return _gpuReason;
    }

    [[nodiscard]] auto GpuName() const noexcept -> std::string_view override {
        return _nvml ? _nvml->DeviceName() : std::string_view {};
    }

  private:
    // The core's clock: cpufreq's live one where the kernel exposes it, the
    // reported /proc/cpuinfo "cpu MHz" otherwise.
    [[nodiscard]] auto CoreClockMHz(unsigned core) const -> std::optional<float> {
        if (_cpufreqLive) {
            return ReadCpufreqMHz(core);
        }
        if (_cpuInfoMHz && core < _cpuInfoMHz->size() && (*_cpuInfoMHz)[core] > 0.0f) {
            return (*_cpuInfoMHz)[core];
        }
        return std::nullopt;
    }

    // The GPU: NVML when the driver is there, otherwise the sysfs card. One
    // source or the other, never both, so the panel cannot show two devices.
    [[nodiscard]] auto ReadGpu() const -> GpuReading {
        if (_nvml) {
            return _nvml->Sample();
        }

        GpuReading reading;
        if (!_drmCard) {
            return reading;
        }

        if (_drmCard->hasUsage) {
            if (const auto percent = FirstInteger(ReadWholeFile(_drmCard->path / "gpu_busy_percent"))) {
                reading.usagePct = static_cast<float>(std::min<uint64_t>(*percent, 100));
            }
        }
        if (_drmCard->hasClock) {
            reading.clockMHz = ReadAmdgpuSclkMHz(_drmCard->path).or_else([this]() -> std::optional<float> { return ReadI915MHz(_drmCard->path); });
        }
        return reading;
    }

    void InitGpu(std::string_view gpuNameHint) {
        auto nvml = std::make_unique<NvmlProbe>();
        if (std::expected<void, ErrorCode> ready = nvml->Init(gpuNameHint); ready) {
            _nvml = std::move(nvml);
            return;
        } else {
            // No NVML is the common case on a machine without the NVIDIA driver,
            // and it is not a fault: the reason goes in the log line and the sysfs
            // cards get their turn.
            ZHLN::Log("[Bench] system telemetry: NVML unavailable ({}).", ready.error());
        }

        // The sysfs cards, matched to the renderer's name by vendor, then by
        // boot_vga, then by whichever exposes a counter.
        const std::optional<std::vector<DrmCard>> cards = ReadDrmCards();
        if (!cards || cards->empty()) {
            _gpuReason = !cards ? "no /sys/class/drm to read GPU counters from" : "no DRM card under /sys/class/drm exposes a counter";
            return;
        }

        const uint32_t wanted = VendorForName(gpuNameHint);
        _drmCard              = *std::ranges::max_element(*cards, std::less {}, [wanted](const DrmCard& card) noexcept { return CardScore(card, wanted); });
    }

    // What is answering for the GPU, for the one start-up log line.
    [[nodiscard]] auto GpuSourceLabel() const noexcept -> std::string_view {
        if (_nvml) {
            return "NVML";
        }
        return _drmCard ? std::string_view("DRM sysfs") : std::string_view {};
    }

    // procfs state: the previous read, the core count it fixed, and the fallback
    // frequency table, read once when cpufreq is missing.
    ProcStat                           _previous;
    std::optional<std::vector<float>>  _cpuInfoMHz;
    size_t                             _cores       = 0;
    bool                               _cpufreqLive = false;

    // GPU state: one source or the other, never both.
    std::unique_ptr<NvmlProbe> _nvml;
    std::optional<DrmCard>     _drmCard;

    std::string _backend;
    std::string _reason;
    std::string _gpuReason;
};

} // namespace

auto MakeSystemProbe(std::string_view gpuNameHint) -> std::unique_ptr<SystemProbe> {
    return std::make_unique<LinuxProbe>(gpuNameHint);
}

} // namespace ZHLN::Bench::Platform

#endif // __linux__
