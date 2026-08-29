// extras/Network/Wire.cppm
// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// ============================================================================
// ZHLN.Wire — dependency-free binary serialization for the Zahlen engine.
//
//   * Exception-free by construction: every fallible operation returns
//     std::expected<T, ZHLN::Wire::Failure>. Nothing throws and no malformed
//     input can escape the Reader — every byte is bounds-checked before it is
//     touched, every length claim is validated before anything is allocated.
//
//   * Reflection-driven aggregates: any aggregate (or any type with a
//     Codec<T> specialization) serializes itself. Field names, per-field
//     annotations (Description / Range / Skip) and type names are baked in at
//     compile time via C++26 static reflection and surface in runtime
//     diagnostics.
//
//   * Annotated, formatted failures: WireError enumerators carry
//     [[= ZHLN::Reflect::Description<"..."> {}]] messages that double as
//     std::format strings. A Failure records the category, the byte offset,
//     the wire path ("snapshot.objects[3].position") and the formatted
//     details — plus the schema annotation of the offending field.
//
// Wire format (v1, host-independent):
//   bool            1 byte (0 or 1, anything else is rejected)
//   unsigned int    LEB128 varint (max 10 bytes; over-long zero tails rejected)
//   signed int      zigzag + varint
//   float/double    4/8 IEEE-754 bytes, little-endian
//   enum            underlying integer, validated against its enumerator list
//   optional<T>     presence byte + T
//   string          varint length + bytes (FixedString<C> enforces capacity)
//   vector<T>/span  varint element count + elements (byte blobs are raw bytes)
//   array<T, N>     N elements (byte arrays are raw, no count)
//   aggregate       reflected fields in declaration order (Skip fields are
//                   not encoded and keep their default on decode)
// ============================================================================

module;

#include <Zahlen/Core/Reflection.hpp>
#include <Zahlen/Core/String.hpp>
#include <Zahlen/Error.hpp>
#include <array>
#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <format>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

export module ZHLN.Wire;

export namespace ZHLN::Wire {

// ============================================================================
// Error codes — annotated descriptions double as std::format strings
// ============================================================================

enum class WireError : uint8_t {
    Truncated[[= ZHLN::Reflect::Description<"wire stream truncated: needed {} byte(s) at offset {} but only {} remain(s)"> {}]] = 1,
    NonCanonicalVarint[[= ZHLN::Reflect::Description<"non-canonical varint at offset {}: trailing zero group (use the shortest encoding)"> {}]],
    VarintOverflow[[= ZHLN::Reflect::Description<"varint at offset {} exceeds the 64-bit range"> {}]],
    InvalidBoolean[[= ZHLN::Reflect::Description<"boolean byte {} at offset {} is neither 0 nor 1"> {}]],
    InvalidEnumValue[[= ZHLN::Reflect::Description<"value {} is not a valid enumerator of '{}'"> {}]],
    ValueOutOfRange[[= ZHLN::Reflect::Description<"value {} is outside the permitted range [{}, {}]"> {}]],
    StringTooLong[[= ZHLN::Reflect::Description<"string of {} byte(s) exceeds the {} byte limit"> {}]],
    FixedStringOverflow[[= ZHLN::Reflect::Description<"string of {} byte(s) does not fit FixedString<{}> ({} usable characters)"> {}]],
    CollectionTooLarge[[= ZHLN::Reflect::Description<"collection of {} element(s) exceeds the {} element limit"> {}]],
    ElementCountExceedsInput[[= ZHLN::Reflect::Description<"collection claims {} element(s) but only {} byte(s) of wire data remain"> {}]],
    BufferOverflow[[= ZHLN::Reflect::Description<"byte limit of {} exceeded while writing {} additional byte(s)"> {}]],
    AllocationFailed[[= ZHLN::Reflect::Description<"allocation of {} byte(s) failed"> {}]],
    UnsupportedType[[= ZHLN::Reflect::Description<"type '{}' has no wire representation"> {}]],
    TrailingBytes[[= ZHLN::Reflect::Description<"{} trailing byte(s) remain after the value ({} byte(s) total)"> {}]],
    ChecksumMismatch[[= ZHLN::Reflect::Description<"CRC32 mismatch: computed 0x{:08x}, expected 0x{:08x}"> {}]],
    DecompressionFailed[[= ZHLN::Reflect::Description<"decompression failed: {}"> {}]],
    CompressionFailed[[= ZHLN::Reflect::Description<"compression failed: {}"> {}]],
    InvalidFrame[[= ZHLN::Reflect::Description<"malformed frame: {}"> {}]],
    FrameTooLarge[[= ZHLN::Reflect::Description<"frame length {} exceeds the {} byte stream limit"> {}]],
    FrameLengthMismatch[[= ZHLN::Reflect::Description<"frame header declares {} byte(s) but {} byte(s) were provided"> {}]],
    UnknownMessageType[[= ZHLN::Reflect::Description<"message type {} is not part of protocol version {}"> {}]],
    ProtocolVersionMismatch[[= ZHLN::Reflect::Description<"protocol version mismatch: message carries {}, client speaks {}"> {}]]
};

// ============================================================================
// Failure — the rich, annotated error type carried by every Result<T>
// ============================================================================

struct Failure {
    /// Categorized engine error (8-byte compressed form, integrates with the
    /// engine-wide std::expected<T, ZHLN::Error> convention).
    ZHLN::Error code {};
    /// Byte offset in the stream where the failure was detected.
    uint64_t offset {};
    /// Wire path of the value being processed, e.g. "snapshot.objects[3].uid".
    std::string path {};
    /// Human message: the enumerator's annotated format string with the
    /// runtime context of the failure site applied.
    std::string details {};
    /// Schema annotation of the innermost offending field, when available
    /// (attached by the reflected aggregate walker):
    /// "ObjectSnapshot.uid: <annotated description>".
    std::string note {};

    /// Single-line, fully annotated diagnostic.
    [[nodiscard]] auto Format() const -> std::string {
        std::string text = std::format("ZHLN.Wire: {}", details.empty() ? std::string(code.Message()) : details);
        if (!path.empty()) {
            text += std::format(" [at {} @ byte {}]", path, offset);
        }
        if (!note.empty()) {
            text += std::format(" ({})", note);
        }
        return text;
    }

    [[nodiscard]] auto ToError() const noexcept -> ZHLN::Error {
        return code;
    }

    auto operator==(const Failure& other) const noexcept -> bool = default;
};

template <typename T>
using Result = std::expected<T, Failure>;

/// Builds an annotated Failure for a WireError without Reader/Writer context
/// (used by the frame and message-envelope codecs).
[[nodiscard]] inline auto MakeFailure(WireError error, auto&&... args) -> Failure {
    return Failure {
        .code    = ZHLN::Error(error),
        .details = ZHLN::Reflect::FormatEnumMessage(error, static_cast<decltype(args)>(args)...)
    };
}

// ============================================================================
// Schema annotations
// ============================================================================

/// [[= ZHLN::Wire::Skip {}]] — the field is not part of the wire format.
struct Skip {};

/// [[= ZHLN::Wire::Range<Min, Max> {}]] — numeric bounds enforced on decode.
template <auto MinValue, auto MaxValue>
struct Range {
    static_assert(MinValue <= MaxValue, "ZHLN::Wire::Range: minValue must be <= maxValue");
    static constexpr auto minValue = MinValue;
    static constexpr auto maxValue = MaxValue;
};

/// [[= ZHLN::Wire::Version<N> {}]] — wire schema version of an annotated type.
template <uint32_t Value>
struct Version {
    static constexpr uint32_t value = Value;
};

// ============================================================================
// Byte buffer — growable, capped, allocation failures become Results
// ============================================================================

inline constexpr size_t DEFAULT_MAX_MESSAGE_BYTES = 32u * 1024u * 1024u;

class Buffer {
  public:
    Buffer() noexcept = default;

