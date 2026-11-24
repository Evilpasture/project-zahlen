// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TestsFramework.hpp"
#include <Zahlen/COM/IErrorRegistry.hpp>
#include <Zahlen/Core/Description.hpp>
#include <Zahlen/Core/Hash.hpp>
#include <Zahlen/Core/Reflection/Utilities.hpp>
#include <Zahlen/Core/ErrorCode.hpp>
#include <expected>
#include <format>
#include <string>
#include <type_traits>

// ============================================================================
// Local Test Enums (Self-Contained)
// ============================================================================

enum class CodecError : uint8_t {
    CorruptedStream ZHLN_ANNOTATION(ZHLN::Description<"The input bitstream is corrupted or incomplete."> {}) = 1,
    UnsupportedVersion ZHLN_ANNOTATION(ZHLN::Description<"The bitstream header version is not supported."> {})
};

enum class NetworkError : uint8_t {
    HostUnreachable ZHLN_ANNOTATION(ZHLN::Description<"Remote host refused the connection or is offline."> {}) = 1,
    ConnectionReset
};

enum class IDError : uint8_t {
    GenerationMismatch ZHLN_ANNOTATION(ZHLN::Description<"Recycled ID failed generation check. Expected generation {}, got {}"> {}) = 1,
    SlotOutOfBounds ZHLN_ANNOTATION(ZHLN::Description<"Slot index {} exceeds maximum capacity of {}"> {}),
    EntityNull ZHLN_ANNOTATION(ZHLN::Description<"Entity ID is null or uninitialized."> {})
};

enum class BareError : uint8_t {
    Unnamed = 1
};

// ============================================================================
// The Carrier Is Sealed
// ============================================================================
//
// An error is a diagnostic, not a number. ErrorCode keeps its two words private
// and has no conversion to an integral type, so a cast to the ordinal is
// ill-formed rather than merely discouraged -- and `if (!code)` keeps working,
// because an explicit conversion function still feeds a direct-init whose target
// it matches exactly (bool), while bool -> int is a promotion and therefore not
// a way in. An ABI boundary that genuinely needs the enumerator spells As<E>(),
// and configure/check_error_ordinals.py keeps that the only way it happens.
static_assert(!std::is_constructible_v<int, ZHLN::ErrorCode>, "static_cast<int>(code) must not compile");
static_assert(!std::is_constructible_v<unsigned, ZHLN::ErrorCode>, "static_cast<unsigned>(code) must not compile");
static_assert(!std::is_convertible_v<ZHLN::ErrorCode, int>, "no implicit conversion to the ordinal");
static_assert(std::is_constructible_v<bool, ZHLN::ErrorCode>, "if (code) is the conversion that stays");
static_assert(std::is_constructible_v<ZHLN::ErrorCode, CodecError>, "the enum constructor is the way in");

// ============================================================================
// Test Suite Class
// ============================================================================

