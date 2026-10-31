#include "syscall.h"

#include <arpa/inet.h>
#include <assert.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <featheros.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <sched.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/reboot.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

namespace
{

constexpr char Help[] =
    "usage: libctest one \"two words\"\n"
    "Checks the C library: startup and arguments, constructors, string.h, ctype.h,\n"
    "stdlib.h, errno, setjmp, exit handlers, the console descriptors, stat, process\n"
    "IDs, the environment, the heap, stdio, files and directories, /dev, /tmp,\n"
    "/proc, getnameinfo, time, posix_spawn with file actions, dup, fcntl, pipes,\n"
    "non-blocking pipes and loopback sockets, the FeatherOS extensions, and floating\n"
    "point. It asks for three lines of input\n"
    "at \"> \" prompts.";

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
        printf("libctest: failed %s\n", name);
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
        // Buffered, so this also checks that exit flushes stdout after the destructors.
        printf("libctest: exit handlers ran in order\n");
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

#if defined(__aarch64__)
// d8 is written behind the compiler's back: declaring it clobbered would make the compiler save
// and restore it around the asm, hiding a longjmp that does not.
void SetD8(uint64_t bits)
{
    asm volatile("fmov d8, %0" : : "r"(bits));
}

uint64_t GetD8()
{
    uint64_t bits;
    asm volatile("fmov %0, d8" : "=r"(bits));
    return bits;
}

[[noreturn]] void JumpWithD8Changed()
{
    SetD8(0x2222);
    longjmp(jump_target, 1);
}
#endif

// AArch64 callers expect d8-d15 to survive calls; x86-64 has no callee-saved vector registers.
bool LongjmpRestoresVectorRegisters()
{
#if defined(__aarch64__)
    const uint64_t original = GetD8();
    SetD8(0x1111);
    if (setjmp(jump_target) == 0)
    {
        JumpWithD8Changed();
    }
    const bool restored = GetD8() == 0x1111;
    SetD8(original);
    return restored;
#else
    return true;
#endif
}

void CheckJumps()
{
    volatile int passes = 0;
    const int first = setjmp(jump_target);
    passes = passes + 1;
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
    Check(LongjmpRestoresVectorRegisters(), "longjmp restores callee-saved vector registers");
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
    // The prompt has no newline, so it shows only because reading stdin flushes stdout.
    printf("> ");
    Check(fgets(line, sizeof(line), stdin) != nullptr && strcmp(line, "read by fgets\n") == 0,
          "fgets from stdin");
    printf("> ");
    int number = 0;
    char word[16];
    Check(scanf("%d %15s", &number, word) == 2 && number == 42 && strcmp(word, "answer") == 0,
          "scanf from stdin");

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
    // The console is one open file behind 0, 1 and 2, so a copy of stdout is stdin again.
    Check(dup2(STDOUT_FILENO, STDIN_FILENO) == STDIN_FILENO && isatty(STDIN_FILENO),
          "dup2 puts the console back on stdin");
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
    Check(EnvironHas("PATH=/system:/bin"), "inherited PATH in environ");
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
    Check(unsetenv("FEATHER") == 0 && getenv("FEATHER") == nullptr &&
              EnvironHas("PATH=/system:/bin"),
          "unsetenv");
    Check(unsetenv("FEATHER") == 0, "unsetenv a missing variable");
}

int live_objects = 0;

struct Counted
{
    Counted()
    {
        ++live_objects;
    }
    ~Counted()
    {
        --live_objects;
    }
    int value = 7;
};

uint64_t ProgramBreak()
{
    return FeatherOS::User::ExtendHeap(0);
}

bool Filled(const unsigned char* bytes, size_t count, unsigned char value)
{
    for (size_t index = 0; index < count; ++index)
    {
        if (bytes[index] != value)
        {
            return false;
        }
    }
    return true;
}

void CheckAllocation()
{
    auto* first = static_cast<unsigned char*>(malloc(100));
    Check(first != nullptr && reinterpret_cast<uintptr_t>(first) % 16 == 0, "malloc aligns");
    memset(first, 0xAB, 100);
    auto* grown = static_cast<unsigned char*>(realloc(first, 5000));
    Check(grown != nullptr && Filled(grown, 100, 0xAB), "realloc keeps the contents");
    memset(grown, 0xCD, 5000);
    free(grown);
    auto* zeroed = static_cast<unsigned char*>(calloc(50, 100));
    Check(zeroed != nullptr && Filled(zeroed, 5000, 0), "calloc zeroes reused memory");
    free(zeroed);
    Check(realloc(malloc(10), 0) == nullptr, "realloc to 0 frees");
    void* from_null = realloc(nullptr, 32);
    Check(from_null != nullptr, "realloc of nullptr allocates");
    free(from_null);
    free(nullptr);

    char* copy = strdup("feather");
    Check(copy != nullptr && strcmp(copy, "feather") == 0, "strdup");
    free(copy);
    copy = strndup("featheros", 7);
    Check(copy != nullptr && strcmp(copy, "feather") == 0, "strndup");
    free(copy);

    errno = 0;
    Check(malloc(SIZE_MAX) == nullptr && errno == ENOMEM, "malloc too large");
    errno = 0;
    Check(calloc(SIZE_MAX / 2, 4) == nullptr && errno == ENOMEM, "calloc overflow");

    auto* object = new Counted;
    Check(live_objects == 1 && object->value == 7, "new runs the constructor");
    delete object;
    auto* objects = new Counted[5];
    Check(live_objects == 5, "new[] constructs each element");
    delete[] objects;
    Check(live_objects == 0, "delete and delete[] run destructors");
}

void CheckHeapReuse()
{
    // Freeing the pieces leaves one large free block at the end of the heap, which goes back to
    // the kernel; the host tests check the merging itself.
    constexpr size_t Pieces = 32;
    constexpr size_t PieceSize = 64 * 1024;
    void* pieces[Pieces];
    for (void*& piece : pieces)
    {
        piece = malloc(PieceSize);
        Check(piece != nullptr, "allocating 2 MiB in pieces");
        memset(piece, 0x11, PieceSize);
    }
    const uint64_t break_with_pieces = ProgramBreak();
    for (void* piece : pieces)
    {
        free(piece);
    }
    Check(ProgramBreak() + Pieces * PieceSize / 2 < break_with_pieces,
          "freeing the pieces returns their memory");
    void* whole = malloc(Pieces * PieceSize - 4096);
    Check(whole != nullptr && ProgramBreak() <= break_with_pieces + 64 * 1024,
          "the returned memory can be allocated again");
    free(whole);
    const uint64_t settled_break = ProgramBreak();
    for (int round = 0; round < 200; ++round)
    {
        free(malloc(8000));
    }
    Check(ProgramBreak() == settled_break, "repeated allocations reuse memory");

    // More than the 1024-page limit fails without using up pages, so the heap can still grow.
    errno = 0;
    Check(malloc(8 * 1024 * 1024) == nullptr && errno == ENOMEM, "malloc past the page limit");
    void* larger = malloc(Pieces * PieceSize + 256 * 1024);
    Check(larger != nullptr && ProgramBreak() > settled_break,
          "the heap grows after a failed request");
    free(larger);
}

constexpr char StreamFile[] = "/LIBCTEST.TXT";

void CheckFormatting()
{
    char text[64];
    Check(snprintf(text, sizeof(text), "%s-%05d-%#x", "id", 42, 255) == 13 &&
              strcmp(text, "id-00042-0xff") == 0,
          "snprintf");
    Check(snprintf(text, 4, "%d", 123456) == 6 && strcmp(text, "123") == 0, "snprintf truncates");
    Check(sprintf(text, "%c%c", 'o', 'k') == 2 && strcmp(text, "ok") == 0, "sprintf");
    Check(printf("libctest: printf %s %d\n", "works", 7) == 25, "printf");
    Check(dprintf(STDOUT_FILENO, "libctest: dprintf %u\n", 3U) == 20, "dprintf");
    Check(fprintf(stderr, "libctest: fprintf to stderr\n") == 28, "fprintf to stderr");
    errno = ENOENT;
    perror("libctest: perror");
    Check(puts("libctest: puts works") == 0, "puts");
}

void CheckFileStreams()
{
    FILE* file = fopen(StreamFile, "w");
    Check(file != nullptr, "fopen for writing");
    Check(fputs("first line\n", file) == 0 && fprintf(file, "%d,%d\n", 1, 2) == 4 &&
              fputc('Z', file) == 'Z',
          "writing a stream");
    Check(ftell(file) == 16, "ftell while writing");
    Check(fclose(file) == 0, "fclose flushes");

    char text[64];
    file = fopen(StreamFile, "r");
    Check(file != nullptr && fgets(text, sizeof(text), file) != nullptr &&
              strcmp(text, "first line\n") == 0,
          "fgets");
    Check(fgetc(file) == '1' && ungetc('1', file) == '1' && fgetc(file) == '1', "ungetc");
    Check(ftell(file) == 12, "ftell while reading");
    char rest[8];
    Check(fread(rest, 1, sizeof(rest), file) == 4 && memcmp(rest, ",2\nZ", 4) == 0,
          "fread to the end");
    Check(feof(file) && !ferror(file), "end of file");
    Check(fseek(file, -1, SEEK_END) == 0 && fgetc(file) == 'Z', "fseek from the end");
    rewind(file);
    Check(fgetc(file) == 'f' && !feof(file), "rewind");
    Check(fseek(file, 6, SEEK_SET) == 0 && fgets(text, 5, file) != nullptr &&
              strcmp(text, "line") == 0,
          "fgets stops at its size");
    Check(fputc('x', file) == EOF && ferror(file), "writing a read-only stream");
    clearerr(file);
    Check(!ferror(file) && fclose(file) == 0, "clearerr");

    file = fopen(StreamFile, "a");
    Check(file != nullptr && fseek(file, 0, SEEK_SET) == 0 && fputs("+end", file) == 0 &&
              fclose(file) == 0,
          "appending after a seek");
    file = fopen(StreamFile, "r");
    char* line = nullptr;
    size_t capacity = 0;
    int lines = 0;
    bool last_line_matches = false;
    for (ssize_t length; (length = getline(&line, &capacity, file)) > 0;)
    {
        ++lines;
        last_line_matches = strcmp(line, "Z+end") == 0;
    }
    Check(lines == 3 && last_line_matches, "getline");
    free(line);
    fclose(file);

    file = fopen(StreamFile, "w+");
    Check(file != nullptr && fputs("abc", file) == 0, "fopen w+");
    rewind(file);
    Check(fgetc(file) == 'a' && fputc('X', file) == 'X' && fseek(file, 0, SEEK_SET) == 0 &&
              fgets(text, sizeof(text), file) != nullptr && strcmp(text, "aXc") == 0,
          "switching between reading and writing");
    FILE* second = fopen(StreamFile, "a");
    Check(second == nullptr && errno == EBUSY, "a second writer");
    fclose(file);

    errno = 0;
    Check(fopen("/missing/file.txt", "r") == nullptr && errno == ENOENT, "fopen a missing file");
    errno = 0;
    Check(fopen("/system/hello.txt", "w") == nullptr && errno == EROFS, "fopen read-only");
    errno = 0;
    Check(fopen(StreamFile, "q") == nullptr && errno == EINVAL, "fopen with a bad mode");
    errno = 0;
    Check(fseek(stdout, 0, SEEK_SET) == -1 && errno == ESPIPE, "fseek on the console");
    Check(fileno(stdin) == 0 && fileno(stderr) == 2, "fileno");
    Check(remove(StreamFile) == 0, "remove");
    errno = 0;
    Check(remove(StreamFile) == -1 && errno == ENOENT, "remove a missing file");
}

constexpr char WorkDirectory[] = "/home/libcwork";

void CheckDescriptorFiles()
{
    const int descriptor = open("/home/LIBCRAW.TXT", O_RDWR | O_CREAT | O_TRUNC, 0644);
    Check(descriptor >= 3 && write(descriptor, "0123456789", 10) == 10, "open and write");
    Check(lseek(descriptor, 0, SEEK_CUR) == 10 && lseek(descriptor, -4, SEEK_END) == 6 &&
              lseek(descriptor, 2, SEEK_SET) == 2,
          "lseek origins");
    char bytes[4];
    Check(read(descriptor, bytes, 3) == 3 && memcmp(bytes, "234", 3) == 0, "read after lseek");
    errno = 0;
    Check(lseek(descriptor, 11, SEEK_SET) == -1 && errno == EINVAL, "lseek past the end");
    struct stat status;
    Check(fstat(descriptor, &status) == 0 && status.st_size == 10 && !isatty(descriptor),
          "fstat a file");
    Check(close(descriptor) == 0, "close");

    errno = 0;
    Check(open("/home/LIBCRAW.TXT", O_WRONLY | O_CREAT | O_EXCL) == -1 && errno == EEXIST,
          "O_EXCL on an existing file");
    const int appender = open("/home/LIBCRAW.TXT", O_WRONLY | O_APPEND);
    Check(appender >= 3 && lseek(appender, 0, SEEK_SET) == 0 && write(appender, "AB", 2) == 2 &&
              close(appender) == 0,
          "O_APPEND after lseek");
    Check(stat("/home/LIBCRAW.TXT", &status) == 0 && status.st_size == 12,
          "O_APPEND wrote at the end");
    errno = 0;
    Check(open("/home/LIBCRAW.TXT", O_RDONLY | 0x40000) == -1 && errno == EINVAL,
          "open with an unknown flag");
    errno = 0;
    Check(lseek(STDOUT_FILENO, 0, SEEK_SET) == -1 && errno == ESPIPE, "lseek on the console");
    Check(isatty(STDOUT_FILENO) == 1, "isatty on the console");
    struct winsize window = {};
    Check(ioctl(STDOUT_FILENO, TIOCGWINSZ, &window) == 0 && window.ws_row >= 1 &&
              window.ws_col >= 1,
          "ioctl reads the console size");
    errno = 0;
    Check(ioctl(STDOUT_FILENO, 0x1234, &window) == -1 && errno == EINVAL,
          "ioctl with an unknown request");
    const int text = open("/system/hello.txt", O_RDONLY);
    errno = 0;
    Check(text >= 3 && ioctl(text, TIOCGWINSZ, &window) == -1 && errno == ENOTTY &&
              close(text) == 0,
          "ioctl on a file");
    Check(access("/system/hello.txt", R_OK) == 0, "access to read");
    errno = 0;
    Check(access("/system/hello.txt", W_OK) == -1 && errno == EROFS, "access to write");
    Check(unlink("/home/LIBCRAW.TXT") == 0, "unlink");
    errno = 0;
    Check(unlink("/home/LIBCRAW.TXT") == -1 && errno == ENOENT, "unlink a missing file");
}

void CheckDirectories()
{
    char directory[64];
    Check(getcwd(directory, sizeof(directory)) != nullptr, "getcwd");
    char original[64];
    strcpy(original, directory);
    Check(mkdir(WorkDirectory, 0755) == 0, "mkdir");
    errno = 0;
    Check(mkdir(WorkDirectory, 0755) == -1 && errno == EEXIST, "mkdir an existing directory");
    Check(chdir(WorkDirectory) == 0 && getcwd(directory, sizeof(directory)) != nullptr &&
              strcmp(directory, WorkDirectory) == 0,
          "chdir");
    errno = 0;
    Check(getcwd(directory, 4) == nullptr && errno == ERANGE, "getcwd with a small buffer");
    FILE* file = fopen("note.txt", "w");
    Check(file != nullptr && fputs("hi", file) == 0 && fclose(file) == 0,
          "a file relative to the working directory");

    DIR* listing = opendir(".");
    Check(listing != nullptr && chdir("/") == 0, "opendir");
    const struct dirent* entry = readdir(listing);
    Check(entry != nullptr && strcmp(entry->d_name, "note.txt") == 0 && entry->d_type == DT_REG,
          "readdir after a chdir");
    Check(readdir(listing) == nullptr, "readdir at the end");
    rewinddir(listing);
    Check(readdir(listing) != nullptr && closedir(listing) == 0, "rewinddir");

    listing = opendir("/system");
    bool found = false;
    for (const struct dirent* system_entry = readdir(listing); system_entry != nullptr;
         system_entry = readdir(listing))
    {
        found = found || strcmp(system_entry->d_name, "hello.txt") == 0;
    }
    Check(found && closedir(listing) == 0, "listing /system");
    errno = 0;
    Check(opendir("/no/such/directory") == nullptr && errno == ENOENT, "opendir a missing one");
    errno = 0;
    Check(opendir("/system/hello.txt") == nullptr && errno == ENOTDIR, "opendir a file");

    errno = 0;
    Check(rmdir(WorkDirectory) == -1 && errno == ENOTEMPTY, "rmdir a non-empty directory");
    Check(chdir(WorkDirectory) == 0 && unlink("note.txt") == 0 && chdir(original) == 0,
          "unlink relative to the working directory");
    Check(rmdir(WorkDirectory) == 0, "rmdir");
}

bool FileContains(const char* path, const char* text)
{
    FILE* file = fopen(path, "r");
    if (file == nullptr)
    {
        return false;
    }
    char contents[1024];
    const size_t length = fread(contents, 1, sizeof(contents) - 1, file);
    fclose(file);
    contents[length] = '\0';
    return strstr(contents, text) != nullptr;
}

// Only numeric output and names /etc/hosts lists, so no name server is asked.
void CheckAddressNames()
{
    sockaddr_in ipv4 = {};
    ipv4.sin_family = AF_INET;
    ipv4.sin_port = htons(8080);
    inet_pton(AF_INET, "10.0.2.15", &ipv4.sin_addr);
    const auto* address = reinterpret_cast<const sockaddr*>(&ipv4);
    char host[NI_MAXHOST];
    char service[NI_MAXSERV];
    Check(getnameinfo(address, sizeof(ipv4), host, sizeof(host), service, sizeof(service),
                      NI_NUMERICHOST | NI_NUMERICSERV) == 0 &&
              strcmp(host, "10.0.2.15") == 0 && strcmp(service, "8080") == 0,
          "getnameinfo numeric IPv4");
    Check(getnameinfo(address, sizeof(ipv4), host, 9, nullptr, 0, NI_NUMERICHOST) == EAI_OVERFLOW &&
              getnameinfo(address, sizeof(ipv4), nullptr, 0, service, 4, 0) == EAI_OVERFLOW,
          "getnameinfo overflow");
    Check(getnameinfo(address, sizeof(ipv4) - 1, host, sizeof(host), nullptr, 0, 0) == EAI_FAMILY &&
              getnameinfo(address, sizeof(ipv4), host, sizeof(host), nullptr, 0, 0x100) ==
                  EAI_BADFLAGS &&
              getnameinfo(address, sizeof(ipv4), nullptr, 0, nullptr, 0, 0) == EAI_NONAME,
          "getnameinfo errors");
    ipv4.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    Check(getnameinfo(address, sizeof(ipv4), host, sizeof(host), nullptr, 0, NI_NAMEREQD) == 0 &&
              strcmp(host, "localhost") == 0,
          "getnameinfo from /etc/hosts");

    sockaddr_in6 ipv6 = {};
    ipv6.sin6_family = AF_INET6;
    inet_pton(AF_INET6, "fe80::1", &ipv6.sin6_addr);
    address = reinterpret_cast<const sockaddr*>(&ipv6);
    Check(getnameinfo(address, sizeof(ipv6), host, sizeof(host), service, sizeof(service),
                      NI_NUMERICHOST) == 0 &&
              strcmp(host, "fe80::1") == 0 && strcmp(service, "0") == 0,
          "getnameinfo numeric IPv6");
    ipv6.sin6_addr = in6addr_loopback;
    Check(getnameinfo(address, sizeof(ipv6), host, sizeof(host), nullptr, 0, 0) == 0 &&
              strcmp(host, "localhost") == 0,
          "getnameinfo IPv6 from /etc/hosts");
}

void CheckVirtualFiles()
{
    FILE* null_file = fopen("/dev/null", "w");
    Check(null_file != nullptr && fputs("discarded", null_file) >= 0 && fclose(null_file) == 0,
          "writing /dev/null");
    char bytes[16];
    int descriptor = open("/dev/null", O_RDONLY);
    Check(descriptor >= 0 && read(descriptor, bytes, sizeof(bytes)) == 0 && !isatty(descriptor) &&
              close(descriptor) == 0,
          "reading /dev/null ends at once");
    memset(bytes, 0x55, sizeof(bytes));
    descriptor = open("/dev/zero", O_RDONLY);
    Check(descriptor >= 0 && read(descriptor, bytes, sizeof(bytes)) == sizeof(bytes) &&
              bytes[0] == 0 && bytes[15] == 0 && close(descriptor) == 0,
          "reading /dev/zero");
    unsigned char first[16];
    unsigned char second[16];
    descriptor = open("/dev/random", O_RDONLY);
    Check(descriptor >= 0 && read(descriptor, first, sizeof(first)) == sizeof(first) &&
              read(descriptor, second, sizeof(second)) == sizeof(second) &&
              memcmp(first, second, sizeof(first)) != 0 && close(descriptor) == 0,
          "reading /dev/random");
    struct stat status;
    Check(stat("/dev/random", &status) == 0 && S_ISCHR(status.st_mode), "devices are S_IFCHR");

    Check(mkdir("/tmp/libctest", 0755) == 0, "mkdir in /tmp");
    FILE* file = fopen("/tmp/libctest/Data.txt", "w");
    Check(file != nullptr && fprintf(file, "kept in RAM %d\n", 42) > 0 && fclose(file) == 0,
          "writing a /tmp file");
    Check(FileContains("/tmp/libctest/Data.txt", "kept in RAM 42") &&
              access("/tmp/libctest/data.txt", F_OK) != 0,
          "/tmp keeps contents and names are case-sensitive");
    Check(unlink("/tmp/libctest/Data.txt") == 0 && rmdir("/tmp/libctest") == 0,
          "removing from /tmp");

    char expected[32];
    snprintf(expected, sizeof(expected), "Pid:\t%d\n", getpid());
    Check(FileContains("/proc/self/status", "Name:\tlibctest\n") &&
              FileContains("/proc/self/status", expected),
          "/proc/self/status");
    Check(FileContains("/proc/meminfo", "MemTotal:\t"), "/proc/meminfo");
    snprintf(expected, sizeof(expected), "%d", getpid());
    bool listed = false;
    DIR* processes = opendir("/proc");
    for (const dirent* entry = processes == nullptr ? nullptr : readdir(processes);
         entry != nullptr; entry = readdir(processes))
    {
        listed = listed || (strcmp(entry->d_name, expected) == 0 && entry->d_type == DT_DIR);
    }
    Check(processes != nullptr && closedir(processes) == 0 && listed, "/proc lists the process");
    errno = 0;
    Check(open("/proc/meminfo", O_WRONLY) == -1 && errno == EROFS, "/proc is read-only");
}

void CheckTime()
{
    const time_t now = time(nullptr);
    Check(now > 1700000000, "time");
    const time_t epoch = 0;
    struct tm calendar;
    Check(gmtime_r(&epoch, &calendar) != nullptr && calendar.tm_year == 70 &&
              calendar.tm_wday == 4 && calendar.tm_yday == 0,
          "gmtime of the epoch");
    Check(strcmp(asctime(&calendar), "Thu Jan  1 00:00:00 1970\n") == 0, "asctime");
    struct tm normalized = {};
    normalized.tm_year = 126;
    normalized.tm_mon = 0;
    normalized.tm_mday = 32;
    normalized.tm_hour = 25;
    Check(mktime(&normalized) == 1769994000 && normalized.tm_mon == 1 && normalized.tm_mday == 2 &&
              normalized.tm_hour == 1 && normalized.tm_wday == 1,
          "mktime normalizes");
    char text[32];
    Check(strftime(text, sizeof(text), "%Y-%m-%d %H:%M", &normalized) == 16 &&
              strcmp(text, "2026-02-02 01:00") == 0,
          "strftime");
    struct timespec before;
    struct timespec after;
    const struct timespec pause = {0, 30000000};
    Check(clock_gettime(CLOCK_MONOTONIC, &before) == 0 && nanosleep(&pause, nullptr) == 0 &&
              clock_gettime(CLOCK_MONOTONIC, &after) == 0,
          "nanosleep");
    const long elapsed_ms =
        (after.tv_sec - before.tv_sec) * 1000 + (after.tv_nsec - before.tv_nsec) / 1000000;
    Check(elapsed_ms >= 30, "CLOCK_MONOTONIC measures the sleep");
    errno = 0;
    const struct timespec invalid = {0, 1000000000};
    Check(nanosleep(&invalid, nullptr) == -1 && errno == EINVAL, "nanosleep rejects bad input");
    const time_t before_epoch = -1;
    errno = 0;
    Check(gmtime(&before_epoch) == nullptr && errno == EOVERFLOW, "gmtime before 1970");

    struct timespec real;
    Check(clock_gettime(CLOCK_REALTIME, &real) == 0 && real.tv_sec - time(nullptr) <= 0 &&
              real.tv_sec - time(nullptr) >= -1,
          "CLOCK_REALTIME agrees with time");
    constexpr time_t Hour = 3600;
    struct timespec ahead = {real.tv_sec + Hour, real.tv_nsec};
    struct timespec moved;
    Check(clock_settime(CLOCK_REALTIME, &ahead) == 0 &&
              clock_gettime(CLOCK_REALTIME, &moved) == 0 && moved.tv_sec >= ahead.tv_sec &&
              moved.tv_sec <= ahead.tv_sec + 1,
          "clock_settime moves the clock");
    clock_gettime(CLOCK_REALTIME, &moved);
    moved.tv_sec -= Hour;
    clock_settime(CLOCK_REALTIME, &moved);
    errno = 0;
    Check(clock_settime(CLOCK_MONOTONIC, &real) == -1 && errno == EINVAL,
          "clock_settime sets only CLOCK_REALTIME");
    errno = 0;
    const struct timespec bad_time = {real.tv_sec, 1000000000};
    Check(clock_settime(CLOCK_REALTIME, &bad_time) == -1 && errno == EINVAL,
          "clock_settime rejects bad input");
}

void CheckSpawn()
{
    char* echo_arguments[] = {const_cast<char*>("echo"), const_cast<char*>("spawned by"),
                              const_cast<char*>("posix_spawn"), nullptr};
    pid_t child = 0;
    int status = -1;
    Check(posix_spawn(&child, "/system/echo", nullptr, nullptr, echo_arguments, environ) == 0 &&
              waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "posix_spawn and waitpid");

    char variable[] = "SPAWNED=yes";
    char* environment[] = {variable, nullptr};
    char* env_arguments[] = {const_cast<char*>("env"), nullptr};
    Check(posix_spawnp(&child, "env", nullptr, nullptr, env_arguments, environment) == 0 &&
              wait(&status) == child && WIFEXITED(status),
          "posix_spawnp with its own environment");

    char* exit_arguments[] = {const_cast<char*>("libctest"), const_cast<char*>("exit7"), nullptr};
    Check(posix_spawn(&child, "libctest", nullptr, nullptr, exit_arguments, environ) == 0 &&
              waitpid(-1, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 7,
          "an exit status through waitpid(-1)");

    char* fault_arguments[] = {const_cast<char*>("hardening"), const_cast<char*>("exec-heap"),
                               nullptr};
    Check(posix_spawn(&child, "hardening", nullptr, nullptr, fault_arguments, environ) == 0 &&
              waitpid(child, &status, 0) == child && WIFSIGNALED(status) && WTERMSIG(status) == 11,
          "a CPU exception reports SIGSEGV");

    char* sleep_arguments[] = {const_cast<char*>("sleep"), const_cast<char*>("0.3"), nullptr};
    Check(posix_spawn(&child, "sleep", nullptr, nullptr, sleep_arguments, environ) == 0 &&
              waitpid(child, &status, WNOHANG) == 0 && waitpid(child, &status, 0) == child,
          "WNOHANG while the child runs");

    Check(posix_spawn(&child, "/home/program", nullptr, nullptr, echo_arguments, environ) == ENOENT,
          "posix_spawn of a missing file");
    errno = 0;
    Check(waitpid(-1, &status, 0) == -1 && errno == ECHILD, "waitpid with no children");
}

bool CopyFile(const char* from, const char* to)
{
    FILE* source = fopen(from, "r");
    FILE* target = source == nullptr ? nullptr : fopen(to, "w");
    char buffer[512];
    size_t length = 1;
    while (target != nullptr && length > 0)
    {
        length = fread(buffer, 1, sizeof(buffer), source);
        if (fwrite(buffer, 1, length, target) != length)
        {
            break;
        }
    }
    const bool copied = target != nullptr && length == 0 && !ferror(source);
    if (source != nullptr)
    {
        fclose(source);
    }
    return target != nullptr && fclose(target) == 0 && copied;
}

// 0 once the child exits with status 0, -1 if it fails, or the spawn's error.
int SpawnAndWait(const char* program, bool search)
{
    char* arguments[] = {const_cast<char*>("echo"), const_cast<char*>("spawned from"),
                         const_cast<char*>("the disk"), nullptr};
    pid_t child = 0;
    int status = -1;
    const int error = search ? posix_spawnp(&child, program, nullptr, nullptr, arguments, environ)
                             : posix_spawn(&child, program, nullptr, nullptr, arguments, environ);
    if (error != 0)
    {
        return error;
    }
    return waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0
               ? 0
               : -1;
}

void CheckDiskPrograms()
{
    Check(CopyFile("/system/echo", "/tmp/disk-echo"), "copying a program to /tmp");
    Check(SpawnAndWait("/tmp/disk-echo", false) == 0, "posix_spawn of a program in /tmp");
    char original[PATH_MAX];
    Check(getcwd(original, sizeof(original)) != nullptr && chdir("/tmp") == 0 &&
              SpawnAndWait("./disk-echo", false) == 0 && chdir(original) == 0,
          "posix_spawn of a relative path");
    Check(SpawnAndWait("disk-echo", true) == ENOENT, "posix_spawnp of a program outside PATH");
    Check(setenv("PATH", "::/nowhere:/tmp/", 1) == 0 && SpawnAndWait("disk-echo", true) == 0 &&
              SpawnAndWait("echo", true) == ENOENT && setenv("PATH", "/system:/bin", 1) == 0,
          "posix_spawnp searches PATH");
    Check(unsetenv("PATH") == 0 && SpawnAndWait("echo", true) == 0 &&
              setenv("PATH", "/system:/bin", 1) == 0,
          "posix_spawnp without PATH");
    Check(SpawnAndWait("/system/hello.txt", false) == ENOEXEC &&
              SpawnAndWait("hello.txt", true) == ENOEXEC,
          "a text file is not a program");
    Check(SpawnAndWait("/dev/zero", false) == ENOEXEC, "a device is not a program");
    Check(SpawnAndWait("/tmp", false) == EISDIR && SpawnAndWait("/tmp/", true) == EISDIR,
          "a directory is not a program");
    Check(SpawnAndWait("/tmp/missing", true) == ENOENT, "posix_spawnp of a missing path");
    Check(unlink("/tmp/disk-echo") == 0, "removing the copied program");
}

void CheckSpawnAttributes()
{
    posix_spawnattr_t attributes;
    short flags = -1;
    pid_t group = -1;
    Check(posix_spawnattr_init(&attributes) == 0 &&
              posix_spawnattr_getflags(&attributes, &flags) == 0 && flags == 0 &&
              posix_spawnattr_getpgroup(&attributes, &group) == 0 && group == 0,
          "posix_spawnattr_init");
    Check(posix_spawnattr_setflags(&attributes, 0x01) == EINVAL &&
              posix_spawnattr_setpgroup(&attributes, -1) == EINVAL,
          "unsupported spawn attributes");

    // A background child has no console, so upper cannot read its line and fails at once.
    char* upper_arguments[] = {const_cast<char*>("upper"), nullptr};
    pid_t child = 0;
    int status = -1;
    Check(posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_BACKGROUND) ==
                  0 &&
              posix_spawn(&child, "upper", nullptr, &attributes, upper_arguments, environ) == 0 &&
              waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 1,
          "a new group with POSIX_SPAWN_BACKGROUND is a background job");

    char* echo_arguments[] = {const_cast<char*>("echo"), const_cast<char*>("session leader"),
                              nullptr};
    Check(posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETSID) == 0 &&
              posix_spawn(&child, "echo", nullptr, &attributes, echo_arguments, environ) == 0 &&
              waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "POSIX_SPAWN_SETSID");
    Check(posix_spawnattr_destroy(&attributes) == 0, "posix_spawnattr_destroy");
}

