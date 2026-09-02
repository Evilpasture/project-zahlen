// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// tests/extras/TestNetworkWire.cpp
//
// Exercises the dependency-free ZHLN.Wire serialization stack and the
// ZHLN.Network protocol layer end-to-end: annotated reflection round-trips,
// hostile/truncated/corrupted input handling, quantization codecs, framing,
// CRC32 and block compression.

#include "TestsFramework.hpp"

// clang-format off
#include <Jolt/Jolt.h>
// clang-format on
#include <Jolt/Math/Quat.h>
#include <Jolt/Math/Vec3.h>
#include <Zahlen/Core/Description.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

import ZHLN.Network;

#if !defined(__cpp_impl_reflection) && !(defined(__has_feature) && __has_feature(reflection))
#error "TestNetworkWire requires a compiler with C++26 static reflection"
#endif

namespace {

using ZHLN::Wire::Failure;
using ZHLN::Wire::Result;
using ZHLN::Wire::WireError;

enum class TestGear : uint8_t {
    Reverse = 1,
    Neutral,
    First ZHLN_ANNOTATION(ZHLN::Description<"lowest forward gear"> {}),
    Second,
    Third
};

struct TestPlayer {
    uint64_t                uid ZHLN_ANNOTATION(ZHLN::Description<"account identity"> {});
    std::string             name;
    int32_t                 hp ZHLN_ANNOTATION(ZHLN::Wire::Range<0, 100> {}, = ZHLN::Description<"hitpoints remaining"> {});
    float                   heading ZHLN_ANNOTATION(ZHLN::Wire::Range<-180.0f, 180.0f> {});
    std::optional<uint16_t> teamId;
    std::vector<uint8_t>    inventory;
    bool                    active;
    TestGear                gear;
    int                     localCache ZHLN_ANNOTATION(ZHLN::Wire::Skip {}); // never serialized
};

struct TestWorld {
    uint32_t              tick;
    std::vector<TestPlayer> players;
};

[[nodiscard]] auto operator==(const TestPlayer& lhs, const TestPlayer& rhs) -> bool {
    return lhs.uid == rhs.uid && lhs.name == rhs.name && lhs.hp == rhs.hp && lhs.heading == rhs.heading
           && lhs.teamId == rhs.teamId && lhs.inventory == rhs.inventory && lhs.active == rhs.active && lhs.gear == rhs.gear;
}

auto Bytes(std::initializer_list<uint8_t> list) -> std::vector<uint8_t> {
    return std::vector<uint8_t>(list);
}

auto Span(std::initializer_list<uint8_t> list) -> std::span<const uint8_t> {
    static thread_local std::vector<uint8_t> storage;
    storage = std::vector<uint8_t>(list);
    return storage;
}

auto Is(const auto& result, WireError expected) -> bool {
    return !result.has_value() && result.error().code.Is(expected);
}

template <typename T>
auto RoundTrip(const T& value) -> Result<T> {
    auto encoded = ZHLN::Wire::Encode(value);
    if (!encoded) {
        return std::unexpected(encoded.error());
    }
    return ZHLN::Wire::Decode<T>(*encoded);
}

auto MakePlayer(uint64_t uid) -> TestPlayer {
    return TestPlayer {
        .uid        = uid,
        .name       = "player-" + std::to_string(uid),
        .hp         = 42,
        .heading    = -95.5f,
        .teamId     = std::optional<uint16_t>(7),
        .inventory  = {0xDE, 0xAD, 0xBE, 0xEF},
        .active     = true,
        .gear       = TestGear::Second,
        .localCache = 123456
    };
}

auto MakeWorld() -> TestWorld {
    return TestWorld {.tick = 31337, .players = {MakePlayer(1), MakePlayer(2), MakePlayer(3)}};
}

// Deterministic xorshift so "random" tests are reproducible.
struct Rng {
    uint64_t state = 0x9E3779B97F4A7C15ull;
    auto Next() -> uint64_t {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return state;
    }
    auto NextByte() -> uint8_t {
        return static_cast<uint8_t>(Next() >> 32);
    }
};

auto Contains(std::string_view haystack, std::string_view needle) -> bool {
    return haystack.find(needle) != std::string_view::npos;
}

template <typename T>
auto CheckRoundTrip(const T& value) -> void {
    auto round = RoundTrip(value);
    ZHLN::Test::ExpectTrue(round.has_value());
    if (round) {
        ZHLN::Test::ExpectEq(round.value(), value);
    }
}

} // namespace

struct TestNetworkWireSuite {
    struct Tests {
        // ------------------------------------------------------------------
        // Primitives
        // ------------------------------------------------------------------
        std::expected<void, ZHLN::Error> primitives_roundtrip() {
            CheckRoundTrip(true);
            CheckRoundTrip(false);

            CheckRoundTrip(uint8_t {0});
            CheckRoundTrip(uint8_t {255});
            CheckRoundTrip(uint16_t {0x8000});
            CheckRoundTrip(uint16_t {65535});
            CheckRoundTrip(uint32_t {0xFFFFFFFF});
            CheckRoundTrip(uint64_t {0xFFFFFFFFFFFFFFFF});
            CheckRoundTrip(uint64_t {300});
            CheckRoundTrip(uint64_t {16384});
            CheckRoundTrip(int8_t {-128});
            CheckRoundTrip(int8_t {127});
            CheckRoundTrip(int16_t {-32768});
            CheckRoundTrip(int32_t {-2147483647 - 1});
            CheckRoundTrip(int64_t {0x7FFFFFFFFFFFFFFF});
            CheckRoundTrip(int64_t {-9223372036854775807LL - 1});
            CheckRoundTrip(char {'A'});

            CheckRoundTrip(-1234.5678f);
            CheckRoundTrip(2.718281828459045e100);
            return {};
        }

