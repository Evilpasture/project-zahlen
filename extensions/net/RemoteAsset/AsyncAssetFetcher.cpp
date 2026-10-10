// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include <HTTP/HTTP.hpp>
#include <RemoteAsset/AsyncAssetFetcher.hpp>
#include <RemoteAsset/URLResolver.hpp>
#include <Zahlen/Core/Platform.hpp>
#include <Zahlen/Error.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <algorithm>
#include <charconv>
#include <chrono>
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

[[nodiscard]] auto CacheCandidate(const DiskCache& cache, const ResolvedURL& resolved, ValidatorFn validator) -> std::optional<CacheEntry> {
    auto hit = cache.ReadEntry(resolved.cacheFileName, validator);
    if (hit && !hit->origin.url.empty() && hit->origin.url != resolved.primary &&
        std::find(resolved.fallbacks.begin(), resolved.fallbacks.end(), hit->origin.url) == resolved.fallbacks.end()) {
        // The existing short filename hash is not enough to identify a URL.
        // Metadata from another URL must never authorize a cache hit or 304.
        return std::nullopt;
    }
    return hit;
}

[[nodiscard]] auto Trim(std::string_view text) -> std::string_view {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
        text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t'))
        text.remove_suffix(1);
    return text;
}

[[nodiscard]] auto EntityTag(std::string_view value) -> bool {
    if (value.starts_with("W/"))
        value.remove_prefix(2);
    if (value.size() < 2 || value.size() > 8192 || value.front() != '"' || value.back() != '"')
        return false;
    value.remove_prefix(1);
    value.remove_suffix(1);
    return std::all_of(value.begin(), value.end(), [](unsigned char c) { return c >= 0x21 && c != '"' && c != 0x7F; });
}

[[nodiscard]] auto SameEntityTag(std::string_view a, std::string_view b) -> bool {
    if (!EntityTag(a) || !EntityTag(b))
        return false;
    if (a.starts_with("W/"))
        a.remove_prefix(2);
    if (b.starts_with("W/"))
        b.remove_prefix(2);
    return a == b;
}

[[nodiscard]] auto SafeHeader(std::string_view text) -> bool {
    return text.size() <= 1024 && std::all_of(text.begin(), text.end(), [](unsigned char c) { return c >= 0x20 && c != 0x7F; });
}

[[nodiscard]] auto DeltaSeconds(std::string_view text) -> std::optional<std::chrono::seconds> {
    text = Trim(text);
    if (text.empty())
        return std::nullopt;
    if (text.size() >= 2 && text.front() == '"' && text.back() == '"') {
        text.remove_prefix(1);
        text.remove_suffix(1);
    }
    int64_t    value  = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc {} || result.ptr != text.data() + text.size() || value < 0)
        return std::nullopt;
    return std::chrono::seconds(value);
}

struct ResponsePolicy {
    CacheOrigin origin;
    bool        noStore = false;
};

