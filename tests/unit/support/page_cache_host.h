#ifndef FEATHEROS_TESTS_PAGE_CACHE_HOST_H
#define FEATHEROS_TESTS_PAGE_CACHE_HOST_H

#include <cstddef>
#include <cstdint>

// The kernel's page allocator and page cache over a static buffer, which every test process sets
// up before its first test: the filesystem tests mount volumes through the same cache the kernel
// uses.
namespace HostPageCache
{

constexpr std::size_t FrameTotal = 4096;

std::uint64_t HostOffset();
// The allocator keeps its state for the life of the process, so this may be called by any test.
bool EnsureAllocator();
bool EnsureCache();
// Drops every cached page, for tests that change the host disk below a volume that stays open.
void InvalidateAll();

} // namespace HostPageCache

#endif