pid_t SpawnSleeper(short flags, pid_t group)
{
    posix_spawnattr_t attributes;
    posix_spawnattr_init(&attributes);
    posix_spawnattr_setflags(&attributes, flags);
    posix_spawnattr_setpgroup(&attributes, group);
    char* arguments[] = {const_cast<char*>("sleep"), const_cast<char*>("0.3"), nullptr};
    pid_t child = 0;
    return posix_spawn(&child, "sleep", nullptr, &attributes, arguments, environ) == 0 ? child : -1;
}

// The shell starts each command as a group of its own and gives it the console.
void CheckProcessGroups()
{
    const pid_t own = getpgrp();
    Check(own == getpid() && getpgid(0) == own && getpgid(getpid()) == own &&
              tcgetpgrp(STDIN_FILENO) == own,
          "the shell runs a command as its own foreground group");
    errno = 0;
    Check(getpgid(99999) == -1 && errno == ESRCH, "getpgid of a missing process");
    errno = 0;
    Check(tcsetpgrp(STDIN_FILENO, own) == -1 && errno == EPERM,
          "only session leaders move the console");
    const int null_device = open("/dev/null", O_RDONLY);
    errno = 0;
    Check(tcgetpgrp(null_device) == -1 && errno == ENOTTY && close(null_device) == 0,
          "tcgetpgrp needs the console");
    char expected[32];
    snprintf(expected, sizeof(expected), "Pgid:\t%d\n", own);
    Check(FileContains("/proc/self/status", expected), "/proc/self/status shows the group");

    const pid_t inherited = SpawnSleeper(0, 0);
    Check(inherited > 0 && getpgid(inherited) == own, "a child joins its parent's group");
    int status = -1;
    Check(waitpid(0, &status, 0) == inherited && WIFEXITED(status), "waitpid(0)");

    const pid_t leader = SpawnSleeper(POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_BACKGROUND, 0);
    const pid_t member = SpawnSleeper(POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_BACKGROUND, leader);
    Check(leader > 0 && member > 0 && getpgid(leader) == leader && getpgid(member) == leader,
          "POSIX_SPAWN_SETPGROUP leads or joins a group");
    Check(tcgetpgrp(STDIN_FILENO) == own, "background groups leave the console alone");
    Check(SpawnSleeper(POSIX_SPAWN_SETPGROUP, 1) == -1, "joining an unrelated group fails");
    const pid_t first = waitpid(-leader, &status, 0);
    const pid_t second = waitpid(-leader, &status, 0);
    errno = 0;
    Check(((first == leader && second == member) || (first == member && second == leader)) &&
              waitpid(-leader, &status, WNOHANG) == -1 && errno == ECHILD,
          "waitpid(-group)");
}