        // ------------------------------------------------------------------
        // Varint byte layout (LEB128, little-endian groups)
        // ------------------------------------------------------------------
        std::expected<void, ZHLN::Error> varint_encoding_bytes() {
            struct Case {
                uint64_t             value;
                std::vector<uint8_t> bytes;
            };
            const std::array cases = {
                Case {0, Bytes({0x00})},
                Case {1, Bytes({0x01})},
                Case {127, Bytes({0x7F})},
                Case {128, Bytes({0x80, 0x01})},
                Case {300, Bytes({0xAC, 0x02})},
                Case {16384, Bytes({0x80, 0x80, 0x01})},
                Case {0xFFFFFFFF, Bytes({0xFF, 0xFF, 0xFF, 0xFF, 0x0F})}
            };

            for (const auto& test: cases) {
                ZHLN::Wire::Writer writer;
                ZHLN::Test::ExpectTrue(writer.PutUVar(test.value).has_value());
                const auto bytes = writer.Bytes();
                ZHLN::Test::ExpectEq(std::vector<uint8_t>(bytes.begin(), bytes.end()), test.bytes);

                ZHLN::Wire::Reader reader(bytes);
                uint64_t           decoded = 0;
                ZHLN::Test::ExpectTrue(reader.GetUVar(decoded).has_value());
                ZHLN::Test::ExpectEq(decoded, test.value);
            }
            return {};
        }

        std::expected<void, ZHLN::Error> varint_rejects_noncanonical_and_truncated() {
            // {0x80, 0x00}: zero value encoded in two bytes — non-canonical.
            {
                ZHLN::Wire::Reader reader(Span({0x80, 0x00}));
                uint64_t           decoded = 0;
                auto               result  = reader.GetUVar(decoded);
                ZHLN::Test::ExpectTrue(Is(result, WireError::NonCanonicalVarint));
                ZHLN::Test::ExpectTrue(Contains(result.error().Format(), "non-canonical"));
            }
            // {0xFF, 0x00}: 127 encoded in two bytes — non-canonical.
            {
                ZHLN::Wire::Reader reader(Span({0xFF, 0x00}));
                uint64_t           decoded = 0;
                ZHLN::Test::ExpectTrue(Is(reader.GetUVar(decoded), WireError::NonCanonicalVarint));
            }
            // {0x80}: dangling continuation — truncated.
            {
                ZHLN::Wire::Reader reader(Span({0x80}));
                uint64_t           decoded = 0;
                ZHLN::Test::ExpectTrue(Is(reader.GetUVar(decoded), WireError::Truncated));
            }
            // 11 continuation bytes — overflow.
            {
                const std::vector<uint8_t> overflow(11, 0xFF);
                ZHLN::Wire::Reader         reader(overflow);
                uint64_t                   decoded = 0;
                ZHLN::Test::ExpectTrue(Is(reader.GetUVar(decoded), WireError::VarintOverflow));
            }
            // 128 needs two bytes and is canonical.
            {
                ZHLN::Wire::Reader reader(Span({0x80, 0x01}));
                uint64_t           decoded = 0;
                ZHLN::Test::ExpectTrue(reader.GetUVar(decoded).has_value());
                ZHLN::Test::ExpectEq(decoded, uint64_t {128});
            }
            return {};
        }

        std::expected<void, ZHLN::Error> integer_narrowing_is_checked() {
            // int8_t cannot hold 200.
            ZHLN::Wire::Writer writer;
            ZHLN::Test::ExpectTrue(writer.Put(static_cast<int32_t>(200)).has_value());
            int8_t             decoded = 0;
            ZHLN::Wire::Reader reader(writer.Bytes());
            auto               result = reader.Get(decoded);
            ZHLN::Test::ExpectTrue(Is(result, WireError::ValueOutOfRange));
            ZHLN::Test::ExpectTrue(Contains(result.error().Format(), "outside the permitted range"));
            return {};
        }

        std::expected<void, ZHLN::Error> booleans_are_validated() {
            ZHLN::Wire::Reader reader(Span({0x02}));
            bool               decoded = false;
            auto               result  = reader.Get(decoded);
            ZHLN::Test::ExpectTrue(Is(result, WireError::InvalidBoolean));
            ZHLN::Test::ExpectTrue(Contains(result.error().Format(), "neither 0 nor 1"));
            return {};
        }

        // ------------------------------------------------------------------
        // Strings, blobs, containers
        // ------------------------------------------------------------------
        std::expected<void, ZHLN::Error> strings_roundtrip_and_limits() {
            for (const std::string_view text: {std::string_view(""), std::string_view("Zahlen"), std::string_view("héllo wörld")}) {
                auto round = RoundTrip(std::string(text));
                ZHLN::Test::ExpectTrue(round.has_value());
                if (round) {
                    ZHLN::Test::ExpectEq(round.value(), std::string(text));
                }
            }

            // 9-byte string against an 8-byte limit.
            ZHLN::Wire::Limits  limits {.maxStringBytes = 8, .maxCollectionElements = 1024};
            ZHLN::Wire::Writer  writer;
            ZHLN::Test::ExpectTrue(writer.Put(std::string("123456789")).has_value());
            std::string         decoded;
            ZHLN::Wire::Reader  reader(writer.Bytes(), limits);
            auto                result = reader.Get(decoded);
            ZHLN::Test::ExpectTrue(Is(result, WireError::StringTooLong));
            ZHLN::Test::ExpectTrue(Contains(result.error().Format(), "9 byte(s) exceeds the 8 byte limit"));

            // Trailing bytes are rejected.
            auto trailing = ZHLN::Wire::Decode<std::string>(Span({0x01, 'A', 0xFF}));
            ZHLN::Test::ExpectTrue(Is(trailing, WireError::TrailingBytes));
            return {};
        }

