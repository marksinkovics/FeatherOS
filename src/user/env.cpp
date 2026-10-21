#include <string.h>
#include <unistd.h>

namespace
{

constexpr char Help[] = "usage: env\n"
                        "Prints the environment, one NAME=value per line. Programs inherit it\n"
                        "from the shell, which gets it from the env lines in /system/init.conf.\n";

void WriteText(const char* text)
{
    write(STDOUT_FILENO, text, strlen(text));
}

} // namespace

int main(int argc, char** argv)
{
    if (argc == 2 && (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0))
    {
        WriteText(Help);
        return 0;
    }
    for (char** entry = environ; *entry != nullptr; ++entry)
    {
        WriteText(*entry);
        WriteText("\n");
    }
    return 0;
}
