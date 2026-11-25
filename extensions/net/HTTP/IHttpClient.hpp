// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Binary-stable COM capability interfaces exposed by libzahlen_plugin_http
// alongside COM::IAppLayer. Hosts include only this header to perform HTTP
// requests at runtime without linking zahlen_http or libcurl.

#include "HTTPTypes.hpp"
#include <Zahlen/Core/Atomic.hpp>
#include <Zahlen/Core/COM/IObject.hpp>
#include <Zahlen/Core/COM/Ref.hpp>
#include <Zahlen/Core/COM/Reflect.hpp>
#include <Zahlen/Core/COM/Types.hpp>
#include <cstdint>
#include <expected>
#include <string_view>
#include <type_traits>

namespace ZHLN::HTTP {

struct ComHeader {
    COM::StringView name {};
    COM::StringView value {};
};

struct ComRequest {
    COM::StringView            url {};
    COM::StringView            method {"GET"};
    COM::Span<const ComHeader> headers {};
    COM::Span<const uint8_t>   body {};
    uint32_t                   timeoutSeconds  = 30;
    bool                       followRedirects = true;
    const ZHLN::Atomic<bool>*  cancelSignal    = nullptr;
};

static_assert(std::is_standard_layout_v<ComHeader> && std::is_trivially_copyable_v<ComHeader>);
static_assert(std::is_standard_layout_v<ComRequest> && std::is_trivially_copyable_v<ComRequest>);

struct IHttpResponse : COM::IObject {
    [[nodiscard]] virtual int32_t                      StatusCode() const noexcept                      = 0;
    [[nodiscard]] virtual bool                         Redirected() const noexcept                      = 0;
    [[nodiscard]] virtual COM::Span<const uint8_t>     Body() const noexcept                            = 0;
    [[nodiscard]] virtual uint32_t                     HeaderCount() const noexcept                     = 0;
    [[nodiscard]] virtual COM::Option<ComHeader>       HeaderAt(uint32_t index) const noexcept          = 0;
    [[nodiscard]] virtual COM::Option<COM::StringView> FindHeader(COM::StringView name) const noexcept  = 0;

    // Inline view helper (compiled in the consumer TU, never on the wire).
    [[nodiscard]] auto Text() const noexcept -> std::string_view {
        const auto bytes = Body();
        return {reinterpret_cast<const char*>(bytes.data), bytes.size};
    }

  protected:
    ~IHttpResponse() = default;
};

static_assert(COM::AuditInterfaceABI<IHttpResponse>(), "IHttpResponse violates the COM ABI rules.");

struct IHttpClient : COM::IObject {
    // Runs `request` synchronously on the calling thread and returns a newly
    // refcounted IHttpResponse (caller adopts the returned reference).
    [[nodiscard]] virtual COM::Result<IHttpResponse*> FetchRaw(const ComRequest& request) const noexcept = 0;

    // Ergonomic owning wrappers (compiled in the consumer TU, never on the wire).
    [[nodiscard]] auto Fetch(const ComRequest& request) const noexcept
        -> std::expected<COM::Ref<IHttpResponse>, ErrorCode> {
        const auto res = FetchRaw(request);
        if (!res.HasValue()) {
            return std::unexpected(res.error);
        }
        if (res.value == nullptr) {
            return std::unexpected(COM::Error::NullArgument);
        }
        return COM::Ref<IHttpResponse>::Adopt(res.value);
    }

    [[nodiscard]] auto Get(std::string_view url, uint32_t timeoutSeconds = 30) const noexcept
        -> std::expected<COM::Ref<IHttpResponse>, ErrorCode> {
        return Fetch(ComRequest {.url = url, .timeoutSeconds = timeoutSeconds});
    }

  protected:
    ~IHttpClient() = default;
};

static_assert(COM::AuditInterfaceABI<IHttpClient>(), "IHttpClient violates the COM ABI rules.");

} // namespace ZHLN::HTTP