        std::expected<void, ZHLN::Error> fixed_string_capacity_is_enforced() {
            ZHLN::FixedString<8> fixed {"four"};
            auto                 round = RoundTrip(fixed);
            ZHLN::Test::ExpectTrue(round.has_value());
            if (round) {
                ZHLN::Test::ExpectEq(std::string_view(round.value()), std::string_view("four"));
            }

            ZHLN::Wire::Writer writer;
            ZHLN::Test::ExpectTrue(writer.Put(std::string("12345678")).has_value()); // 8 chars: too long for FixedString<8>
            ZHLN::FixedString<8> decoded;
            ZHLN::Wire::Reader   reader(writer.Bytes());
            auto                 result = reader.Get(decoded);
            ZHLN::Test::ExpectTrue(Is(result, WireError::FixedStringOverflow));
            ZHLN::Test::ExpectTrue(Contains(result.error().Format(), "FixedString<8>"));
            return {};
        }

        std::expected<void, ZHLN::Error> optionals_roundtrip() {
            {
                std::optional<int32_t> value;
                auto                   round = RoundTrip(value);
                ZHLN::Test::ExpectTrue(round.has_value());
                if (round) {
                    ZHLN::Test::ExpectFalse(round.value().has_value());
                }
            }
            {
                std::optional<int32_t> value {-70000};
                auto                   round = RoundTrip(value);
                ZHLN::Test::ExpectTrue(round.has_value());
                if (round) {
                    ZHLN::Test::ExpectTrue(round.value().has_value());
                    ZHLN::Test::ExpectEq(*round.value(), -70000);
                }
            }
            return {};
        }

        std::expected<void, ZHLN::Error> enums_are_validated() {
            auto round = RoundTrip(TestGear::First);
            ZHLN::Test::ExpectTrue(round.has_value());
            if (round) {
                ZHLN::Test::ExpectEq(round.value(), TestGear::First);
            }

            ZHLN::Wire::Writer writer;
            ZHLN::Test::ExpectTrue(writer.Put(static_cast<uint8_t>(77)).has_value());
            TestGear           decoded = TestGear::Neutral;
            ZHLN::Wire::Reader reader(writer.Bytes());
            auto               result = reader.Get(decoded);
            ZHLN::Test::ExpectTrue(Is(result, WireError::InvalidEnumValue));
            const auto text = result.error().Format();
            ZHLN::Test::ExpectTrue(Contains(text, "not a valid enumerator"));
            ZHLN::Test::ExpectTrue(Contains(text, "TestGear"));
            return {};
        }

        std::expected<void, ZHLN::Error> collections_roundtrip() {
            {
                const std::vector<int32_t> values = {-1, 0, 1, 2147483647, -2147483647 - 1};
                auto                       round  = RoundTrip(values);
                ZHLN::Test::ExpectTrue(round.has_value());
                if (round) {
                    ZHLN::Test::ExpectEq(round.value(), values);
                }
            }
            {
                const std::vector<std::string> values = {"alpha", "", "gamma"};
                auto                           round  = RoundTrip(values);
                ZHLN::Test::ExpectTrue(round.has_value());
                if (round) {
                    ZHLN::Test::ExpectEq(round.value(), values);
                }
            }
            {
                const std::vector<int32_t> empty;
                auto                       round = RoundTrip(empty);
                ZHLN::Test::ExpectTrue(round.has_value());
                if (round) {
                    ZHLN::Test::ExpectTrue(round.value().empty());
                }
            }
            {
                const std::array<uint8_t, 4> raw = {0xDE, 0xAD, 0xBE, 0xEF};
                auto                         round = RoundTrip(raw);
                ZHLN::Test::ExpectTrue(round.has_value());
                if (round) {
                    ZHLN::Test::ExpectEq(round.value(), raw);
                }
            }
            {
                const std::pair<uint16_t, std::string> value {5555, "pair"};
                auto                                   round = RoundTrip(value);
                ZHLN::Test::ExpectTrue(round.has_value());
                if (round) {
                    ZHLN::Test::ExpectEq(round.value().first, uint16_t {5555});
                    ZHLN::Test::ExpectEq(round.value().second, std::string("pair"));
                }
            }
            {
                const std::tuple<uint8_t, float, std::string> value {9, 1.5f, "tuple"};
                auto                                          round = RoundTrip(value);
                ZHLN::Test::ExpectTrue(round.has_value());
                if (round) {
                    ZHLN::Test::ExpectEq(std::get<0>(round.value()), uint8_t {9});
                    ZHLN::Test::ExpectEq(std::get<1>(round.value()), 1.5f);
                    ZHLN::Test::ExpectEq(std::get<2>(round.value()), std::string("tuple"));
                }
            }
            {
                const std::vector<bool> values = {true, false, true, true};
                auto                    round  = RoundTrip(values);
                ZHLN::Test::ExpectTrue(round.has_value());
                if (round) {
                    ZHLN::Test::ExpectEq(round.value(), values);
                }
            }
            return {};
        }