namespace {

namespace ScopedDomainA {
enum class Error : uint8_t {
    Failed ZHLN_ANNOTATION(ZHLN::Description<"Domain A failure."> {}) = 1,
};
} // namespace ScopedDomainA

namespace ScopedDomainB {
enum class Error : uint8_t {
    Failed ZHLN_ANNOTATION(ZHLN::Description<"Domain B failure."> {}) = 1,
};
} // namespace ScopedDomainB

static_assert(ZHLN::ErrorCode(ScopedDomainA::Error::Failed) != ZHLN::ErrorCode(ScopedDomainB::Error::Failed));
static_assert(ZHLN::ErrorCode(ScopedDomainA::Error::Failed) == ScopedDomainA::Error::Failed);
static_assert(!ZHLN::ErrorCode(ScopedDomainA::Error::Failed).get<ScopedDomainB::Error>());
static_assert(!(ZHLN::ErrorCode(ScopedDomainA::Error::Failed) == ScopedDomainB::Error::Failed));

// A registry with the test-local categories registered, as a caller would
// build before CreateEngine. Registration against an instance is explicit; what
// ErrorCode(E) does on its own is anchor E in the engine-owned *static* describe
// table (ErrorCode.hpp -> TemplatedDetail::AnchorEnumCategory), which is why
// std::format("{}", code) works with no registry in hand. The two tables are the
// same process-wide store -- see src/engine/ErrorRegistry.cpp.
auto MakeTestRegistry() -> ZHLN::COM::Ref<ZHLN::COM::IErrorRegistry> {
    auto registry = ZHLN::COM::CreateErrorRegistry();
    if (!registry) {
        return {};
    }
    (void) ZHLN::COM::RegisterEnum<CodecError>(**registry);
    (void) ZHLN::COM::RegisterEnum<NetworkError>(**registry);
    (void) ZHLN::COM::RegisterEnum<IDError>(**registry);
    (void) ZHLN::COM::RegisterEnum<BareError>(**registry);
    (void) ZHLN::COM::RegisterEnum<ScopedDomainA::Error>(**registry);
    (void) ZHLN::COM::RegisterEnum<ScopedDomainB::Error>(**registry);
    return std::move(*registry);
}

auto Describe(ZHLN::COM::IErrorRegistry& registry, const ZHLN::ErrorCode code) -> ZHLN::COM::ErrorDescription {
    ZHLN::COM::ErrorDescription desc;
    auto pack = ZHLN::COM::OutChain(desc);
    (void) registry.Describe(code, pack.Head());
    return desc;
}

struct ErrorTestSuite {
    struct Tests {
        // --- 1. Basic Error Type & State ---
        std::expected<void, ZHLN::ErrorCode> default_constructor_is_falsy() {
            ZHLN::ErrorCode err;
            if (err) {
                return std::unexpected(CodecError::CorruptedStream);
            }
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> active_error_is_truthy() {
            ZHLN::ErrorCode err {CodecError::CorruptedStream};
            if (!err) {
                return std::unexpected(CodecError::CorruptedStream);
            }
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> type_and_value_matching() {
            ZHLN::ErrorCode err {CodecError::CorruptedStream};

            if (!err.get<CodecError>()) {
                return std::unexpected(CodecError::CorruptedStream);
            }
            if (err.get<CodecError>() != CodecError::CorruptedStream) {
                return std::unexpected(CodecError::CorruptedStream);
            }
            if (err.get<NetworkError>()) {
                return std::unexpected(NetworkError::ConnectionReset);
            }
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> value_extraction() {
            ZHLN::ErrorCode err {CodecError::UnsupportedVersion};
            if (err.get<CodecError>() != CodecError::UnsupportedVersion) {
                return std::unexpected(CodecError::CorruptedStream);
            }
            return {};
        }

        // --- 2. Registry Category & Message Resolution ---
        std::expected<void, ZHLN::ErrorCode> category_and_message_resolution() {
            auto registry = MakeTestRegistry();
            if (!registry) {
                return std::unexpected(CodecError::CorruptedStream);
            }
            const auto codec = Describe(*registry, ZHLN::ErrorCode {CodecError::CorruptedStream});
            const auto net   = Describe(*registry, ZHLN::ErrorCode {NetworkError::HostUnreachable});

            ZHLN::Println("    [Description] {}: {}", std::string_view(codec.category), std::string_view(codec.message));
            ZHLN::Println("    [Description] {}: {}", std::string_view(net.category), std::string_view(net.message));

            if (std::string_view(codec.message) != "The input bitstream is corrupted or incomplete.") {
                return std::unexpected(CodecError::CorruptedStream);
            }
            if (std::string_view(net.message) != "Remote host refused the connection or is offline.") {
                return std::unexpected(CodecError::CorruptedStream);
            }
            return {};
        }

        // --- 3. Parameterized Description Formatting (Zero-Allocation) ---
        std::expected<void, ZHLN::ErrorCode> formatted_description_with_arguments() {
            auto msg1 = ZHLN::Reflect::FormatEnumMessage(IDError::GenerationMismatch, 2u, 1u);
            auto msg2 = ZHLN::Reflect::FormatEnumMessage(IDError::SlotOutOfBounds, 1050, 1024);

            ZHLN::Println("    [Formatted Message 1] {}", msg1);
            ZHLN::Println("    [Formatted Message 2] {}", msg2);

            ZHLN::Test::ExpectEq(msg1, "Recycled ID failed generation check. Expected generation 2, got 1");
            ZHLN::Test::ExpectEq(msg2, "Slot index 1050 exceeds maximum capacity of 1024");

            return {};
        }

        // --- 4. Description Formatting with Zero Arguments ---
        std::expected<void, ZHLN::ErrorCode> formatted_description_without_arguments() {
            auto msg = ZHLN::Reflect::FormatEnumMessage(IDError::EntityNull);

            ZHLN::Println("    [Formatted Message (Zero-Arg)] {}", msg);

            ZHLN::Test::ExpectEq(msg, "Entity ID is null or uninitialized.");
            return {};
        }

        // --- 5. Formatted std::string Generation ---
        std::expected<void, ZHLN::ErrorCode> formatted_description_to_std_string() {
            std::string str = ZHLN::Reflect::FormatEnumMessageString(IDError::GenerationMismatch, 10u, 4u);

            ZHLN::Println("    [Formatted std::string] {}", str);

            ZHLN::Test::ExpectEq(str, "Recycled ID failed generation check. Expected generation 10, got 4");
            return {};
        }

        // --- 6. The Code Is the Whole Carrier ---
        std::expected<void, ZHLN::ErrorCode> errorcode_carries_the_words() {
            // There is no second wrapper type to promote into: the code alone
            // carries category + value, and a registry resolves its text.
            auto registry = MakeTestRegistry();
            if (!registry) {
                return std::unexpected(CodecError::CorruptedStream);
            }
            const ZHLN::ErrorCode code {CodecError::CorruptedStream};
            if (code.get<CodecError>() != CodecError::CorruptedStream) {
                return std::unexpected(CodecError::CorruptedStream);
            }
            const auto desc = Describe(*registry, code);
            ZHLN::Println("    [Carrier] {}: {}", std::string_view(desc.category), std::string_view(desc.message));
            if (std::string_view(desc.message) != "The input bitstream is corrupted or incomplete.") {
                return std::unexpected(CodecError::CorruptedStream);
            }
            ZHLN::ErrorCode none;
            if (none) {
                return std::unexpected(CodecError::CorruptedStream);
            }
            return {};
        }

        // --- 7. Unknown Categories Degrade to "Unknown" ---
        // --- 7. An Unregistered Code Is a Miss, Not "Unknown" ---
        // Describe used to report success and fill every field with "Unknown",
        // which made the raw-code fallback in COM::DescribeError unreachable and
        // let a caller print a description that described nothing.
        std::expected<void, ZHLN::ErrorCode> unknown_category_is_reported_as_a_miss() {
            auto registry = MakeTestRegistry();
            if (!registry) {
                return std::unexpected(CodecError::CorruptedStream);
            }
            const ZHLN::ErrorCode foreign {0x5EED1E55u, 7u};
            if (!foreign) {
                return std::unexpected(CodecError::CorruptedStream);
            }
            if (foreign.get<CodecError>()) {
                return std::unexpected(CodecError::CorruptedStream);
            }
            ZHLN::COM::ErrorDescription desc;
            auto                           pack = ZHLN::COM::OutChain(desc);
            const auto                     described = registry->Describe(foreign, pack.Head());
            if (described.HasValue()) {
                return std::unexpected(CodecError::CorruptedStream);
            }
            if (described.error != ZHLN::ErrorCode(ZHLN::COM::Error::NotRegistered)) {
                return std::unexpected(CodecError::CorruptedStream);
            }
            // Nothing was written into the payload, so an ignored result cannot
            // masquerade as a description.
            if (!std::string_view(desc.category).empty() || !std::string_view(desc.name).empty()) {
                return std::unexpected(CodecError::CorruptedStream);
            }
            // ... and the one-shot rendering falls back to the raw words.
            const auto text = ZHLN::COM::DescribeError(*registry, foreign);
            if (text != std::format("error {:#010x}:{}", foreign.CategoryHash(), foreign.Value())) {
                return std::unexpected(CodecError::CorruptedStream);
            }
            return {};
        }

        // --- 7b. A Recorded Detail Is Copied Out of the Recorder ---
        // RecordDetail exists for the plugin that is about to be unloaded: the
        // detail, the file and the function all point into that plugin's memory
        // when the call returns, so the registry has to own what it keeps.
        std::expected<void, ZHLN::ErrorCode> recorded_detail_outlives_its_source_buffers() {
            auto registry = MakeTestRegistry();
            if (!registry) {
                return std::unexpected(CodecError::CorruptedStream);
            }
            // LastError before anything was recorded is a miss, not an empty record.
            ZHLN::COM::ErrorRecord empty {};
            auto                    emptyPack = ZHLN::COM::OutChain(empty);
            if (registry->LastError(emptyPack.Head()).HasValue()) {
                return std::unexpected(CodecError::CorruptedStream);
            }

            std::string file     = "plugin/Transient.cpp";
            std::string function = "Transient::Evaluate()";
            std::string channel  = "Script";
            std::string detail   = "part 3: activeMorphCount=6 > 4";
            ZHLN::COM::LogSource source;
            source.file     = ZHLN::COM::StringView(file);
            source.function = ZHLN::COM::StringView(function);
            source.channel  = ZHLN::COM::StringView(channel);
            source.line     = 42;
            registry->RecordDetail(ZHLN::ErrorCode(CodecError::CorruptedStream), source, ZHLN::COM::StringView(detail));

            // Simulate dlclose: every buffer the call borrowed is gone (and, for
            // good measure, overwritten with something else).
            file.assign(file.size(), 'X');
            function.assign(function.size(), 'Y');
            channel.assign(channel.size(), 'W');
            detail.assign(detail.size(), 'Z');
            file.clear();
            function.clear();
            channel.clear();
            detail.clear();

            ZHLN::COM::ErrorRecord record {};
            auto                    pack = ZHLN::COM::OutChain(record);
            const auto              read = registry->LastError(pack.Head());
            if (!read.HasValue()) {
                return std::unexpected(CodecError::CorruptedStream);
            }
            if (record.code != ZHLN::ErrorCode(CodecError::CorruptedStream)) {
                return std::unexpected(CodecError::CorruptedStream);
            }
            if (std::string_view(record.detail) != "part 3: activeMorphCount=6 > 4") {
                return std::unexpected(CodecError::CorruptedStream);
            }
            // The record stores the base name: String64 cannot hold an absolute
            // build path, and truncating one cut the filename off the end.
            if (std::string_view(record.file) != "Transient.cpp" || std::string_view(record.function) != "Transient::Evaluate()") {
                return std::unexpected(CodecError::CorruptedStream);
            }
            // The channel is the fourth thing the record owns: it was a view into
            // a plugin's .rodata, and that plugin has been unloaded above.
            if (record.line != 42u || std::string_view(record.channel) != "Script") {
                return std::unexpected(CodecError::CorruptedStream);
            }
            return {};
        }

        // --- 7c. A New Registry Can Be Seeded From The Caller's ---
        std::expected<void, ZHLN::ErrorCode> create_error_registry_seeds_from_the_caller() {
            auto first = MakeTestRegistry();
            if (!first) {
                return std::unexpected(CodecError::CorruptedStream);
            }
            ZHLN::COM::LogSource source;
            source.file = ZHLN::COM::StringView("boot/Startup.cpp");
            first->RecordDetail(ZHLN::ErrorCode(NetworkError::HostUnreachable), source, ZHLN::COM::StringView("connect timed out"));

            // ErrorConfig names the registry to seed from; the engine factory
            // passes exactly this when a caller rebuilds after a failure.
            auto seeded = ZHLN::COM::CreateErrorRegistry(ZHLN::COM::InChain(
                ZHLN::COM::ErrorConfig {.registry = first.Get()}
            ));
            if (!seeded) {
                return std::unexpected(CodecError::CorruptedStream);
            }
            ZHLN::COM::ErrorRecord record {};
            auto                    pack = ZHLN::COM::OutChain(record);
            if (!(*seeded)->LastError(pack.Head()).HasValue()) {
                return std::unexpected(CodecError::CorruptedStream);
            }
            if (record.code != ZHLN::ErrorCode(NetworkError::HostUnreachable) || std::string_view(record.detail) != "connect timed out") {
                return std::unexpected(CodecError::CorruptedStream);
            }
            // The seeded registry is a fresh object, not the one that was passed in.
            if ((*seeded).Get() == first.Get()) {
                return std::unexpected(CodecError::CorruptedStream);
            }
            return {};
        }

        // --- 8. Hash Collisions Fail Loudly; Same-Name Re-Registration Replaces ---
        std::expected<void, ZHLN::ErrorCode> register_category_collision_is_rejected() {
            auto registry = MakeTestRegistry();
            if (!registry) {
                return std::unexpected(CodecError::CorruptedStream);
            }
            const uint32_t hash = ZHLN::Hash32("tests::Foreign");
            const std::array<ZHLN::COM::ErrorEnumEntry, 1> entries {{
                {.value = 3, .name = "ForeignFailure", .message = "foreign failure"},
            }};
            if (!registry->RegisterCategory(hash, "TestForeign", entries).HasValue()) {
                return std::unexpected(CodecError::CorruptedStream);
            }
            // Same hash, same name: allowed (idempotent / plugin reload).
            if (!registry->RegisterCategory(hash, "TestForeign", entries).HasValue()) {
                return std::unexpected(CodecError::CorruptedStream);
            }
            // Same hash, different name: a collision, rejected -- never the
            // old global registry's silent first-wins drop.
            if (registry->RegisterCategory(hash, "TestForeignImpostor", entries).HasValue()) {
                return std::unexpected(CodecError::CorruptedStream);
            }
            const auto desc = Describe(*registry, ZHLN::ErrorCode {hash, 3u});
            if (std::string_view(desc.category) != "TestForeign" || std::string_view(desc.message) != "foreign failure" ||
                std::string_view(desc.name) != "ForeignFailure") {
                return std::unexpected(CodecError::CorruptedStream);
            }
            // Unregister removes it again (plugin unload cleanup).
            if (!registry->UnregisterCategory(hash)) {
                return std::unexpected(CodecError::CorruptedStream);
            }
            // After UnregisterCategory the code is a miss again: the payload is
            // left untouched rather than stamped "Unknown".
            ZHLN::COM::ErrorDescription afterUnregister;
            auto                        afterPack = ZHLN::COM::OutChain(afterUnregister);
            if (registry->Describe(ZHLN::ErrorCode {hash, 3u}, afterPack.Head()).HasValue()) {
                return std::unexpected(CodecError::CorruptedStream);
            }
            if (!std::string_view(afterUnregister.category).empty()) {
                return std::unexpected(CodecError::CorruptedStream);
            }
            return {};
        }

        // --- 9. Unannotated Enumerators Fall Back to Their Name ---
        std::expected<void, ZHLN::ErrorCode> message_falls_back_to_enumerator_name() {
            auto registry = MakeTestRegistry();
            if (!registry) {
                return std::unexpected(CodecError::CorruptedStream);
            }
            const auto desc = Describe(*registry, ZHLN::ErrorCode {BareError::Unnamed});
            if (std::string_view(desc.name) != "Unnamed" || std::string_view(desc.message) != "Unnamed") {
                return std::unexpected(CodecError::CorruptedStream);
            }
            return {};
        }

        // --- 10. Formatting an ErrorCode Prints Its Words ---
        // Statically registered enums describe themselves at compile time:
        // RegisterEnum anchors the category in the static describe table, so
        // the formatter prints "Category::Name: message" with no registry
        // instance in hand. Only never-anchored categories (dynamic plugins
        // that skipped registration) fall back to the raw two words.
        std::expected<void, ZHLN::ErrorCode> errorcode_formats_as_its_words() {
            auto registry = MakeTestRegistry();
            if (!registry) {
                return std::unexpected(CodecError::CorruptedStream);
            }
            const ZHLN::ErrorCode code {CodecError::CorruptedStream};
            if (std::format("{}", code) != "CodecError::CorruptedStream: The input bitstream is corrupted or incomplete.") {
                return std::unexpected(CodecError::CorruptedStream);
            }
            return {};
        }

        // --- 11. Identically Named Enums in Distinct Namespaces Stay Distinct ---
        std::expected<void, ZHLN::ErrorCode> scoped_error_enums_do_not_collide() {
            auto registry = MakeTestRegistry();
            if (!registry) {
                return std::unexpected(CodecError::CorruptedStream);
            }
            const auto a = Describe(*registry, ZHLN::ErrorCode {ScopedDomainA::Error::Failed});
            const auto b = Describe(*registry, ZHLN::ErrorCode {ScopedDomainB::Error::Failed});

            if (std::string_view(a.category) == std::string_view(b.category)) {
                return std::unexpected(CodecError::CorruptedStream);
            }
            if (std::string_view(a.message) != "Domain A failure." || std::string_view(b.message) != "Domain B failure.") {
                return std::unexpected(CodecError::CorruptedStream);
            }
            return {};
        }
    };
};

} // namespace
