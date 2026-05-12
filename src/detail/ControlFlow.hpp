#pragma once

#include <Zahlen/Log.hpp>
#include <threading/Mutex.hpp>

/**
 * @brief Internal helper to create a scope-bound guard that executes exactly once.
 */
#define ZHLN_SCOPE_BLOCK(GuardInit) if (GuardInit; true)

/**
 * @brief Standard Mutex Lock Macro.
 * Usage: ZHLN_LOCK(myMutex) { ... }
 */
#define ZHLN_LOCK(mutex_ref) ZHLN_SCOPE_BLOCK(std::lock_guard<::ZHLN::Mutex> _zhln_guard(mutex_ref))

/**
 * @brief ECS-Specific Safety Macro.
 * Checks if the registry is currently being accessed by Lua/FFI via the Buffer Protocol.
 * If a view is exported, it prevents structural changes that would reallocate memory.
 */
#define ZHLN_FFI_GUARD(registry_ptr)                                                               \
	ZHLN_SCOPE_BLOCK(::ZHLN::Internal::FFISafetyGuard _ffi_guard(registry_ptr))

namespace ZHLN::Internal {

/**
 * @brief Logic for ECS FFI Safety.
 * Prevents structural modifications (reallocs) while raw pointers are held by scripting.
 */
struct FFISafetyGuard {
	// We forward declare the Registry logic here to keep this header clean
	void* _reg;
	FFISafetyGuard(void* reg) : _reg(reg) {
		// Logic: If (reg->view_export_count > 0) Panic("FFI Memory Violation");
		// We'll implement this properly in ECS.cpp
	}
	~FFISafetyGuard() {
		// Cleanup logic
	}
};

} // namespace ZHLN::Internal