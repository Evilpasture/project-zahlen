// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include <RemoteAsset/AsyncAssetFetcher.hpp>
#include <RemoteAsset/URLResolver.hpp>

#include <HTTP/HTTP.hpp>
#include <Zahlen/Core/Platform.hpp>
#include <Zahlen/Error.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>

#include <format>
#include <span>
#include <utility>

namespace ZHLN::Remote {

namespace {

inline constexpr char kUserAgent[] = "project-zahlen";
inline constexpr char kAcceptAny[] = "*/*";

[[nodiscard]] auto MakeJob() -> FetchJob* {
    FetchJob* job = GetFetchJobPool().Create();
    job->refCount.store(0, std::memory_order::relaxed);
    job->cancelSignal.store(false, std::memory_order::relaxed);
    job->isDone.store(false, std::memory_order::relaxed);
    job->fetcher         = nullptr;
    job->validator       = nullptr;
    job->timeoutSeconds  = 30;
    job->result          = std::unexpected(ErrorCode {});
    return job;
}

} // namespace

AsyncAssetFetcher::AsyncAssetFetcher(DiskCache cache, uint32_t timeoutSeconds)
: m_cache(std::move(cache)), m_timeoutSeconds(timeoutSeconds) {
}

AsyncAssetFetcher::~AsyncAssetFetcher() {
    FetchHandle live[FetchSlotTable::kMaxSlots] {};
    uint32_t    liveCount = 0;
    m_slotTable.ForEachOccupied([&](FetchHandle handle, FetchJob* job) {
        job->cancelSignal.store(true, std::memory_order::relaxed);
        if (liveCount < FetchSlotTable::kMaxSlots) {
            live[liveCount++] = handle;
        }
    });
    for (uint32_t i = 0; i < liveCount; ++i) {
        m_slotTable.Free(live[i]);
    }
    while (m_activeWorkers.load(std::memory_order::acquire) > 0) {
        CPURelax();
    }
}

auto AsyncAssetFetcher::Request(std::string_view uri, ValidatorFn validator, bool forceRefresh) -> FetchHandle {
    const ValidatorFn check    = (validator != nullptr) ? validator : Validators::AnyNonEmpty;
    const ResolvedURL resolved = ResolveURL(uri);

    if (!forceRefresh) {
        if (auto hit = m_cache.Read(resolved.cacheFileName, check)) {
            FetchJob* job = MakeJob();
            job->fetcher  = this;
            job->isDone.store(true, std::memory_order::relaxed);
            job->result = FetchPayload {
                .data           = std::move(*hit),
                .localCachePath = m_cache.Root() / resolved.cacheFileName,
                .fromCache      = true,
                .sourceUrl      = (m_cache.Root() / resolved.cacheFileName).string(),
            };
            auto handle = m_slotTable.Allocate(job);
            if (!handle) {
                GetFetchJobPool().Destroy(job);
                return {};
            }
            return *handle;
        }
        if (m_cache.Exists(resolved.cacheFileName)) {
            m_cache.Invalidate(resolved.cacheFileName);
        }
    }

    FetchJob* job       = MakeJob();
    job->fetcher        = this;
    job->canonicalKey   = resolved.cacheFileName;
    job->candidates.push_back(resolved.primary);
    for (const auto& fallback: resolved.fallbacks) {
        job->candidates.push_back(fallback);
    }
    job->validator      = check;
    job->timeoutSeconds = m_timeoutSeconds;

    auto handle = m_slotTable.Allocate(job);
    if (!handle) {
        GetFetchJobPool().Destroy(job);
        return {};
    }

    job->Retain();
    m_activeWorkers.fetch_add(1, std::memory_order::relaxed);

    const TaskSystem::Task task {.func = &WorkerTrampoline, .arg = job};
    TaskSystem::Dispatch(std::span<const TaskSystem::Task>(&task, 1));
    return *handle;
}

auto AsyncAssetFetcher::FetchSync(std::string_view uri, ValidatorFn validator) -> std::expected<FetchPayload, ErrorCode> {
    const ValidatorFn check    = (validator != nullptr) ? validator : Validators::AnyNonEmpty;
    const ResolvedURL resolved = ResolveURL(uri);

    if (auto hit = m_cache.Read(resolved.cacheFileName, check)) {
        return FetchPayload {
            .data           = std::move(*hit),
            .localCachePath = m_cache.Root() / resolved.cacheFileName,
            .fromCache      = true,
            .sourceUrl      = (m_cache.Root() / resolved.cacheFileName).string(),
        };
    }
    if (m_cache.Exists(resolved.cacheFileName)) {
        m_cache.Invalidate(resolved.cacheFileName);
    }

    FetchJob job;
    job.fetcher        = this;
    job.canonicalKey   = resolved.cacheFileName;
    job.candidates.push_back(resolved.primary);
    for (const auto& fallback: resolved.fallbacks) {
        job.candidates.push_back(fallback);
    }
    job.validator      = check;
    job.timeoutSeconds = m_timeoutSeconds;

    ExecuteJob(&job);
    return std::move(job.result);
}

void AsyncAssetFetcher::WorkerTrampoline(void* arg) noexcept {
    auto* job     = static_cast<FetchJob*>(arg);
    auto* fetcher = job->fetcher;
    if (fetcher != nullptr) {
        fetcher->ExecuteJob(job);
        fetcher->m_activeWorkers.fetch_sub(1, std::memory_order::release);
    }
    job->Release();
}

void AsyncAssetFetcher::ExecuteJob(FetchJob* job) noexcept {
    if (job->cancelSignal.load(std::memory_order::relaxed)) {
        job->lastError = "cancelled";
        job->result    = std::unexpected(HTTP::HTTPError::TransferFailed);
        job->isDone.store(true, std::memory_order::release);
        return;
    }

    std::string detail;
    ErrorCode   lastErrorCode = HTTP::HTTPError::ConnectionFailed;
    const auto  note = [&detail](std::string text) {
        if (detail.size() >= 400) {
            return;
        }
        if (!detail.empty()) {
            detail += "; ";
        }
        detail += std::move(text);
    };

    for (const auto& url: job->candidates) {
        if (job->cancelSignal.load(std::memory_order::relaxed)) {
            note("cancelled");
            lastErrorCode = HTTP::HTTPError::TransferFailed;
            break;
        }

        const HTTP::Request req {
            .url            = url,
            .headers        = {HTTP::Header {.name = "User-Agent", .value = std::string(kUserAgent)},
                               HTTP::Header {.name = "Accept", .value = std::string(kAcceptAny)}},
            .timeoutSeconds = job->timeoutSeconds,
            .cancelSignal   = &job->cancelSignal,
        };

        auto resp = HTTP::Fetch(req);
        if (!resp) {
            lastErrorCode = resp.error();
            const Error err = resp.error();
            note(std::format("{}: {} ({})", url, err.Category(), err.Message()));
            continue;
        }
        if (resp->statusCode == 404) {
            lastErrorCode = FetchError::NotFound;
            note(std::format("{}: HTTP 404", url));
            continue;
        }
        if ((resp->statusCode < 200) || (resp->statusCode >= 300)) {
            lastErrorCode = FetchError::HTTPStatus;
            note(std::format("{}: HTTP {}", url, resp->statusCode));
            continue;
        }
        if (job->validator != nullptr && !job->validator(resp->body)) {
            lastErrorCode = FetchError::Rejected;
            note(std::format("{}: {} byte(s) the validator rejected", url, resp->body.size()));
            continue;
        }

        const bool writeOk = m_cache.WriteAtomic(job->canonicalKey, resp->body);
        job->result        = FetchPayload {
            .data           = std::move(resp->body),
            .localCachePath = writeOk ? (m_cache.Root() / job->canonicalKey) : std::filesystem::path {},
            .fromCache      = false,
            .sourceUrl      = url,
        };
        job->isDone.store(true, std::memory_order::release);
        return;
    }

    job->lastError = detail.empty() ? std::string("no candidate answered") : std::move(detail);
    job->result    = std::unexpected(HTTP::HTTPError::ConnectionFailed);
    job->isDone.store(true, std::memory_order::release);
}

auto AsyncAssetFetcher::PollResult(FetchHandle handle) -> std::optional<std::expected<FetchPayload, ErrorCode>> {
    FetchJob* job = m_slotTable.GetJobAndRetain(handle);
    if (job == nullptr) {
        return std::nullopt;
    }
    if (!job->isDone.load(std::memory_order::acquire)) {
        job->Release();
        return std::nullopt;
    }
    auto out = std::move(job->result);
    m_slotTable.Free(handle);
    job->Release();
    return out;
}

void AsyncAssetFetcher::Cancel(FetchHandle handle) {
    FetchJob* job = m_slotTable.GetJobAndRetain(handle);
    if (job == nullptr) {
        return;
    }
    job->cancelSignal.store(true, std::memory_order::relaxed);
    m_slotTable.Free(handle);
    job->Release();
}

auto AsyncAssetFetcher::Status(FetchHandle handle) const -> FetchStatus {
    FetchJob* job = m_slotTable.GetJobAndRetain(handle);
    if (job == nullptr) {
        return FetchStatus::Idle;
    }
    FetchStatus status = FetchStatus::Pending;
    if (job->isDone.load(std::memory_order::acquire)) {
        status = job->result.has_value() ? FetchStatus::Succeeded : FetchStatus::Failed;
    }
    job->Release();
    return status;
}

auto AsyncAssetFetcher::Take(FetchHandle handle) -> std::optional<FetchResult> {
    FetchJob* job = m_slotTable.GetJobAndRetain(handle);
    if (job == nullptr) {
        return std::nullopt;
    }
    if (!job->isDone.load(std::memory_order::acquire)) {
        job->Release();
        return std::nullopt;
    }
    FetchResult out;
    if (job->result.has_value()) {
        out = std::move(*job->result);
    } else {
        out.errorMessage = std::move(job->lastError);
        if (out.errorMessage.empty()) {
            out.errorMessage = "fetch failed";
        }
    }
    m_slotTable.Free(handle);
    job->Release();
    return out;
}

} // namespace ZHLN::Remote
