// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

// Dynamically loadable COM::IAppLayer + HTTP::IHttpClient plugin. Seals
// libcurl behind a runtime .so boundary so hosts can perform HTTP transfers
// without linking zahlen_http or libcurl.

#include <HTTP/HTTP.hpp>
#include <HTTP/IHttpClient.hpp>

#include <Zahlen/COM/IEngine.hpp>
#include <Zahlen/COM/IPlugin.hpp>
#include <Zahlen/Common.h>
#include <Zahlen/Core/COM/Contract.hpp>
#include <new>
#include <string>
#include <string_view>
#include <utility>

namespace {

using namespace ZHLN;

class HttpResponseObject final : public COM::Implements<HttpResponseObject, HTTP::IHttpResponse> {
  public:
    explicit HttpResponseObject(HTTP::Response response) noexcept
        : _response(std::move(response)) {}

    auto StatusCode() const noexcept -> int32_t override {
        return _response.statusCode;
    }

    auto Redirected() const noexcept -> bool override {
        return _response.redirected;
    }

    auto Body() const noexcept -> COM::Span<const uint8_t> override {
        return {_response.body.data(), _response.body.size()};
    }

    auto HeaderCount() const noexcept -> uint32_t override {
        return static_cast<uint32_t>(_response.headers.size());
    }

    auto HeaderAt(uint32_t index) const noexcept -> COM::Option<HTTP::ComHeader> override {
        if (index >= _response.headers.size()) {
            return {};
        }
        const auto& h = _response.headers[index];
        return HTTP::ComHeader {
            .name  = COM::StringView(h.name.data(), h.name.size()),
            .value = COM::StringView(h.value.data(), h.value.size()),
        };
    }

    auto FindHeader(COM::StringView name) const noexcept -> COM::Option<COM::StringView> override {
        if (name.size > 0 && name.data == nullptr) {
            return {};
        }
        const auto found = _response.FindHeader(std::string_view(name.data, name.size));
        if (!found) {
            return {};
        }
        return COM::StringView(found->data(), found->size());
    }

  private:
    HTTP::Response _response;
};

class HttpPlugin final : public COM::Implements<HttpPlugin, COM::IAppLayer, HTTP::IHttpClient> {
  public:
    HttpPlugin() noexcept = default;

    // --- COM::IAppLayer ------------------------------------------------------

    auto OnStart(COM::IEngine*) noexcept -> COM::Result<void> override {
        return {};
    }

    auto OnTick(COM::IEngine*, float) noexcept -> COM::Result<void> override {
        return {};
    }

    auto OnBeforeRender(COM::IEngine*, float) noexcept -> COM::Result<void> override {
        return {};
    }

    auto OnAfterTick(COM::IEngine*, float) noexcept -> COM::Result<void> override {
        return {};
    }

    void OnStop(COM::IEngine*) noexcept override {}

    // --- HTTP::IHttpClient ---------------------------------------------------

    auto FetchRaw(const HTTP::ComRequest& request) const noexcept -> COM::Result<HTTP::IHttpResponse*> override {
        if ((request.url.size > 0 && request.url.data == nullptr) ||
            (request.method.size > 0 && request.method.data == nullptr) ||
            (request.headers.size > 0 && request.headers.data == nullptr) ||
            (request.body.size > 0 && request.body.data == nullptr)) {
            return COM::Error::NullArgument;
        }

        HTTP::Request native {};
        if (request.url.data != nullptr) {
            native.url.assign(request.url.data, request.url.size);
        }
        if (request.method.data != nullptr && request.method.size > 0) {
            native.method.assign(request.method.data, request.method.size);
        } else {
            native.method = "GET";
        }
        native.headers.reserve(request.headers.size);
        for (size_t i = 0; i < request.headers.size; ++i) {
            const auto& h = request.headers.data[i];
            if ((h.name.size > 0 && h.name.data == nullptr) || (h.value.size > 0 && h.value.data == nullptr)) {
                return COM::Error::NullArgument;
            }
            native.headers.push_back(HTTP::Header {
                .name  = h.name.data != nullptr ? std::string(h.name.data, h.name.size) : std::string {},
                .value = h.value.data != nullptr ? std::string(h.value.data, h.value.size) : std::string {},
            });
        }
        if (request.body.size > 0) {
            native.body.assign(request.body.data, request.body.data + request.body.size);
        }
        native.timeoutSeconds  = request.timeoutSeconds;
        native.followRedirects = request.followRedirects;
        native.cancelSignal    = request.cancelSignal;

        auto fetched = HTTP::Fetch(native);
        if (!fetched) {
            return fetched.error();
        }
        auto* responseObj = new (std::nothrow) HttpResponseObject(std::move(*fetched));
        if (responseObj == nullptr) {
            return COM::Error::AllocationFailed;
        }
        return static_cast<HTTP::IHttpResponse*>(responseObj);
    }
};

} // namespace

namespace ZHLN::COM {

ZHLN_EXPORT auto CreateAppLayerRaw(const ChainInNode* config) noexcept -> Result<IAppLayer*> {
    if (config != nullptr) {
        return Error::UnsupportedExtension;
    }
    auto* instance = new (std::nothrow) HttpPlugin();
    if (instance == nullptr) {
        return Error::AllocationFailed;
    }
    return static_cast<IAppLayer*>(instance);
}

} // namespace ZHLN::COM

template <>
ZHLN_EXPORT void ZHLN::COM::EntryPointWitness<&ZHLN::COM::CreateAppLayerRaw>::Seal() noexcept {}