[[nodiscard]] auto OriginFor(const HTTP::Response& response, const std::string& url, const CacheOrigin* previous = nullptr) -> ResponsePolicy {
    ResponsePolicy policy;
    if (previous != nullptr)
        policy.origin = *previous; // 304 can omit stored headers.
    policy.origin.url = url;
    if (const auto etag = response.FindHeader("ETag")) {
        policy.origin.etag = EntityTag(*etag) ? std::string(*etag) : std::string {};
    }
    if (const auto modified = response.FindHeader("Last-Modified")) {
        policy.origin.lastModified = SafeHeader(*modified) ? std::string(*modified) : std::string {};
    }
    // An ETag on the redirected resource is not a validator for the original
    // URL. Re-download redirected resources when due rather than alias tags.
    if (response.redirected) {
        policy.origin.etag.clear();
        policy.origin.lastModified.clear();
    }

    bool hasCacheControl = false;
    for (const auto& header: response.headers) {
        if (HTTP::SameFieldName(header.name, "Vary") && Trim(header.value) == "*")
            policy.noStore = true;
        if (!HTTP::SameFieldName(header.name, "Cache-Control"))
            continue;
        if (!hasCacheControl) {
            hasCacheControl              = true;
            policy.origin.mustRevalidate = false;
            policy.origin.maxAge.reset();
        }
        std::string_view value = header.value;
        while (!value.empty()) {
            // Quoted extension values can contain commas; they are not new
            // directives (e.g. private="x-foo,x-bar").
            size_t end    = 0;
            bool   quoted = false;
            for (; end < value.size(); ++end) {
                if (quoted && value[end] == '\\' && end + 1 < value.size()) {
                    ++end;
                    continue;
                }
                if (value[end] == '"')
                    quoted = !quoted;
                if (!quoted && value[end] == ',')
                    break;
            }
            if (quoted)
                policy.origin.mustRevalidate = true; // malformed: fail closed
            const auto directive = Trim(value.substr(0, end));
            const auto equal     = directive.find('=');
            const auto name      = Trim(directive.substr(0, equal));
            if (HTTP::SameFieldName(name, "no-store"))
                policy.noStore = true;
            if (HTTP::SameFieldName(name, "no-cache"))
                policy.origin.mustRevalidate = true;
            if (HTTP::SameFieldName(name, "max-age")) {
                const auto age       = equal == std::string_view::npos ? std::nullopt : DeltaSeconds(directive.substr(equal + 1));
                const auto lifetime  = age.value_or(std::chrono::seconds::zero());
                policy.origin.maxAge = policy.origin.maxAge ? std::min(*policy.origin.maxAge, lifetime) : lifetime;
            }
            if (end == value.size())
                break;
            value.remove_prefix(end + 1);
        }
    }
    if (const auto age = response.FindHeader("Age"); age && policy.origin.maxAge) {
        const auto elapsed   = DeltaSeconds(*age);
        policy.origin.maxAge = elapsed && *elapsed < *policy.origin.maxAge ? *policy.origin.maxAge - *elapsed : std::chrono::seconds::zero();
    }
    return policy;
}

