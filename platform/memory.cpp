// Copyright (c) 2026 IEX Matching Engine Project

#include "platform/memory.hpp"

#include <new>          // std::bad_alloc
#include <sys/mman.h>   // mmap, munmap

namespace iex::platform {

void* alloc_huge(std::size_t bytes) {
    // MAP_HUGETLB does not exist on XNU. Standard anonymous mmap is the only
    // option. TLB pressure is higher than on Linux for large pools; this is a
    // documented macOS limitation.
    void* ptr = ::mmap(nullptr, bytes,
                       PROT_READ | PROT_WRITE,
                       MAP_ANON | MAP_PRIVATE,
                       -1, 0);
    if (ptr == MAP_FAILED) throw std::bad_alloc{};
    return ptr;
}

void free_huge(void* ptr, std::size_t bytes) {
    ::munmap(ptr, bytes);
}

} // namespace iex::platform
