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

// A wire StringView is valid when it is empty or points at storage. Building a
// std::string_view from {nullptr, n} is undefined, and this adapter is the only
// thing standing between a caller's struct and that.
[[nodiscard]] bool Valid(const StringView value) noexcept {
    return value.size == 0 || value.data != nullptr;
}

[[nodiscard]] auto Text(const StringView value) noexcept -> std::string_view {
    return value.size != 0 ? std::string_view(value.data, value.size) : std::string_view {};
}

// The COM adapter over the engine-first registry: it owns a plain
// ZHLN::ErrorRegistry and adds exactly what the COM contract needs -- chain
// out-nodes and the fixed-width ErrorDescription/ErrorRecord payloads.
class ErrorRegistry final: public Implements<ErrorRegistry, IErrorRegistry> {
  public:
    auto RegisterCategory(const uint32_t categoryHash, const StringView categoryName, const Span<const ErrorEnumEntry> entries) noexcept
        -> Result<void> override {
        if (!Valid(categoryName)) {
            return Error::NullArgument;
        }
        if (entries.size != 0 && entries.data == nullptr) {
            return Error::NullArgument;
        }
        std::vector<ZHLN::CategoryEntryInput> views;
        views.reserve(entries.size);
        for (const auto& entry: entries) {
            if (!Valid(entry.name) || !Valid(entry.message)) {
                return Error::NullArgument;
            }
            views.push_back(ZHLN::CategoryEntryInput {
                .value   = entry.value,
                .name    = Text(entry.name),
                .message = Text(entry.message),
            });
        }
        if (!_registry.RegisterCategory(categoryHash, Text(categoryName), views)) {
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
        // An unknown code is a failure, not a description that says "Unknown":
        // reporting success here made the caller's raw-code fallback (see
        // COM::DescribeError) unreachable, so every unregistered enumerator
        // was described as if it were a registered one named Unknown.
        std::string category;
        std::string name;
        std::string message;
        if (!_registry.Describe(code, category, name, message)) {
            return Error::NotRegistered;
        }
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
        // Same rule as LoggerHub::Write: empty channel is untagged, non-empty
        // must be a well-formed wire string.
        if (!Valid(detail) || !Valid(source.file) || !Valid(source.function) ||
            (source.channel.size != 0 && !Valid(source.channel))) {
            return;
        }
        // The registry copies every one of these into storage it owns; the
        // caller's struct and its strings may be gone by the time anyone reads
        // the record back.
        _registry.RecordDetail(
            code,
            ZHLN::LogSource {
                .file     = Text(source.file),
                .function = Text(source.function),
                .channel  = Text(source.channel),
                .line     = source.line,
            },
            Text(detail)
        );
    }

    auto LastError(ChainOutNode* outRecord) const noexcept -> Result<void> override {
        if (outRecord == nullptr) {
            return Error::NullArgument;
        }
        const auto last = _registry.LastDetail();
        if (!last.has_value()) {
            return Error::NotRegistered;
        }
        const ErrorRecord record {
            .code     = last->code,
            .detail   = String256(std::string_view(last->detail)),
            .file     = String64(std::string_view(last->file)),
            .function = String128(std::string_view(last->function)),
            .line     = last->line,
            .channel  = last->channel,
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

auto CreateErrorRegistryRaw(const ChainInNode* config) noexcept -> Result<IErrorRegistry*> {
    // The chain can seed the new registry from one the caller already holds --
    // typically the one that recorded why their previous engine failed.
    IErrorRegistry* seed = nullptr;
    if (config != nullptr) {
        const Result<void> read = DispatchInChain<ErrorConfig>(config, [&]<typename Node>(const Node& node) -> Result<void> {
            if constexpr (std::is_same_v<Node, ErrorConfig>) {
                seed = node.registry;
            }
            return {};
        });
        if (!read.HasValue()) {
            return read.error;
        }
    }

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

    if (seed != nullptr) {
        // Carry the last recorded error across. The category table is
        // process-wide and engine-owned, so it is already shared; what a new
        // registry would otherwise lose is the one record that says why the
        // caller is creating a second one.
        ErrorRecord record {};
        auto        pack = OutChain(record);
        if (seed->LastError(pack.Head()).HasValue()) {
            LogSource source;
            source.file     = StringView(std::string_view(record.file));
            source.function = StringView(std::string_view(record.function));
            source.line     = record.line;
            source.channel  = std::string_view(record.channel);
            registry->RecordDetail(record.code, source, StringView(std::string_view(record.detail)));
        }
    }
    return static_cast<IErrorRegistry*>(registry);
}

} // namespace ZHLN::COM
