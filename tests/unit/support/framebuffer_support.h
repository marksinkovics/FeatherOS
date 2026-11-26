#ifndef FEATHEROS_TESTS_FRAMEBUFFER_SUPPORT_H
#define FEATHEROS_TESTS_FRAMEBUFFER_SUPPORT_H

#include <limine.h>

#include "kernel/framebuffer/framebuffer.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace FramebufferSupport
{

// The back buffer the kernel allocates at boot, here owned for the rest of the test run so that
// a Framebuffer never outlives it.
inline bool Init(Framebuffer& framebuffer, limine_framebuffer& descriptor)
{
    static std::vector<std::unique_ptr<std::vector<uint8_t>>> buffers;
    const size_t required = Framebuffer::RequiredBytes(&descriptor);
    buffers.push_back(std::make_unique<std::vector<uint8_t>>(required));
    return framebuffer.init(&descriptor, buffers.back()->data(), required);
}

} // namespace FramebufferSupport

#endif