    explicit Buffer(size_t maxBytes) noexcept: m_maxBytes(maxBytes) {
    }

    Buffer(const Buffer&) = delete;
    auto operator=(const Buffer&) -> Buffer& = delete;

    Buffer(Buffer&& other) noexcept
        : m_data(other.m_data), m_size(other.m_size), m_capacity(other.m_capacity), m_maxBytes(other.m_maxBytes) {
        other.m_data     = nullptr;
        other.m_size     = 0;
        other.m_capacity = 0;
    }

    auto operator=(Buffer&& other) noexcept -> Buffer& {
        if (this != &other) {
            delete[] m_data;
            m_data        = other.m_data;
            m_size        = other.m_size;
            m_capacity    = other.m_capacity;
            m_maxBytes    = other.m_maxBytes;
            other.m_data  = nullptr;
            other.m_size  = 0;
            other.m_capacity = 0;
        }
        return *this;
    }

    ~Buffer() {
        delete[] m_data;
    }

    [[nodiscard]] auto Data() const noexcept -> std::span<const uint8_t> {
        return {m_data, m_size};
    }

    [[nodiscard]] auto Size() const noexcept -> size_t {
        return m_size;
    }

    [[nodiscard]] auto MaxBytes() const noexcept -> size_t {
        return m_maxBytes;
    }

    auto SetMaxBytes(size_t maxBytes) noexcept -> void {
        m_maxBytes = maxBytes;
    }

    auto Clear() noexcept -> void {
        m_size = 0;
    }

    /// Drops the first count bytes (stream-style consumption).
    auto Consume(size_t count) noexcept -> void {
        if (count >= m_size) {
            m_size = 0;
            return;
        }
        const size_t remaining = m_size - count;
        if (remaining > 0) {
            std::memmove(m_data, m_data + count, remaining);
        }
        m_size = remaining;
    }

    auto AppendByte(uint8_t byte) -> Result<void> {
        if (m_size + 1 > m_maxBytes) {
            return std::unexpected(FailBuffer(WireError::BufferOverflow, m_maxBytes, 1));
        }
        if (m_size + 1 > m_capacity && !Grow(m_size + 1)) {
            return std::unexpected(FailBuffer(WireError::AllocationFailed, m_size + 1));
        }
        m_data[m_size] = byte;
        ++m_size;
        return {};
    }

    auto Append(std::span<const uint8_t> bytes) -> Result<void> {
        if (bytes.empty()) {
            return {};
        }
        if (m_size + bytes.size() > m_maxBytes) {
            return std::unexpected(FailBuffer(WireError::BufferOverflow, m_maxBytes, bytes.size()));
        }
        if (m_size + bytes.size() > m_capacity && !Grow(m_size + bytes.size())) {
            return std::unexpected(FailBuffer(WireError::AllocationFailed, m_size + bytes.size()));
        }
        std::memcpy(m_data + m_size, bytes.data(), bytes.size());
        m_size += bytes.size();
        return {};
    }

  private:
    [[nodiscard]] auto FailBuffer(WireError error, auto&&... args) const -> Failure {
        return Failure {
            .code    = ZHLN::Error(error),
            .offset  = m_size,
            .details = ZHLN::Reflect::FormatEnumMessage(error, static_cast<decltype(args)>(args)...)
        };
    }

    auto Grow(size_t needed) noexcept -> bool {
        const size_t wanted   = (m_capacity * 2 > needed) ? m_capacity * 2 : needed;
        const size_t capacity = (wanted < 256) ? 256 : wanted;
        auto*        fresh    = new (std::nothrow) uint8_t[capacity];
        if (fresh == nullptr) {
            return false;
        }
        if (m_size > 0) {
            std::memcpy(fresh, m_data, m_size);
        }
        delete[] m_data;
        m_data     = fresh;
        m_capacity = capacity;
        return true;
    }

    uint8_t* m_data     = nullptr;
    size_t   m_size     = 0;
    size_t   m_capacity = 0;
    size_t   m_maxBytes = DEFAULT_MAX_MESSAGE_BYTES;
};

// ============================================================================
// Path tracking — locates failures inside nested structures for free
// ============================================================================

inline constexpr size_t MAX_PATH_DEPTH = 24;

class PathTracker {
  public:
    auto Push(std::string_view segment) noexcept -> void {
        if (m_depth < m_frames.size()) {
            m_frames[m_depth] = Frame {.segment = segment, .index = 0, .indexed = false};
        }
        ++m_depth;
    }

    auto PushIndex(size_t index) noexcept -> void {
        if (m_depth < m_frames.size()) {
            m_frames[m_depth] = Frame {.segment = {}, .index = index, .indexed = true};
        }
        ++m_depth;
    }

    auto Pop() noexcept -> void {
        if (m_depth > 0) {
            --m_depth;
        }
    }

    [[nodiscard]] auto Render() const -> std::string {
        std::string rendered;
        const size_t usable = (m_depth < m_frames.size()) ? m_depth : m_frames.size();
        for (size_t i = 0; i < usable; ++i) {
            if (m_frames[i].indexed) {
                rendered += std::format("[{}]", m_frames[i].index);
            } else {
                if (i > 0) {
                    rendered += '.'; // named segment follows either a name or an index
                }
                rendered += std::string(m_frames[i].segment);
            }
        }
        if (m_depth > m_frames.size()) {
            rendered += std::format("<+{}>", m_depth - m_frames.size());
        }
        return rendered;
    }

  private:
    struct Frame {
        std::string_view segment;
        size_t           index {};
        bool             indexed {};
    };
    std::array<Frame, MAX_PATH_DEPTH> m_frames {};
    size_t                            m_depth = 0;
};

/// RAII guard: pushes a named path segment and pops it on scope exit.
class PathScope {
  public:
    PathScope(PathTracker& tracker, std::string_view segment) noexcept: m_tracker(&tracker) {
        m_tracker->Push(segment);
    }

    PathScope(const PathScope&) = delete;
    auto operator=(const PathScope&) -> PathScope& = delete;
    PathScope(PathScope&&)                 = delete;
    auto operator=(PathScope&&) -> PathScope& = delete;

    ~PathScope() {
        if (m_tracker != nullptr) {
            m_tracker->Pop();
        }
    }

  private:
    PathTracker* m_tracker;
};

/// RAII guard: pushes "[index]" and pops it on scope exit.
class IndexPathScope {
  public:
    IndexPathScope(PathTracker& tracker, size_t index) noexcept: m_tracker(&tracker) {
        m_tracker->PushIndex(index);
    }

    IndexPathScope(const IndexPathScope&) = delete;
    auto operator=(const IndexPathScope&) -> IndexPathScope& = delete;
    IndexPathScope(IndexPathScope&&)          = delete;
    auto operator=(IndexPathScope&&) -> IndexPathScope& = delete;

    ~IndexPathScope() {
        if (m_tracker != nullptr) {
            m_tracker->Pop();
        }
    }

  private:
    PathTracker* m_tracker;
};

// ============================================================================
// Writer
// ============================================================================

class Writer {
  public:
    Writer() noexcept = default;

    explicit Writer(size_t maxBytes) noexcept: m_buffer(maxBytes) {
    }

    [[nodiscard]] auto Bytes() const noexcept -> std::span<const uint8_t> {
        return m_buffer.Data();
    }

    [[nodiscard]] auto Size() const noexcept -> size_t {
        return m_buffer.Size();
    }

    auto Clear() noexcept -> void {
        m_buffer.Clear();
    }

    auto SetMaxBytes(size_t maxBytes) noexcept -> void {
        m_buffer.SetMaxBytes(maxBytes);
    }

    /// Typed entry point: encodes any wire-supported value.
    template <typename T>
    auto Put(const T& value) -> Result<void>;