volatile sig_atomic_t last_signal = 0;
volatile sig_atomic_t handled_count = 0;
volatile sig_atomic_t nested_signal = 0;

void RecordSignal(int signal_number)
{
    last_signal = signal_number;
    handled_count = handled_count + 1;
}

// A handler that is itself interrupted by another signal's handler.
void RaiseNested(int signal_number)
{
    last_signal = signal_number;
    raise(SIGUSR2);
    nested_signal = last_signal;
}

// Floating-point work inside the handler must not leak into the code it interrupted.
void DisturbFloatingPoint(int)
{
    volatile double noise = 3.0;
    for (int index = 0; index < 10; ++index)
    {
        noise = noise * 1.25 + 0.5;
    }
    handled_count = handled_count + (noise > 0 ? 1 : 0);
}

void CheckSignalSets()
{
    sigset_t set = 0;
    Check(sigemptyset(&set) == 0 && set == 0 && sigaddset(&set, SIGINT) == 0 &&
              sigaddset(&set, SIGTERM) == 0 && sigismember(&set, SIGINT) == 1 &&
              sigismember(&set, SIGHUP) == 0 && sigdelset(&set, SIGINT) == 0 &&
              sigismember(&set, SIGINT) == 0 && sigaddset(&set, 0) == -1 && sigfillset(&set) == 0 &&
              sigismember(&set, SIGKILL) == 1,
          "signal sets");
    Check(strcmp(strsignal(SIGTERM), "Terminated") == 0 &&
              strcmp(strsignal(SIGKILL), "Killed") == 0 &&
              strcmp(strsignal(99), "Unknown signal") == 0,
          "strsignal");
}

