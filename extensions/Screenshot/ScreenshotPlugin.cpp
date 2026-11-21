// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#include <Screenshot/Screenshot.hpp>
#include <Zahlen/COM/IEngine.hpp>
#include <Zahlen/Common.h>
#include <Zahlen/COM/IPlugin.hpp>
#include <Zahlen/Core/COM/Contract.hpp>
#include <Zahlen/Core/COM/Chain.hpp>
#include <Zahlen/Core/COM/Reflect.hpp>
#include <new>
#include <string>
#include <string_view>
#include <utility>

namespace {

using namespace ZHLN;

class ScreenshotPlugin final : public COM::Implements<ScreenshotPlugin, COM::IAppLayer> {
  public:
    ScreenshotPlugin(std::string path, uint32_t afterFrames) noexcept
        : _outcome {}
        , _layer(std::move(path), afterFrames, _outcome) {}

    auto OnStart(COM::IEngine* engine) noexcept -> COM::Result<void> override {
        if (engine == nullptr) {
            return COM::Error::NullArgument;
        }
        _layer.Start(*engine);
        return {};
    }

    auto OnTick(COM::IEngine*, float) noexcept -> COM::Result<void> override {
        return {};
    }

    auto OnBeforeRender(COM::IEngine*, float) noexcept -> COM::Result<void> override {
        return {};
    }

    auto OnAfterTick(COM::IEngine* engine, float) noexcept -> COM::Result<void> override {
        if (engine == nullptr) {
            return COM::Error::NullArgument;
        }
        _layer.AfterTick(*engine);
        if (_outcome.status == Screenshot::CaptureStatus::ReadFailed) {
            return _outcome.error;
        }
        if (_outcome.status == Screenshot::CaptureStatus::WriteFailed) {
            return ErrorCode(Screenshot::Error::WriteFailed);
        }
        return {};
    }

    void OnStop(COM::IEngine*) noexcept override {}

  private:
    Screenshot::CaptureOutcome _outcome;
    Screenshot::CaptureLayer   _layer;
};

} // namespace

namespace ZHLN::COM {

ZHLN_EXPORT auto CreateAppLayer(const ChainInNode* config) noexcept -> Result<IAppLayer*> {
    Screenshot::PluginConfig parsed {};
    const auto               checked = DispatchInChain<Screenshot::PluginConfig>(
        config, [&](const Screenshot::PluginConfig& value) noexcept {
            parsed = value;
        });
    if (!checked.HasValue()) {
        return checked.error;
    }
    if (parsed.path.size > 0 && parsed.path.data == nullptr) {
        return Error::NullArgument;
    }
    std::string ownedPath;
    if (parsed.path.size > 0) {
        ownedPath.assign(parsed.path.data, parsed.path.size);
    }
    auto* instance = new (std::nothrow) ScreenshotPlugin(std::move(ownedPath), parsed.afterFrames);
    if (instance == nullptr) {
        return Error::AllocationFailed;
    }
    return static_cast<IAppLayer*>(instance);
}

} // namespace ZHLN::COM

template <>
ZHLN_EXPORT void ZHLN::COM::EntryPointWitness<&ZHLN::COM::CreateAppLayer>::Seal() noexcept {}
