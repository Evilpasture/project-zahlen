// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TestsFramework.hpp"
#include <Zahlen/Core/Description.hpp>
#include <Zahlen/Core/Reflection/Utilities.hpp>
#include <Zahlen/ErrorCode.hpp>
#include <expected>
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

enum class HandleError : uint8_t {
    GenerationMismatch ZHLN_ANNOTATION(ZHLN::Description<"Recycled handle failed generation check. Expected generation {}, got {}"> {}) = 1,
    SlotOutOfBounds ZHLN_ANNOTATION(ZHLN::Description<"Slot index {} exceeds maximum capacity of {}"> {}),
    EntityNull ZHLN_ANNOTATION(ZHLN::Description<"Entity handle is null or uninitialized."> {})
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

struct ErrorTestSuite {
    struct Tests {
        // --- 1. Basic Error Type & State ---
        std::expected<void, ZHLN::ErrorCode> default_constructor_is_falsy() {
            ZHLN::Error err;
            if (err) {
                return std::unexpected(CodecError::CorruptedStream);
            }
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> active_error_is_truthy() {
            ZHLN::Error err {CodecError::CorruptedStream};
            if (!err) {
                return std::unexpected(CodecError::CorruptedStream);
            }
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> type_and_value_matching() {
            ZHLN::Error err {CodecError::CorruptedStream};

            if (!err.Is<CodecError>()) {
                return std::unexpected(CodecError::CorruptedStream);
            }
            if (!err.Is(CodecError::CorruptedStream)) {
                return std::unexpected(CodecError::CorruptedStream);
            }
            if (err.Is<NetworkError>()) {
                return std::unexpected(NetworkError::ConnectionReset);
            }
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> value_extraction() {
            ZHLN::Error err {CodecError::UnsupportedVersion};
            if (err.As<CodecError>() != CodecError::UnsupportedVersion) {
                return std::unexpected(CodecError::CorruptedStream);
            }
            return {};
        }

        // --- 2. Static Reflection Category & Message Resolution ---
        std::expected<void, ZHLN::ErrorCode> category_and_message_resolution() {
            ZHLN::Error err {CodecError::CorruptedStream};
            ZHLN::Error netErr {NetworkError::HostUnreachable};

            ZHLN::Println("    [Static Description] {}: {}", err.Category(), err.Message());
            ZHLN::Println("    [Static Description] {}: {}", netErr.Category(), netErr.Message());

            if (err.Category().empty() || err.Message().empty()) {
                return std::unexpected(CodecError::CorruptedStream);
            }
            return {};
        }

        // --- 3. Parameterized Description Formatting (Zero-Allocation) ---
        std::expected<void, ZHLN::ErrorCode> formatted_description_with_arguments() {
            auto msg1 = ZHLN::Reflect::FormatEnumMessage(HandleError::GenerationMismatch, 2u, 1u);
            auto msg2 = ZHLN::Reflect::FormatEnumMessage(HandleError::SlotOutOfBounds, 1050, 1024);

            ZHLN::Println("    [Formatted Message 1] {}", msg1);
            ZHLN::Println("    [Formatted Message 2] {}", msg2);

            ZHLN::Test::ExpectEq(msg1, "Recycled handle failed generation check. Expected generation 2, got 1");
            ZHLN::Test::ExpectEq(msg2, "Slot index 1050 exceeds maximum capacity of 1024");

            return {};
        }

        // --- 4. Description Formatting with Zero Arguments ---
        std::expected<void, ZHLN::ErrorCode> formatted_description_without_arguments() {
            auto msg = ZHLN::Reflect::FormatEnumMessage(HandleError::EntityNull);

            ZHLN::Println("    [Formatted Message (Zero-Arg)] {}", msg);

            ZHLN::Test::ExpectEq(msg, "Entity handle is null or uninitialized.");
            return {};
        }

        // --- 5. Formatted std::string Generation ---
        std::expected<void, ZHLN::ErrorCode> formatted_description_to_std_string() {
            std::string str = ZHLN::Reflect::FormatEnumMessageString(HandleError::GenerationMismatch, 10u, 4u);

            ZHLN::Println("    [Formatted std::string] {}", str);

            ZHLN::Test::ExpectEq(str, "Recycled handle failed generation check. Expected generation 10, got 4");
            return {};
        }

        // --- 6. ErrorCode <-> Error Interop ---
        std::expected<void, ZHLN::ErrorCode> errorcode_carries_and_promotes() {
            // The carrier is the same two words as the diagnostic form: promoting
            // is a copy, and it is where the annotated text becomes reachable.
            ZHLN::ErrorCode code {CodecError::CorruptedStream};

            ZHLN::Error promoted = code;
            if (!promoted.Is<CodecError>() || !promoted.Is(CodecError::CorruptedStream)) {
                return std::unexpected(CodecError::CorruptedStream);
            }

            // Building the carrier registered the category, so the promoted Error
            // resolves exactly like one constructed from the enum itself.
            ZHLN::Println("    [Carrier] {}: {}", promoted.Category(), promoted.Message());
            if (promoted.Category().empty() || promoted.Message().empty()) {
                return std::unexpected(CodecError::CorruptedStream);
            }

            // Demotion: back to the carrier without disturbing the words.
            ZHLN::ErrorCode back = promoted;
            if (!(back == code) || !back.Is(CodecError::CorruptedStream)) {
                return std::unexpected(CodecError::CorruptedStream);
            }
            if (back.ToError().Message() != promoted.Message()) {
                return std::unexpected(CodecError::CorruptedStream);
            }

            ZHLN::ErrorCode none;
            if (none) {
                return std::unexpected(CodecError::CorruptedStream);
            }
            return {};
        }
    };
};

// Exported for the core group binary (RunCoreTests.cpp), which
// aggregates every suite in this directory through Runner::RunDeferred.
auto RunErrorSuite() -> ZHLN::Test::TestStats {
    return ZHLN::Test::RunSuite<ErrorTestSuite>();
}

