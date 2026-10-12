#include "kernel/config.h"
#include "kernel/framebuffer/framebuffer.h"
#include "kernel/framebuffer/term.h"
#include "kernel/klogger.h"
#include "kernel/panic.h"

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

namespace
{

}

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

} // namespace

// Finally, define the start and end markers for the Limine requests.
// These can also be moved anywhere, to any .cpp file, as seen fit.

namespace
{

__attribute__((used, section(".limine_requests_start"))) volatile LIMINE_REQUESTS_START_MARKER;

__attribute__((used, section(".limine_requests_end"))) volatile LIMINE_REQUESTS_END_MARKER;

} // namespace

// Halt and catch fire function.
namespace
{

void hcf()
{
    for (;;)
    {
#if defined(ARCH_x86_64)
        asm("hlt");
#elif defined(ARCH_aarch64)
        asm("wfi");
#else
#error "Unsupported architecture halt instruction"
#endif
    }
}

} // namespace

static Framebuffer framebuffer;
static FeatherOS::Term term;
static FeatherOS::KLogger klogger;

// Extern declarations for global constructors array.
extern void (*__init_array[])();
extern void (*__init_array_end[])();

extern "C" void kmain()
{

    // Call global constructors.
    for (size_t i = 0; &__init_array[i] != __init_array_end; i++)
    {
        __init_array[i]();
    }

    if (hhdm_request.response == nullptr)
    {
        hcf();
    }

    klogger.Init(hhdm_request.response->offset);
    klogger.Info("Limine transferred control to the kernel");

    // Ensure the bootloader actually understands our base revision (see spec).
    if (LIMINE_BASE_REVISION_SUPPORTED == false)
    {
        FeatherOS::Panic("Unsupported Limine base revision");
    }

    bool framebuffer_ready = false;
    if (framebuffer_request.response != nullptr &&
        framebuffer_request.response->framebuffer_count > 0)
    {
        limine_framebuffer* l_framebuffer = framebuffer_request.response->framebuffers[0];
        framebuffer_ready = framebuffer.init(l_framebuffer);
    }

    if (framebuffer_ready)
    {
        term.Init(&framebuffer);
        klogger.AttachTerminal(&term);
        klogger.Info("Framebuffer console initialized");
    }
    else
    {
        klogger.Warning("No supported framebuffer; using serial console only");
    }

    klogger.Info("Kernel initialization complete");

    hcf();
}