        std::expected<void, ZHLN::Error> collection_count_bombs_fail_fast() {
            // count = 0x80 0x80 0x80 0x01 = 268435456 elements, no data after.
            ZHLN::Wire::Reader reader(Span({0x80, 0x80, 0x80, 0x01}));
            std::vector<int32_t> decoded;
            auto                 result = reader.Get(decoded);
            ZHLN::Test::ExpectTrue(!result.has_value());
            ZHLN::Test::ExpectTrue(Is(result, WireError::ElementCountExceedsInput) || Is(result, WireError::CollectionTooLarge));

            // A vector<uint8_t> claiming 100 bytes with 2 available.
            ZHLN::Wire::Reader blobReader(Span({100, 0x41, 0x42}));
            std::vector<uint8_t> blob;
            ZHLN::Test::ExpectTrue(Is(blobReader.Get(blob), WireError::ElementCountExceedsInput));
            return {};
        }

        // ------------------------------------------------------------------
        // Reflection-driven aggregates + annotations
        // ------------------------------------------------------------------
        std::expected<void, ZHLN::Error> aggregate_roundtrip() {
            const TestWorld world = MakeWorld();
            auto            round = RoundTrip(world);
            ZHLN::Test::ExpectTrue(round.has_value());
            if (!round) {
                ZHLN::Test::ExpectTrue(false);
                return {};
            }
            ZHLN::Test::ExpectEq(round.value().tick, world.tick);
            ZHLN::Test::ExpectEq(round.value().players.size(), world.players.size());
            for (size_t i = 0; i < world.players.size(); ++i) {
                ZHLN::Test::ExpectTrue(round.value().players[i] == world.players[i]);
            }
            return {};
        }

        std::expected<void, ZHLN::Error> skip_annotation_excludes_field() {
            TestPlayer player = MakePlayer(9);
            player.localCache = 111;
            auto encodedA     = ZHLN::Wire::Encode(player);
            ZHLN::Test::ExpectTrue(encodedA.has_value());

            player.localCache = 222;
            auto encodedB     = ZHLN::Wire::Encode(player);
            ZHLN::Test::ExpectTrue(encodedB.has_value());

            // The skipped field must not influence the bytes.
            ZHLN::Test::ExpectEq(*encodedA, *encodedB);

            // And decoding leaves the member at its default.
            TestPlayer decoded {};
            decoded.localCache = -1;
            ZHLN::Wire::Reader reader(*encodedA);
            ZHLN::Test::ExpectTrue(reader.Get(decoded).has_value());
            ZHLN::Test::ExpectEq(decoded.localCache, -1);
            ZHLN::Test::ExpectEq(decoded.uid, player.uid);
            return {};
        }

        std::expected<void, ZHLN::Error> range_annotation_is_enforced_on_decode() {
            TestPlayer player  = MakePlayer(1);
            player.hp          = 150; // outside [0, 100]
            auto       encoded = ZHLN::Wire::Encode(player);
            ZHLN::Test::ExpectTrue(encoded.has_value());

            auto decoded = ZHLN::Wire::Decode<TestPlayer>(*encoded);
            ZHLN::Test::ExpectTrue(Is(decoded, WireError::ValueOutOfRange));

            const auto text = decoded.error().Format();
            ZHLN::Test::ExpectTrue(Contains(text, "150"));
            ZHLN::Test::ExpectTrue(Contains(text, "[0, 100]"));
            ZHLN::Test::ExpectTrue(Contains(text, "hp"));
            return {};
        }

        std::expected<void, ZHLN::Error> failures_carry_path_and_annotation_note() {
            TestWorld world     = MakeWorld();
            world.players[2].hp = 200;
            auto       encoded  = ZHLN::Wire::Encode(world);
            ZHLN::Test::ExpectTrue(encoded.has_value());

            auto decoded = ZHLN::Wire::Decode<TestWorld>(*encoded);
            ZHLN::Test::ExpectTrue(Is(decoded, WireError::ValueOutOfRange));

            const auto& failure = decoded.error();
            ZHLN::Test::ExpectTrue(Contains(failure.path, "players[2].hp"));
            ZHLN::Test::ExpectTrue(Contains(failure.note, "TestPlayer.hp"));
            ZHLN::Test::ExpectTrue(Contains(failure.note, "hitpoints remaining"));

            const auto formatted = failure.Format();
            ZHLN::Test::ExpectTrue(Contains(formatted, "ZHLN.Wire:"));
            ZHLN::Test::ExpectTrue(Contains(formatted, "[at players[2].hp @ byte"));
            ZHLN::Test::ExpectTrue(Contains(formatted, "(TestPlayer.hp: hitpoints remaining)"));
            return {};
        }

        std::expected<void, ZHLN::Error> schema_version_annotation() {
            ZHLN::Test::ExpectEq(ZHLN::Wire::SchemaVersionOf<ZHLN::Net::InitialSnapshotMessage>(), uint32_t {2});
            ZHLN::Test::ExpectEq(ZHLN::Wire::SchemaVersionOf<ZHLN::Net::PhysicsBatchMessage>(), uint32_t {2});
            ZHLN::Test::ExpectEq(ZHLN::Wire::SchemaVersionOf<TestWorld>(), uint32_t {1});
            return {};
        }

