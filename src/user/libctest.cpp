#include "runtime.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <setjmp.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

using FeatherOS::User::WriteLine;
using FeatherOS::User::WriteNumber;
using FeatherOS::User::WriteText;

namespace
{

constexpr char Help[] =
    "usage: libctest one \"two words\"\n"
    "Checks the C library: startup and arguments, constructors, string.h, ctype.h,\n"
    "stdlib.h, errno, setjmp, exit handlers, the console descriptors, stat, process\n"
    "IDs, and the environment. It asks for one line of input at a \"> \" prompt.";

#if defined(__x86_64__)
constexpr uintptr_t KernelAddress = 0xFFFF800000000000;
#elif defined(__aarch64__)
constexpr uintptr_t KernelAddress = 0xFFFF000000000000;
#endif

uint64_t passed_checks = 0;
bool constructor_ran = false;
bool second_handler_ran = false;
bool handlers_in_order = false;
bool static_object_destroyed = false;

void Check(bool passed, const char* name)
{
    if (!passed)
    {
        WriteText("libctest: failed ");
        WriteLine(name);
        exit(1);
    }
    ++passed_checks;
}

struct StaticObject
{
    StaticObject() : constructed(true) {}
    ~StaticObject()
    {
        static_object_destroyed = handlers_in_order;
    }
    bool constructed;
};

StaticObject static_object;

__attribute__((constructor)) void MarkConstructorRan()
{
    constructor_ran = true;
}

// Runs from .fini_array, after every atexit handler and static destructor.
__attribute__((destructor)) void ReportExitOrder()
{
    if (static_object_destroyed)
    {
        WriteLine("libctest: exit handlers ran in order");
    }
}

void FirstHandler()
{
    handlers_in_order = second_handler_ran;
}

void SecondHandler()
{
    second_handler_ran = true;
}

int CountCalls()
{
    static int calls = 0;
    return ++calls;
}

int LocalStatic()
{
    static const int value = CountCalls();
    return value;
}

void CheckArguments(int argc, char** argv)
{
    Check(argc == 3, "argc");
    Check(strcmp(argv[0], "libctest") == 0, "argv[0]");
    Check(strcmp(argv[1], "one") == 0, "argv[1]");
    Check(strcmp(argv[2], "two words") == 0, "quoted argv[2]");
    Check(argv[argc] == nullptr, "argv terminator");
    // The strings are writable copies, so strtok may split them in place.
    Check(strcmp(strtok(argv[2], " "), "two") == 0, "strtok on argv");
}

void CheckStartup()
{
    Check(static_object.constructed, "static constructor");
    Check(constructor_ran, "constructor attribute");
    Check(LocalStatic() == 1 && LocalStatic() == 1, "function-local static");
    Check(atexit(FirstHandler) == 0 && atexit(SecondHandler) == 0, "atexit");
}

void CheckMemory()
{
    char buffer[16];
    Check(memset(buffer, 'x', 4) == buffer && buffer[3] == 'x', "memset");
    Check(memcpy(buffer, "abcdef", 7) == buffer && strcmp(buffer, "abcdef") == 0, "memcpy");
    memmove(buffer + 2, buffer, 4);
    Check(strcmp(buffer, "ababcd") == 0, "memmove forward overlap");
    memmove(buffer, buffer + 2, 5);
    Check(strcmp(buffer, "abcd") == 0, "memmove backward overlap");
    Check(memcmp("abc", "abd", 3) < 0 && memcmp("abd", "abc", 3) > 0, "memcmp order");
    Check(memcmp("abc", "abd", 2) == 0, "memcmp prefix");
    Check(memcmp("\xff", "\x01", 1) > 0, "memcmp unsigned bytes");
    const char bytes[] = {'h', 'e', 'l', 'l', 'o'};
    Check(memchr(bytes, 'l', sizeof(bytes)) == bytes + 2 &&
              memchr(bytes, 'z', sizeof(bytes)) == nullptr,
          "memchr");
}

void CheckStrings()
{
    char buffer[16];
    Check(strlen("") == 0 && strlen("feather") == 7, "strlen");
    Check(strnlen("feather", 3) == 3 && strnlen("os", 8) == 2, "strnlen");
    Check(strcpy(buffer, "os") == buffer && strcmp(buffer, "os") == 0, "strcpy");
    memset(buffer, 'x', sizeof(buffer));
    strncpy(buffer, "ab", 5);
    Check(buffer[1] == 'b' && buffer[2] == '\0' && buffer[4] == '\0' && buffer[5] == 'x',
          "strncpy pads");
    strcpy(buffer, "feather");
    Check(strcmp(strcat(buffer, "os"), "featheros") == 0, "strcat");
    strcpy(buffer, "ab");
    Check(strcmp(strncat(buffer, "cdef", 2), "abcd") == 0, "strncat");
    Check(strcmp("a", "b") < 0 && strcmp("b", "a") > 0 && strcmp("a", "a") == 0, "strcmp");
    Check(strcmp("ab", "abc") < 0, "strcmp prefix");
    Check(strncmp("abcx", "abcy", 3) == 0 && strncmp("abcx", "abcy", 4) < 0, "strncmp");

    const char path[] = "/home/docs/plan.txt";
    Check(strchr(path, 'o') == path + 2 && strchr(path, 'z') == nullptr, "strchr");
    Check(strchr(path, '\0') == path + strlen(path), "strchr terminator");
    Check(strrchr(path, '/') == path + 10, "strrchr");
    Check(strstr(path, "docs") == path + 6 && strstr(path, "nope") == nullptr, "strstr");
    Check(strstr(path, "") == path, "strstr empty pattern");
    Check(strspn("  \tword", " \t") == 3 && strcspn("key=value", "=") == 3, "strspn/strcspn");
    Check(strpbrk(path, ".") == path + 15 && strpbrk(path, "!?") == nullptr, "strpbrk");

    char line[] = " ls  -l /home ";
    Check(strcmp(strtok(line, " "), "ls") == 0, "strtok first");
    Check(strcmp(strtok(nullptr, " "), "-l") == 0, "strtok second");
    Check(strcmp(strtok(nullptr, " "), "/home") == 0, "strtok last");
    Check(strtok(nullptr, " ") == nullptr, "strtok end");
    char pairs[] = "a=1;b=2";
    char* state = nullptr;
    Check(strcmp(strtok_r(pairs, ";", &state), "a=1") == 0 &&
              strcmp(strtok_r(nullptr, ";", &state), "b=2") == 0 &&
              strtok_r(nullptr, ";", &state) == nullptr,
          "strtok_r");
    Check(strcmp(strerror(ENOENT), "No such file or directory") == 0, "strerror");
    Check(strcmp(strerror(9999), "Unknown error") == 0, "strerror unknown");
}

void CheckCharacterClasses()
{
    Check(isalpha('a') && isalpha('Z') && !isalpha('1') && !isalpha(-1), "isalpha");
    Check(isdigit('7') && !isdigit('a'), "isdigit");
    Check(isxdigit('F') && isxdigit('a') && !isxdigit('g'), "isxdigit");
    Check(isalnum('q') && isalnum('3') && !isalnum('_'), "isalnum");
    Check(isspace(' ') && isspace('\t') && isspace('\n') && !isspace('x'), "isspace");
    Check(isblank('\t') && !isblank('\n'), "isblank");
    Check(isupper('Q') && !isupper('q') && islower('q') && !islower('Q'), "isupper/islower");
    Check(ispunct('!') && !ispunct('a') && !ispunct(' '), "ispunct");
    Check(isprint(' ') && !isprint('\n') && isgraph('~') && !isgraph(' '), "isprint/isgraph");
    Check(iscntrl('\n') && iscntrl(0x7F) && !iscntrl('a'), "iscntrl");
    Check(tolower('A') == 'a' && toupper('z') == 'Z' && toupper('1') == '1', "case mapping");
}

void CheckIntegerParsing()
{
    char* end = nullptr;
    const char signed_text[] = "  -42xyz";
    Check(strtol(signed_text, &end, 10) == -42 && end == signed_text + 5, "strtol sign and end");
    Check(strtol("0x1F", nullptr, 0) == 31 && strtol("017", nullptr, 0) == 15 &&
              strtol("19", nullptr, 0) == 19,
          "strtol base 0");
    Check(strtol("z", nullptr, 36) == 35 && strtol("-101", nullptr, 2) == -5, "strtol bases");
    const char bare_prefix[] = "0x";
    Check(strtol(bare_prefix, &end, 16) == 0 && end == bare_prefix + 1, "strtol bare 0x");
    const char no_digits[] = "abc";
    Check(strtol(no_digits, &end, 10) == 0 && end == no_digits, "strtol no digits");
    errno = 0;
    Check(strtol("9223372036854775808", nullptr, 10) == LONG_MAX && errno == ERANGE,
          "strtol overflow");
    errno = 0;
    Check(strtol("-9223372036854775808", nullptr, 10) == LONG_MIN && errno == 0, "strtol minimum");
    errno = 0;
    Check(strtol("1", nullptr, 1) == 0 && errno == EINVAL, "strtol invalid base");
    Check(strtoul("-1", nullptr, 10) == ULONG_MAX, "strtoul negation");
    errno = 0;
    Check(strtoul("18446744073709551616", nullptr, 10) == ULONG_MAX && errno == ERANGE,
          "strtoul overflow");
    Check(strtoull("ffffffffffffffff", nullptr, 16) == ULLONG_MAX, "strtoull hex");
    Check(strtoll("-77", nullptr, 8) == -63, "strtoll octal");
    Check(atoi("123") == 123 && atoi(" -7") == -7 && atol("40") == 40 && atoll("-5") == -5,
          "atoi family");
}

void CheckArithmetic()
{
    Check(abs(-5) == 5 && labs(-6L) == 6 && llabs(-7LL) == 7 && abs(3) == 3, "abs family");
    const div_t quotient = div(7, -2);
    Check(quotient.quot == -3 && quotient.rem == 1, "div truncates");
    const ldiv_t long_quotient = ldiv(-9L, 4L);
    Check(long_quotient.quot == -2 && long_quotient.rem == -1, "ldiv");
    srand(5);
    const int first = rand();
    srand(5);
    Check(rand() == first && first >= 0 && first <= RAND_MAX, "srand repeats rand");
}

int CompareInts(const void* left, const void* right)
{
    const int a = *static_cast<const int*>(left);
    const int b = *static_cast<const int*>(right);
    return (a > b) - (a < b);
}

struct Entry
{
    char key;
    int value;
};

int CompareEntries(const void* left, const void* right)
{
    return static_cast<const Entry*>(left)->key - static_cast<const Entry*>(right)->key;
}

void CheckSorting()
{
    int numbers[] = {5, 3, 9, 1, 3, 0, -2, 8, 7};
    const int sorted[] = {-2, 0, 1, 3, 3, 5, 7, 8, 9};
    qsort(numbers, 9, sizeof(int), CompareInts);
    Check(memcmp(numbers, sorted, sizeof(sorted)) == 0, "qsort ints");
    Entry entries[] = {{'d', 4}, {'a', 1}, {'c', 3}, {'b', 2}};
    qsort(entries, 4, sizeof(Entry), CompareEntries);
    Check(entries[0].value == 1 && entries[3].value == 4, "qsort structs");
    const int present = 7;
    const int missing = 4;
    Check(bsearch(&present, numbers, 9, sizeof(int), CompareInts) == &numbers[6], "bsearch found");
    Check(bsearch(&missing, numbers, 9, sizeof(int), CompareInts) == nullptr, "bsearch missing");
    qsort(numbers, 0, sizeof(int), CompareInts);
}

jmp_buf jump_target;

void JumpFromInner()
{
    longjmp(jump_target, 7);
}

void JumpFromOuter()
{
    char frame[64];
    memset(frame, 0, sizeof(frame));
    JumpFromInner();
}

void CheckJumps()
{
    volatile int passes = 0;
    const int first = setjmp(jump_target);
    ++passes;
    if (first == 0)
    {
        longjmp(jump_target, 0);
    }
    Check(first == 1 && passes == 2, "longjmp with 0 returns 1");

    const int second = setjmp(jump_target);
    if (second == 0)
    {
        JumpFromOuter();
    }
    Check(second == 7, "longjmp out of nested calls");
}

bool WriteAll(int descriptor, const char* text)
{
    const auto length = static_cast<ssize_t>(strlen(text));
    return write(descriptor, text, strlen(text)) == length;
}

void CheckConsoleDescriptors()
{
    Check(write(STDOUT_FILENO, "", 0) == 0, "empty write");
    Check(WriteAll(STDOUT_FILENO, "libctest: standard output works\n"), "write to stdout");
    Check(WriteAll(STDERR_FILENO, "libctest: standard error works\n"), "write to stderr");

    // The smoke test types a line at this prompt. A short read leaves the rest of the line for
    // the next one.
    Check(WriteAll(STDOUT_FILENO, "> "), "write the prompt");
    char line[64];
    Check(read(STDIN_FILENO, line, 5) == 5 && memcmp(line, "typed", 5) == 0, "short console read");
    const ssize_t rest = read(STDIN_FILENO, line, sizeof(line));
    Check(rest == 12 && memcmp(line, " into stdin\n", 12) == 0, "rest of the console line");

    struct stat status;
    Check(fstat(STDOUT_FILENO, &status) == 0 && S_ISCHR(status.st_mode), "fstat on stdout");
    errno = 0;
    Check(read(99, line, 1) == -1 && errno == EBADF, "read from a bad descriptor");
    errno = 0;
    Check(write(STDOUT_FILENO, reinterpret_cast<const void*>(KernelAddress), 4) == -1 &&
              errno == EFAULT,
          "write from kernel memory");
    Check(close(STDIN_FILENO) == 0, "close stdin");
    errno = 0;
    Check(read(STDIN_FILENO, line, 1) == -1 && errno == EBADF, "read a closed stdin");
    errno = 0;
    Check(close(STDIN_FILENO) == -1 && errno == EBADF, "close stdin twice");
}

void CheckFileStatus()
{
    struct stat status;
    Check(stat("/system/hello.txt", &status) == 0 && S_ISREG(status.st_mode) &&
              status.st_size == 29 && (status.st_mode & 0222) == 0,
          "stat a read-only file");
    Check(stat("/home", &status) == 0 && S_ISDIR(status.st_mode) && (status.st_mode & 0200) != 0,
          "stat a directory");
    errno = 0;
    Check(stat("/no/such/file", &status) == -1 && errno == ENOENT, "stat a missing file");
    errno = 0;
    Check(fstat(42, &status) == -1 && errno == EBADF, "fstat a bad descriptor");
}

void CheckProcessIds()
{
    const pid_t self = getpid();
    const pid_t parent = getppid();
    Check(self > 0 && parent > 0 && self != parent, "getpid and getppid");
}

bool EnvironHas(const char* entry)
{
    for (char** cursor = environ; *cursor != nullptr; ++cursor)
    {
        if (strcmp(*cursor, entry) == 0)
        {
            return true;
        }
    }
    return false;
}

void CheckEnvironment()
{
    // init.conf sets these, and the shell passes them on.
    const char* home = getenv("HOME");
    Check(home != nullptr && strcmp(home, "/home") == 0, "inherited HOME");
    Check(EnvironHas("PATH=/system"), "inherited PATH in environ");
    Check(getenv("NOT_SET") == nullptr, "missing variable");

    Check(setenv("FEATHER", "os", 0) == 0 && strcmp(getenv("FEATHER"), "os") == 0, "setenv");
    Check(setenv("FEATHER", "kept", 0) == 0 && strcmp(getenv("FEATHER"), "os") == 0,
          "setenv without overwrite");
    Check(setenv("FEATHER", "light", 1) == 0 && strcmp(getenv("FEATHER"), "light") == 0 &&
              EnvironHas("FEATHER=light") && !EnvironHas("FEATHER=os"),
          "setenv with overwrite");
    Check(setenv("HOME", getenv("HOME"), 1) == 0 && strcmp(getenv("HOME"), "/home") == 0,
          "setenv to its own value");
    errno = 0;
    Check(setenv("BAD=NAME", "x", 1) == -1 && errno == EINVAL, "setenv with '=' in the name");
    errno = 0;
    Check(setenv("", "x", 1) == -1 && errno == EINVAL, "setenv with an empty name");
    static char large[1100];
    memset(large, 'x', sizeof(large) - 1);
    errno = 0;
    Check(setenv("LARGE", large, 1) == -1 && errno == ENOMEM, "setenv past the size limit");
    Check(unsetenv("FEATHER") == 0 && getenv("FEATHER") == nullptr && EnvironHas("PATH=/system"),
          "unsetenv");
    Check(unsetenv("FEATHER") == 0, "unsetenv a missing variable");
}

} // namespace

int main(int argc, char** argv)
{
    if (argc == 2 && (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0))
    {
        WriteLine(Help);
        return 0;
    }
    CheckArguments(argc, argv);
    CheckStartup();
    CheckMemory();
    CheckStrings();
    CheckCharacterClasses();
    CheckIntegerParsing();
    CheckArithmetic();
    CheckSorting();
    CheckJumps();
    CheckConsoleDescriptors();
    CheckFileStatus();
    CheckProcessIds();
    CheckEnvironment();
    WriteText("libctest: ");
    WriteNumber(passed_checks);
    WriteLine(" checks passed");
    return 0;
}
