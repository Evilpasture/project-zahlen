# CDN cache freshness, integrity, and retention

`CDNManager::Fetch`, `Load`, and `RequestAsset` share the same
`Remote::DiskCache` and HTTP revalidation path. Existing callers get automatic
management without invalidation calls, cleanup polling, or a frame-loop hook.

These are three separate checks:

1. **Origin freshness:** has the representation at this URL changed?
2. **Local integrity:** are these still the complete bytes that were downloaded?
3. **Disk retention:** how long, and how much, may the cache keep?

## Defaults

| Setting | Default | Meaning |
| --- | --- | --- |
| `revalidateAfter` | Every request in `ZHLN_DEV_MODE`; 5 minutes otherwise | Maximum local reuse interval before checking the origin. Server constraints can shorten it. |
| `maxAge` | 7 days | Maximum age since the **body download**, persisted as the body's modification time. Neither a local hit nor a 304 renews this retention age. |
| `maxBytes` | 1 GiB | Budget per cache directory, including bodies **and metadata**. Oldest downloads are evicted first. |
| `cleanupInterval` | 5 minutes | Minimum interval between full-directory sweeps triggered by reads or `Exists`. |

## Updating an asset at the same URL

On the next request, a development build contacts the server even if the local
file is recent and passes its format validator:

- Prefer `If-None-Match` with the stored ETag; otherwise use
  `If-Modified-Since` with Last-Modified.
- A valid **304 Not Modified** reuses the verified bytes without rewriting the
  body. Only its last-origin-check metadata is renewed.
- A **200 OK** is validated, replaces the cached body, and stores the new
  validators and whole-body checksum.
- With no usable validators, download normally rather than treating a
  format-valid old file as current. Legacy files without metadata likewise get
  an unconditional download before being trusted by the fetcher.

Origin checks send `Cache-Control: no-cache`. Response `no-cache` forces another
check on every request; `max-age` (reduced by `Age`) can shorten the local
interval. `no-store` and `Vary: *` prevent persistence. A 304 can update storage
policy too. This is an asset cache, **not a full browser/RFC HTTP cache**:
`Expires`, general `Vary`-keyed variants, heuristic freshness, and stale-on-error
serving are not implemented. Failed checks are errors, not stale successes.

Validators belong to a specific resolved URL, never to another fallback URL.
Conditional requests do not automatically follow redirects. A redirect is
retried without validators, and redirected responses do not provide reusable
validators for the original URL. This conservatively re-downloads redirected
assets when due. Only complete 200 responses can install new bodies; partial
206 and unsolicited 304 responses are rejected.

Before accepting a 304, the cache checks that the saved snapshot still matches
its on-disk body and metadata. Eviction, corruption, or a concurrent replacement
causes one unconditional retry, not a false hit or a missing returned file.
`AsyncAssetFetcher::Request(..., forceRefresh=true)` always downloads
unconditionally. Revalidation for async requests runs on the existing worker;
fresh local hits still complete synchronously.

**Server limitations matter:** ETags are opaque server assertions, not
necessarily digests. Prefer strong, content-dependent ETags for development.
Last-Modified has second-level granularity and can miss rapid same-size edits;
a client cannot discover changed content from an incorrectly unchanged server
validator. Detection happens on a subsequent fetch request, not via a file
watcher or push notification.

## What content validation means

Each HTTP-backed entry has a bounded, versioned `.meta` sidecar containing its
source URL, validators, freshness constraints, body size, and a **whole-body
FNV-1a 64-bit checksum**. Reads check that association before trusting the bytes,
including same-length edits that still pass a GLB header check. Malformed or
mismatched metadata makes the entry a miss and removes the invalid pair.

This checksum detects accidental local corruption; it is **not cryptographic
authentication** or a trusted publisher's digest. `ValidatorFn` still supplies
format/application checks. The built-in `IsGLB` checks the container header and
length, not all glTF semantics; `AnyNonEmpty` only checks non-emptiness. Use the
format parser and, if required, a trusted digest/signature for stronger checks.

`DiskCache::Read` is a local read, not a network operation. `ReadEntry` supplies
the freshness decision to the fetcher. Direct `WriteAtomic` calls without an
origin retain their local-file semantics, clear old HTTP metadata, and do not
establish origin freshness.

