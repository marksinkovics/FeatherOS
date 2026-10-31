#ifndef FEATHEROS_LIBC_HOSTS_FILE_H
#define FEATHEROS_LIBC_HOSTS_FILE_H

#include <stddef.h>
#include <stdint.h>

// The lines of /etc/hosts: an address, then its canonical name and any aliases, with "#" starting
// a comment.
namespace FeatherOS::LibC::Hosts
{

constexpr size_t MaximumNames = 8;

struct Name
{
    const char* text;
    size_t length;
};

struct Line
{
    // 4 or 16 bytes, in network order.
    size_t address_length;
    uint8_t address[16];
    // The canonical name first; names past MaximumNames are left out.
    size_t name_count;
    Name names[MaximumNames];
};

// The next line with a valid address and at least one name, skipping blank, comment and malformed
// lines; false at the end of the text. The names point into the text.
bool NextLine(const char** cursor, const char* end, Line* line);
// Names compare without regard to case, and a final dot on name is ignored.
bool Names(const Line& line, const char* name);

} // namespace FeatherOS::LibC::Hosts

#endif
