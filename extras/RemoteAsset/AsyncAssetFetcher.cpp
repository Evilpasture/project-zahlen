// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// extras/RemoteAsset/AsyncAssetFetcher.cpp

#include <RemoteAsset/AsyncAssetFetcher.hpp>
#include <RemoteAsset/URLResolver.hpp>

#include <HTTP/HTTP.hpp>
#include <Zahlen/Error.hpp>

#include <format>
#include <stop_token>
#include <utility>

namespace ZHLN::Remote {

namespace
{

// The caller's identity on the wire, and the most permissive Accept: what the
// bytes are is the validator's question, not the transport's.
inline constexpr char kUserAgent[] = "project-zahlen";
inline constexpr char kAcceptAny[] = "*/*";

// How much failure history a single request may carry.
inline constexpr size_t kMaxDetailChars = 400;

struct Transfer {
    bool                 ok = false;
    std::vector<uint8_t> bytes;
    std::string          source; // which candidate answered
    std::string          detail; // every failure, for the log and the HUD
};

// One transfer, tried against each candidate in turn. An HTTP status is data
// and not an error (see HTTP.hpp), so a 404 is a reason to try the next
// candidate and a failed transfer is a reason to say what failed. Logging is
// left to the caller: this runs on the worker, and the frame loop reports the
// outcome once, in order, when it observes it.
[[nodiscard]] auto TransferAgainst(const std::vector<std::string>& candidates, ValidatorFn validator, uint32_t timeoutSeconds, const std::stop_token& stop) -> Transfer {
    Transfer outcome;
    const auto note = [&outcome](std::string text) -> void {
        if (outcome.detail.size() >= kMaxDetailChars) {
            return;
        }
        if (!outcome.detail.empty()) {
            outcome.detail += "; ";
        }
        outcome.detail += std::move(text);
    };

    for (const std::string& url: candidates) {
        if (stop.stop_requested()) {
            note("cancelled");
            break;
        }

        const ZHLN::HTTP::Request request {
            .url            = url,
            .headers        = {ZHLN::HTTP::Header {.name = "User-Agent", .value = std::string(kUserAgent)},
                               ZHLN::HTTP::Header {.name = "Accept", .value = std::string(kAcceptAny)}},
            .timeoutSeconds = timeoutSeconds,
        };

        auto response = ZHLN::HTTP::Fetch(request);
        if (!response) {
            const ZHLN::Error err = response.error();
            note(std::format("{}: {} ({})", url, err.Category(), err.Message()));
            continue;
        }
        if ((response->statusCode < 200) || (response->statusCode >= 300)) {
            note(std::format("{}: HTTP {}", url, response->statusCode));
            continue;
        }
        if ((validator == nullptr) || !validator(response->body)) {
            note(std::format("{}: {} byte(s) the validator rejected", url, response->body.size()));
            continue;
        }

        outcome.bytes  = std::move(response->body);
        outcome.source = url;
        outcome.ok     = true;
        break;
    }
    return outcome;
}

} // namespace

struct AsyncAssetFetcher::Job {
    std::string              url;
    std::string              cacheFileName;
    std::vector<std::string> candidates;
    DiskCache                cache;
    ValidatorFn              validator;
    uint32_t                 timeoutSeconds;
    uint32_t                 requestId;

