// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <RemoteAsset/AsyncAssetFetcher.hpp>
#include <Zahlen/AssetManager.hpp>

#include <optional>
#include <string_view>

namespace ZHLN {

inline auto RequestRemoteAsset(AssetManager& assets, std::string_view url, Remote::ValidatorFn validator = Remote::Validators::AnyNonEmpty)
    -> std::optional<Remote::FetchHandle> {
    auto* fetcher = assets.RemoteFetcher();
    if (fetcher == nullptr) {
        return std::nullopt;
    }
    const Remote::FetchHandle handle = fetcher->Request(url, validator);
    if (!handle.IsValid()) {
        return std::nullopt;
    }
    return handle;
}

} // namespace ZHLN