    // -- low-level primitives (buffer failures gain the current path) -------
    auto PutByte(uint8_t byte) -> Result<void> {
        const Result<void> res = m_buffer.AppendByte(byte);
        if (!res) {
            return std::unexpected(WithPosition(res.error()));
        }
        return {};
    }

    auto PutBytes(std::span<const uint8_t> bytes) -> Result<void> {
        const Result<void> res = m_buffer.Append(bytes);
        if (!res) {
            return std::unexpected(WithPosition(res.error()));
        }
        return {};
    }

    auto PutUVar(uint64_t value) -> Result<void> {
        while (value >= 0x80) {
            const Result<void> res = PutByte(static_cast<uint8_t>(value | 0x80u));
            if (!res) {
                return res;
            }
            value >>= 7;
        }
        return PutByte(static_cast<uint8_t>(value));
    }

    [[nodiscard]] auto Path() noexcept -> PathTracker& {
        return m_path;
    }

    [[nodiscard]] auto Fail(WireError error, auto&&... args) const -> Failure {
        return Failure {
            .code    = ZHLN::Error(error),
            .offset  = m_buffer.Size(),
            .path    = m_path.Render(),
            .details = ZHLN::Reflect::FormatEnumMessage(error, static_cast<decltype(args)>(args)...)
        };
    }

  private:
    [[nodiscard]] auto WithPosition(const Failure& failure) const -> Failure {
        Failure patched = failure;
        patched.path    = m_path.Render();
        patched.offset  = m_buffer.Size();
        return patched;
    }

    Buffer      m_buffer;
    PathTracker m_path;
};

// ============================================================================
// Reader — every access is bounds-checked; malformed input can never escape
// ============================================================================

struct Limits {
    size_t maxStringBytes        = 1u << 20; // 1 MiB
    size_t maxCollectionElements = 1u << 20; // 1 M elements
};

class Reader {
  public:
    Reader() noexcept = default;

    explicit Reader(std::span<const uint8_t> data, Limits limits = Limits {}) noexcept: m_data(data), m_limits(limits) {
    }

    [[nodiscard]] auto Remaining() const noexcept -> size_t {
        return m_data.size() - m_pos;
    }

    [[nodiscard]] auto Position() const noexcept -> size_t {
        return m_pos;
    }

    [[nodiscard]] auto IsAtEnd() const noexcept -> bool {
        return m_pos >= m_data.size();
    }

    [[nodiscard]] auto LimitsRef() const noexcept -> const Limits& {
        return m_limits;
    }

    /// Typed entry point: decodes into out. Fails without touching out when
    /// the stream is malformed.
    template <typename T>
    auto Get(T& out) -> Result<void>;

    // -- low-level primitives -------------------------------------------------
    auto GetUVar(uint64_t& out) -> Result<void> {
        uint64_t value = 0;
        unsigned shift = 0;
        size_t   start = m_pos;
        for (size_t count = 1; count <= 10; ++count) {
            if (m_pos >= m_data.size()) {
                return std::unexpected(Fail(WireError::Truncated, 1, start, Remaining()));
            }
            const uint8_t byte = m_data[m_pos];
            ++m_pos;
            const uint64_t payload = static_cast<uint64_t>(byte & 0x7Fu);
            if (shift >= 64 || (shift == 63 && (payload & ~1ull) != 0)) {
                return std::unexpected(Fail(WireError::VarintOverflow, start));
            }
            value |= payload << shift;
            if ((byte & 0x80u) == 0) {
                if (count > 1 && byte == 0) {
                    // A final zero group means a shorter encoding existed;
                    // reject to catch stream desynchronization early.
                    return std::unexpected(Fail(WireError::NonCanonicalVarint, start));
                }
                out = value;
                return {};
            }
            shift += 7;
        }
        return std::unexpected(Fail(WireError::VarintOverflow, start));
    }

    auto GetByte(uint8_t& out) -> Result<void> {
        if (m_pos >= m_data.size()) {
            return std::unexpected(Fail(WireError::Truncated, 1, m_pos, Remaining()));
        }
        out = m_data[m_pos];
        ++m_pos;
        return {};
    }

    /// Borrows count bytes without copying; advances the cursor.
    auto Take(size_t count) -> Result<std::span<const uint8_t>> {
        if (count > Remaining()) {
            return std::unexpected(Fail(WireError::Truncated, count, m_pos, Remaining()));
        }
        const std::span<const uint8_t> taken = m_data.subspan(m_pos, count);
        m_pos += count;
        return taken;
    }

    [[nodiscard]] auto Path() noexcept -> PathTracker& {
        return m_path;
    }

    [[nodiscard]] auto Fail(WireError error, auto&&... args) const -> Failure {
        return Failure {
            .code    = ZHLN::Error(error),
            .offset  = m_pos,
            .path    = m_path.Render(),
            .details = ZHLN::Reflect::FormatEnumMessage(error, static_cast<decltype(args)>(args)...)
        };
    }

  private:
    std::span<const uint8_t> m_data {};
    size_t                   m_pos = 0;
    Limits                   m_limits {};
    PathTracker              m_path;
};

// ============================================================================
// Codec — customization point for non-aggregate / foreign types
// ============================================================================

/// Specialize for types that need hand-written encoding (e.g. Jolt math):
///
///   template <> struct ZHLN::Wire::Codec<JPH::Vec3> {
///       static auto Encode(const JPH::Vec3& value, ZHLN::Wire::Writer& writer) -> ZHLN::Wire::Result<void>;
///       static auto Decode(JPH::Vec3& value, ZHLN::Wire::Reader& reader)     -> ZHLN::Wire::Result<void>;
///   };
template <typename T>
struct Codec;

template <typename T>
concept CustomCodable = requires(const T& value, T& out, Writer& writer, Reader& reader) {
    { Codec<T>::Encode(value, writer) } -> std::same_as<Result<void>>;
    { Codec<T>::Decode(out, reader) } -> std::same_as<Result<void>>;
};

} // namespace ZHLN::Wire

// ============================================================================
// Type traits and encoding details (exported: reachable wherever the
// templates below are instantiated)
// ============================================================================

