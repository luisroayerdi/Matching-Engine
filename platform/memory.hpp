#pragma once
// Copyright (c) 2026 IEX Matching Engine Project
// Anonymous memory allocation. macOS only.
// MAP_HUGETLB does not exist on XNU; alloc_huge() uses standard mmap.

#include <cstddef>

namespace iex::platform {

// Allocate `bytes` of anonymous read-write memory via mmap(MAP_ANON|MAP_PRIVATE).
// Returns a page-aligned pointer. Throws std::bad_alloc on failure.
[[nodiscard]] void* alloc_huge(std::size_t bytes);

// Release memory returned by alloc_huge.
// `bytes` must match the original allocation size.
void free_huge(void* ptr, std::size_t bytes);

} // namespace iex::platform