[[nodiscard]] auto IsRedirect(int32_t status) -> bool {
    return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
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

    auto cached = forceRefresh ? std::nullopt : CacheCandidate(m_cache, resolved, check);
    if (cached && !cached->needsRevalidation) {
        FetchJob* job = MakeJob();
        job->fetcher  = this;
        job->isDone.store(true, std::memory_order::relaxed);
        job->result = FetchPayload {
            .data           = std::move(cached->data),
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

    FetchJob* job       = MakeJob();
    job->fetcher        = this;
    job->canonicalKey   = resolved.cacheFileName;
    job->cached         = std::move(cached);
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

    auto cached = CacheCandidate(m_cache, resolved, check);
    if (cached && !cached->needsRevalidation) {
        return FetchPayload {
            .data           = std::move(cached->data),
            .localCachePath = m_cache.Root() / resolved.cacheFileName,
            .fromCache      = true,
            .sourceUrl      = (m_cache.Root() / resolved.cacheFileName).string(),
        };
    }

    FetchJob job;
    job.fetcher        = this;
    job.canonicalKey   = resolved.cacheFileName;
    job.cached         = std::move(cached);
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
        bool conditional = job->cached && job->cached->origin.url == url && (EntityTag(job->cached->origin.etag) || !job->cached->origin.lastModified.empty());
        // At most one unconditional retry: a redirect, unusable 304, or a cache
        // entry evicted/replaced while the request was in flight needs bytes.
        for (int attempt = 0; attempt < 2; ++attempt) {
            if (job->cancelSignal.load(std::memory_order::relaxed)) {
                note("cancelled");
                lastErrorCode = HTTP::HTTPError::TransferFailed;
                break;
            }
            HTTP::Request req {
                .url = url,
                .headers =
                    {HTTP::Header {.name = "User-Agent", .value = std::string(kUserAgent)}, HTTP::Header {.name = "Accept", .value = std::string(kAcceptAny)},
                     HTTP::Header {.name = "Cache-Control", .value = "no-cache"}},
                .timeoutSeconds  = job->timeoutSeconds,
                .followRedirects = !conditional,
                .cancelSignal    = &job->cancelSignal,
            };
            const bool sentETag = conditional && EntityTag(job->cached->origin.etag);
            if (sentETag) {
                req.headers.push_back({.name = "If-None-Match", .value = job->cached->origin.etag});
            } else if (conditional) {
                req.headers.push_back({.name = "If-Modified-Since", .value = job->cached->origin.lastModified});
            }
            auto resp = HTTP::Fetch(req);
            if (!resp) {
                lastErrorCode   = resp.error();
                const Error err = resp.error();
                note(std::format("{}: {} ({})", url, err.Category(), err.Message()));
                break;
            }
            if (job->cancelSignal.load(std::memory_order::relaxed)) {
                note("cancelled");
                lastErrorCode = HTTP::HTTPError::TransferFailed;
                break;
            }
            if (conditional && IsRedirect(resp->statusCode)) {
                // Never forward a resource's validators to a new redirect target.
                conditional = false;
                continue;
            }
            if (resp->statusCode == 304) {
                bool matches = conditional;
                if (const auto etag = resp->FindHeader("ETag"); sentETag && etag) {
                    matches = SameEntityTag(*etag, job->cached->origin.etag);
                }
                if (matches) {
                    const auto policy = OriginFor(*resp, url, &job->cached->origin);
                    if (m_cache.MarkRevalidated(job->canonicalKey, *job->cached, policy.origin, !policy.noStore)) {
                        job->result = FetchPayload {
                            .data           = std::move(job->cached->data),
                            .localCachePath = policy.noStore ? std::filesystem::path {} : m_cache.Root() / job->canonicalKey,
                            .fromCache      = true,
                            .sourceUrl      = url,
                        };
                        job->cached.reset();
                        job->isDone.store(true, std::memory_order::release);
                        return;
                    }
                }
                if (conditional) {
                    conditional = false;
                    continue;
                }
                lastErrorCode = FetchError::HTTPStatus;
                note(std::format("{}: unsolicited HTTP 304 without a usable cached representation", url));
                break;
            }
            if (resp->statusCode == 404 || resp->statusCode == 410) {
                if (!job->cached || job->cached->origin.url.empty() || job->cached->origin.url == url) {
                    m_cache.Invalidate(job->canonicalKey);
                }
                lastErrorCode = resp->statusCode == 404 ? FetchError::NotFound : FetchError::HTTPStatus;
                note(std::format("{}: HTTP {}", url, resp->statusCode));
                break;
            }
            // A full GET must yield a complete representation, never a 206
            // partial response or an empty 204 mistaken for an asset.
            if (resp->statusCode != 200) {
                lastErrorCode = FetchError::HTTPStatus;
                note(std::format("{}: HTTP {}", url, resp->statusCode));
                break;
            }
            if (job->validator != nullptr && !job->validator(resp->body)) {
                lastErrorCode = FetchError::Rejected;
                note(std::format("{}: {} byte(s) the validator rejected", url, resp->body.size()));
                break;
            }

            const auto policy  = OriginFor(*resp, url);
            bool       writeOk = false;
            if (policy.noStore) {
                m_cache.Invalidate(job->canonicalKey);
            } else {
                writeOk = m_cache.WriteAtomic(job->canonicalKey, resp->body, policy.origin);
            }
            job->result = FetchPayload {
                .data           = std::move(resp->body),
                .localCachePath = writeOk ? m_cache.Root() / job->canonicalKey : std::filesystem::path {},
                .fromCache      = false,
                .sourceUrl      = url,
            };
            job->cached.reset();
            job->isDone.store(true, std::memory_order::release);
            return;
        }
        if (job->cancelSignal.load(std::memory_order::relaxed)) {
            break;
        }
    }

    job->cached.reset();
    job->lastError = detail.empty() ? std::string("no candidate answered") : std::move(detail);
    job->result    = std::unexpected(lastErrorCode);
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