void CheckSignalHandlers()
{
    Check(signal(SIGUSR1, RecordSignal) == SIG_DFL && raise(SIGUSR1) == 0 &&
              last_signal == SIGUSR1 && handled_count == 1,
          "a handler runs before raise returns");
    volatile double kept = 1.5 * getpid();
    const double expected = kept;
    Check(signal(SIGUSR2, DisturbFloatingPoint) == SIG_DFL && raise(SIGUSR2) == 0 &&
              handled_count == 2 && kept == expected,
          "a handler keeps the interrupted code's registers");

    sigset_t block = 0;
    sigaddset(&block, SIGUSR1);
    sigset_t previous = 0;
    handled_count = 0;
    Check(sigprocmask(SIG_BLOCK, &block, &previous) == 0 && raise(SIGUSR1) == 0 &&
              handled_count == 0,
          "a blocked signal waits");
    Check(sigprocmask(SIG_SETMASK, &previous, nullptr) == 0 && handled_count == 1,
          "unblocking runs the waiting handler");

    Check(signal(SIGUSR2, RecordSignal) == DisturbFloatingPoint &&
              signal(SIGUSR1, RaiseNested) == RecordSignal && raise(SIGUSR1) == 0 &&
              nested_signal == SIGUSR2,
          "a handler can be interrupted by another signal's handler");

    struct sigaction action = {};
    action.sa_handler = RecordSignal;
    action.sa_flags = SA_RESETHAND;
    struct sigaction now = {};
    handled_count = 0;
    Check(sigaction(SIGUSR1, &action, nullptr) == 0 && raise(SIGUSR1) == 0 && handled_count == 1 &&
              sigaction(SIGUSR1, nullptr, &now) == 0 && now.sa_handler == SIG_DFL,
          "SA_RESETHAND goes back to the default action");
    Check(signal(SIGUSR2, SIG_IGN) == RecordSignal && raise(SIGUSR2) == 0,
          "an ignored signal does nothing");
    signal(SIGUSR2, SIG_DFL);
    errno = 0;
    Check(sigaction(SIGKILL, &action, nullptr) == -1 && errno == EINVAL &&
              signal(19, RecordSignal) == SIG_ERR,
          "SIGKILL and the stop signals cannot be handled");
}

void CheckKill()
{
    errno = 0;
    Check(kill(getpid(), 0) == 0 && kill(99999, SIGTERM) == -1 && errno == ESRCH, "kill");
    errno = 0;
    Check(kill(1, SIGTERM) == -1 && errno == EPERM, "init cannot be signaled");
    errno = 0;
    Check(kill(getpid(), 99) == -1 && errno == EINVAL, "kill with an unknown signal");

    const pid_t sleeper = SpawnSleeper(0, 0);
    int status = -1;
    Check(sleeper > 0 && kill(sleeper, SIGTERM) == 0 && waitpid(sleeper, &status, 0) == sleeper &&
              WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM,
          "SIGTERM ends a child");

    // The writer's reader is gone before it starts, so its first write raises SIGPIPE.
    int ends[2];
    posix_spawn_file_actions_t actions;
    char* echo_arguments[] = {const_cast<char*>("echo"), const_cast<char*>("nobody reads this"),
                              nullptr};
    pid_t writer = 0;
    Check(pipe2(ends, O_CLOEXEC) == 0 && close(ends[0]) == 0 &&
              posix_spawn_file_actions_init(&actions) == 0 &&
              posix_spawn_file_actions_adddup2(&actions, ends[1], STDOUT_FILENO) == 0 &&
              posix_spawn(&writer, "echo", &actions, nullptr, echo_arguments, environ) == 0 &&
              close(ends[1]) == 0 && waitpid(writer, &status, 0) == writer && WIFSIGNALED(status) &&
              WTERMSIG(status) == SIGPIPE,
          "SIGPIPE ends a writer without a reader");

    // A child's exit raises SIGCHLD, which interrupts calls that wait.
    signal(SIGCHLD, RecordSignal);
    handled_count = 0;
    const pid_t child = SpawnSleeper(0, 0);
    errno = 0;
    Check(child > 0 && pause() == -1 && errno == EINTR && last_signal == SIGCHLD &&
              waitpid(child, &status, 0) == child,
          "SIGCHLD interrupts pause");
    const pid_t second = SpawnSleeper(0, 0);
    const struct timespec long_sleep = {5, 0};
    struct timespec remaining = {0, 0};
    errno = 0;
    Check(second > 0 && nanosleep(&long_sleep, &remaining) == -1 && errno == EINTR &&
              remaining.tv_sec >= 3 && waitpid(second, &status, 0) == second,
          "a signal cuts nanosleep short");
    signal(SIGCHLD, SIG_DFL);
}

unsigned long long OwnCpuMilliseconds()
{
    FILE* file = fopen("/proc/self/status", "r");
    char line[64];
    unsigned long long milliseconds = 0;
    while (file != nullptr && fgets(line, sizeof(line), file) != nullptr)
    {
        sscanf(line, "CpuTimeMs:\t%llu", &milliseconds);
    }
    if (file != nullptr)
    {
        fclose(file);
    }
    return milliseconds;
}

void CheckProcessAccounting()
{
    const long cpus = sysconf(_SC_NPROCESSORS_ONLN);
    Check(cpus >= 1 && sysconf(_SC_NPROCESSORS_CONF) == cpus && sysconf(_SC_PAGESIZE) == 4096 &&
              sysconf(_SC_OPEN_MAX) == 32,
          "sysconf");
    errno = 0;
    Check(sysconf(9999) == -1 && errno == EINVAL, "sysconf with an unknown name");
    Check(FileContains("/proc/self/status", "LastCpu:\t") &&
              FileContains("/proc/self/status", "StartTimeMs:\t") &&
              FileContains("/proc/stat", "Cpu0BusyMs:\t"),
          "/proc reports CPU time");
    // Spinning for 300 ms must show up as CPU time, counted in 10 ms timer ticks.
    const unsigned long long before = OwnCpuMilliseconds();
    timespec start{};
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &start);
    volatile unsigned long long spins = 0;
    do
    {
        spins = spins + 1;
        clock_gettime(CLOCK_MONOTONIC, &now);
    } while ((now.tv_sec - start.tv_sec) * 1000 + (now.tv_nsec - start.tv_nsec) / 1000000 < 300);
    Check(OwnCpuMilliseconds() >= before + 150, "CPU time grows while the process runs");
}

// Threads here only compute and touch their own data: the C library is not thread-safe yet.
struct SumTask
{
    unsigned long first;
    unsigned long last;
    unsigned long sum;
    pthread_t self;
};

void* SumRange(void* argument)
{
    auto* task = static_cast<SumTask*>(argument);
    task->self = pthread_self();
    for (unsigned long value = task->first; value <= task->last; ++value)
    {
        task->sum += value;
    }
    return task;
}

int release_threads = 0;

void* WaitForRelease(void*)
{
    while (__atomic_load_n(&release_threads, __ATOMIC_ACQUIRE) == 0)
    {
        sched_yield();
    }
    return nullptr;
}

// Several pages deep, so a thread's stack has to grow on demand.
unsigned long DeepSum(unsigned depth)
{
    volatile char page[4096];
    page[0] = static_cast<char>(depth);
    return depth == 0 ? 0
                      : depth + DeepSum(depth - 1) + static_cast<unsigned long>(page[0] - depth);
}

void* DeepStack(void*)
{
    return reinterpret_cast<void*>(DeepSum(40));
}

__thread int tls_counter = 5;
__thread char tls_zeroed[100];
alignas(64) thread_local char tls_aligned[64];

struct TlsReport
{
    int start;
    bool zeroed;
    bool aligned;
    uintptr_t address;
};

void* ReportThreadLocal(void* argument)
{
    auto* report = static_cast<TlsReport*>(argument);
    report->start = tls_counter;
    report->zeroed = true;
    for (const char byte : tls_zeroed)
    {
        report->zeroed = report->zeroed && byte == 0;
    }
    report->aligned = reinterpret_cast<uintptr_t>(tls_aligned) % 64 == 0;
    report->address = reinterpret_cast<uintptr_t>(&tls_counter);
    // Leaves data behind for the next thread that gets this stack slot.
    tls_counter = 1000;
    memset(tls_zeroed, 'x', sizeof(tls_zeroed));
    return nullptr;
}

// Runs four threads and checks each started from the template with its own copy.
bool ThreadLocalRound()
{
    TlsReport reports[4] = {};
    pthread_t threads[4];
    bool fine = true;
    for (int index = 0; index < 4; ++index)
    {
        fine = pthread_create(&threads[index], nullptr, ReportThreadLocal, &reports[index]) == 0 &&
               fine;
    }
    for (int index = 0; index < 4; ++index)
    {
        fine = pthread_join(threads[index], nullptr) == 0 && fine;
        fine = fine && reports[index].start == 5 && reports[index].zeroed &&
               reports[index].aligned &&
               reports[index].address != reinterpret_cast<uintptr_t>(&tls_counter);
        for (int other = 0; other < index; ++other)
        {
            fine = fine && reports[other].address != reports[index].address;
        }
    }
    return fine;
}

pthread_mutex_t counter_mutex = PTHREAD_MUTEX_INITIALIZER;
unsigned long shared_counter = 0;

void* CountUnderMutex(void*)
{
    for (int round = 0; round < 20000; ++round)
    {
        pthread_mutex_lock(&counter_mutex);
        // A plain read and write: without the mutex, increments from other CPUs would be lost.
        shared_counter = shared_counter + 1;
        pthread_mutex_unlock(&counter_mutex);
    }
    return nullptr;
}

void* TryLockFromThread(void* argument)
{
    return reinterpret_cast<void*>(
        static_cast<uintptr_t>(pthread_mutex_trylock(static_cast<pthread_mutex_t*>(argument))));
}

void* UnlockFromThread(void* argument)
{
    return reinterpret_cast<void*>(
        static_cast<uintptr_t>(pthread_mutex_unlock(static_cast<pthread_mutex_t*>(argument))));
}

uintptr_t RunWithMutex(void* (*function)(void*), pthread_mutex_t* mutex)
{
    pthread_t thread = 0;
    void* value = nullptr;
    pthread_create(&thread, nullptr, function, mutex);
    pthread_join(thread, &value);
    return reinterpret_cast<uintptr_t>(value);
}

// A bounded queue that producers fill and consumers drain, each side waiting on a condition.
struct Queue
{
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
    int items[8];
    int count;
    int produced;
    long consumed_sum;
};

constexpr int ItemsPerProducer = 500;

void* Produce(void* argument)
{
    auto* queue = static_cast<Queue*>(argument);
    for (int item = 1; item <= ItemsPerProducer; ++item)
    {
        pthread_mutex_lock(&queue->mutex);
        while (queue->count == 8)
        {
            pthread_cond_wait(&queue->not_full, &queue->mutex);
        }
        queue->items[queue->count++] = item;
        pthread_cond_signal(&queue->not_empty);
        pthread_mutex_unlock(&queue->mutex);
    }
    return nullptr;
}

void* Consume(void* argument)
{
    auto* queue = static_cast<Queue*>(argument);
    for (int taken = 0; taken < ItemsPerProducer; ++taken)
    {
        pthread_mutex_lock(&queue->mutex);
        while (queue->count == 0)
        {
            pthread_cond_wait(&queue->not_empty, &queue->mutex);
        }
        queue->consumed_sum += queue->items[--queue->count];
        pthread_cond_signal(&queue->not_full);
        pthread_mutex_unlock(&queue->mutex);
    }
    return nullptr;
}

struct Gate
{
    pthread_mutex_t mutex;
    pthread_cond_t opened;
    bool open;
    int waiting;
};