        // ------------------------------------------------------------------
        // Hostile input: truncation and corruption sweeps
        // ------------------------------------------------------------------
        std::expected<void, ZHLN::Error> truncation_sweep_always_fails() {
            const TestWorld world   = MakeWorld();
            auto            encoded = ZHLN::Wire::Encode(world);
            ZHLN::Test::ExpectTrue(encoded.has_value());

            bool allFailed = true;
            for (size_t cut = 0; cut < encoded->size(); ++cut) {
                auto decoded = ZHLN::Wire::Decode<TestWorld>(std::span<const uint8_t>(*encoded).first(cut));
                if (decoded.has_value()) {
                    allFailed = false;
                    ZHLN::Test::ExpectTrue(false); // a truncated stream must never decode
                }
            }
            ZHLN::Test::ExpectTrue(allFailed);
            return {};
        }

        std::expected<void, ZHLN::Error> corruption_sweep_never_escapes() {
            const TestWorld world   = MakeWorld();
            auto            encoded = ZHLN::Wire::Encode(world);
            ZHLN::Test::ExpectTrue(encoded.has_value());
            ZHLN::Test::ExpectTrue(encoded->size() > 8);

            Rng rng;
            for (size_t index = 0; index < encoded->size(); ++index) {
                for (const uint8_t mutation: {uint8_t {0xFF}, uint8_t {0x00}, rng.NextByte()}) {
                    std::vector<uint8_t> corrupted = *encoded;
                    corrupted[index]               = mutation;
                    // Must terminate with a verdict either way — never crash,
                    // never read out of bounds (the point of this test under ASan).
                    auto decoded = ZHLN::Wire::Decode<TestWorld>(corrupted);
                    (void) decoded.has_value();
                }
            }
            return {};
        }

        std::expected<void, ZHLN::Error> writer_limit_is_enforced() {
            ZHLN::Wire::Writer writer(4);
            auto               result = writer.PutBytes(Bytes({1, 2, 3, 4, 5, 6}));
            ZHLN::Test::ExpectTrue(Is(result, WireError::BufferOverflow));
            ZHLN::Test::ExpectEq(writer.Size(), size_t {0});
            return {};
        }

        // ------------------------------------------------------------------
        // CRC32
        // ------------------------------------------------------------------
        std::expected<void, ZHLN::Error> crc32_known_vectors() {
            const std::vector<uint8_t> empty;
            ZHLN::Test::ExpectEq(ZHLN::Wire::Checksum::Crc32(empty), uint32_t {0x00000000});

            const std::string_view payload = "123456789";
            const auto*            raw     = reinterpret_cast<const uint8_t*>(payload.data());
            ZHLN::Test::ExpectEq(ZHLN::Wire::Checksum::Crc32({raw, payload.size()}), uint32_t {0xCBF43926});
            return {};
        }

        // ------------------------------------------------------------------
        // Block compression
        // ------------------------------------------------------------------
        std::expected<void, ZHLN::Error> compression_roundtrip() {
            Rng rng;
            const std::array sizes = {size_t {0}, size_t {1}, size_t {2}, size_t {7}, size_t {64}, size_t {1000}, size_t {4096}, size_t {65536}, size_t {100000}};

            for (const size_t size: sizes) {
                std::vector<uint8_t> zeros(size, 0x00);
                std::vector<uint8_t> random(size, 0x00);
                std::vector<uint8_t> text(size, 0x00);
                std::vector<uint8_t> ramp(size, 0x00);
                for (size_t i = 0; i < size; ++i) {
                    random[i] = rng.NextByte();
                    text[i]   = static_cast<uint8_t>("the quick brown fox jumps over the lazy dog "[i % 44]);
                    ramp[i]   = static_cast<uint8_t>(i * 7);
                }

                for (const auto& input: {zeros, random, text, ramp}) {
                    auto compressed = ZHLN::Wire::Compression::Compress(input);
                    ZHLN::Test::ExpectTrue(compressed.has_value());
                    if (!compressed) {
                        continue;
                    }
                    ZHLN::Test::ExpectTrue(compressed->size() <= ZHLN::Wire::Compression::CompressBound(input.size()));

                    auto restored = ZHLN::Wire::Compression::Decompress(*compressed, input.size());
                    ZHLN::Test::ExpectTrue(restored.has_value());
                    if (restored) {
                        ZHLN::Test::ExpectEq(*restored, input);
                    }
                }
            }

            // Highly compressible data must actually compress.
            const std::vector<uint8_t> zeros(64 * 1024, 0x00);
            auto                       compressed = ZHLN::Wire::Compression::Compress(zeros);
            ZHLN::Test::ExpectTrue(compressed.has_value());
            if (compressed) {
                ZHLN::Test::ExpectTrue(compressed->size() < 1024);
            }
            return {};
        }

        std::expected<void, ZHLN::Error> decompression_rejects_hostile_streams() {
            // Match offset 0 with nothing produced: invalid.
            {
                auto result = ZHLN::Wire::Compression::Decompress(Span({0x00, 0x00, 0x00}), 64);
                ZHLN::Test::ExpectTrue(Is(result, WireError::DecompressionFailed));
            }
            // Offset larger than the bytes produced so far.
            {
                auto result = ZHLN::Wire::Compression::Decompress(Span({0x10, 'A', 0x05, 0x00}), 64);
                ZHLN::Test::ExpectTrue(Is(result, WireError::DecompressionFailed));
                ZHLN::Test::ExpectTrue(Contains(result.error().Format(), "out of range"));
            }
            // Truncated length extension.
            {
                auto result = ZHLN::Wire::Compression::Decompress(Span({0xF0, 0xFF}), 64);
                ZHLN::Test::ExpectTrue(Is(result, WireError::DecompressionFailed));
            }
            // Output cap is enforced while expanding (zip-bomb guard).
            {
                const std::vector<uint8_t> zeros(64 * 1024, 0x00);
                auto                       compressed = ZHLN::Wire::Compression::Compress(zeros);
                ZHLN::Test::ExpectTrue(compressed.has_value());
                if (compressed) {
                    auto result = ZHLN::Wire::Compression::Decompress(*compressed, 4096);
                    ZHLN::Test::ExpectTrue(Is(result, WireError::DecompressionFailed));
                    ZHLN::Test::ExpectTrue(Contains(result.error().Format(), "exceed the 4096 byte limit"));
                }
            }
            // Size mismatch against the announced raw length.
            {
                const std::vector<uint8_t> zeros(2048, 0x00);
                auto                       compressed = ZHLN::Wire::Compression::Compress(zeros);
                ZHLN::Test::ExpectTrue(compressed.has_value());
                if (compressed) {
                    auto result = ZHLN::Wire::Compression::Decompress(*compressed, 2047);
                    ZHLN::Test::ExpectTrue(Is(result, WireError::DecompressionFailed));
                }
            }
            return {};
        }

