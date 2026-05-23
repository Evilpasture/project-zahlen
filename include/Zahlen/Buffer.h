#pragma once
#include <Zahlen/Common.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Numerical bits for the 'flags' field
typedef enum ZHLN_BufferFlags : uint8_t {
	ZHLN_BUFFER_CONTIGUOUS = 1 << 0,
	ZHLN_BUFFER_ALIGNED_16 = 1 << 1,
	ZHLN_BUFFER_ALIGNED_32 = 1 << 2, // Safe for AVX
	ZHLN_BUFFER_WRITABLE = 1 << 3,
} ZHLN_BufferFlags;

typedef enum ZHLN_OwnerType : uint8_t {
	ZHLN_OWNER_NONE = 0,
	ZHLN_OWNER_PHYSICS_WORLD = 1,
	ZHLN_OWNER_ECS_REGISTRY = 2
} ZHLN_OwnerType;

// This is the "Source of Truth" that LuaJIT will mirror
typedef struct ZHLN_BufferView {
	void* buf;		   // The starting address of the data
	void* obj;		   // The "owner" C++ object (e.g., PhysicsWorld*)
	size_t len;		   // Total size in bytes
	uint32_t itemsize; // Size of a single element

	char format[8]; // e.g., "d" (double), "T{fff}" (struct of 3 floats)
	int readonly;

	uint32_t ndim;
	size_t shape[4];
	size_t strides[4];

	uint32_t flags;
	uint32_t owner_type;
} ZHLN_BufferView;

// SoA Access
ZHLN_API ZHLN_BufferView ZHLN_GetPhysicsPositions(struct ZHLN_Engine* engine);
ZHLN_API ZHLN_BufferView ZHLN_GetPhysicsLinearVelocities(struct ZHLN_Engine* engine);

#ifdef __cplusplus
} // extern "C"

#include <type_traits>
namespace ZHLN {
// C++ alias
using BufferView = ::ZHLN_BufferView;
static_assert(std::is_trivial_v<BufferView>, "BufferView must be trivial for FFI safety!");
} // namespace ZHLN
#endif