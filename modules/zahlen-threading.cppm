module;

#include <atomic>
#include <concepts>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <queue>
#include <thread>
#include <type_traits>
#include <utility>

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#include <immintrin.h>
#endif

#include <Zahlen/Threading/Channel.hpp>
#include <Zahlen/Threading/ConditionalVariable.hpp>
#include <Zahlen/Threading/Mutex.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <Zahlen/Threading/Thread.hpp>

export module zahlen:threading;

export import :core;

export namespace ZHLN {
using ZHLN::Fiber;
using ZHLN::FiberFunc;
using ZHLN::GetCurrentFiber;
using ZHLN::YieldFiber;

using ZHLN::CPURelax;
using ZHLN::Mutex;
using ZHLN::MutexGuard;

using ZHLN::ConditionalVariable;

using ZHLN::Channel;

namespace TaskSystem {
using ZHLN::TaskSystem::Counter;
using ZHLN::TaskSystem::Dispatch;
using ZHLN::TaskSystem::GetWorkerCount;
using ZHLN::TaskSystem::GetWorkerIndex;
using ZHLN::TaskSystem::Init;
using ZHLN::TaskSystem::ParallelFor;
using ZHLN::TaskSystem::Shutdown;
using ZHLN::TaskSystem::Task;
using ZHLN::TaskSystem::TaskFn;
using ZHLN::TaskSystem::Wait;
using ZHLN::TaskSystem::WakeUp;
} // namespace TaskSystem
} // namespace ZHLN
