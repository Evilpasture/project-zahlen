// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// extras/RemoteAsset/AsyncAssetFetcher.hpp
//
// The background half of remote fetching: a download is a worker thread, and
// this class owns its whole lifecycle -- the start, the supersede, the reap,
// the publish, and the join at destruction. The caller's contract is three
// calls:
//
//     Request(url)   start one (frame thread, any frame)
//     Poll()         once per frame; reaps the workers that are done
//     Take(id)       move the finished payload out, once
//
// and the destructor: it asks every still-running worker to retire and joins
// it, so destroying the fetcher never leaves a joinable thread behind (which
// is a std::terminate, not a leak) and never blocks on a lock a worker still
// holds.
//
// Supersede: a Request for a URL that already has a transfer in flight stops
// that transfer -- it retires between candidates, because a libcurl transfer
// in progress is not interruptible, only outwaitable -- and starts the new
// one. The superseded transfer keeps the cache file it earned (the next pick
// of the model is a hit) and publishes to its own slot, which the caller no
// longer reads: each request's payload lives in its own slot, keyed by the id
// Request returned, so nothing needs a generation counter to keep an old
// result out of the new request's state.
//
// Memory: a payload sits in its slot from the moment the worker publishes it
// until Take moves it out. A request nobody Takes keeps its bytes until the
// fetcher is destroyed; that is the rule, and it is the reason Take exists at
// all.
//
// Threading: Request and Poll run on the frame thread. The workers touch
// shared state only under m_mutex, and they set their finished flag only
// after the last lock release, so a thread Poll or the destructor sees as
// finished has touched nothing it will touch again.

#pragma once

#include <RemoteAsset/DiskCache.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace ZHLN::Remote {

enum class FetchStatus : uint8_t {
    Idle,      // nothing for this id (or no such id), or taken
    Pending,   // a transfer or a cache check is in flight
    Succeeded, // the payload is in the slot
    Failed     // the error message is in the slot
};

struct FetchResult {
    bool                 fromCache = false; // true: served from the disk cache, no network
    std::string          sourceUrl;         // what served the bytes (a URL, or the cache file)
    std::vector<uint8_t> data;
    std::string          errorMessage;      // every candidate's failure, for the log and the HUD
};

class AsyncAssetFetcher
{
  public:
    // @p cache is copied (it is a directory name, and the workers each need
    // their own copy to write into). @p timeoutSeconds is the per-candidate
    // transfer budget; 60 is the value a model download wants.
    explicit AsyncAssetFetcher(DiskCache cache, uint32_t timeoutSeconds = 60);

    // Asks every in-flight worker to retire and joins it. A transfer already
    // inside libcurl runs to its own timeout, which is the one thing that can
    // delay destruction; a worker between candidates retires at the next one.
    ~AsyncAssetFetcher();

    AsyncAssetFetcher(const AsyncAssetFetcher&)            = delete;
    AsyncAssetFetcher& operator=(const AsyncAssetFetcher&) = delete;

    // Starts a download (or serves a cache hit, synchronously, on the calling
    // thread) and returns the request's id. @p validator decides whether the
    // bytes -- and any cached copy -- are what the caller will feed them to.
    // @p forceRefresh skips the cache read and re-fetches over the wire.
    uint32_t Request(std::string_view url, ValidatorFn validator = Validators::AnyNonEmpty, bool forceRefresh = false);

    // Call once per frame: joins the workers that have finished and keeps
    // them out of the list. A join that ever takes a moment is a transfer
    // that finished mid-frame, which is the frame's own clock, not the
    // network's.
    void Poll();

    [[nodiscard]] auto Status(uint32_t requestId) const -> FetchStatus;

    // Moves the finished payload out of the slot and takes the id back to
    // Idle. nullopt when the request is not finished, was not started, or was
    // already taken.
    [[nodiscard]] auto Take(uint32_t requestId) -> std::optional<FetchResult>;

  private:
    struct Job;

    DiskCache              m_cache;
    uint32_t               m_timeoutSeconds;
    mutable std::mutex     m_mutex;
    std::vector<std::unique_ptr<Job>> m_jobs;

    // Slot i is request i: its status, and the payload when it is finished.
    // Slot 0 is request 0; the vector grows by one per Request and is never
    // shrunk, so an id is a stable index for the fetcher's lifetime.
    struct Slot {
        FetchStatus                status = FetchStatus::Idle;
        std::optional<FetchResult> result;
        // A note the frame thread made while starting the request (a corrupt
        // cache file, removed) that the worker folds into the final report.
        std::string                warning;
    };
    std::vector<Slot>                  m_slots;
};

} // namespace ZHLN::Remote
