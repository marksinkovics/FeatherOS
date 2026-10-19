#ifndef FEATHEROS_KERNEL_RUNTIME_H
#define FEATHEROS_KERNEL_RUNTIME_H

#include "kernel/memory/memory_map.h"

#include <stdint.h>

struct limine_framebuffer;

namespace FeatherOS
{

class KLogger;

namespace KernelRuntime
{

struct BootInfo
{
    const void* platform_data;
    uint64_t hhdm_offset;
    Memory::Summary memory_summary;
    limine_framebuffer* framebuffer;
    const void* initramfs_data;
    uint64_t initramfs_size;
};

[[noreturn]] void Run(KLogger& logger, const BootInfo& boot_info);

} // namespace KernelRuntime
} // namespace FeatherOS

#endif