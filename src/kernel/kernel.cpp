#include <stdint.h>
#include <stddef.h>
#include <stivale2.h>

#include "kernel/config.h"
#include "kernel/framebuffer/framebuffer.h"

static uint8_t stack[4096];

// We are now going to define a framebuffer header tag.
// This tag tells the bootloader that we want a graphical framebuffer instead
// of a CGA-compatible text mode. Omitting this tag will make the bootloader
// default to text mode, if available.
static struct stivale2_header_tag_framebuffer framebuffer_hdr_tag = {
    // Same as above.
    .tag = {
        .identifier = STIVALE2_HEADER_TAG_FRAMEBUFFER_ID,
        .next = 0
    },
    .framebuffer_width  = FeatherOS::Config::Width,
    .framebuffer_height = FeatherOS::Config::Height,
    .framebuffer_bpp    = FeatherOS::Config::Bpp,
    .unused             = 0
};

// The stivale2 specification says we need to define a "header structure".
// This structure needs to reside in the .stivale2hdr ELF section in order
// for the bootloader to find it. We use this __attribute__ directive to
// tell the compiler to put the following structure in said section.
__attribute__((section(".stivale2hdr"), used))
static struct stivale2_header stivale_hdr = {
    // The entry_point member is used to specify an alternative entry
    // point that the bootloader should jump to instead of the executable's
    // ELF entry point. We do not care about that so we leave it zeroed.
    .entry_point = 0,
    // Let's tell the bootloader where our stack is.
    // We need to add the sizeof(stack) since in x86(_64) the stack grows
    // downwards.
    .stack = (uintptr_t)stack + sizeof(stack),
    // Bit 1, if set, causes the bootloader to return to us pointers in the
    // higher half, which we likely want since this is a higher half kernel.
    // Bit 2, if set, tells the bootloader to enable protected memory ranges,
    // that is, to respect the ELF PHDR mandated permissions for the executable's
    // segments.
    // Bit 3, if set, enables fully virtual kernel mappings, which we want as
    // they allow the bootloader to pick whichever *physical* memory address is
    // available to load the kernel, rather than relying on us telling it where
    // to load it.
    // Bit 4 disables a deprecated feature and should always be set.
    .flags =  (1 << 1) | (1 << 2) | (1 << 3) | (1 << 4),
    // This header structure is the root of the linked list of header tags and
    // points to the first one in the linked list.
    .tags = (uintptr_t)&framebuffer_hdr_tag
};

// We will now write a helper function which will allow us to scan for tags
// that we want FROM the bootloader (structure tags).
void *stivale2_get_tag(struct stivale2_struct *stivale2_struct, uint64_t id) {
    struct stivale2_tag *current_tag = (stivale2_tag *)stivale2_struct->tags;
    for (;;) {
        // If the tag pointer is NULL (end of linked list), we did not find
        // the tag. Return NULL to signal this.
        if (current_tag == NULL) {
            return NULL;
        }

        // Check whether the identifier matches. If it does, return a pointer
        // to the matching tag.
        if (current_tag->identifier == id) {
            return current_tag;
        }

        // Get a pointer to the next tag in the linked list and repeat.
        current_tag = (stivale2_tag *)current_tag->next;
    }
}

static Framebuffer framebuffer;

extern "C" void _start(struct stivale2_struct *stivale2_struct) {

    struct stivale2_struct_tag_framebuffer* fb_tag = (struct stivale2_struct_tag_framebuffer*)stivale2_get_tag(stivale2_struct, STIVALE2_STRUCT_TAG_FRAMEBUFFER_ID);

    framebuffer.init(fb_tag);
    framebuffer.clear(FeatherOS::Color::White);
    framebuffer.drawRectangle(100, 100, 30, 30, FeatherOS::Color::Red);
    framebuffer.drawRectangle(150, 100, 30, 30, FeatherOS::Color::Green);
    framebuffer.drawRectangle(200, 100, 30, 30, FeatherOS::Color::Blue);
    framebuffer.drawRectangle(250, 100, 30, 30, FeatherOS::Color::Yellow);
    framebuffer.draw();

    // We're done, just hang...
    for (;;) {
        asm ("hlt");
    }
}
