#include "command_line.h"

#include <spawn.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

constexpr char Help[] = "usage: fanout <1-16>\n"
                        "Starts that many echo children at once, then waits for all of them.";

int main(int argc, char** argv)
{
    if (FeatherOS::User::HelpRequested(argc, argv))
    {
        puts(Help);
        return 0;
    }
    constexpr unsigned long long MaximumChildren = 16;
    unsigned long long count = 0;
    if (argc != 2 || !FeatherOS::User::ParseUnsigned(argv[1], &count) || count == 0 ||
        count > MaximumChildren)
    {
        puts("usage: fanout <1-16>");
        return 1;
    }

    char echo[] = "echo";
    char child_word[] = "child";
    char* const arguments[] = {echo, child_word, nullptr};
    pid_t children[MaximumChildren];
    for (unsigned long long index = 0; index < count; ++index)
    {
        if (posix_spawnp(&children[index], "echo", nullptr, nullptr, arguments, environ) != 0)
        {
            puts("fanout: spawn failed");
            return 1;
        }
    }
    for (unsigned long long index = 0; index < count; ++index)
    {
        int status = 0;
        if (waitpid(children[index], &status, 0) != children[index] || !WIFEXITED(status) ||
            WEXITSTATUS(status) != 0)
        {
            puts("fanout: child failed");
            return 1;
        }
    }
    printf("fanout: %llu children exited\n", count);
    return 0;
}