export namespace ZHLN::Wire::detail {

template <typename T>
struct AlwaysFalse : std::false_type {};

template <typename T>
concept ByteLike = std::same_as<T, char> || std::same_as<T, char8_t> || std::same_as<T, unsigned char>
                   || std::same_as<T, uint8_t>;

template <typename T>
struct FixedStringTrait : std::false_type {};

template <size_t Capacity>
struct FixedStringTrait<ZHLN::FixedString<Capacity>> : std::true_type {
    static constexpr size_t capacity = Capacity;
};

template <typename T>
concept FixedStringLike = FixedStringTrait<std::remove_cvref_t<T>>::value;

template <typename T>
struct OptionalTrait : std::false_type {};

template <typename U>
struct OptionalTrait<std::optional<U>> : std::true_type {
    using element = U;
};

template <typename T>
concept OptionalLike = OptionalTrait<std::remove_cvref_t<T>>::value;

/// vector<T> or span<T>: the element type of a sequence container.
template <typename T>
struct SequenceTrait : std::false_type {};

template <typename U, typename A>
struct SequenceTrait<std::vector<U, A>> : std::true_type {
    using element = U;
};

template <typename U, size_t Extent>
struct SequenceTrait<std::span<U, Extent>> : std::true_type {
    using element = U;
};

template <typename T>
struct VectorTrait : std::false_type {};

template <typename U, typename A>
struct VectorTrait<std::vector<U, A>> : std::true_type {
    using element = U;
};

template <typename T>
concept VectorLike = VectorTrait<std::remove_cvref_t<T>>::value;

template <typename T>
struct SpanTrait : std::false_type {};

template <typename U, size_t Extent>
struct SpanTrait<std::span<U, Extent>> : std::true_type {
    using element = U;
};

template <typename T>
concept SpanLike = SpanTrait<std::remove_cvref_t<T>>::value;

template <typename T>
struct ArrayTrait : std::false_type {};

template <typename U, size_t N>
struct ArrayTrait<std::array<U, N>> : std::true_type {
    using element = U;
    static constexpr size_t extent = N;
};

template <typename T>
concept ArrayLike = ArrayTrait<std::remove_cvref_t<T>>::value;

template <typename T>
concept PairLike = requires { typename std::pair<typename T::first_type, typename T::second_type>; }
                   && std::same_as<T, std::pair<typename T::first_type, typename T::second_type>>;

template <typename T>
concept TupleLike = requires { std::tuple_size<std::remove_cvref_t<T>>::value; } && !PairLike<std::remove_cvref_t<T>>
                    && !ArrayLike<std::remove_cvref_t<T>> && !SpanLike<std::remove_cvref_t<T>>;

/// vector<uint8_t> / span-of-bytes: length-prefixed raw byte blobs.
template <typename T>
concept ByteBlob =
    (VectorLike<T> && std::same_as<typename VectorTrait<std::remove_cvref_t<T>>::element, uint8_t>)
    || (SpanLike<T> && std::same_as<std::remove_cvref_t<typename SpanTrait<std::remove_cvref_t<T>>::element>, uint8_t>);

constexpr auto ZigzagEncode(int64_t value) noexcept -> uint64_t {
    return (static_cast<uint64_t>(value) << 1) ^ static_cast<uint64_t>(value >> 63);
}

constexpr auto ZigzagDecode(uint64_t value) noexcept -> int64_t {
    return static_cast<int64_t>((value >> 1) ^ (0ull - (value & 1ull)));
}

inline auto PutLE64(Writer& writer, uint64_t value) -> Result<void> {
    std::array<uint8_t, 8> bytes {};
    for (size_t i = 0; i < bytes.size(); ++i) {
        bytes[i] = static_cast<uint8_t>(value >> (i * 8));
    }
    return writer.PutBytes(bytes);
}

inline auto PutLE32(Writer& writer, uint32_t value) -> Result<void> {
    std::array<uint8_t, 4> bytes {};
    for (size_t i = 0; i < bytes.size(); ++i) {
        bytes[i] = static_cast<uint8_t>(value >> (i * 8));
    }
    return writer.PutBytes(bytes);
}

inline auto GetLE64(Reader& reader, uint64_t& out) -> Result<void> {
    const auto bytes = reader.Take(8);
    if (!bytes) {
        return std::unexpected(bytes.error());
    }
    uint64_t value = 0;
    for (size_t i = 0; i < 8; ++i) {
        value |= static_cast<uint64_t>((*bytes)[i]) << (i * 8);
    }
    out = value;
    return {};
}

inline auto GetLE32(Reader& reader, uint32_t& out) -> Result<void> {
    const auto bytes = reader.Take(4);
    if (!bytes) {
        return std::unexpected(bytes.error());
    }
    uint32_t value = 0;
    for (size_t i = 0; i < 4; ++i) {
        value |= static_cast<uint32_t>((*bytes)[i]) << (i * 8);
    }
    out = value;
    return {};
}

inline constexpr bool ReflectionAvailable =
#if defined(__cpp_impl_reflection) || (defined(__has_feature) && __has_feature(reflection))
    true;
#else
    false;
#endif

} // namespace ZHLN::Wire::detail

// ============================================================================
// Value dispatch (encode)
// ============================================================================