void* WaitAtGate(void* argument)
{
    auto* gate = static_cast<Gate*>(argument);
    pthread_mutex_lock(&gate->mutex);
    ++gate->waiting;
    while (!gate->open)
    {
        pthread_cond_wait(&gate->opened, &gate->mutex);
    }
    pthread_mutex_unlock(&gate->mutex);
    return nullptr;
}

pthread_once_t once_control = PTHREAD_ONCE_INIT;
int once_runs = 0;

void RunOnce()
{
    ++once_runs;
}

void* CallOnce(void*)
{
    pthread_once(&once_control, RunOnce);
    return reinterpret_cast<void*>(static_cast<uintptr_t>(once_runs));
}

int detached_go = 0;
int detached_done = 0;

// Waits for the go-ahead, so the join attempt meets a thread that is still running.
void* FinishDetached(void*)
{
    while (__atomic_load_n(&detached_go, __ATOMIC_ACQUIRE) == 0)
    {
        sched_yield();
    }
    __atomic_store_n(&detached_done, 1, __ATOMIC_RELEASE);
    return nullptr;
}

void CheckSynchronization()
{
    pthread_t threads[8];
    for (pthread_t& thread : threads)
    {
        pthread_create(&thread, nullptr, CountUnderMutex, nullptr);
    }
    for (const pthread_t thread : threads)
    {
        pthread_join(thread, nullptr);
    }
    Check(shared_counter == 8 * 20000, "a mutex keeps a shared counter exact across threads");

    pthread_mutex_t mutex;
    pthread_mutex_init(&mutex, nullptr);
    pthread_mutex_lock(&mutex);
    const uintptr_t busy = RunWithMutex(TryLockFromThread, &mutex);
    pthread_mutex_unlock(&mutex);
    Check(busy == EBUSY && pthread_mutex_trylock(&mutex) == 0 &&
              pthread_mutex_unlock(&mutex) == 0 && pthread_mutex_destroy(&mutex) == 0,
          "pthread_mutex_trylock");

    pthread_mutexattr_t attributes;
    pthread_mutexattr_init(&attributes);
    pthread_mutexattr_settype(&attributes, PTHREAD_MUTEX_ERRORCHECK);
    pthread_mutex_init(&mutex, &attributes);
    pthread_mutex_lock(&mutex);
    Check(pthread_mutex_lock(&mutex) == EDEADLK &&
              RunWithMutex(UnlockFromThread, &mutex) == EPERM &&
              pthread_mutex_unlock(&mutex) == 0 && pthread_mutex_unlock(&mutex) == EPERM,
          "an error-checking mutex refuses a relock and a stranger's unlock");
    pthread_mutexattr_settype(&attributes, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&mutex, &attributes);
    pthread_mutex_lock(&mutex);
    pthread_mutex_lock(&mutex);
    pthread_mutex_unlock(&mutex);
    const uintptr_t still_held = RunWithMutex(TryLockFromThread, &mutex);
    pthread_mutex_unlock(&mutex);
    const uintptr_t released = RunWithMutex(TryLockFromThread, &mutex);
    Check(still_held == EBUSY && released == 0, "a recursive mutex counts its owner's locks");

    Queue queue = {
        PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, PTHREAD_COND_INITIALIZER, {}, 0, 0, 0};
    pthread_t workers[4];
    pthread_create(&workers[0], nullptr, Produce, &queue);
    pthread_create(&workers[1], nullptr, Produce, &queue);
    pthread_create(&workers[2], nullptr, Consume, &queue);
    pthread_create(&workers[3], nullptr, Consume, &queue);
    for (const pthread_t worker : workers)
    {
        pthread_join(worker, nullptr);
    }
    Check(queue.count == 0 &&
              queue.consumed_sum == 2L * ItemsPerProducer * (ItemsPerProducer + 1) / 2,
          "condition variables hand items from producers to consumers");

    Gate gate = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, false, 0};
    pthread_t waiters[4];
    for (pthread_t& waiter : waiters)
    {
        pthread_create(&waiter, nullptr, WaitAtGate, &gate);
    }
    for (bool all_waiting = false; !all_waiting;)
    {
        pthread_mutex_lock(&gate.mutex);
        all_waiting = gate.waiting == 4;
        pthread_mutex_unlock(&gate.mutex);
        sched_yield();
    }
    pthread_mutex_lock(&gate.mutex);
    gate.open = true;
    pthread_cond_broadcast(&gate.opened);
    pthread_mutex_unlock(&gate.mutex);
    bool released_all = true;
    for (const pthread_t waiter : waiters)
    {
        released_all = pthread_join(waiter, nullptr) == 0 && released_all;
    }
    Check(released_all, "pthread_cond_broadcast wakes every waiter");

    pthread_condattr_t condition_attributes;
    pthread_condattr_init(&condition_attributes);
    pthread_condattr_setclock(&condition_attributes, CLOCK_MONOTONIC);
    pthread_cond_t never;
    pthread_cond_init(&never, &condition_attributes);
    struct timespec start;
    struct timespec deadline;
    struct timespec end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    deadline = start;
    deadline.tv_nsec += 60000000;
    if (deadline.tv_nsec >= 1000000000)
    {
        deadline.tv_nsec -= 1000000000;
        ++deadline.tv_sec;
    }
    pthread_mutex_init(&mutex, nullptr);
    pthread_mutex_lock(&mutex);
    const int timed = pthread_cond_timedwait(&never, &mutex, &deadline);
    clock_gettime(CLOCK_MONOTONIC, &end);
    const long waited =
        (end.tv_sec - start.tv_sec) * 1000 + (end.tv_nsec - start.tv_nsec) / 1000000;
    Check(timed == ETIMEDOUT && waited >= 50 && waited < 2000 && pthread_mutex_unlock(&mutex) == 0,
          "pthread_cond_timedwait gives up at its deadline, holding the mutex");

    for (pthread_t& thread : threads)
    {
        pthread_create(&thread, nullptr, CallOnce, nullptr);
    }
    bool once_seen = true;
    for (const pthread_t thread : threads)
    {
        void* value = nullptr;
        once_seen =
            pthread_join(thread, &value) == 0 && value == reinterpret_cast<void*>(1) && once_seen;
    }
    Check(once_runs == 1 && once_seen, "pthread_once runs its routine once, before any return");

    pthread_attr_t detached;
    pthread_attr_init(&detached);
    pthread_attr_setdetachstate(&detached, PTHREAD_CREATE_DETACHED);
    pthread_t detached_thread = 0;
    Check(pthread_create(&detached_thread, &detached, FinishDetached, nullptr) == 0 &&
              pthread_join(detached_thread, nullptr) == EINVAL,
          "a detached thread cannot be joined");
    __atomic_store_n(&detached_go, 1, __ATOMIC_RELEASE);
    while (__atomic_load_n(&detached_done, __ATOMIC_ACQUIRE) == 0)
    {
        sched_yield();
    }
    // Once it has ended, its slot is free again: 15 joinable threads fit beside the main one.
    pthread_t full[15];
    int started = 0;
    __atomic_store_n(&release_threads, 0, __ATOMIC_RELEASE);
    for (int attempt = 0; attempt < 100 && started < 15; ++attempt)
    {
        if (pthread_create(&full[started], nullptr, WaitForRelease, nullptr) == 0)
        {
            ++started;
        }
        else
        {
            sched_yield();
        }
    }
    __atomic_store_n(&release_threads, 1, __ATOMIC_RELEASE);
    for (int index = 0; index < started; ++index)
    {
        pthread_join(full[index], nullptr);
    }
    Check(started == 15, "a detached thread gives its slot back when it ends");
}

int RunChild(const char* mode);

void* ReportErrno(void*)
{
    errno = 0;
    close(-1);
    return reinterpret_cast<void*>(static_cast<uintptr_t>(errno));
}

// Each thread keeps 16 blocks of its own pattern live at once and checks every block before
// freeing it, so a heap shared without a lock would show up as a corrupted pattern.
void* HammerHeap(void* argument)
{
    const auto seed = static_cast<unsigned char>(reinterpret_cast<uintptr_t>(argument));
    unsigned char* blocks[16] = {};
    size_t sizes[16] = {};
    uintptr_t failures = 0;
    for (unsigned round = 0; round < 2000; ++round)
    {
        const unsigned slot = round % 16;
        if (blocks[slot] != nullptr)
        {
            for (size_t index = 0; index < sizes[slot]; ++index)
            {
                failures += blocks[slot][index] != seed ? 1 : 0;
            }
            free(blocks[slot]);
        }
        sizes[slot] = (round * 37 + seed) % 700 + 1;
        blocks[slot] = static_cast<unsigned char*>(malloc(sizes[slot]));
        if (blocks[slot] == nullptr)
        {
            return reinterpret_cast<void*>(uintptr_t{1});
        }
        if (round % 5 == 0)
        {
            sizes[slot] += 100;
            blocks[slot] = static_cast<unsigned char*>(realloc(blocks[slot], sizes[slot]));
        }
        memset(blocks[slot], seed, sizes[slot]);
    }
    for (unsigned char* block : blocks)
    {
        free(block);
    }
    return reinterpret_cast<void*>(failures);
}

FILE* shared_lines = nullptr;

void* WriteLines(void* argument)
{
    const auto writer = static_cast<int>(reinterpret_cast<uintptr_t>(argument));
    for (int line = 0; line < 100; ++line)
    {
        fprintf(shared_lines, "writer %d line %d end\n", writer, line);
    }
    return nullptr;
}

int slow_constructions = 0;

struct SlowInit
{
    int value;

    SlowInit() : value(0)
    {
        __atomic_add_fetch(&slow_constructions, 1, __ATOMIC_ACQ_REL);
        for (int pause = 0; pause < 20; ++pause)
        {
            sched_yield();
        }
        value = 42;
    }
};

void* ReadSlowStatic(void*)
{
    static SlowInit object;
    return reinterpret_cast<void*>(static_cast<uintptr_t>(object.value));
}

struct Destroyed
{
    int id;
    pthread_t thread;
};

Destroyed destroyed[8];
int destroyed_count = 0;

struct Tracked
{
    int id;

    explicit Tracked(int number) : id(number) {}

    ~Tracked()
    {
        const int index = __atomic_fetch_add(&destroyed_count, 1, __ATOMIC_ACQ_REL);
        destroyed[index] = {id, pthread_self()};
    }
};

thread_local Tracked first_tracked(1);
thread_local Tracked second_tracked(2);

void* UseTracked(void*)
{
    return reinterpret_cast<void*>(static_cast<uintptr_t>(first_tracked.id + second_tracked.id));
}

void* TryLockStdout(void*)
{
    const int locked = ftrylockfile(stdout);
    if (locked == 0)
    {
        funlockfile(stdout);
    }
    return reinterpret_cast<void*>(static_cast<uintptr_t>(locked == 0 ? 1 : 0));
}

unsigned long ReadOwnPages()
{
    char text[512];
    FILE* status_file = fopen("/proc/self/status", "r");
    const size_t length =
        status_file == nullptr ? 0 : fread(text, 1, sizeof(text) - 1, status_file);
    if (status_file != nullptr)
    {
        fclose(status_file);
    }
    text[length] = '\0';
    const char* pages = strstr(text, "\nPages:\t");
    return pages == nullptr ? 0 : strtoul(pages + 8, nullptr, 10);
}

void CheckMemoryReturns()
{
    const unsigned long before = ReadOwnPages();
    pthread_t deep = 0;
    pthread_create(&deep, nullptr, DeepStack, nullptr);
    pthread_join(deep, nullptr);
    const unsigned long after = ReadOwnPages();
    Check(before > 0 && after <= before, "a joined thread's stack pages go back");

    // This process has threads by now, so the pages the heap returns need a TLB shootdown.
    void* const start = sbrk(0);
    void* block = malloc(512 * 1024);
    const unsigned long grown = ReadOwnPages();
    free(block);
    Check(block != nullptr && grown > after + 100 &&
              static_cast<char*>(sbrk(0)) < static_cast<char*>(start) + 256 * 1024 &&
              ReadOwnPages() < grown - 100,
          "a process with threads still gives heap pages back");
}

