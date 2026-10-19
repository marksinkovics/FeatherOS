#include "kernel/config.h"
#include "kernel/arch/serial.h"
#include "kernel/exception.h"
#include "kernel/halt.h"
#include "kernel/kernel_runtime.h"
#include "kernel/klogger.h"
#include "kernel/memory/heap.h"
#include "kernel/memory/memory_map.h"
#include "kernel/memory/page_allocator.h"
#include "kernel/memory/virtual_memory.h"
#include "kernel/panic.h"
#if defined(FEATHEROS_KERNEL_TEST)
#include "kernel/kernel_test.h"
#endif

#include <stdint.h>
#include <stddef.h>
#include <limine.h>

// AArch64 uses revision 0 temporarily for the identity map needed by early PL011 MMIO.
#if defined(ARCH_aarch64)
__attribute__((used, section(".limine_requests"))) volatile LIMINE_BASE_REVISION(0);
#elif defined(ARCH_x86_64)
// Revision 3 is the latest revision supported by the x86-64 boot path.
__attribute__((used, section(".limine_requests"))) volatile LIMINE_BASE_REVISION(3);
#else
#error "Unsupported architecture: define ARCH_x86_64 or ARCH_aarch64"
#endif

// The Limine requests can be placed anywhere, but it is important that
// the compiler does not optimise them away, so, usually, they should
// be made volatile or equivalent, _and_ they should be accessed at least
// once or marked as used with the "used" attribute as done here.

namespace
{

__attribute__((used, section(".limine_requests"))) volatile limine_hhdm_request hhdm_request = {
    .id = LIMINE_HHDM_REQUEST, .revision = 0, .response = nullptr};

__attribute__((
    used, section(".limine_requests"))) volatile limine_framebuffer_request framebuffer_request = {
    .id = LIMINE_FRAMEBUFFER_REQUEST, .revision = 0, .response = nullptr};

__attribute__((used,
               section(".limine_requests"))) volatile limine_module_request initramfs_request = {
    .id = LIMINE_MODULE_REQUEST,
    .revision = 0,
    .response = nullptr,
    .internal_module_count = 0,
    .internal_modules = nullptr};

#if defined(ARCH_aarch64)
__attribute__((used, section(".limine_requests"))) volatile limine_dtb_request dtb_request = {
    .id = LIMINE_DTB_REQUEST, .revision = 0, .response = nullptr};
#endif

} // namespace

// Finally, define the start and end markers for the Limine requests.
// These can also be moved anywhere, to any .cpp file, as seen fit.

namespace
{

__attribute__((used, section(".limine_requests_start"))) volatile LIMINE_REQUESTS_START_MARKER;

__attribute__((used, section(".limine_requests_end"))) volatile LIMINE_REQUESTS_END_MARKER;

} // namespace

static FeatherOS::KLogger klogger;

// Extern declarations for global constructors array.
extern void (*__init_array[])();
extern void (*__init_array_end[])();

extern "C" void kmain()
{
    if (hhdm_request.response == nullptr)
    {
        FeatherOS::HaltForever();
    }

    FeatherOS::Serial::Init(hhdm_request.response->offset);
    if (LIMINE_BASE_REVISION_SUPPORTED == false)
    {
        FeatherOS::Panic("Unsupported Limine base revision");
    }

    const void* platform_data = nullptr;
#if defined(ARCH_aarch64)
    if (dtb_request.response == nullptr || dtb_request.response->dtb_ptr == nullptr)
    {
        FeatherOS::Panic("Limine did not provide the AArch64 device tree");
    }
    const auto* device_tree = static_cast<const uint8_t*>(dtb_request.response->dtb_ptr);
    const uint32_t device_tree_magic = (static_cast<uint32_t>(device_tree[0]) << 24) |
                                       (static_cast<uint32_t>(device_tree[1]) << 16) |
                                       (static_cast<uint32_t>(device_tree[2]) << 8) |
                                       device_tree[3];
    if (device_tree_magic != 0xD00DFEED)
    {
        FeatherOS::Panic("Limine provided an invalid AArch64 device tree");
    }
    platform_data = device_tree;
#endif

    if (!FeatherOS::Memory::InitializeMemoryMap())
    {
        FeatherOS::Panic("Limine memory map is missing or invalid");
    }
    FeatherOS::Memory::Summary memory_summary;
    if (!FeatherOS::Memory::GetSummary(&memory_summary))
    {
        FeatherOS::Panic("Limine memory map summary is unavailable");
    }
    if (!FeatherOS::Memory::InitializePageAllocator(hhdm_request.response->offset))
    {
        FeatherOS::Panic("Physical page-frame allocator initialization failed");
    }
    if (!FeatherOS::Memory::InitializeVirtualMemory(hhdm_request.response->offset))
    {
        FeatherOS::Panic("Virtual-memory initialization failed");
    }
    if (!FeatherOS::Memory::InitializeHeap())
    {
        FeatherOS::Panic("Kernel heap initialization failed");
    }

    FeatherOS::InitializeExceptions();

    for (size_t i = 0; &__init_array[i] != __init_array_end; i++)
    {
        __init_array[i]();
    }

    klogger.Init(hhdm_request.response->offset);
    klogger.Info("Limine transferred control to the kernel");

#if defined(FEATHEROS_KERNEL_TEST)
    FeatherOS::KernelTest::Run(platform_data);
#endif

    limine_framebuffer* framebuffer = nullptr;
    if (framebuffer_request.response != nullptr &&
        framebuffer_request.response->framebuffer_count > 0)
    {
        framebuffer = framebuffer_request.response->framebuffers[0];
    }

    limine_file* initramfs_module = nullptr;
    if (initramfs_request.response != nullptr && initramfs_request.response->module_count > 0 &&
        initramfs_request.response->modules != nullptr)
    {
        initramfs_module = initramfs_request.response->modules[0];
    }

    const FeatherOS::KernelRuntime::BootInfo boot_info = {
        .platform_data = platform_data,
        .hhdm_offset = hhdm_request.response->offset,
        .memory_summary = memory_summary,
        .framebuffer = framebuffer,
        .initramfs_data = initramfs_module == nullptr ? nullptr : initramfs_module->address,
        .initramfs_size = initramfs_module == nullptr ? 0 : initramfs_module->size,
    };
    FeatherOS::KernelRuntime::Run(klogger, boot_info);
}