export namespace ZHLN::Wire {

template <typename T>
auto EncodeValue(const T& value, Writer& writer) -> Result<void> {
    using Type = std::remove_cvref_t<T>;

    if constexpr (CustomCodable<Type>) {
        return Codec<Type>::Encode(value, writer);
    } else if constexpr (std::same_as<Type, bool>) {
        return writer.PutByte(value ? 1 : 0);
    } else if constexpr (std::integral<Type>) {
        if constexpr (std::signed_integral<Type> && !detail::ByteLike<Type>) {
            return writer.PutUVar(detail::ZigzagEncode(static_cast<int64_t>(value)));
        } else {
            using Unsigned = std::make_unsigned_t<Type>;
            return writer.PutUVar(static_cast<uint64_t>(static_cast<Unsigned>(value)));
        }
    } else if constexpr (std::floating_point<Type>) {
        if constexpr (sizeof(Type) == 4) {
            return detail::PutLE32(writer, std::bit_cast<uint32_t>(value));
        } else if constexpr (sizeof(Type) == 8) {
            return detail::PutLE64(writer, std::bit_cast<uint64_t>(value));
        } else {
            static_assert(detail::AlwaysFalse<Type>::value, "ZHLN.Wire: only 32-bit and 64-bit floats are supported");
        }
    } else if constexpr (std::is_enum_v<Type>) {
        using Underlying = std::underlying_type_t<Type>;
        return EncodeValue(static_cast<Underlying>(value), writer);
    } else if constexpr (detail::FixedStringLike<Type>) {
        return EncodeValue(static_cast<std::string_view>(value), writer);
    } else if constexpr (std::same_as<Type, std::string> || std::same_as<Type, std::string_view>) {
        const Result<void> length = writer.PutUVar(value.size());
        if (!length) {
            return length;
        }
        const auto* raw = reinterpret_cast<const uint8_t*>(value.data());
        return writer.PutBytes({raw, value.size()});
    } else if constexpr (detail::ByteBlob<Type>) {
        const Result<void> length = writer.PutUVar(value.size());
        if (!length) {
            return length;
        }
        return writer.PutBytes({value.data(), value.size()});
    } else if constexpr (detail::VectorLike<Type> || detail::SpanLike<Type>) {
        using Element = typename detail::SequenceTrait<Type>::element;
        const Result<void> length = writer.PutUVar(value.size());
        if (!length) {
            return length;
        }
        for (size_t index = 0; index < value.size(); ++index) {
            const IndexPathScope scope(writer.Path(), index);
            // static_cast handles proxy references (e.g. vector<bool>).
            const Result<void> res = EncodeValue(static_cast<Element>(value[index]), writer);
            if (!res) {
                return res;
            }
        }
        return {};
    } else if constexpr (detail::ArrayLike<Type>) {
        if constexpr (detail::ByteLike<typename detail::ArrayTrait<Type>::element>) {
            const auto* raw = reinterpret_cast<const uint8_t*>(value.data());
            return writer.PutBytes({raw, value.size()});
        } else {
            for (size_t index = 0; index < value.size(); ++index) {
                const IndexPathScope scope(writer.Path(), index);
                const Result<void>           res = EncodeValue(value[index], writer);
                if (!res) {
                    return res;
                }
            }
            return {};
        }
    } else if constexpr (detail::PairLike<Type>) {
        const Result<void> first = EncodeValue(value.first, writer);
        if (!first) {
            return first;
        }
        return EncodeValue(value.second, writer);
    } else if constexpr (detail::TupleLike<Type>) {
        Result<void> result {};
        [&]<size_t... Is>(std::index_sequence<Is...>) {
            ((result ? (result = EncodeValue(std::get<Is>(value), writer)) : result), ...);
        }(std::make_index_sequence<std::tuple_size_v<Type>>());
        return result;
    } else if constexpr (std::is_array_v<Type>) {
        using Element = std::remove_extent_t<Type>;
        if constexpr (detail::ByteLike<Element>) {
            const auto* raw = reinterpret_cast<const uint8_t*>(value);
            return writer.PutBytes({raw, std::extent_v<Type>});
        } else {
            for (size_t index = 0; index < std::extent_v<Type>; ++index) {
                const IndexPathScope scope(writer.Path(), index);
                const Result<void>           res = EncodeValue(value[index], writer);
                if (!res) {
                    return res;
                }
            }
            return {};
        }
    } else if constexpr (detail::OptionalLike<Type>) {
        if (value.has_value()) {
            const Result<void> presence = writer.PutByte(1);
            if (!presence) {
                return presence;
            }
            return EncodeValue(*value, writer);
        }
        return writer.PutByte(0);
    } else if constexpr (std::is_aggregate_v<Type> && detail::ReflectionAvailable) {
        return EncodeAggregate(value, writer);
    } else {
        static_assert(detail::AlwaysFalse<Type>::value,
                      "ZHLN.Wire: type has no wire representation. Add a ZHLN::Wire::Codec<T> specialization, "
                      "convert it to a wire-supported type, or build with a reflection-capable compiler.");
        return std::unexpected(writer.Fail(WireError::UnsupportedType, ZHLN::Reflect::TypeName<Type>()));
    }
}

// ============================================================================
// Value dispatch (decode)
// ============================================================================

template <typename T>
auto DecodeValue(T& out, Reader& reader) -> Result<void> {
    using Type = std::remove_cvref_t<T>;

    if constexpr (CustomCodable<Type>) {
        return Codec<Type>::Decode(out, reader);
    } else if constexpr (std::same_as<Type, bool>) {
        uint8_t           byte = 0;
        const Result<void> res = reader.GetByte(byte);
        if (!res) {
            return res;
        }
        if (byte > 1) {
            return std::unexpected(reader.Fail(WireError::InvalidBoolean, byte, reader.Position() - 1));
        }
        out = (byte != 0);
        return {};
    } else if constexpr (std::integral<Type>) {
        uint64_t           raw = 0;
        const Result<void> res = reader.GetUVar(raw);
        if (!res) {
            return res;
        }
        if constexpr (std::signed_integral<Type> && !detail::ByteLike<Type>) {
            const int64_t decoded = detail::ZigzagDecode(raw);
            if (decoded < static_cast<int64_t>(std::numeric_limits<Type>::min())
                || decoded > static_cast<int64_t>(std::numeric_limits<Type>::max())) {
                return std::unexpected(reader.Fail(WireError::ValueOutOfRange, decoded,
                                                   static_cast<long long>(std::numeric_limits<Type>::min()),
                                                   static_cast<long long>(std::numeric_limits<Type>::max())));
            }
            out = static_cast<Type>(decoded);
        } else {
            using Unsigned = std::make_unsigned_t<Type>;
            if (raw > static_cast<uint64_t>(std::numeric_limits<Unsigned>::max())) {
                return std::unexpected(reader.Fail(WireError::ValueOutOfRange, raw, 0,
                                                   static_cast<unsigned long long>(std::numeric_limits<Unsigned>::max())));
            }
            out = static_cast<Type>(static_cast<Unsigned>(raw));
        }
        return {};
    } else if constexpr (std::floating_point<Type>) {
        if constexpr (sizeof(Type) == 4) {
            uint32_t           raw = 0;
            const Result<void> res = detail::GetLE32(reader, raw);
            if (!res) {
                return res;
            }
            out = std::bit_cast<Type>(raw);
            return {};
        } else if constexpr (sizeof(Type) == 8) {
            uint64_t           raw = 0;
            const Result<void> res = detail::GetLE64(reader, raw);
            if (!res) {
                return res;
            }
            out = std::bit_cast<Type>(raw);
            return {};
        } else {
            static_assert(detail::AlwaysFalse<Type>::value, "ZHLN.Wire: only 32-bit and 64-bit floats are supported");
        }
    } else if constexpr (std::is_enum_v<Type>) {
        using Underlying     = std::underlying_type_t<Type>;
        Underlying           raw {};
        const Result<void>   res = DecodeValue(raw, reader);
        if (!res) {
            return res;
        }
        if constexpr (detail::ReflectionAvailable) {
            if (!ZHLN::Reflect::EnumHasValue<Type>(static_cast<std::underlying_type_t<Type>>(raw))) {
                return std::unexpected(reader.Fail(WireError::InvalidEnumValue,
                                                   static_cast<unsigned long long>(raw), ZHLN::Reflect::TypeName<Type>()));
            }
        }
        out = static_cast<Type>(raw);
        return {};
    } else if constexpr (detail::OptionalLike<Type>) {
        uint8_t            presence = 0;
        const Result<void> res      = reader.GetByte(presence);
        if (!res) {
            return res;
        }
        if (presence > 1) {
            return std::unexpected(reader.Fail(WireError::InvalidBoolean, presence, reader.Position() - 1));
        }
        if (presence == 0) {
            out.reset();
            return {};
        }
        out.emplace();
        return DecodeValue(*out, reader);
    } else if constexpr (std::same_as<Type, std::string>) {
        uint64_t           length = 0;
        const Result<void> lenRes = reader.GetUVar(length);
        if (!lenRes) {
            return lenRes;
        }
        if (length > reader.LimitsRef().maxStringBytes) {
            return std::unexpected(reader.Fail(WireError::StringTooLong, length, reader.LimitsRef().maxStringBytes));
        }
        const auto bytes = reader.Take(static_cast<size_t>(length));
        if (!bytes) {
            return std::unexpected(bytes.error());
        }
        out.assign(reinterpret_cast<const char*>(bytes->data()), bytes->size());
        return {};
    } else if constexpr (detail::FixedStringLike<Type>) {
        constexpr size_t kCapacity = detail::FixedStringTrait<Type>::capacity;
        uint64_t           length = 0;
        const Result<void> lenRes = reader.GetUVar(length);
        if (!lenRes) {
            return lenRes;
        }
        if (length > kCapacity - 1) {
            return std::unexpected(reader.Fail(WireError::FixedStringOverflow, length, kCapacity, kCapacity - 1));
        }
        const auto bytes = reader.Take(static_cast<size_t>(length));
        if (!bytes) {
            return std::unexpected(bytes.error());
        }
        out.assign(std::string_view(reinterpret_cast<const char*>(bytes->data()), bytes->size()));
        return {};
    } else if constexpr (std::same_as<Type, std::vector<uint8_t>>) {
        uint64_t           length = 0;
        const Result<void> lenRes = reader.GetUVar(length);
        if (!lenRes) {
            return lenRes;
        }
        if (length > reader.Remaining()) {
            return std::unexpected(reader.Fail(WireError::ElementCountExceedsInput, length, reader.Remaining()));
        }
        const auto bytes = reader.Take(static_cast<size_t>(length));
        if (!bytes) {
            return std::unexpected(bytes.error());
        }
        out.assign(bytes->begin(), bytes->end());
        return {};
    } else if constexpr (std::same_as<Type, std::vector<bool>>) {
        uint64_t           count = 0;
        const Result<void> lenRes = reader.GetUVar(count);
        if (!lenRes) {
            return lenRes;
        }
        if (count > reader.LimitsRef().maxCollectionElements) {
            return std::unexpected(reader.Fail(WireError::CollectionTooLarge, count, reader.LimitsRef().maxCollectionElements));
        }
        if (count > reader.Remaining()) {
            return std::unexpected(reader.Fail(WireError::ElementCountExceedsInput, count, reader.Remaining()));
        }
        out.clear();
        out.reserve(static_cast<size_t>(count));
        for (size_t index = 0; index < count; ++index) {
            const IndexPathScope scope(reader.Path(), index);
            bool                          element = false;
            const Result<void>            res     = DecodeValue(element, reader);
            if (!res) {
                out.clear();
                return res;
            }
            out.push_back(element);
        }
        return {};
    } else if constexpr (detail::VectorLike<Type>) {
        using Element = typename detail::VectorTrait<Type>::element;
        uint64_t           count = 0;
        const Result<void> lenRes = reader.GetUVar(count);
        if (!lenRes) {
            return lenRes;
        }
        if (count > reader.LimitsRef().maxCollectionElements) {
            return std::unexpected(reader.Fail(WireError::CollectionTooLarge, count, reader.LimitsRef().maxCollectionElements));
        }
        if (count > reader.Remaining() && !std::is_empty_v<Element>) {
            // Every non-empty element costs at least one wire byte, so a count
            // larger than the remaining input is malformed by construction.
            return std::unexpected(reader.Fail(WireError::ElementCountExceedsInput, count, reader.Remaining()));
        }
        out.clear();
        out.reserve(static_cast<size_t>(count) < 4096 ? static_cast<size_t>(count) : 4096);
        for (size_t index = 0; index < count; ++index) {
            const IndexPathScope scope(reader.Path(), index);
            out.emplace_back();
            const Result<void> res = DecodeValue(out.back(), reader);
            if (!res) {
                out.clear();
                return res;
            }
        }
        return {};
    } else if constexpr (detail::ArrayLike<Type>) {
        if constexpr (detail::ByteLike<typename detail::ArrayTrait<Type>::element>) {
            const auto bytes = reader.Take(out.size());
            if (!bytes) {
                return std::unexpected(bytes.error());
            }
            std::memcpy(out.data(), bytes->data(), bytes->size());
            return {};
        } else {
            for (size_t index = 0; index < out.size(); ++index) {
                const IndexPathScope scope(reader.Path(), index);
                const Result<void>            res = DecodeValue(out[index], reader);
                if (!res) {
                    return res;
                }
            }
            return {};
        }
    } else if constexpr (detail::PairLike<Type>) {
        const Result<void> first = DecodeValue(out.first, reader);
        if (!first) {
            return first;
        }
        return DecodeValue(out.second, reader);
    } else if constexpr (detail::TupleLike<Type>) {
        Result<void> result {};
        [&]<size_t... Is>(std::index_sequence<Is...>) {
            ((result ? (result = DecodeValue(std::get<Is>(out), reader)) : result), ...);
        }(std::make_index_sequence<std::tuple_size_v<Type>>());
        return result;
    } else if constexpr (std::is_array_v<Type>) {
        using Element = std::remove_extent_t<Type>;
        if constexpr (detail::ByteLike<Element>) {
            constexpr size_t kExtent = std::extent_v<Type>;
            const auto       bytes   = reader.Take(kExtent);
            if (!bytes) {
                return std::unexpected(bytes.error());
            }
            std::memcpy(out, bytes->data(), kExtent);
            return {};
        } else {
            constexpr size_t kExtent = std::extent_v<Type>;
            for (size_t index = 0; index < kExtent; ++index) {
                const IndexPathScope scope(reader.Path(), index);
                const Result<void>            res = DecodeValue(out[index], reader);
                if (!res) {
                    return res;
                }
            }
            return {};
        }
    } else if constexpr (std::is_aggregate_v<Type> && detail::ReflectionAvailable) {
        return DecodeAggregate(out, reader);
    } else {
        static_assert(detail::AlwaysFalse<Type>::value,
                      "ZHLN.Wire: type has no wire representation. Add a ZHLN::Wire::Codec<T> specialization, "
                      "convert it to a wire-supported type, or build with a reflection-capable compiler.");
        return std::unexpected(reader.Fail(WireError::UnsupportedType, ZHLN::Reflect::TypeName<Type>()));
    }
}

} // namespace ZHLN::Wire