void CheckThreadSafeLibrary()
{
    errno = EINTR;
    pthread_t thread = 0;
    void* value = nullptr;
    Check(pthread_create(&thread, nullptr, ReportErrno, nullptr) == 0 &&
              pthread_join(thread, &value) == 0 &&
              value == reinterpret_cast<void*>(static_cast<uintptr_t>(EBADF)) && errno == EINTR,
          "each thread has its own errno");

    pthread_t threads[8];
    for (uintptr_t index = 0; index < 8; ++index)
    {
        pthread_create(&threads[index], nullptr, HammerHeap, reinterpret_cast<void*>(index + 0x40));
    }
    bool heap_intact = true;
    for (const pthread_t worker : threads)
    {
        heap_intact = pthread_join(worker, &value) == 0 && value == nullptr && heap_intact;
    }
    Check(heap_intact, "malloc, realloc and free from many threads at once");

    shared_lines = fopen("/tmp/lines.txt", "w");
    for (uintptr_t index = 0; index < 4; ++index)
    {
        pthread_create(&threads[index], nullptr, WriteLines, reinterpret_cast<void*>(index));
    }
    for (uintptr_t index = 0; index < 4; ++index)
    {
        pthread_join(threads[index], nullptr);
    }
    fclose(shared_lines);
    FILE* lines = fopen("/tmp/lines.txt", "r");
    char text[64];
    int whole = 0;
    int read_count = 0;
    while (lines != nullptr && fgets(text, sizeof(text), lines) != nullptr)
    {
        int writer = -1;
        int line = -1;
        char end[8] = {};
        ++read_count;
        whole += sscanf(text, "writer %d line %d %7s", &writer, &line, end) == 3 &&
                         strcmp(end, "end") == 0 && writer >= 0 && writer < 4
                     ? 1
                     : 0;
    }
    if (lines != nullptr)
    {
        fclose(lines);
    }
    unlink("/tmp/lines.txt");
    Check(read_count == 400 && whole == 400, "each fprintf call's line stays whole");

    for (pthread_t& worker : threads)
    {
        pthread_create(&worker, nullptr, ReadSlowStatic, nullptr);
    }
    bool all_initialized = true;
    for (const pthread_t worker : threads)
    {
        all_initialized = pthread_join(worker, &value) == 0 &&
                          value == reinterpret_cast<void*>(uintptr_t{42}) && all_initialized;
    }
    Check(all_initialized && slow_constructions == 1,
          "a function-local static is built once while other threads wait");

    Check(pthread_create(&thread, nullptr, UseTracked, nullptr) == 0 &&
              pthread_join(thread, &value) == 0 && value == reinterpret_cast<void*>(uintptr_t{3}) &&
              destroyed_count == 2 && destroyed[0].id == 2 && destroyed[1].id == 1 &&
              destroyed[0].thread == thread && destroyed[1].thread == thread,
          "thread_local destructors run in their thread, newest first");

    flockfile(stdout);
    pthread_create(&thread, nullptr, TryLockStdout, nullptr);
    pthread_join(thread, &value);
    const bool excluded = value == nullptr;
    funlockfile(stdout);
    pthread_create(&thread, nullptr, TryLockStdout, nullptr);
    pthread_join(thread, &value);
    Check(excluded && value == reinterpret_cast<void*>(uintptr_t{1}),
          "flockfile keeps other threads off a stream");

    const int status = RunChild("thread-local-exit");
    char order[8] = {};
    const int order_file = open("/tmp/order.txt", O_RDONLY);
    const ssize_t order_length = order_file < 0 ? -1 : read(order_file, order, sizeof(order) - 1);
    if (order_file >= 0)
    {
        close(order_file);
    }
    unlink("/tmp/order.txt");
    Check(WIFEXITED(status) && WEXITSTATUS(status) == 0 && order_length == 2 &&
              strcmp(order, "TA") == 0,
          "exit runs thread_local destructors before atexit handlers");
}

int order_file = -1;

struct WritesAtExit
{
    bool used = false;

    ~WritesAtExit()
    {
        write(order_file, "T", 1);
    }
};

thread_local WritesAtExit writes_at_exit;

void WriteAtexitMark()
{
    write(order_file, "A", 1);
}

int RunThreadLocalExitChild()
{
    order_file = open("/tmp/order.txt", O_WRONLY | O_CREAT | O_TRUNC);
    atexit(WriteAtexitMark);
    writes_at_exit.used = true;
    exit(0);
}

void CheckThreadLocalStorage()
{
    bool zeroed = true;
    for (const char byte : tls_zeroed)
    {
        zeroed = zeroed && byte == 0;
    }
    Check(tls_counter == 5 && zeroed && reinterpret_cast<uintptr_t>(tls_aligned) % 64 == 0,
          "thread-local variables start from their template");
    tls_counter = 77;
    memset(tls_zeroed, 'm', sizeof(tls_zeroed));
    Check(ThreadLocalRound(), "each thread gets its own thread-local variables");
    Check(tls_counter == 77 && tls_zeroed[99] == 'm',
          "a thread's thread-local changes stay its own");
    Check(ThreadLocalRound(), "a reused stack slot starts from the template again");
    static thread_local int counted = 0;
    ++counted;
    Check(counted == 1, "thread_local in a function");
}

int RunChild(const char* mode)
{
    char* arguments[] = {const_cast<char*>("libctest"), const_cast<char*>(mode), nullptr};
    pid_t child = 0;
    int status = -1;
    if (posix_spawn(&child, "libctest", nullptr, nullptr, arguments, environ) != 0 ||
        waitpid(child, &status, 0) != child)
    {
        return -1;
    }
    return status;
}

void CheckThreads()
{
    SumTask tasks[4];
    pthread_t threads[4];
    bool created = true;
    for (unsigned long index = 0; index < 4; ++index)
    {
        tasks[index] = {index * 1000 + 1, index * 1000 + 1000, 0, 0};
        created = created && pthread_create(&threads[index], nullptr, SumRange, &tasks[index]) == 0;
    }
    Check(created, "pthread_create");
    bool joined = true;
    for (unsigned long index = 0; index < 4; ++index)
    {
        void* value = nullptr;
        joined = joined && pthread_join(threads[index], &value) == 0 && value == &tasks[index] &&
                 tasks[index].sum == (index * 1000 + 1 + index * 1000 + 1000) * 500;
    }
    Check(joined, "pthread_join returns each thread's value");
    Check(pthread_self() == 1 && pthread_equal(tasks[0].self, threads[0]) != 0 &&
              pthread_equal(tasks[0].self, tasks[1].self) == 0,
          "pthread_self");
    Check(pthread_join(pthread_self(), nullptr) == EDEADLK &&
              pthread_join(threads[0], nullptr) == ESRCH && pthread_join(999, nullptr) == ESRCH,
          "pthread_join refuses itself and unknown threads");

    pthread_t waiting[15];
    int created_count = 0;
    while (created_count < 15 &&
           pthread_create(&waiting[created_count], nullptr, WaitForRelease, nullptr) == 0)
    {
        ++created_count;
    }
    pthread_t extra = 0;
    Check(created_count == 15 && pthread_create(&extra, nullptr, WaitForRelease, nullptr) == EAGAIN,
          "16 threads at most");
    char text[512];
    FILE* status_file = fopen("/proc/self/status", "r");
    const size_t length =
        status_file == nullptr ? 0 : fread(text, 1, sizeof(text) - 1, status_file);
    if (status_file != nullptr)
    {
        fclose(status_file);
    }
    text[length] = '\0';
    Check(strstr(text, "Threads:\t16\n") != nullptr, "/proc counts the threads");
    __atomic_store_n(&release_threads, 1, __ATOMIC_RELEASE);
    bool all_joined = true;
    for (int index = 0; index < created_count; ++index)
    {
        all_joined = pthread_join(waiting[index], nullptr) == 0 && all_joined;
    }
    Check(all_joined, "joined threads free their slots");
    pthread_t deep = 0;
    void* deep_value = nullptr;
    Check(pthread_create(&deep, nullptr, DeepStack, nullptr) == 0 &&
              pthread_join(deep, &deep_value) == 0 &&
              deep_value == reinterpret_cast<void*>(40UL * 41 / 2),
          "a thread's stack grows on demand");

    int status = RunChild("thread-exit");
    Check(WIFEXITED(status) && WEXITSTATUS(status) == 9, "exit in a thread ends the process");
    status = RunChild("thread-fault");
    Check(WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV,
          "a fault in a thread ends the process");
    status = RunChild("main-exit");
    Check(WIFEXITED(status) && WEXITSTATUS(status) == 4,
          "the process outlives a main thread that calls pthread_exit");
    status = RunChild("main-exit-last");
    Check(WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "the last pthread_exit ends the process with status 0");
    char* arguments[] = {const_cast<char*>("libctest"), const_cast<char*>("thread-block"), nullptr};
    pid_t child = 0;
    const timespec pause_time = {0, 50000000};
    Check(posix_spawn(&child, "libctest", nullptr, nullptr, arguments, environ) == 0 &&
              nanosleep(&pause_time, nullptr) == 0 && kill(child, SIGTERM) == 0 &&
              waitpid(child, &status, 0) == child && WIFSIGNALED(status) &&
              WTERMSIG(status) == SIGTERM,
          "a signal ends a process whose threads are all blocked");
}

void* ExitFromThread(void*)
{
    exit(9);
}

void* FaultInThread(void*)
{
    *static_cast<volatile int*>(nullptr) = 1;
    return nullptr;
}

void* ExitAfterMain(void* argument)
{
    const timespec pause_time = {0, 30000000};
    nanosleep(&pause_time, nullptr);
    if (argument != nullptr)
    {
        exit(4);
    }
    return nullptr;
}

void* BlockForever(void*)
{
    for (;;)
    {
        pause();
    }
}

// The children CheckThreads starts.
int RunThreadChild(const char* mode)
{
    pthread_t thread = 0;
    if (strcmp(mode, "thread-exit") == 0 || strcmp(mode, "thread-fault") == 0)
    {
        pthread_create(&thread, nullptr, mode[7] == 'e' ? ExitFromThread : FaultInThread, nullptr);
        pthread_join(thread, nullptr);
        return 1;
    }
    if (strcmp(mode, "thread-block") == 0)
    {
        pthread_create(&thread, nullptr, BlockForever, nullptr);
        pthread_join(thread, nullptr);
        return 1;
    }
    pthread_create(&thread, nullptr, ExitAfterMain,
                   strcmp(mode, "main-exit") == 0 ? reinterpret_cast<void*>(1) : nullptr);
    pthread_exit(nullptr);
}

void CheckTerminalSettings()
{
    termios original;
    Check(tcgetattr(STDIN_FILENO, &original) == 0 && (original.c_lflag & ICANON) != 0 &&
              (original.c_lflag & ECHO) != 0 && (original.c_lflag & ISIG) != 0 &&
              (original.c_iflag & ICRNL) != 0 && original.c_cc[VMIN] == 1 &&
              original.c_cc[VTIME] == 0,
          "tcgetattr reports line input");
    termios raw = original;
    cfmakeraw(&raw);
    raw.c_cc[VMIN] = 0;
    termios check;
    Check(tcsetattr(STDIN_FILENO, TCSANOW, &raw) == 0 && tcgetattr(STDIN_FILENO, &check) == 0 &&
              (check.c_lflag & (ICANON | ECHO | ISIG)) == 0 && (check.c_iflag & ICRNL) == 0 &&
              check.c_cc[VMIN] == 0,
          "tcsetattr switches to raw input");
    char bytes[8];
    Check(read(STDIN_FILENO, bytes, sizeof(bytes)) == 0, "a raw read without a minimum returns");
    raw.c_cc[VTIME] = 2;
    struct timespec before;
    struct timespec after;
    clock_gettime(CLOCK_MONOTONIC, &before);
    const bool timed_read =
        tcsetattr(STDIN_FILENO, TCSANOW, &raw) == 0 && read(STDIN_FILENO, bytes, 1) == 0;
    clock_gettime(CLOCK_MONOTONIC, &after);
    const long elapsed_ms =
        (after.tv_sec - before.tv_sec) * 1000 + (after.tv_nsec - before.tv_nsec) / 1000000;
    Check(timed_read && elapsed_ms >= 150 && elapsed_ms < 2000, "a raw read times out");
    Check(tcsetattr(STDIN_FILENO, TCSAFLUSH, &original) == 0 &&
              tcgetattr(STDIN_FILENO, &check) == 0 && (check.c_lflag & ICANON) != 0,
          "tcsetattr restores line input");
    errno = 0;
    Check(tcsetattr(STDIN_FILENO, 99, &original) == -1 && errno == EINVAL,
          "tcsetattr with an unknown action");
    const int file = open("/system/hello.txt", O_RDONLY);
    errno = 0;
    Check(file >= 3 && tcgetattr(file, &check) == -1 && errno == ENOTTY && close(file) == 0,
          "tcgetattr on a file");
    // The child switches to raw input and exits without switching back.
    char* arguments[] = {const_cast<char*>("libctest"), const_cast<char*>("raw-exit"), nullptr};
    pid_t child = 0;
    int status = -1;
    Check(posix_spawn(&child, "libctest", nullptr, nullptr, arguments, environ) == 0 &&
              waitpid(child, &status, 0) == child && WIFEXITED(status) &&
              WEXITSTATUS(status) == 0 && tcgetattr(STDIN_FILENO, &check) == 0 &&
              (check.c_lflag & ICANON) != 0,
          "terminal settings reset when their owner exits");
}