        // ------------------------------------------------------------------
        // Framing
        // ------------------------------------------------------------------
        std::expected<void, ZHLN::Error> frame_roundtrip() {
            Rng rng;
            // Small payload: stays raw.
            const std::vector<uint8_t> small = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
            // Large compressible payload: takes the compressed path.
            std::vector<uint8_t> repetitive(4096, 0);
            for (size_t i = 0; i < repetitive.size(); ++i) {
                repetitive[i] = static_cast<uint8_t>(i % 3);
            }
            // Large incompressible payload: stays raw even though it passes the
            // size threshold.
            std::vector<uint8_t> random(4096, 0);
            for (auto& byte: random) {
                byte = rng.NextByte();
            }

            for (const auto& payload: {small, repetitive, random}) {
                auto frame = ZHLN::Net::EncodeFrame(payload);
                ZHLN::Test::ExpectTrue(frame.has_value());
                if (!frame) {
                    continue;
                }
                auto decoded = ZHLN::Net::DecodeFrame(*frame);
                ZHLN::Test::ExpectTrue(decoded.has_value());
                if (decoded) {
                    ZHLN::Test::ExpectEq(*decoded, payload);
                }
            }

            // The repetitive payload must actually be compressed inside the frame.
            auto framed = ZHLN::Net::EncodeFrame(repetitive);
            ZHLN::Test::ExpectTrue(framed.has_value());
            if (framed) {
                ZHLN::Test::ExpectTrue(framed->size() < repetitive.size());
            }

            // Empty payload round-trips.
            auto emptyFrame = ZHLN::Net::EncodeFrame({});
            ZHLN::Test::ExpectTrue(emptyFrame.has_value());
            if (emptyFrame) {
                auto decoded = ZHLN::Net::DecodeFrame(*emptyFrame);
                ZHLN::Test::ExpectTrue(decoded.has_value());
                if (decoded) {
                    ZHLN::Test::ExpectTrue(decoded->empty());
                }
            }

            // Datagram form round-trips.
            auto datagram = ZHLN::Net::EncodeDatagram(small);
            ZHLN::Test::ExpectTrue(datagram.has_value());
            if (datagram) {
                auto decoded = ZHLN::Net::DecodeDatagram(*datagram);
                ZHLN::Test::ExpectTrue(decoded.has_value());
                if (decoded) {
                    ZHLN::Test::ExpectEq(*decoded, small);
                }
            }
            return {};
        }

        std::expected<void, ZHLN::Error> frame_corruption_is_detected() {
            const std::vector<uint8_t> payload(64, 0xAB);
            auto                       frame = ZHLN::Net::EncodeFrame(payload);
            ZHLN::Test::ExpectTrue(frame.has_value());
            if (!frame) {
                return {};
            }

            // Flip one payload byte: CRC mismatch.
            {
                std::vector<uint8_t> corrupted = *frame;
                corrupted[6]                   = corrupted[6] ^ 0xFF;
                auto result = ZHLN::Net::DecodeFrame(corrupted);
                ZHLN::Test::ExpectTrue(Is(result, WireError::ChecksumMismatch));
                ZHLN::Test::ExpectTrue(Contains(result.error().Format(), "CRC32 mismatch"));
            }
            // Truncated frame.
            {
                auto truncated = ZHLN::Net::DecodeFrame(std::span<const uint8_t>(*frame).first(frame->size() - 1));
                ZHLN::Test::ExpectTrue(!truncated.has_value());
            }
            // Length header longer than the buffer.
            {
                std::vector<uint8_t> stretched = *frame;
                stretched[0]                   = 0x7F; // huge length
                auto result = ZHLN::Net::DecodeFrame(stretched);
                ZHLN::Test::ExpectTrue(!result.has_value());
            }
            // Length prefix shorter than the minimum frame body.
            {
                auto result = ZHLN::Net::PeekFrameLength(Span({0x00, 0x00, 0x00, 0x02}));
                ZHLN::Test::ExpectTrue(!result.has_value());
            }
            return {};
        }

