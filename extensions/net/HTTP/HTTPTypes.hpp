// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Core/Description.hpp>
#include <Zahlen/Error.hpp>
#include <cstdint>

namespace ZHLN::HTTP {

// Hard ceiling on a response body, in bytes (256 MiB).
inline constexpr uint64_t kMaxBodyBytes = 268435456ULL;

// How many redirects a request may follow before libcurl is told to stop,
// which arrives as Error::TooManyRedirects.
inline constexpr uint32_t kMaxRedirects = 20;

// What stopped a transfer, grouped by retryability.
enum class Error : uint8_t {
    ClientInitFailed   ZHLN_ANNOTATION(ZHLN::Description<"Failed to initialize CURL easy session"> {}) = 1,
    ConnectionFailed   ZHLN_ANNOTATION(ZHLN::Description<"Failed to connect to host"> {}),
    Timeout            ZHLN_ANNOTATION(ZHLN::Description<"HTTP request timed out"> {}),
    TransferFailed     ZHLN_ANNOTATION(ZHLN::Description<"HTTP data transfer error"> {}),
    InvalidURL         ZHLN_ANNOTATION(ZHLN::Description<"Supplied URL is invalid or malformed"> {}),
    MalformedRequest   ZHLN_ANNOTATION(ZHLN::Description<"Request method or header is malformed"> {}),
    TooManyRedirects   ZHLN_ANNOTATION(ZHLN::Description<"Exceeded maximum redirect limit"> {}),
    SSLHandshakeFailed ZHLN_ANNOTATION(ZHLN::Description<"TLS/SSL certificate or handshake error"> {}),
    InternalError      ZHLN_ANNOTATION(ZHLN::Description<"Internal HTTP client error"> {})
};

} // namespace ZHLN::HTTP
