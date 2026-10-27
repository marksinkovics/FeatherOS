#include "command_line.h"

#include <featheros.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

namespace
{

// A pointer stored in data needs a RELATIVE relocation, so this only reads correctly if the
// kernel applied the image's relocations at its load address.
const char* const relocated_labels[] = {"relocated"};

} // namespace

constexpr char Help[] = "usage: aslr\n"
                        "Prints where this run's image, stack, arguments, and heap were placed;\n"
                        "run it twice to see address-space layout randomization.";

int main(int argc, char** argv)
{
    if (FeatherOS::User::HelpRequested(argc, argv))
    {
        puts(Help);
        return 0;
    }
    if (strcmp(relocated_labels[0], "relocated") != 0)
    {
        puts("aslr: relocations were not applied");
        return 1;
    }
    volatile int stack_marker = 0;
    printf("aslr: image=%lu stack=%lu args=%lu heap=%lu\n",
           static_cast<unsigned long>(reinterpret_cast<uintptr_t>(&relocated_labels)),
           static_cast<unsigned long>(reinterpret_cast<uintptr_t>(&stack_marker)),
           static_cast<unsigned long>(reinterpret_cast<uintptr_t>(featheros_arguments_page())),
           static_cast<unsigned long>(reinterpret_cast<uintptr_t>(sbrk(0))));
    return 0;
}
