// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TestsFramework.hpp"
#include <Zahlen/Core/Reflection.hpp>
#include <expected>
#include <string>

// ============================================================================
// Local Test Enums (Self-Contained)
// ============================================================================

enum class CodecError : uint32_t {
    Success = 0,
    CorruptedStream[[= ZHLN::Reflect::Description("The input bitstream is corrupted or incomplete.")]],
    UnsupportedVersion[[= ZHLN::Reflect::Description("The bitstream header version is not supported.")]]
};

enum class NetworkError : uint32_t {
    Success = 0,
    HostUnreachable[[= ZHLN::Reflect::Description("Remote host refused the connection or is offline.")]],
    ConnectionReset
};

enum class HandleError : uint32_t {
    Success = 0,
    GenerationMismatch[[= ZHLN::Reflect::Description("Recycled handle failed generation check. Expected generation {}, got {}")]],
    SlotOutOfBounds[[= ZHLN::Reflect::Description("Slot index {} exceeds maximum capacity of {}")]],
    EntityNull[[= ZHLN::Reflect::Description("Entity handle is null or uninitialized.")]]
};

// ============================================================================
// Test Suite Class
// ============================================================================

struct ErrorTestSuite {
    struct Tests {
        // --- 1. Basic Error Type & State ---
        std::expected<void, ZHLN::Error> default_constructor_is_falsy() {
            ZHLN::Error err;
            if (err) {
                return std::unexpected(CodecError::CorruptedStream);
            }
            return {};
        }

        std::expected<void, ZHLN::Error> active_error_is_truthy() {
            ZHLN::Error err {CodecError::CorruptedStream};
            if (!err) {
                return std::unexpected(CodecError::CorruptedStream);
            }
            return {};
        }

        std::expected<void, ZHLN::Error> type_and_value_matching() {
            ZHLN::Error err {CodecError::CorruptedStream};

            if (!err.Is<CodecError>()) {
                return std::unexpected(CodecError::CorruptedStream);
            }
            if (!err.Is(CodecError::CorruptedStream)) {
                return std::unexpected(CodecError::CorruptedStream);
            }
            if (err.Is(CodecError::Success)) {
                return std::unexpected(CodecError::CorruptedStream);
            }
            if (err.Is<NetworkError>()) {
                return std::unexpected(NetworkError::ConnectionReset);
            }
            return {};
        }

        std::expected<void, ZHLN::Error> value_extraction() {
            ZHLN::Error err {CodecError::UnsupportedVersion};
            if (err.As<CodecError>() != CodecError::UnsupportedVersion) {
                return std::unexpected(CodecError::CorruptedStream);
            }
            return {};
        }

        // --- 2. Static Reflection Category & Message Resolution ---
        std::expected<void, ZHLN::Error> category_and_message_resolution() {
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
        std::expected<void, ZHLN::Error> formatted_description_with_arguments() {
            auto msg1 = ZHLN::Reflect::FormatEnumMessage(HandleError::GenerationMismatch, 2u, 1u);
            auto msg2 = ZHLN::Reflect::FormatEnumMessage(HandleError::SlotOutOfBounds, 1050, 1024);

            ZHLN::Println("    [Formatted Message 1] {}", msg1.string_view());
            ZHLN::Println("    [Formatted Message 2] {}", msg2.string_view());

            ZHLN::Test::ExpectEq(msg1.string_view(), "Recycled handle failed generation check. Expected generation 2, got 1");

            ZHLN::Test::ExpectEq(msg2.string_view(), "Slot index 1050 exceeds maximum capacity of 1024");

            return {};
        }

        // --- 4. Description Formatting with Zero Arguments ---
        std::expected<void, ZHLN::Error> formatted_description_without_arguments() {
            auto msg = ZHLN::Reflect::FormatEnumMessage(HandleError::EntityNull);

            ZHLN::Println("    [Formatted Message (Zero-Arg)] {}", msg.string_view());

            ZHLN::Test::ExpectEq(msg.string_view(), "Entity handle is null or uninitialized.");
            return {};
        }

        // --- 5. Formatted std::string Generation ---
        std::expected<void, ZHLN::Error> formatted_description_to_std_string() {
            std::string str = ZHLN::Reflect::FormatEnumMessageString(HandleError::GenerationMismatch, 10u, 4u);

            ZHLN::Println("    [Formatted std::string] {}", str);

            ZHLN::Test::ExpectEq(str, "Recycled handle failed generation check. Expected generation 10, got 4");
            return {};
        }
    };
};

int main() {
    return ZHLN::Test::Runner::Run<ErrorTestSuite>();
}