        // ------------------------------------------------------------------
        // Message envelope + typed messages
        // ------------------------------------------------------------------
        std::expected<void, ZHLN::Error> envelope_roundtrip_and_validation() {
            const std::vector<uint8_t> payload = {0x01, 0x02, 0x03};
            auto                       message = ZHLN::Net::EncodeEnvelope(ZHLN::Net::MessageType::ClientInput, payload);
            ZHLN::Test::ExpectTrue(message.has_value());
            if (message) {
                auto decoded = ZHLN::Net::DecodeEnvelope(*message);
                ZHLN::Test::ExpectTrue(decoded.has_value());
                if (decoded) {
                    ZHLN::Test::ExpectEq(decoded->type, ZHLN::Net::MessageType::ClientInput);
                    ZHLN::Test::ExpectEq(decoded->payload, payload);
                }
            }

            // Bad magic.
            {
                auto result = ZHLN::Net::DecodeEnvelope(Span({'X', 'W', 2, 1}));
                ZHLN::Test::ExpectTrue(Is(result, WireError::InvalidFrame));
            }
            // Wrong version.
            {
                auto result = ZHLN::Net::DecodeEnvelope(Span({'Z', 'W', 99, 1}));
                ZHLN::Test::ExpectTrue(Is(result, WireError::ProtocolVersionMismatch));
            }
            // Unknown message type.
            {
                auto result = ZHLN::Net::DecodeEnvelope(Span({'Z', 'W', 2, 99}));
                ZHLN::Test::ExpectTrue(Is(result, WireError::UnknownMessageType));
            }
            return {};
        }

        std::expected<void, ZHLN::Error> client_hello_and_welcome_roundtrip() {
            const ZHLN::Net::ClientHello hello {.protocolVersion = 2, .userId = 42, .token = "s3cret-token"};
            auto                         encoded = ZHLN::Net::EncodeClientHello(hello);
            ZHLN::Test::ExpectTrue(encoded.has_value());
            if (encoded) {
                auto decoded = ZHLN::Net::DecodeClientHello(*encoded);
                ZHLN::Test::ExpectTrue(decoded.has_value());
                if (decoded) {
                    ZHLN::Test::ExpectEq(decoded->protocolVersion, uint32_t {2});
                    ZHLN::Test::ExpectEq(decoded->userId, uint64_t {42});
                    ZHLN::Test::ExpectEq(decoded->token, std::string("s3cret-token"));
                }
            }

            const ZHLN::Net::ServerWelcome welcome {.serverTick = 500, .realtimePort = 5556, .tickRateHz = 60};
            auto                          welcomeBytes = ZHLN::Net::EncodeServerWelcome(welcome);
            ZHLN::Test::ExpectTrue(welcomeBytes.has_value());
            if (welcomeBytes) {
                auto decoded = ZHLN::Net::DecodeServerWelcome(*welcomeBytes);
                ZHLN::Test::ExpectTrue(decoded.has_value());
                if (decoded) {
                    ZHLN::Test::ExpectEq(decoded->serverTick, uint32_t {500});
                    ZHLN::Test::ExpectEq(decoded->realtimePort, uint16_t {5556});
                    ZHLN::Test::ExpectEq(decoded->tickRateHz, uint8_t {60});
                }
            }

            // tickRateHz is Range<1, 240>-annotated: 0 must be rejected.
            {
                ZHLN::Wire::Writer writer;
                ZHLN::Test::ExpectTrue(writer.Put(uint32_t {1}).has_value());
                ZHLN::Test::ExpectTrue(writer.Put(uint16_t {5556}).has_value());
                ZHLN::Test::ExpectTrue(writer.Put(uint8_t {0}).has_value());
                auto envelope = ZHLN::Net::EncodeEnvelope(ZHLN::Net::MessageType::ServerWelcome, writer.Bytes());
                ZHLN::Test::ExpectTrue(envelope.has_value());
                auto decoded = ZHLN::Net::DecodeServerWelcome(*envelope);
                ZHLN::Test::ExpectTrue(Is(decoded, WireError::ValueOutOfRange));
                ZHLN::Test::ExpectTrue(Contains(decoded.error().path, "tickRateHz"));
                ZHLN::Test::ExpectTrue(Contains(decoded.error().Format(), "ServerWelcome.tickRateHz"));
            }
            return {};
        }

        std::expected<void, ZHLN::Error> client_input_roundtrip_and_ranges() {
            const ZHLN::Net::ClientInputMessage input {.userId = 7, .sequence = 99, .moveFlags = 1 | 8, .yaw = -42.5f};
            auto                                encoded = ZHLN::Net::EncodeClientInput(input);
            ZHLN::Test::ExpectTrue(encoded.has_value());
            if (encoded) {
                auto decoded = ZHLN::Net::DecodeClientInput(*encoded);
                ZHLN::Test::ExpectTrue(decoded.has_value());
                if (decoded) {
                    ZHLN::Test::ExpectEq(decoded->userId, uint64_t {7});
                    ZHLN::Test::ExpectEq(decoded->sequence, uint32_t {99});
                    ZHLN::Test::ExpectEq(decoded->moveFlags, uint8_t {9});
                    ZHLN::Test::ExpectEq(decoded->yaw, -42.5f);
                }
            }

            // moveFlags is Range<0, 31>: 99 must be rejected with the annotated note.
            {
                ZHLN::Net::ClientInputMessage bad = input;
                bad.moveFlags                     = 99;
                auto bytes                       = ZHLN::Net::EncodeClientInput(bad);
                ZHLN::Test::ExpectTrue(bytes.has_value());
                auto decoded = ZHLN::Net::DecodeClientInput(*bytes);
                ZHLN::Test::ExpectTrue(Is(decoded, WireError::ValueOutOfRange));
                ZHLN::Test::ExpectTrue(Contains(decoded.error().Format(), "ClientInputMessage.moveFlags"));
                ZHLN::Test::ExpectTrue(Contains(decoded.error().Format(), "Movement bitfield"));
            }

            // yaw is Range<-1000, 1000>: 4000 must be rejected.
            {
                ZHLN::Net::ClientInputMessage bad = input;
                bad.yaw                           = 4000.0f;
                auto bytes                       = ZHLN::Net::EncodeClientInput(bad);
                ZHLN::Test::ExpectTrue(bytes.has_value());
                auto decoded = ZHLN::Net::DecodeClientInput(*bytes);
                ZHLN::Test::ExpectTrue(Is(decoded, WireError::ValueOutOfRange));
                ZHLN::Test::ExpectTrue(Contains(decoded.error().path, "yaw"));
            }
            return {};
        }