This change does not invalidate already-imported prefab/GPU objects. For
example, `LoadGLBPrefabFromMemory` has its own virtual-path-keyed prefab cache;
receiving changed bytes does not automatically replace that in-memory object.
Live asset hot reload belongs to that application/import layer, not this disk
cache.

## Retention and configuration

The cache sweeps on open and after successful commits, regardless of the sweep
interval. Every lookup checks the requested body's retention age even when a
full sweep is not due. Expired, empty, short, corrupt, or validator-rejected
entries are removed. Bodies and sidecars are evicted together, and orphan
metadata is cleaned up.

Cleanup is **opportunistic**: an idle or closed application does not delete
files on a timer. The next cache operation/open catches up. Reading a hit does
not renew its body mtime. A write temporarily needs space for staging as well
as the previous entry; abandoned staging files are reclaimed after 24 hours.

No configuration is required for the defaults. To override them:

```cpp
#include <CDN/CDN.hpp>
#include <chrono>

const ZHLN::CDN::CDNConfig config {
    .baseURL = "https://cdn.example.com/assets",
    .cachePolicy = {
        .maxAge = std::chrono::hours(48),
        .maxBytes = 512ULL * 1024ULL * 1024ULL,
        .cleanupInterval = std::chrono::minutes(2),
        .revalidateAfter = std::chrono::seconds::zero(), // always check, in any build
    },
};
auto cdn = ZHLN::CDN::CDNManager::Create(config);
```

`maxAge <= 0` disables body-age expiry; `maxBytes == 0` disables the budget.
Disabling both explicitly opts into unbounded retention. In contrast,
`revalidateAfter <= 0` means **always check**, not disable checking.
`cleanupInterval <= 0` sweeps on every lookup. Large durations are compared
without file-clock overflow. Future body timestamps expire when retention is
enabled; future metadata timestamps require another origin check.

The same policy can be passed directly to `Remote::DiskCache(directory, policy)`
when using `Remote::AsyncAssetFetcher` without the CDN wrapper.

## Ownership and safety

- Use a **dedicated cache directory**: all top-level regular files are managed.
  The CDN default is `<engine cache dir>/cdn`; direct DiskCache defaults to
  `<engine cache dir>/http`, also for an empty root, never the working directory.
  Maintenance does not follow symlinks or subdirectories.
- Keys are plain filenames from `Remote::ResolveURL`. Traversal, nested paths,
  and reserved `.meta`/`.tmp-` bookkeeping names are refused.
- Legacy `.glb` to `.bin` migration preserves download time and any metadata.
  Explicit `Invalidate` removes both spellings and their sidecars.
- Copies share state. Independent caches for the same canonical root serialize
  filesystem operations within the process, not network requests. Use
  consistent policies for a shared root; capacity is per directory, not host.
- Both files are staged before committing. Individual renames are atomic, but
  the pair is **not** a cross-process transaction. Its checksum/size association
  rejects torn generations. Failed commits are misses; filesystem cleanup and
  cross-process budget enforcement remain best effort.

`Load` and async payloads return owned bytes even when storage fails, the entry
exceeds the budget, or the server says `no-store`. Async `localCachePath` is
then empty. `Fetch`, which promises a persisted path, returns
`CDNError::CacheWrite` instead. The HTTP per-body size limit still applies.

Returned paths are **cache-owned**, not durable storage: later operations can
evict them. Use `Load`/the payload's `data`, or copy a file that must outlive
cache management. No normal fetch requires manual invalidation or maintenance.

## Regression tests

`TestRemoteAsset` and `TestCDN` cover retention and budgets, whole-body integrity,
bounded metadata, same-URL edits, ETag/Last-Modified/no-validator behavior,
restart persistence, 304 races and retries, server cache constraints, redirect
safety, errors, and blocking/async fetches. Expiry fixtures backdate timestamps;
mutable loopback resources simulate edits without external services or TTL
sleeps. Run both normal and `ZHLN_DEV_MODE` builds to cover the default policy.

With the project's normal toolchain and optional HTTP/CDN targets enabled:

```sh
cmake --build build --target TestRemoteAsset TestCDN
ctest --test-dir build -R '^(TestRemoteAsset|TestCDN)$' --output-on-failure
```