// ============================================================================
// Reflection-driven aggregate encoding (C++26 static reflection)
// ============================================================================

#if defined(__cpp_impl_reflection) || (defined(__has_feature) && __has_feature(reflection))

export namespace ZHLN::Wire::detail {

template <auto MemberInfo>
consteval auto MemberSkipped() -> bool {
    return ZHLN::Reflect::HasAnnotation<Skip, MemberInfo>();
}

template <auto MemberInfo>
consteval auto MemberDescription() -> std::string_view {
    return ZHLN::Reflect::GetDescriptionText<MemberInfo>();
}

struct RangeSpec {
    bool        active {};
    long double minValue {};
    long double maxValue {};
};

template <auto MemberInfo, std::size_t Index>
consteval auto ExtractRangeAt() -> RangeSpec {
    constexpr auto annotations = ZHLN::Reflect::detail::AnnotationsOf<MemberInfo>();
    constexpr auto type        = std::meta::dealias(std::meta::type_of(annotations[Index]));
    if constexpr (std::meta::has_template_arguments(type)) {
        if constexpr (std::meta::template_of(type) == ^^Range) {
            using RangeType = typename[:type:];
            return RangeSpec {.active = true,
                              .minValue = static_cast<long double>(RangeType::minValue),
                              .maxValue = static_cast<long double>(RangeType::maxValue)};
        }
    }
    return RangeSpec {};
}

template <auto MemberInfo>
consteval auto MemberRange() -> RangeSpec {
    constexpr std::size_t count = ZHLN::Reflect::detail::AnnotationsOf<MemberInfo>().size();
    RangeSpec             result {};
    [&]<std::size_t... Is>(std::index_sequence<Is...>) {
        ((result.active ? true : (result = ExtractRangeAt<MemberInfo, Is>(), true)), ...);
    }(std::make_index_sequence<count>());
    return result;
}

template <typename T, std::size_t Index>
consteval auto ExtractVersionAt() -> std::optional<uint32_t> {
    constexpr auto entity     = std::meta::dealias(^^std::remove_cvref_t<T>);
    constexpr auto annotations = ZHLN::Reflect::detail::AnnotationsOf<entity>();
    constexpr auto type        = std::meta::dealias(std::meta::type_of(annotations[Index]));
    if constexpr (std::meta::has_template_arguments(type)) {
        if constexpr (std::meta::template_of(type) == ^^Version) {
            using VersionType = typename[:type:];
            return VersionType::value;
        }
    }
    return std::nullopt;
}

/// Wire schema version declared via [[= ZHLN::Wire::Version<N> {}]]; default 1.
template <typename T>
consteval auto SchemaVersionOf() -> uint32_t {
    constexpr auto        entity = std::meta::dealias(^^std::remove_cvref_t<T>);
    constexpr std::size_t count  = ZHLN::Reflect::detail::AnnotationsOf<entity>().size();
    uint32_t              result = 1;
    [&]<std::size_t... Is>(std::index_sequence<Is...>) {
        ((result != 1 ? true
                      : (ExtractVersionAt<T, Is>().has_value() ? (result = *ExtractVersionAt<T, Is>(), true) : false)),
         ...);
    }(std::make_index_sequence<count>());
    return result;
}

template <typename T>
consteval auto HasBaseClasses() -> bool {
    return !ZHLN::Reflect::BaseClasses<T>().empty();
}

inline void AttachFieldNote(Failure& failure, std::string_view typeName, std::string_view fieldName, std::string_view description) {
    if (!failure.note.empty()) {
        return; // innermost annotation wins
    }
    failure.note = std::format("{}.{}", typeName, fieldName);
    if (!description.empty()) {
        failure.note += std::string(": ") + std::string(description);
    }
}

} // namespace ZHLN::Wire::detail

