#pragma once

// Use the host's allocator ABI and implementation. Never pass allocations
// between separately compiled Vulkan Memory Allocator implementations.
#undef VMA_IMPLEMENTATION
#include <string.h>  // Normally supplied by VMA's embedded implementation.
#include "../../../../vendor/rexglue-sdk/thirdparty/vulkan-memory-allocator/include/vk_mem_alloc.h"