void CheckSystemCalls()
{
    void* const failed = reinterpret_cast<void*>(-1);
    char* start = static_cast<char*>(sbrk(0));
    Check(start != failed && sbrk(4096) == start && sbrk(0) == start + 4096, "sbrk grows the heap");
    start[4095] = 'x';
    Check(sbrk(-4096) == start + 4096 && sbrk(0) == start, "sbrk shrinks the heap");
    errno = 0;
    Check(sbrk(INTPTR_MIN) == failed && errno == EINVAL, "sbrk below the heap's start");

    constexpr size_t Large = 1024 * 1024;
    void* block = malloc(Large);
    char* grown = static_cast<char*>(sbrk(0));
    free(block);
    Check(block != nullptr && static_cast<char*>(sbrk(0)) + Large / 2 < grown,
          "free returns memory to the kernel");

    // The program's own sbrk memory above malloc's keeps malloc from lowering the break.
    block = malloc(Large);
    char* own = static_cast<char*>(sbrk(4096));
    free(block);
    own[4095] = 'y';
    Check(block != nullptr && own != failed && sbrk(0) == own + 4096 && own[4095] == 'y',
          "malloc leaves the program's sbrk memory alone");
    Check(sbrk(-4096) == own + 4096, "returning the program's sbrk memory");
    const int cpu = sched_getcpu();
    Check(cpu >= 0 && cpu < 64, "sched_getcpu");
    errno = 0;
    Check(reboot(RB_POWER_OFF) == -1 && errno == EPERM, "reboot is refused to programs");
    errno = 0;
    Check(reboot(0) == -1 && errno == EINVAL, "reboot rejects unknown commands");
    Check(featheros_arguments_page() != nullptr, "featheros_arguments_page");
    char line[8];
    errno = 0;
    Check(featheros_read_line("> ", line, 0) == -1 && errno == EINVAL,
          "featheros_read_line needs room");
}

void CheckDescriptors()
{
    const int copy = dup(STDOUT_FILENO);
    Check(copy > STDERR_FILENO && WriteAll(copy, "libctest: written through dup\n") &&
              close(copy) == 0,
          "dup of the console");
    const int file = open("/tmp/libctest-dup.txt", O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    Check(file >= 0 && fcntl(file, F_GETFD) == FD_CLOEXEC && fcntl(file, F_SETFD, 0) == 0 &&
              fcntl(file, F_GETFD) == 0,
          "O_CLOEXEC and F_GETFD/F_SETFD");
    const int saved = fcntl(STDOUT_FILENO, F_DUPFD_CLOEXEC, 20);
    Check(saved >= 20 && dup2(file, STDOUT_FILENO) == STDOUT_FILENO &&
              write(STDOUT_FILENO, "into the file", 13) == 13 &&
              dup2(saved, STDOUT_FILENO) == STDOUT_FILENO && close(saved) == 0,
          "dup2 over stdout and back");
    char text[16] = {};
    Check(lseek(file, 0, SEEK_SET) == 0 && read(file, text, sizeof(text)) == 13 &&
              strcmp(text, "into the file") == 0 && close(file) == 0,
          "dup2 shares the file");
    errno = 0;
    Check(dup2(STDOUT_FILENO, -1) == -1 && errno == EBADF && dup(99) == -1 && errno == EBADF,
          "bad descriptors for dup");

    posix_spawn_file_actions_t actions;
    char* echo_arguments[] = {const_cast<char*>("echo"), const_cast<char*>("into a file"), nullptr};
    pid_t child = 0;
    int status = -1;
    Check(posix_spawn_file_actions_init(&actions) == 0 &&
              posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/tmp/libctest-out.txt",
                                               O_WRONLY | O_CREAT | O_TRUNC, 0644) == 0 &&
              posix_spawn(&child, "echo", &actions, nullptr, echo_arguments, environ) == 0 &&
              waitpid(child, &status, 0) == child && WIFEXITED(status) &&
              FileContains("/tmp/libctest-out.txt", "into a file\n"),
          "posix_spawn_file_actions_addopen");
    Check(WriteAll(STDOUT_FILENO, "libctest: stdout is back after posix_spawn\n"),
          "file actions leave the caller's descriptors");

    const int input = open("/tmp/libctest-out.txt", O_RDONLY | O_CLOEXEC);
    char* upper_arguments[] = {const_cast<char*>("upper"), nullptr};
    Check(input >= 0 && posix_spawn_file_actions_init(&actions) == 0 &&
              posix_spawn_file_actions_adddup2(&actions, input, STDIN_FILENO) == 0 &&
              posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/tmp/libctest-upper.txt",
                                               O_WRONLY | O_CREAT | O_TRUNC, 0644) == 0 &&
              posix_spawn(&child, "upper", &actions, nullptr, upper_arguments, environ) == 0 &&
              waitpid(child, &status, 0) == child && WIFEXITED(status) &&
              WEXITSTATUS(status) == 0 && close(input) == 0 &&
              FileContains("/tmp/libctest-upper.txt", "INTO A FILE\n"),
          "posix_spawn_file_actions_adddup2 onto stdin");
    Check(posix_spawn_file_actions_init(&actions) == 0 &&
              posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/tmp/missing", O_RDONLY,
                                               0) == 0 &&
              posix_spawn(&child, "upper", &actions, nullptr, upper_arguments, environ) == ENOENT,
          "a failing file action fails posix_spawn");
    Check(unlink("/tmp/libctest-dup.txt") == 0 && unlink("/tmp/libctest-out.txt") == 0 &&
              unlink("/tmp/libctest-upper.txt") == 0,
          "removing the descriptor test files");
}

// Everything a descriptor yields until its end, up to size - 1 bytes.
size_t ReadToEnd(int descriptor, char* text, size_t size)
{
    size_t length = 0;
    for (ssize_t count = 1; count > 0 && length < size - 1; length += static_cast<size_t>(count))
    {
        count = read(descriptor, text + length, size - 1 - length);
        if (count < 0)
        {
            break;
        }
    }
    text[length] = '\0';
    return length;
}

void CheckPipes()
{
    int ends[2] = {-1, -1};
    Check(pipe(ends) == 0 && ends[0] > STDERR_FILENO && ends[1] == ends[0] + 1, "pipe");
    struct stat status;
    Check(fstat(ends[0], &status) == 0 && S_ISFIFO(status.st_mode) && !isatty(ends[0]),
          "a pipe is S_IFIFO");
    errno = 0;
    Check(lseek(ends[0], 0, SEEK_SET) == -1 && errno == ESPIPE, "pipes cannot seek");
    char text[64];
    Check(write(ends[1], "through a pipe", 14) == 14 && read(ends[0], text, 7) == 7 &&
              memcmp(text, "through", 7) == 0,
          "bytes come out in order");
    Check(close(ends[1]) == 0 && ReadToEnd(ends[0], text, sizeof(text)) == 7 &&
              strcmp(text, " a pipe") == 0 && read(ends[0], text, 1) == 0,
          "the end of a pipe once its writer closes");
    close(ends[0]);

    Check(pipe(ends) == 0 && close(ends[0]) == 0, "closing the read end");
    // SIGPIPE would end the program, so it is ignored to see the error itself.
    const sighandler_t previous_pipe = signal(SIGPIPE, SIG_IGN);
    errno = 0;
    Check(write(ends[1], "lost", 4) == -1 && errno == EPIPE, "EPIPE without a reader");
    signal(SIGPIPE, previous_pipe);
    close(ends[1]);

    Check(pipe2(ends, O_CLOEXEC) == 0 && fcntl(ends[0], F_GETFD) == FD_CLOEXEC &&
              fcntl(ends[1], F_GETFD) == FD_CLOEXEC,
          "pipe2 with O_CLOEXEC");
    posix_spawn_file_actions_t actions;
    char* echo_arguments[] = {const_cast<char*>("echo"), const_cast<char*>("from a child"),
                              nullptr};
    pid_t child = 0;
    int child_status = -1;
    Check(posix_spawn_file_actions_init(&actions) == 0 &&
              posix_spawn_file_actions_adddup2(&actions, ends[1], STDOUT_FILENO) == 0 &&
              posix_spawn(&child, "echo", &actions, nullptr, echo_arguments, environ) == 0 &&
              close(ends[1]) == 0 && ReadToEnd(ends[0], text, sizeof(text)) == 13 &&
              strcmp(text, "from a child\n") == 0 && waitpid(child, &child_status, 0) == child &&
              close(ends[0]) == 0,
          "reading a child's output through a pipe");

    char* upper_arguments[] = {const_cast<char*>("upper"), nullptr};
    Check(pipe2(ends, O_CLOEXEC) == 0 && posix_spawn_file_actions_init(&actions) == 0 &&
              posix_spawn_file_actions_adddup2(&actions, ends[0], STDIN_FILENO) == 0 &&
              posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/tmp/libctest-pipe.txt",
                                               O_WRONLY | O_CREAT | O_TRUNC, 0644) == 0 &&
              posix_spawn(&child, "upper", &actions, nullptr, upper_arguments, environ) == 0 &&
              close(ends[0]) == 0 && write(ends[1], "piped words\n", 12) == 12 &&
              close(ends[1]) == 0 && waitpid(child, &child_status, 0) == child &&
              WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0 &&
              FileContains("/tmp/libctest-pipe.txt", "PIPED WORDS\n") &&
              unlink("/tmp/libctest-pipe.txt") == 0,
          "writing a child's input through a pipe");
}

bool WaitFor(int descriptor, short events, short* returned)
{
    pollfd entry = {descriptor, events, 0};
    const bool ready = poll(&entry, 1, 5000) == 1;
    *returned = entry.revents;
    return ready;
}

void CheckNonBlockingPipes()
{
    int ends[2] = {-1, -1};
    char text[8];
    errno = 0;
    Check(pipe2(ends, O_NONBLOCK | O_CLOEXEC) == 0 &&
              fcntl(ends[0], F_GETFL) == (O_RDONLY | O_NONBLOCK) &&
              fcntl(ends[1], F_GETFL) == (O_WRONLY | O_NONBLOCK),
          "pipe2 with O_NONBLOCK and F_GETFL");
    Check(read(ends[0], text, sizeof(text)) == -1 && errno == EAGAIN, "EAGAIN from an empty pipe");
    char block[512];
    memset(block, 'b', sizeof(block));
    size_t total = 0;
    ssize_t written = 0;
    while ((written = write(ends[1], block, sizeof(block))) > 0)
    {
        total += static_cast<size_t>(written);
    }
    pollfd full = {ends[1], POLLOUT, 0};
    Check(written == -1 && errno == EAGAIN && total == 4096 && poll(&full, 1, 0) == 0,
          "EAGAIN from a full pipe");
    const int copy = dup(ends[0]);
    Check(copy >= 0 && fcntl(copy, F_SETFL, O_RDONLY) == 0 &&
              (fcntl(ends[0], F_GETFL) & O_NONBLOCK) == 0 && close(copy) == 0,
          "F_SETFL changes the open file dup shares");
    short events = 0;
    Check(fcntl(ends[0], F_SETFL, O_NONBLOCK) == 0 && read(ends[0], block, sizeof(block)) == 512 &&
              WaitFor(ends[1], POLLOUT, &events) && events == POLLOUT,
          "reading makes room");
    close(ends[0]);
    close(ends[1]);
    errno = 0;
    Check(fcntl(STDIN_FILENO, F_SETFL, O_NONBLOCK) == -1 && errno == EINVAL &&
              (fcntl(STDIN_FILENO, F_GETFL) & O_NONBLOCK) == 0,
          "the console stays blocking");
}

