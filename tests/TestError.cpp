// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TestsFramework.hpp"
#include <expected>

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

// ============================================================================
// Test Suite Class
// ============================================================================

struct ErrorTestSuite {
    // Nested struct containing test cases exclusively
    struct Tests {
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

        std::expected<void, ZHLN::Error> category_and_message_resolution() {
            ZHLN::Error err {CodecError::CorruptedStream};
            ZHLN::Error netErr {NetworkError::HostUnreachable};

            ZHLN::Println("    [Reflected Description] {}: {}", err.Category(), err.Message());
            ZHLN::Println("    [Reflected Description] {}: {}", netErr.Category(), netErr.Message());

            if (err.Category().empty() || err.Message().empty()) {
                return std::unexpected(CodecError::CorruptedStream);
            }
            return {};
        }
    };

    // Suite-level helper functions (Safe from automatic test execution)
    void some_internal_helper() {
        // ...
    }
};

int main() {
    return ZHLN::Test::Runner::Run<ErrorTestSuite>();
}
