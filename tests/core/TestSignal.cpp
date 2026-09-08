// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TestsFramework.hpp"
#include <Zahlen/Core/SignalManager.hpp>
#include <atomic>
#include <expected>

namespace {

struct SafeHandler ZHLN_ANNOTATION(ZHLN::SignalSafe {}) {
    void operator()(const ZHLN::SignalEvent&) const noexcept {
    }
};

struct UnsafeHandler {
    void operator()(const ZHLN::SignalEvent&) const noexcept {
    }
};

std::atomic<int> g_emptyCalls {0};

struct EmptyCounter ZHLN_ANNOTATION(ZHLN::SignalSafe {}) {
    void operator()(const ZHLN::SignalEvent&) const noexcept {
        g_emptyCalls.fetch_add(1, std::memory_order::relaxed);
    }
};

struct StatefulCounter ZHLN_ANNOTATION(ZHLN::SignalSafe {}) {
    std::atomic<int>* count = nullptr;

    void operator()(const ZHLN::SignalEvent&) const noexcept {
        if (count != nullptr) {
            count->fetch_add(1, std::memory_order::relaxed);
        }
    }
};

} // namespace

struct SignalTestSuite {
    struct Tests {
        std::expected<void, ZHLN::Error> annotation_traits() {
            static_assert(ZHLN::Reflect::TypeHasAnnotation<ZHLN::SignalSafe, SafeHandler>());
            static_assert(ZHLN::Reflect::FunctionHasAnnotation<ZHLN::SignalSafe, SafeHandler {}>());
            static_assert(ZHLN::AsyncSignalSafeCallable<SafeHandler, const ZHLN::SignalEvent&>);
            if constexpr (ZHLN::Reflect::ReflectionAvailable) {
                static_assert(!ZHLN::Reflect::TypeHasAnnotation<ZHLN::SignalSafe, UnsafeHandler>());
                static_assert(!ZHLN::AsyncSignalSafeCallable<UnsafeHandler, const ZHLN::SignalEvent&>);
            }
            return {};
        }

        std::expected<void, ZHLN::Error> enum_names() {
            ZHLN::Test::ExpectEq(ZHLN::Reflect::EnumToString(ZHLN::Signal::AccessViolation), "AccessViolation");
            ZHLN::Test::ExpectEq(ZHLN::Reflect::EnumToString(ZHLN::Signal::IllegalInstruction), "IllegalInstruction");
            ZHLN::Test::ExpectEq(ZHLN::Reflect::EnumCount<ZHLN::Signal>(), static_cast<size_t>(10));
            return {};
        }

        std::expected<void, ZHLN::Error> dispatch_empty_handler() {
            g_emptyCalls.store(0, std::memory_order::relaxed);
            const uint32_t id = ZHLN::SignalManager::RegisterSafeHandler<EmptyCounter {}>(ZHLN::Signal::User1);
            if (!ZHLN::Test::ExpectTrue(id != ZHLN::SignalManager::InvalidId)) {
                return {};
            }

            const ZHLN::SignalEvent ev {.signal = ZHLN::Signal::User1, .faultAddress = nullptr, .threadId = 1};
            ZHLN::SignalManager::Dispatch(ev);
            ZHLN::Test::ExpectEq(g_emptyCalls.load(std::memory_order::relaxed), 1);

            ZHLN::SignalManager::Unregister(id);
            ZHLN::SignalManager::Dispatch(ev);
            ZHLN::Test::ExpectEq(g_emptyCalls.load(std::memory_order::relaxed), 1);
            return {};
        }

        std::expected<void, ZHLN::Error> dispatch_stateful_handler() {
            std::atomic<int> count {0};
            StatefulCounter  handler {.count = &count};
            const uint32_t   id = ZHLN::SignalManager::RegisterHandler(ZHLN::Signal::User2, handler);
            if (!ZHLN::Test::ExpectTrue(id != ZHLN::SignalManager::InvalidId)) {
                return {};
            }

            const ZHLN::SignalEvent ev {.signal = ZHLN::Signal::User2, .faultAddress = nullptr, .threadId = 7};
            ZHLN::SignalManager::Dispatch(ev);
            ZHLN::Test::ExpectEq(count.load(std::memory_order::relaxed), 1);

            ZHLN::SignalManager::Unregister(id);
            return {};
        }

        std::expected<void, ZHLN::Error> install_is_idempotent() {
            ZHLN::SignalManager::Install();
            ZHLN::Test::ExpectTrue(ZHLN::SignalManager::IsInstalled());
            ZHLN::SignalManager::Install();
            ZHLN::Test::ExpectTrue(ZHLN::SignalManager::IsInstalled());
            ZHLN::SignalManager::Uninstall();
            ZHLN::Test::ExpectFalse(ZHLN::SignalManager::IsInstalled());
            return {};
        }
    };
};

auto RunSignalSuite() -> ZHLN::Test::TestStats {
    return ZHLN::Test::RunSuite<SignalTestSuite>();
}