export namespace ZHLN::Wire {

template <typename T>
    requires std::is_aggregate_v<std::remove_cvref_t<T>>
auto EncodeAggregate(const T& value, Writer& writer) -> Result<void> {
    using Type = std::remove_cvref_t<T>;
    static_assert(!detail::HasBaseClasses<Type>(),
                  "ZHLN.Wire: aggregates with base classes are not wire-serializable; flatten the fields or add a Codec<T> specialization");

    std::optional<Failure> failure;
    [:ZHLN::Reflect::Expand(ZHLN::Reflect::detail::NonStaticDataMembers<Type>()):] >> [&]<auto member>() -> auto {
        if (failure.has_value()) {
            return;
        }
        constexpr bool skipped = detail::MemberSkipped<member>();
        if constexpr (skipped) {
            return;
        } else {
            constexpr std::string_view name = std::meta::has_identifier(member) ? std::meta::identifier_of(member) : "<anonymous>";
            const PathScope     scope(writer.Path(), name);
            const Result<void>          res = writer.Put(value.[:member:]);
            if (!res) {
                Failure failing = std::move(res).error();
                detail::AttachFieldNote(failing, ZHLN::Reflect::TypeName<Type>(), name, detail::MemberDescription<member>());
                failure = std::move(failing);
            }
        }
    };
    if (failure.has_value()) {
        return std::unexpected(std::move(*failure));
    }
    return {};
}

template <typename T>
    requires std::is_aggregate_v<std::remove_cvref_t<T>>
auto DecodeAggregate(T& out, Reader& reader) -> Result<void> {
    using Type = std::remove_cvref_t<T>;
    static_assert(!detail::HasBaseClasses<Type>(),
                  "ZHLN.Wire: aggregates with base classes are not wire-serializable; flatten the fields or add a Codec<T> specialization");

    std::optional<Failure> failure;
    [:ZHLN::Reflect::Expand(ZHLN::Reflect::detail::NonStaticDataMembers<Type>()):] >> [&]<auto member>() -> auto {
        if (failure.has_value()) {
            return;
        }
        constexpr bool skipped = detail::MemberSkipped<member>();
        if constexpr (skipped) {
            return;
        } else {
            using Field                    = typename[:std::meta::type_of(member):];
            constexpr std::string_view name = std::meta::has_identifier(member) ? std::meta::identifier_of(member) : "<anonymous>";

            if constexpr (std::same_as<Field, std::string_view> || detail::SpanLike<Field>) {
                static_assert(detail::AlwaysFalse<Field>::value,
                              "ZHLN.Wire: string_view/span members are write-only; use std::string, FixedString or a "
                              "resizable container in decodable messages");
            } else {
                const PathScope scope(reader.Path(), name);
                Result<void>            res = reader.Get(out.[:member:]);
                if (res) {
                    if constexpr (std::is_arithmetic_v<Field>) {
                        constexpr auto range = detail::MemberRange<member>();
                        if constexpr (range.active) {
                            const long double current = static_cast<long double>(out.[:member:]);
                            if (current < range.minValue || current > range.maxValue) {
                                res = std::unexpected(
                                    reader.Fail(WireError::ValueOutOfRange, current, range.minValue, range.maxValue));
                            }
                        }
                    }
                }
                if (!res) {
                    Failure failing = std::move(res).error();
                    detail::AttachFieldNote(failing, ZHLN::Reflect::TypeName<Type>(), name, detail::MemberDescription<member>());
                    failure = std::move(failing);
                }
            }
        }
    };
    if (failure.has_value()) {
        return std::unexpected(std::move(*failure));
    }
    return {};
}

} // namespace ZHLN::Wire

#else

export namespace ZHLN::Wire {

// Reflection-free fallback: aggregates require hand-written Codec<T> specializations.
template <typename T>
auto EncodeAggregate(const T& /*value*/, Writer& writer) -> Result<void> {
    static_assert(detail::AlwaysFalse<T>::value,
                  "ZHLN.Wire: reflected aggregate serialization requires a compiler with C++26 static reflection "
                  "(__cpp_impl_reflection); provide a Codec<T> specialization instead");
    return std::unexpected(writer.Fail(WireError::UnsupportedType, ZHLN::Reflect::TypeName<T>()));
}

template <typename T>
auto DecodeAggregate(T& /*out*/, Reader& reader) -> Result<void> {
    static_assert(detail::AlwaysFalse<T>::value,
                  "ZHLN.Wire: reflected aggregate serialization requires a compiler with C++26 static reflection "
                  "(__cpp_impl_reflection); provide a Codec<T> specialization instead");
    return std::unexpected(reader.Fail(WireError::UnsupportedType, ZHLN::Reflect::TypeName<T>()));
}

} // namespace ZHLN::Wire

#endif

// ============================================================================
// Convenience API + out-of-line typed entry points
// ============================================================================

export namespace ZHLN::Wire {

template <typename T>
auto Writer::Put(const T& value) -> Result<void> {
    return EncodeValue(value, *this);
}

template <typename T>
auto Reader::Get(T& out) -> Result<void> {
    return DecodeValue(out, *this);
}

template <typename T>
[[nodiscard]] auto Encode(const T& value, size_t maxBytes = DEFAULT_MAX_MESSAGE_BYTES) -> Result<std::vector<uint8_t>> {
    Writer              writer(maxBytes);
    const Result<void>  res = writer.Put(value);
    if (!res) {
        return std::unexpected(res.error());
    }
    const auto bytes = writer.Bytes();
    return std::vector<uint8_t>(bytes.begin(), bytes.end());
}

template <typename T>
[[nodiscard]] auto Decode(std::span<const uint8_t> bytes) -> Result<T> {
    T                   out {};
    Reader              reader(bytes);
    const Result<void>  res = reader.Get(out);
    if (!res) {
        return std::unexpected(res.error());
    }
    if (reader.Remaining() != 0) {
        return std::unexpected(reader.Fail(WireError::TrailingBytes, reader.Remaining(), bytes.size()));
    }
    return out;
}

} // namespace ZHLN::Wire

// ============================================================================
// CRC32 (IEEE 802.3, reflected) — used by the network frame codec
// ============================================================================

