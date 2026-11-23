// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

// The COM adapter over the engine-first error registry: it owns a plain
// ZHLN::ErrorRegistry and translates to/from the fixed-width COM payloads and
// chain out-nodes. src/engine/ErrorRegistry.{hpp,cpp} stays COM-free.

#include "EngineErrors.hpp"
#include "ErrorRegistry.hpp"
#include "FramePlan.hpp"
#include <Zahlen/Core/COM/Reflect.hpp>
#include <Zahlen/COM/ErrorTypes.hpp>
#include <Zahlen/COM/FrameTypes.hpp>
#include <Zahlen/COM/IErrorRegistry.hpp>
#include <Zahlen/Core/ErrorDescribe.hpp>
#include <Zahlen/Input/ActionValues.hpp>
#include <Zahlen/Render/FrameResult.hpp>
#include <Zahlen/Render/Info.hpp>
#include <new>
#include <string>
#include <string_view>
#include <vector>

namespace ZHLN::COM {

namespace {

// The COM adapter over the engine-first registry: it owns a plain
// ZHLN::ErrorRegistry and adds exactly what the COM contract needs -- chain
// out-nodes and the fixed-width ErrorDescription/ErrorRecord payloads.
class ErrorRegistry final: public Implements<ErrorRegistry, IErrorRegistry> {
  public:
    auto RegisterCategory(const uint32_t categoryHash, const StringView categoryName, const Span<const ErrorEnumEntry> entries) noexcept
        -> Result<void> override {
        std::vector<ZHLN::CategoryEntryInput> views;
        views.reserve(entries.size);
        for (const auto& entry: entries) {
            views.push_back(ZHLN::CategoryEntryInput {
                .value   = entry.value,
                .name    = std::string_view(entry.name),
                .message = std::string_view(entry.message),
            });
        }
        if (!_registry.RegisterCategory(categoryHash, std::string_view(categoryName), views)) {
            return Error::InvalidArgument;
        }
        return {};
    }

    bool UnregisterCategory(const uint32_t categoryHash) noexcept override {
        return _registry.UnregisterCategory(categoryHash);
    }

    auto Describe(const ErrorCode code, ChainOutNode* outDesc) const noexcept -> Result<void> override {
        if (outDesc == nullptr) {
            return Error::NullArgument;
        }
        std::string category("Unknown");
        std::string name("Unknown");
        std::string message("Unknown");
        (void) _registry.Describe(code, category, name, message);
        const ErrorDescription desc {
            .category = String64(std::string_view(category)),
            .name     = String64(std::string_view(name)),
            .message  = String256(std::string_view(message)),
        };
        return DispatchOutChain<ErrorDescription>(outDesc, [&](ErrorDescription& out) -> Result<void> {
            out = desc;
            return {};
        });
    }

    void RecordDetail(const ErrorCode code, const LogSource source, const StringView detail) noexcept override {
        _registry.RecordDetail(code, source, std::string_view(detail));
    }

    auto LastError(ChainOutNode* outRecord) const noexcept -> Result<void> override {
        if (outRecord == nullptr) {
            return Error::NullArgument;
        }
        const auto* last = _registry.LastDetail();
        if (last == nullptr) {
            return Error::InvalidArgument;
        }
        const ErrorRecord record {
            .code   = last->code,
            .detail = String256(std::string_view(last->detail)),
            .source = last->source,
        };
        return DispatchOutChain<ErrorRecord>(outRecord, [&](ErrorRecord& out) -> Result<void> {
            out = record;
            return {};
        });
    }

  private:
    ZHLN::ErrorRegistry _registry;
};

} // namespace

auto CreateErrorRegistryRaw(const ChainInNode* /*config*/) noexcept -> Result<IErrorRegistry*> {
    auto* registry = new (std::nothrow) ErrorRegistry();
    if (registry == nullptr) {
        return Error::AllocationFailed;
    }
    // Pre-register every category the engine itself can return. Registration
    // cannot fail here (fresh registry, distinct categories); ignore results.
    (void) RegisterEnum<Error>(*registry);
    (void) RegisterEnum<FramePipelineError>(*registry);
    (void) RegisterEnum<FrameResult>(*registry);
    (void) RegisterEnum<RenderFeatureError>(*registry);
    (void) RegisterEnum<InputBindingParseError>(*registry);
    (void) RegisterEnum<FrameError>(*registry);
    (void) RegisterEnum<EngineInitError>(*registry);
    (void) RegisterEnum<KernelInitError>(*registry);
    (void) RegisterEnum<WorldInitError>(*registry);
    (void) RegisterEnum<RenderSystemError>(*registry);
    return static_cast<IErrorRegistry*>(registry);
}

} // namespace ZHLN::COM
