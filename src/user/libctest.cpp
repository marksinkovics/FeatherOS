#include "syscall.h"

#include <assert.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <featheros.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <sched.h>
#include <setjmp.h>
#include <stdarg.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

namespace
{

constexpr char Help[] =
    "usage: libctest one \"two words\"\n"
    "Checks the C library: startup and arguments, constructors, string.h, ctype.h,\n"
    "stdlib.h, errno, setjmp, exit handlers, the console descriptors, stat, process\n"
    "IDs, the environment, the heap, stdio, files and directories, /dev, /tmp,\n"
    "/proc, time, posix_spawn with file actions, dup, fcntl, pipes, the FeatherOS\n"
    "extensions, and floating point. It asks for three lines of input at \"> \"\n"
    "prompts.";

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

    char* sleep_arguments[] = {const_cast<char*>("sleep"), const_cast<char*>("300"), nullptr};
    Check(posix_spawn(&child, "sleep", nullptr, nullptr, sleep_arguments, environ) == 0 &&
              waitpid(child, &status, WNOHANG) == 0 && waitpid(child, &status, 0) == child,
          "WNOHANG while the child runs");

    Check(posix_spawn(&child, "/home/program", nullptr, nullptr, echo_arguments, environ) == ENOENT,
          "posix_spawn outside /system");
    errno = 0;
    Check(waitpid(-1, &status, 0) == -1 && errno == ECHILD, "waitpid with no children");
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
    char* arguments[] = {const_cast<char*>("sleep"), const_cast<char*>("300"), nullptr};
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
    errno = 0;
    Check(write(ends[1], "lost", 4) == -1 && errno == EPIPE, "EPIPE without a reader");
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
              status == 134,
          "a failed assert aborts");
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
    CheckTime();
    CheckSpawn();
    CheckSpawnAttributes();
    CheckProcessGroups();
    CheckDescriptors();
    CheckPipes();
    CheckSystemCalls();
    CheckScanning();
    CheckFloatingPoint();
    CheckAssert();
    printf("libctest: %llu checks passed\n", static_cast<unsigned long long>(passed_checks));
    return 0;
}
