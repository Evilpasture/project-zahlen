#pragma once
#include <cstddef>
#include <cstdint>

namespace ZHLN {

/**
 * @brief Zero-copy Memory Descriptor (Inspired by Py_buffer)
 */
struct BufferView {
	void* buf = nullptr;	 // Base memory address
	size_t len = 0;			 // Total size in bytes
	uint32_t itemsize = 0;	 // Bytes per element (e.g., sizeof(float))
	char format[4] = {0};	 // "f" (float), "d" (double), "I" (uint32)
	uint32_t ndim = 0;		 // Number of dimensions
	size_t shape[4] = {0};	 // Dimension sizes
	size_t strides[4] = {0}; // Bytes to skip to get to next element
};

} // namespace ZHLN