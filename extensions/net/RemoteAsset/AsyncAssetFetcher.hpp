// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <RemoteAsset/DiskCache.hpp>
#include <RemoteAsset/FetchSlotTable.hpp>

#include <Zahlen/Core/Atomic.hpp>
#include <Zahlen/Error.hpp>

#include <cstdint>
#include <expected>
#include <optional>
#include <string_view>

namespace ZHLN::Remote {

enum class FetchStatus : uint8_t {
    Idle,
    Pending,
    Succeeded,
    Failed
};

using FetchResult = FetchPayload;

class AsyncAssetFetcher {
  public:
    explicit AsyncAssetFetcher(DiskCache cache, uint32_t timeoutSeconds = 60);
    ~AsyncAssetFetcher();

    AsyncAssetFetcher(const AsyncAssetFetcher&)            = delete;
    auto operator=(const AsyncAssetFetcher&) -> AsyncAssetFetcher& = delete;

    auto Request(std::string_view uri, ValidatorFn validator = Validators::AnyNonEmpty, bool forceRefresh = false)
        -> FetchHandle;

    auto FetchSync(std::string_view uri, ValidatorFn validator = Validators::AnyNonEmpty)
        -> std::expected<FetchPayload, ErrorCode>;

    auto PollResult(FetchHandle handle) -> std::optional<std::expected<FetchPayload, ErrorCode>>;

    void Cancel(FetchHandle handle);

    void Poll() noexcept {}

    [[nodiscard]] auto Status(FetchHandle handle) const -> FetchStatus;

    [[nodiscard]] auto Take(FetchHandle handle) -> std::optional<FetchResult>;

    [[nodiscard]] auto Cache() const noexcept -> const DiskCache& {
        return m_cache;
    }

  private:
    static void WorkerTrampoline(void* arg) noexcept;
    void        ExecuteJob(FetchJob* job) noexcept;

    DiskCache              m_cache;
    uint32_t               m_timeoutSeconds;
    FetchSlotTable         m_slotTable;
    ZHLN::Atomic<uint32_t> m_activeWorkers {0};
};

} // namespace ZHLN::Remote