    // A plain std::thread rather than a jthread on purpose: a superseded
    // download has to finish in the background, and a jthread's destructor
    // joins, which would stall the frame for the whole remaining transfer.
    // `stop` is the request to retire (the worker honours it between
    // candidates); `finished` is what Poll reaps on, and it is set only after
    // the worker has published, so a thread seen as finished has touched
    // nothing it will touch again.
    std::stop_source         stop;
    std::thread              thread;
    std::atomic<bool>        finished {false};
};

AsyncAssetFetcher::AsyncAssetFetcher(DiskCache cache, uint32_t timeoutSeconds)
: m_cache(std::move(cache)), m_timeoutSeconds(timeoutSeconds) {
}

AsyncAssetFetcher::~AsyncAssetFetcher() {
    {
        std::lock_guard lock(m_mutex);
        for (auto& job: m_jobs) {
            job->stop.request_stop();
        }
    }
    // Outside the lock: a worker that is finishing still takes the lock to
    // publish, and holding it here would wait on itself.
    for (auto& job: m_jobs) {
        if (job->thread.joinable()) {
            job->thread.join();
        }
    }
}

uint32_t AsyncAssetFetcher::Request(std::string_view url, ValidatorFn validator, bool forceRefresh) {
    std::lock_guard lock(m_mutex);

    const std::string urlCopy(url);

    // A pick that supersedes an in-flight download of the same URL does not
    // wait for it: the old job's stop is requested (it retires between
    // candidates), and it keeps the cache file it earns.
    for (auto& job: m_jobs) {
        if (job->url == urlCopy) {
            job->stop.request_stop();
        }
    }

    m_slots.push_back(Slot {});
    const uint32_t    id = static_cast<uint32_t>(m_slots.size() - 1);
    const ResolvedURL resolved = ResolveURL(urlCopy);

    // Cache first, network second. The cache read stays on the calling thread
    // -- it is a few megabytes from a local disk, and a hit means the asset is
    // available before this call returns. Only the transfer goes to a worker,
    // because that is the part that can take a minute.
    if (!forceRefresh) {
        if (auto hit = m_cache.Read(resolved.cacheFileName, validator)) {
            FetchResult result;
            result.fromCache = true;
            result.sourceUrl = (m_cache.Root() / resolved.cacheFileName).string();
            result.data      = std::move(*hit);
            m_slots[id].status = FetchStatus::Succeeded;
            m_slots[id].result = std::move(result);
            return id;
        }
        if (m_cache.Exists(resolved.cacheFileName)) {
            m_cache.Invalidate(resolved.cacheFileName);
            // The worker publishes the note out of the slot, so it lands in
            // the same one-line report as the transfer's own failures.
            m_slots[id].warning = std::format(
                "'{}' did not pass its validator; removed it", (m_cache.Root() / resolved.cacheFileName).string()
            );
        }
    }

    auto job = std::make_unique<Job>();
    job->url            = urlCopy;
    job->cacheFileName  = resolved.cacheFileName;
    job->candidates.push_back(std::move(resolved.primary));
    for (auto& fallback: resolved.fallbacks) {
        job->candidates.push_back(std::move(fallback));
    }
    job->cache          = m_cache;
    job->validator      = validator;
    job->timeoutSeconds = m_timeoutSeconds;
    job->requestId      = id;

    // The unique_ptr, not the Job itself: the job vector may reallocate under
    // the worker's feet, and a std::thread, unlike a jthread, starts its
    // callable with no arguments, so the token is captured rather than passed.
    m_jobs.push_back(std::move(job));
    Job* const jobPtr = m_jobs.back().get();

    m_slots[id].status = FetchStatus::Pending;
    const std::stop_token stop = jobPtr->stop.get_token();
    jobPtr->thread = std::thread([this, jobPtr, id, stop] -> void {
        const Transfer outcome = TransferAgainst(jobPtr->candidates, jobPtr->validator, jobPtr->timeoutSeconds, stop);

        FetchResult result;
        if (outcome.ok) {
            result.data      = std::move(outcome.bytes);
            result.sourceUrl = std::move(outcome.source);
            if (jobPtr->cache.WriteAtomic(jobPtr->cacheFileName, result.data)) {
                // From here on the honest source is the cache: that is what the
                // next run reads, and what a device-lost rebuild would
                // re-import.
                result.sourceUrl = (jobPtr->cache.Root() / jobPtr->cacheFileName).string();
            }
        } else {
            result.errorMessage = outcome.detail.empty() ? std::string("no candidate answered") : std::move(outcome.detail);
        }

        {
            std::lock_guard lock(this->m_mutex);
            Slot& slot = this->m_slots[id];
            if (!slot.warning.empty()) {
                result.errorMessage =
                    std::move(slot.warning) + (result.errorMessage.empty() ? std::string() : std::string("; ")) + std::move(result.errorMessage);
            }
            slot.status = outcome.ok ? FetchStatus::Succeeded : FetchStatus::Failed;
            slot.result = std::move(result);
        }
        // After the last lock release: the reap side may then join.
        jobPtr->finished.store(true, std::memory_order::release);
    });
    return id;
}

void AsyncAssetFetcher::Poll() {
    std::vector<Job*> done;
    {
        std::lock_guard lock(m_mutex);
        for (auto it = m_jobs.begin(); it != m_jobs.end();) {
            if (it->get()->finished.load(std::memory_order::acquire)) {
                done.push_back(it->get());
                it = m_jobs.erase(it);
            } else {
                ++it;
            }
        }
    }
    // Joins the downloads that are done, and only those. A join that ever
    // takes a moment is a transfer that finished mid-frame, which is the
    // frame's own clock, not the network's.
    for (Job* job: done) {
        if (job->thread.joinable()) {
            job->thread.join();
        }
    }
}

[[nodiscard]] auto AsyncAssetFetcher::Status(uint32_t requestId) const -> FetchStatus {
    std::lock_guard lock(m_mutex);
    if (requestId >= m_slots.size()) {
        return FetchStatus::Idle;
    }
    return m_slots[requestId].status;
}

[[nodiscard]] auto AsyncAssetFetcher::Take(uint32_t requestId) -> std::optional<FetchResult> {
    std::lock_guard lock(m_mutex);
    if (requestId >= m_slots.size()) {
        return std::nullopt;
    }
    Slot& slot = m_slots[requestId];
    if (!slot.result.has_value()) {
        return std::nullopt;
    }
    slot.status = FetchStatus::Idle;
    std::optional<FetchResult> out = std::move(slot.result);
    slot.result.reset();
    return out;
}

} // namespace ZHLN::Remote