        std::expected<void, ZHLN::Error> snapshot_and_physics_roundtrip_with_quantization() {
            ZHLN::Net::InitialSnapshotMessage snapshot;
            snapshot.serverTick = 1234;
            snapshot.objects    = {
                {1, JPH::Vec3(-100.5f, 0.0f, 88.25f), JPH::Vec3(2.0f, 3.0f, 4.0f)},
                {2, JPH::Vec3(0.0f, 0.0f, 0.0f), JPH::Vec3(1.0f, 1.0f, 1.0f)}
            };
            auto encoded = ZHLN::Net::EncodeInitialSnapshot(snapshot);
            ZHLN::Test::ExpectTrue(encoded.has_value());
            if (encoded) {
                auto decoded = ZHLN::Net::DecodeInitialSnapshot(*encoded);
                ZHLN::Test::ExpectTrue(decoded.has_value());
                if (decoded) {
                    ZHLN::Test::ExpectEq(decoded->serverTick, uint32_t {1234});
                    ZHLN::Test::ExpectEq(decoded->objects.size(), size_t {2});
                    for (size_t i = 0; i < decoded->objects.size(); ++i) {
                        const auto& a = decoded->objects[i];
                        const auto& b = snapshot.objects[i];
                        ZHLN::Test::ExpectEq(a.uid, b.uid);
                        ZHLN::Test::ExpectTrue(std::abs(a.position.GetX() - b.position.GetX()) < 1.0f / 128.0f);
                        ZHLN::Test::ExpectTrue(std::abs(a.position.GetY() - b.position.GetY()) < 1.0f / 128.0f);
                        ZHLN::Test::ExpectTrue(std::abs(a.position.GetZ() - b.position.GetZ()) < 1.0f / 128.0f);
                        ZHLN::Test::ExpectTrue(std::abs(a.size.GetX() - b.size.GetX()) < 1.0f / 128.0f);
                    }
                }
            }

            ZHLN::Net::PhysicsBatchMessage batch;
            batch.serverTick = 4321;
            batch.bodies     = {
                {10, JPH::Vec3(5.5f, -6.25f, 7.75f), JPH::Quat(0.5f, 0.5f, 0.5f, 0.5f), JPH::Vec3(1.0f, -2.0f, 3.0f)}
            };
            auto physicsBytes = ZHLN::Net::EncodePhysicsBatch(batch);
            ZHLN::Test::ExpectTrue(physicsBytes.has_value());
            if (physicsBytes) {
                auto decoded = ZHLN::Net::DecodePhysicsBatch(*physicsBytes);
                ZHLN::Test::ExpectTrue(decoded.has_value());
                if (decoded) {
                    ZHLN::Test::ExpectEq(decoded->bodies.size(), size_t {1});
                    const auto& body = decoded->bodies[0];
                    ZHLN::Test::ExpectEq(body.uid, uint64_t {10});
                    ZHLN::Test::ExpectTrue(std::abs(body.position.GetX() - 5.5f) < 1.0f / 128.0f);
                    ZHLN::Test::ExpectTrue(std::abs(body.velocity.GetZ() - 3.0f) < 1.0f / 128.0f);
                    ZHLN::Test::ExpectTrue(std::abs(body.rotation.Length() - 1.0f) < 0.001f);
                }
            }
            return {};
        }

        std::expected<void, ZHLN::Error> quaternion_sign_canonicalization() {
            ZHLN::Net::PhysicsBatchMessage positive;
            positive.bodies = {{1, JPH::Vec3(0, 0, 0), JPH::Quat(0.1f, 0.2f, 0.3f, 0.9f), JPH::Vec3(0, 0, 0)}};

            ZHLN::Net::PhysicsBatchMessage negative = positive;
            negative.bodies[0].rotation            = JPH::Quat(-0.1f, -0.2f, -0.3f, -0.9f);

            auto a = ZHLN::Net::EncodePhysicsBatch(positive);
            auto b = ZHLN::Net::EncodePhysicsBatch(negative);
            ZHLN::Test::ExpectTrue(a.has_value());
            ZHLN::Test::ExpectTrue(b.has_value());
            if (a && b) {
                // +q and -q are the same orientation: identical bytes.
                ZHLN::Test::ExpectEq(*a, *b);
            }
            return {};
        }

        std::expected<void, ZHLN::Error> nonfinite_positions_are_rejected() {
            ZHLN::Net::InitialSnapshotMessage snapshot;
            snapshot.objects = {{1, JPH::Vec3(std::numeric_limits<float>::infinity(), 0.0f, 0.0f), JPH::Vec3(1, 1, 1)}};
            auto result = ZHLN::Net::EncodeInitialSnapshot(snapshot);
            ZHLN::Test::ExpectTrue(Is(result, WireError::ValueOutOfRange));
            ZHLN::Test::ExpectTrue(Contains(result.error().path, "position"));
            return {};
        }
    };
};

int main() {
    return ZHLN::Test::Runner::Run<TestNetworkWireSuite>();
}