export namespace ZHLN::Wire::Checksum {

namespace detail {
consteval auto MakeCrcTable() {
    std::array<uint32_t, 256> table {};
    for (uint32_t index = 0; index < 256; ++index) {
        uint32_t value = index;
        for (int bit = 0; bit < 8; ++bit) {
            value = (value & 1u) != 0 ? (0xEDB88320u ^ (value >> 1)) : (value >> 1);
        }
        table[index] = value;
    }
    return table;
}
} // namespace detail

inline constexpr auto kCrcTable = detail::MakeCrcTable();

[[nodiscard]] inline auto Crc32(std::span<const uint8_t> data) noexcept -> uint32_t {
    uint32_t crc = 0xFFFFFFFFu;
    for (const uint8_t byte: data) {
        crc = kCrcTable[(crc ^ byte) & 0xFFu] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

} // namespace ZHLN::Wire::Checksum

// ============================================================================
// Block compression — compact LZ77 ("mini-LZ4"), dependency-free
//
// Token stream (mirrors the LZ4 block layout):
//   token byte: high nibble = literal length (15 = extended), low nibble =
//   match length - 4 (15 = extended). Then the literal bytes, then — unless
//   this is the final sequence — a 2-byte little-endian back-reference offset
//   in [1, 65535] and the extended match length bytes. The last sequence of
//   a block contains literals only. Length extensions add 255 per byte while
//   the byte equals 255.
// ============================================================================

export namespace ZHLN::Wire::Compression {

inline constexpr size_t WINDOW_SIZE = 64 * 1024;
inline constexpr size_t MIN_MATCH   = 4;
inline constexpr size_t MAX_MATCH   = 65535;
inline constexpr size_t HASH_LOG    = 12;
inline constexpr size_t HASH_SIZE   = 1u << HASH_LOG;

/// Worst-case encoded size (everything literal, every length extended).
[[nodiscard]] inline auto CompressBound(size_t rawSize) noexcept -> size_t {
    return rawSize + (rawSize / 255) + 16;
}

namespace detail {

inline auto Hash4(std::span<const uint8_t> data, size_t index) noexcept -> uint32_t {
    const uint32_t value = static_cast<uint32_t>(data[index]) | (static_cast<uint32_t>(data[index + 1]) << 8)
                           | (static_cast<uint32_t>(data[index + 2]) << 16) | (static_cast<uint32_t>(data[index + 3]) << 24);
    return (value * 2654435761u) >> (32 - HASH_LOG);
}

} // namespace detail

[[nodiscard]] auto Compress(std::span<const uint8_t> raw, size_t maxOutput = DEFAULT_MAX_MESSAGE_BYTES) -> Result<std::vector<uint8_t>> {
    if (raw.empty()) {
        return std::vector<uint8_t> {0x00};
    }
    if (CompressBound(raw.size()) > maxOutput) {
        return std::unexpected(Failure {
            .code    = ZHLN::Error(WireError::CompressionFailed),
            .details = ZHLN::Reflect::FormatEnumMessage(WireError::CompressionFailed,
                                                        std::format("input of {} byte(s) exceeds the {} byte output limit",
                                                                    raw.size(), maxOutput))
        });
    }

    Buffer output(CompressBound(raw.size()));
    // 0xFFFFFFFF marks an empty slot; position 0 is a legitimate candidate.
    std::vector<uint32_t> table(HASH_SIZE, 0xFFFFFFFFu);

    const auto emitLength = [&output](size_t length) -> Result<void> {
        while (length >= 255) {
            const Result<void> res = output.AppendByte(255);
            if (!res) {
                return res;
            }
            length -= 255;
        }
        return output.AppendByte(static_cast<uint8_t>(length));
    };

    const auto emitLiterals = [&output, &raw](size_t begin, size_t end) -> Result<void> {
        return output.Append(raw.subspan(begin, end - begin));
    };

    size_t anchor   = 0; // start of the pending literal run
    size_t position = 0;

    while (position + MIN_MATCH <= raw.size()) {
        const uint32_t hash      = detail::Hash4(raw, position);
        const uint32_t candidate = table[hash];
        table[hash]              = static_cast<uint32_t>(position);

        // Offsets are stored as 16-bit values in [1, 65535]; WINDOW_SIZE itself is not representable.
        const bool usable = candidate != 0xFFFFFFFFu && candidate < position && (position - candidate) < WINDOW_SIZE
                            && std::memcmp(raw.data() + candidate, raw.data() + position, MIN_MATCH) == 0;
        if (!usable) {
            ++position;
            continue;
        }

        // Extend the match as far as it actually goes.
        size_t matchLength = MIN_MATCH;
        while (matchLength < MAX_MATCH && position + matchLength < raw.size()
               && raw[candidate + matchLength] == raw[position + matchLength]) {
            ++matchLength;
        }

        const size_t literalLength = position - anchor;
        const size_t matchToken    = matchLength - MIN_MATCH;
        const uint8_t token =
            static_cast<uint8_t>(((literalLength < 15 ? literalLength : 15) << 4) | (matchToken < 15 ? matchToken : 15));

        Result<void> res = output.AppendByte(token);
        if (res && literalLength >= 15) {
            res = emitLength(literalLength - 15);
        }
        if (res) {
            res = emitLiterals(anchor, position);
        }
        if (res) {
            const size_t offset = position - candidate;
            res                 = output.AppendByte(static_cast<uint8_t>(offset & 0xFFu));
            if (res) {
                res = output.AppendByte(static_cast<uint8_t>((offset >> 8) & 0xFFu));
            }
        }
        if (res && matchToken >= 15) {
            res = emitLength(matchToken - 15);
        }
        if (!res) {
            return std::unexpected(res.error());
        }

        anchor   = position + matchLength;
        position = anchor;
    }

    // Final literal-only sequence.
    const size_t literalLength = raw.size() - anchor;
    Result<void> res           = output.AppendByte(static_cast<uint8_t>((literalLength < 15 ? literalLength : 15) << 4));
    if (res && literalLength >= 15) {
        res = emitLength(literalLength - 15);
    }
    if (res) {
        res = emitLiterals(anchor, raw.size());
    }
    if (!res) {
        return std::unexpected(res.error());
    }

    const auto bytes = output.Data();
    return std::vector<uint8_t>(bytes.begin(), bytes.end());
}

[[nodiscard]] auto Decompress(std::span<const uint8_t> compressed, size_t maxDecompressed) -> Result<std::vector<uint8_t>> {
    // maxDecompressed == 0 means "no explicit cap".
    std::vector<uint8_t> out;
    if (maxDecompressed != 0) {
        out.reserve(maxDecompressed < (1u << 20) ? maxDecompressed : (1u << 20));
    }

    const auto fits = [&](size_t additional) -> bool {
        return maxDecompressed == 0 || out.size() + additional <= maxDecompressed;
    };

    const auto readLength = [&](size_t& position, size_t base) -> Result<size_t> {
        size_t length = base;
        while (true) {
            if (position >= compressed.size()) {
                return std::unexpected(Failure {
                    .code    = ZHLN::Error(WireError::DecompressionFailed),
                    .details = ZHLN::Reflect::FormatEnumMessage(WireError::DecompressionFailed, "truncated length extension")
                });
            }
            const uint8_t byte = compressed[position];
            ++position;
            length += byte;
            if (byte != 255) {
                return length;
            }
        }
    };

    size_t position = 0;
    while (position < compressed.size()) {
        const uint8_t token = compressed[position];
        ++position;

        size_t literalLength = token >> 4;
        if (literalLength == 15) {
            const auto extended = readLength(position, 15);
            if (!extended) {
                return std::unexpected(extended.error());
            }
            literalLength = *extended;
        }
        if (literalLength > compressed.size() - position) {
            return std::unexpected(Failure {
                .code    = ZHLN::Error(WireError::DecompressionFailed),
                .details = ZHLN::Reflect::FormatEnumMessage(
                    WireError::DecompressionFailed,
                    std::format("literal run of {} byte(s) at input offset {} overruns the block", literalLength, position))
            });
        }
        if (!fits(literalLength)) {
            return std::unexpected(Failure {
                .code    = ZHLN::Error(WireError::DecompressionFailed),
                .details = ZHLN::Reflect::FormatEnumMessage(
                    WireError::DecompressionFailed,
                    std::format("decompressed size would exceed the {} byte limit", maxDecompressed))
            });
        }
        out.insert(out.end(), compressed.begin() + static_cast<std::ptrdiff_t>(position),
                   compressed.begin() + static_cast<std::ptrdiff_t>(position + literalLength));
        position += literalLength;

        if (position == compressed.size()) {
            break; // final sequence: literals only
        }
        if (compressed.size() - position < 2) {
            return std::unexpected(Failure {
                .code    = ZHLN::Error(WireError::DecompressionFailed),
                .details = ZHLN::Reflect::FormatEnumMessage(WireError::DecompressionFailed, "truncated match offset")
            });
        }
        const size_t offset = static_cast<size_t>(compressed[position]) | (static_cast<size_t>(compressed[position + 1]) << 8);
        position += 2;
        if (offset == 0 || offset > out.size()) {
            return std::unexpected(Failure {
                .code    = ZHLN::Error(WireError::DecompressionFailed),
                .details = ZHLN::Reflect::FormatEnumMessage(
                    WireError::DecompressionFailed,
                    std::format("back-reference offset {} is out of range ({} byte(s) produced so far)", offset, out.size()))
            });
        }

        size_t matchLength = static_cast<size_t>(token & 0x0Fu) + MIN_MATCH;
        if ((token & 0x0Fu) == 15) {
            const auto extended = readLength(position, 15);
            if (!extended) {
                return std::unexpected(extended.error());
            }
            matchLength = *extended + MIN_MATCH;
        }
        if (!fits(matchLength)) {
            return std::unexpected(Failure {
                .code    = ZHLN::Error(WireError::DecompressionFailed),
                .details = ZHLN::Reflect::FormatEnumMessage(
                    WireError::DecompressionFailed,
                    std::format("decompressed size would exceed the {} byte limit", maxDecompressed))
            });
        }

        size_t source = out.size() - offset;
        for (size_t index = 0; index < matchLength; ++index) {
            out.push_back(out[source]); // overlapping copies replicate runs, exactly like LZ4
            ++source;
        }
    }
    return out;
}

} // namespace ZHLN::Wire::Compression
