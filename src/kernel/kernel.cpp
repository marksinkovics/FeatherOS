#include "kernel/config.h"
#include "kernel/framebuffer/framebuffer.h"
#include "kernel/framebuffer/term.h"
#include "kernel/klogger.h"
#include "kernel/panic.h"

#include <stdint.h>
#include <stddef.h>
#include <limine.h>

// Set the base revision to 3, this is recommended as this is the latest
// base revision described by the Limine boot protocol specification.
// See specification for further info.

namespace
{

__attribute__((used, section(".limine_requests"))) volatile LIMINE_BASE_REVISION(3);

}

// The Limine requests can be placed anywhere, but it is important that
// the compiler does not optimise them away, so, usually, they should
// be made volatile or equivalent, _and_ they should be accessed at least
// once or marked as used with the "used" attribute as done here.

namespace
{

__attribute__((
    used, section(".limine_requests"))) volatile limine_framebuffer_request framebuffer_request = {
    .id = LIMINE_FRAMEBUFFER_REQUEST, .revision = 0, .response = nullptr};

}

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
#if defined(__x86_64__)
        asm("hlt");
#elif defined(__aarch64__) || defined(__riscv)
        asm("wfi");
#elif defined(__loongarch64)
        asm("idle 0");
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

    klogger.Init();
    klogger.Info("Limine transferred control to the kernel");

    // Ensure the bootloader actually understands our base revision (see spec).
    if (LIMINE_BASE_REVISION_SUPPORTED == false)
    {
        FeatherOS::Panic("Unsupported Limine base revision");
    }

    FEATHEROS_ASSERT(framebuffer_request.response != nullptr);
    FEATHEROS_ASSERT(framebuffer_request.response->framebuffer_count > 0);

    // Fetch the first framebuffer.
    limine_framebuffer* l_framebuffer = framebuffer_request.response->framebuffers[0];

    if (!framebuffer.init(l_framebuffer))
    {
        FeatherOS::Panic("Framebuffer mode is unsupported");
    }
    term.Init(&framebuffer);
    klogger.AttachTerminal(&term);
    klogger.Info("Framebuffer console initialized");
    klogger.Info("Kernel initialization complete");

    hcf();
}