sockaddr_in LoopbackAddress(uint16_t port)
{
    sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    return address;
}

void CheckNonBlockingSockets()
{
    const int listener = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    sockaddr_in address = LoopbackAddress(0);
    socklen_t length = sizeof(address);
    Check(listener >= 0 && fcntl(listener, F_GETFD) == FD_CLOEXEC &&
              bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0 &&
              listen(listener, 2) == 0 &&
              getsockname(listener, reinterpret_cast<sockaddr*>(&address), &length) == 0,
          "a non-blocking listener");
    errno = 0;
    Check(accept(listener, nullptr, nullptr) == -1 && errno == EAGAIN,
          "EAGAIN from accept with nothing waiting");

    const int client = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    errno = 0;
    Check(connect(client, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == -1 &&
              errno == EINPROGRESS,
          "EINPROGRESS from connect");
    short events = 0;
    int error = -1;
    socklen_t error_length = sizeof(error);
    Check(WaitFor(client, POLLOUT, &events) && events == POLLOUT &&
              getsockopt(client, SOL_SOCKET, SO_ERROR, &error, &error_length) == 0 && error == 0 &&
              error_length == sizeof(int),
          "the connection opens in the background");
    int accepted = -1;
    Check(WaitFor(listener, POLLIN, &events) &&
              (accepted = accept4(listener, nullptr, nullptr, SOCK_NONBLOCK)) >= 0 &&
              (fcntl(accepted, F_GETFL) & O_NONBLOCK) != 0,
          "accept4 with SOCK_NONBLOCK");
    char text[8];
    errno = 0;
    Check(recv(accepted, text, sizeof(text), 0) == -1 && errno == EAGAIN,
          "EAGAIN from recv with nothing arrived");
    Check(send(client, "ping", 4, 0) == 4 && WaitFor(accepted, POLLIN, &events) &&
              recv(accepted, text, sizeof(text), 0) == 4 && memcmp(text, "ping", 4) == 0,
          "data through non-blocking sockets");
    errno = 0;
    Check(fcntl(client, F_SETFL, 0) == 0 && recv(client, text, sizeof(text), MSG_DONTWAIT) == -1 &&
              errno == EAGAIN,
          "MSG_DONTWAIT on a blocking socket");
    timeval timeout = {.tv_sec = 1, .tv_usec = 500000};
    timeval read_back = {};
    socklen_t timeout_length = sizeof(read_back);
    Check(setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0 &&
              getsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &read_back, &timeout_length) == 0 &&
              read_back.tv_sec == 1 && read_back.tv_usec == 500000,
          "getsockopt SO_RCVTIMEO");
    close(accepted);
    close(client);
    close(listener);

    // Nothing listens on the port the listener had, so the connection is refused.
    const int refused = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    errno = 0;
    Check(connect(refused, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == -1 &&
              errno == EINPROGRESS && WaitFor(refused, POLLOUT, &events) &&
              (events & POLLERR) != 0 &&
              getsockopt(refused, SOL_SOCKET, SO_ERROR, &error, &error_length) == 0 &&
              error == ECONNREFUSED,
          "SO_ERROR reports a refused connection");
    close(refused);
}

void CheckScanning()
{
    int count = 0;
    char fruit[16];
    Check(sscanf("7 apples", "%d %15s", &count, fruit) == 2 && count == 7 &&
              strcmp(fruit, "apples") == 0,
          "sscanf");
    FILE* file = fopen("/home/LIBCSCAN.TXT", "w+");
    Check(file != nullptr && fprintf(file, "width=%d height=%d\n", 640, 480) > 0, "fprintf");
    rewind(file);
    int width = 0;
    int height = 0;
    Check(fscanf(file, "width=%d height=%d", &width, &height) == 2 && width == 640 && height == 480,
          "fscanf");
    Check(fclose(file) == 0 && remove("/home/LIBCSCAN.TXT") == 0, "removing the scan file");
}

bool Formats(const char* expected, const char* format, ...) __attribute__((format(printf, 2, 3)));

bool Formats(const char* expected, const char* format, ...)
{
    char text[128];
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(text, sizeof(text), format, arguments);
    va_end(arguments);
    if (strcmp(text, expected) != 0)
    {
        printf("libctest: \"%s\" gave \"%s\", expected \"%s\"\n", format, text, expected);
        return false;
    }
    return true;
}

bool Near(double value, double expected, double tolerance)
{
    return fabs(value - expected) <= tolerance;
}

void CheckFloatingPoint()
{
    Check(Formats("3.141593 3.142e+00 3.14159 0x1.921fb54442d18p+1", "%f %.3e %g %a", M_PI, M_PI,
                  M_PI, M_PI),
          "printf of pi");
    Check(Formats("[  -0.500] [1e+100] [0.0001] [1e-05] [  inf] [nan]",
                  "[%8.3f] [%g] [%g] [%g] [%5f] [%f]", -0.5, 1e100, 1e-4, 1e-5, HUGE_VAL, NAN),
          "printf widths and %g");
    Check(Formats("0.1000000000000000055511151231257827021181583404541015625", "%.55f", 0.1),
          "printf of 0.1 exactly");
    Check(Formats("2.5 2 4", "%.1f %.0f %.0f", 2.5, 2.5, 3.5), "printf rounds ties to even");
#if __LDBL_MANT_DIG__ == 64
    Check(Formats("0.1000000000000000000013553", "%.25Lf", 0.1L), "printf of an x87 long double");
#elif __LDBL_MANT_DIG__ == 113
    Check(Formats("0.1000000000000000000000000", "%.25Lf", 0.1L), "printf of a quad long double");
#endif

    char* end = nullptr;
    const char number[] = "  -1.25e3xyz";
    Check(strtod(number, &end) == -1250.0 && end == number + 9, "strtod");
    Check(strtod("0x1.8p1", nullptr) == 3.0 && atof("2.5") == 2.5, "strtod hex and atof");
    Check(strtof("0.1", nullptr) == 0.1f, "strtof");
    const long double parsed = strtold("0.1", nullptr);
    const long double literal = 0.1L;
    // An x87 long double fills 16 bytes, but only the first 10 hold the value.
    const size_t value_bytes = __LDBL_MANT_DIG__ == 64 ? 10 : sizeof(long double);
    Check(memcmp(&parsed, &literal, value_bytes) == 0, "strtold");
    errno = 0;
    Check(isinf(strtod("1e400", nullptr)) && errno == ERANGE, "strtod overflow");
    Check(isnan(strtod("nan", nullptr)), "strtod NaN");

    double value = 0;
    float single = 0;
    Check(sscanf("6.25 0.5", "%lf %f", &value, &single) == 2 && value == 6.25 && single == 0.5f,
          "sscanf floats");

    Check(sqrt(2.0) * sqrt(2.0) - 2.0 < 1e-15 && sqrt(16.0) == 4.0, "sqrt");
    Check(Near(sin(M_PI / 6), 0.5, 1e-15) && Near(cos(M_PI / 3), 0.5, 1e-15) &&
              Near(tan(M_PI / 4), 1.0, 1e-15),
          "sin, cos, and tan");
    Check(Near(exp(1.0), M_E, 1e-15) && Near(log(M_E), 1.0, 1e-15) && log2(1024.0) == 10.0 &&
              Near(log10(1000.0), 3.0, 1e-15),
          "exp and the logarithms");
    Check(pow(2.0, 10.0) == 1024.0 && Near(pow(2.0, 0.5), M_SQRT2, 1e-15), "pow");
    Check(Near(atan2(1.0, 1.0), M_PI_4, 1e-15) && Near(asin(1.0), M_PI_2, 1e-15) &&
              Near(acos(-1.0), M_PI, 1e-15),
          "the inverse functions");
    Check(floor(-2.5) == -3.0 && ceil(-2.5) == -2.0 && round(-2.5) == -3.0 && trunc(-2.5) == -2.0 &&
              fmod(10.0, 3.0) == 1.0,
          "rounding and fmod");
    int exponent = 0;
    Check(frexp(48.0, &exponent) == 0.75 && exponent == 6 && ldexp(0.75, 6) == 48.0, "frexp");
    errno = 0;
    Check(isnan(log(-1.0)) && errno == EDOM, "log of a negative number");
    errno = 0;
    Check(isinf(exp(1000.0)) && errno == ERANGE, "exp overflow");
    errno = 0;
    Check(isinf(pow(0.0, -1.0)) && errno == ERANGE, "pow pole");
    Check(sqrtf(2.25f) == 1.5f && Near(sinf(1.0f), 0.841470985, 1e-7), "float functions");

    // Lots of floating-point work while other programs run, checked against a known sum.
    double sum = 0;
    for (int term = 1; term <= 200000; ++term)
    {
        sum += 1.0 / (static_cast<double>(term) * term);
    }
    Check(Near(sum, M_PI * M_PI / 6, 1e-5), "a long floating-point loop");
}

void CheckAssert()
{
    const uint64_t child = FeatherOS::User::Spawn("libctest assert");
    uint64_t status = 0;
    Check(!FeatherOS::User::IsSyscallError(child) &&
              !FeatherOS::User::IsSyscallError(FeatherOS::User::Wait(child, &status)) &&
              status == FeatherOS::User::ExitStatusSignalBase + SIGABRT,
          "a failed assert aborts with SIGABRT");
    assert(passed_checks > 0);
}

} // namespace

int main(int argc, char** argv)
{
    if (argc == 2 && (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0))
    {
        puts(Help);
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "exit7") == 0)
    {
        return 7;
    }
    if (argc == 2 && strcmp(argv[1], "thread-local-exit") == 0)
    {
        return RunThreadLocalExitChild();
    }
    if (argc == 2 && strncmp(argv[1], "thread-", 7) == 0)
    {
        return RunThreadChild(argv[1]);
    }
    if (argc == 2 && strncmp(argv[1], "main-exit", 9) == 0)
    {
        return RunThreadChild(argv[1]);
    }
    if (argc == 2 && strcmp(argv[1], "raw-exit") == 0)
    {
        termios raw;
        if (tcgetattr(STDIN_FILENO, &raw) != 0)
        {
            return 1;
        }
        cfmakeraw(&raw);
        return tcsetattr(STDIN_FILENO, TCSANOW, &raw) == 0 && tcgetattr(STDIN_FILENO, &raw) == 0 &&
                       (raw.c_lflag & ICANON) == 0
                   ? 0
                   : 1;
    }
    // The child CheckAssert starts: a failed assert prints its message and aborts.
    if (argc == 2 && strcmp(argv[1], "assert") == 0)
    {
        assert(argc == 1);
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
    CheckAllocation();
    CheckHeapReuse();
    CheckFormatting();
    CheckFileStreams();
    CheckDescriptorFiles();
    CheckDirectories();
    CheckVirtualFiles();
    CheckAddressNames();
    CheckTime();
    CheckSpawn();
    CheckDiskPrograms();
    CheckSpawnAttributes();
    CheckProcessGroups();
    CheckSignalSets();
    CheckSignalHandlers();
    CheckKill();
    CheckProcessAccounting();
    CheckDescriptors();
    CheckPipes();
    CheckNonBlockingPipes();
    CheckNonBlockingSockets();
    CheckSystemCalls();
    CheckTerminalSettings();
    CheckThreads();
    CheckThreadLocalStorage();
    CheckSynchronization();
    CheckThreadSafeLibrary();
    CheckMemoryReturns();
    CheckScanning();
    CheckFloatingPoint();
    CheckAssert();
    printf("libctest: %llu checks passed\n", static_cast<unsigned long long>(passed_checks));
    return 0;
}
