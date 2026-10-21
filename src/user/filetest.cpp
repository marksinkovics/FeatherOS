#include "process.h"
#include "runtime.h"

using namespace FeatherOS::User;

namespace
{

#if defined(__x86_64__)
constexpr uint64_t KernelAddress = 0xFFFF800000000000;
#elif defined(__aarch64__)
constexpr uint64_t KernelAddress = 0xFFFF000000000000;
#endif

constexpr char TestFile[] = "/FILETEST.TXT";
constexpr char InitramfsFile[] = "/system/hello.txt";
// Larger than one disk cluster, so the file spans several.
constexpr uint64_t PatternSize = 3000;

uint8_t pattern[PatternSize];
uint8_t buffer[PatternSize];
uint64_t passed_checks = 0;

void Check(bool passed, const char* name)
{
    if (!passed)
    {
        WriteText("filetest: failed ");
        WriteLine(name);
        Exit(1);
    }
    ++passed_checks;
}

bool Valid(uint64_t descriptor)
{
    return !IsSyscallError(descriptor) && descriptor >= FirstFileDescriptor;
}

bool SameBytes(const uint8_t* left, const uint8_t* right, uint64_t length)
{
    for (uint64_t index = 0; index < length; ++index)
    {
        if (left[index] != right[index])
        {
            return false;
        }
    }
    return true;
}

void CheckArgumentsAndCreation()
{
    const auto* kernel_pointer = reinterpret_cast<const char*>(KernelAddress);
    Check(OpenFile(TestFile, OpenRead) == SyscallErrorNotFound, "opening a missing file");
    Check(OpenFile(TestFile, 0) == SyscallErrorInvalidArgument, "opening without access");
    Check(OpenFile(TestFile, OpenRead | OpenCreate) == SyscallErrorInvalidArgument,
          "creating without write access");
    Check(OpenFile(kernel_pointer, OpenRead) == SyscallErrorBadAddress, "a kernel path pointer");

    const uint64_t writer = OpenFile(TestFile, OpenWrite | OpenCreate | OpenTruncate);
    Check(Valid(writer), "creating a file");
    Check(WriteFile(writer, pattern, PatternSize) == PatternSize, "writing across clusters");
    Check(WriteFile(writer, kernel_pointer, 16) == SyscallErrorBadAddress,
          "writing from kernel memory");
    Check(OpenFile(TestFile, OpenRead) == SyscallErrorBusy, "reading a file being written");
    Check(RemoveFile(TestFile) == SyscallErrorBusy, "removing an open file");
    Check(ReadFile(writer, buffer, 1) == SyscallErrorBadDescriptor,
          "reading a write-only descriptor");
    Check(CloseFile(writer) == 0, "closing a descriptor");
    Check(CloseFile(writer) == SyscallErrorBadDescriptor, "closing it twice");
    Check(ReadFile(writer, buffer, 1) == SyscallErrorBadDescriptor, "using a closed descriptor");
}

void CheckReading()
{
    const uint64_t reader = OpenFile(TestFile, OpenRead);
    const uint64_t second_reader = OpenFile(TestFile, OpenRead);
    Check(Valid(reader) && Valid(second_reader), "two readers at once");
    uint64_t total = 0;
    while (total < PatternSize)
    {
        const uint64_t wanted = PatternSize - total < 700 ? PatternSize - total : 700;
        const uint64_t count = ReadFile(reader, buffer + total, wanted);
        if (IsSyscallError(count) || count == 0)
        {
            break;
        }
        total += count;
    }
    Check(total == PatternSize && SameBytes(buffer, pattern, PatternSize), "reading it back");
    Check(ReadFile(reader, buffer, 10) == 0, "reading at the end of the file");
    Check(ReadFile(reader, reinterpret_cast<void*>(KernelAddress), 10) == SyscallErrorBadAddress,
          "reading into kernel memory");
    Check(SeekFile(second_reader, 100) == 0 && ReadFile(second_reader, buffer, 10) == 10 &&
              SameBytes(buffer, pattern + 100, 10),
          "seeking");
    Check(SeekFile(second_reader, PatternSize + 1) == SyscallErrorInvalidArgument,
          "seeking past the end");
    Check(WriteFile(reader, "x", 1) == SyscallErrorBadDescriptor, "writing a read-only descriptor");
    Check(CloseFile(reader) == 0 && CloseFile(second_reader) == 0, "closing both readers");

    const uint64_t appender = OpenFile(TestFile, OpenWrite);
    Check(Valid(appender) && SeekFile(appender, PatternSize) == 0 &&
              WriteFile(appender, "END", 3) == 3 && CloseFile(appender) == 0,
          "appending");
    const uint64_t tail_reader = OpenFile(TestFile, OpenRead);
    Check(SeekFile(tail_reader, PatternSize - 1) == 0 && ReadFile(tail_reader, buffer, 10) == 4 &&
              buffer[0] == pattern[PatternSize - 1] &&
              SameBytes(buffer + 1, reinterpret_cast<const uint8_t*>("END"), 3) &&
              CloseFile(tail_reader) == 0,
          "reading the appended bytes");
}

void CheckInitramfsAndLimits()
{
    const uint64_t archive_file = OpenFile(InitramfsFile, OpenRead);
    Check(Valid(archive_file) && ReadFile(archive_file, buffer, 9) == 9 &&
              SameBytes(buffer, reinterpret_cast<const uint8_t*>("FeatherOS"), 9),
          "reading the initramfs");
    Check(OpenFile(InitramfsFile, OpenWrite) == SyscallErrorReadOnly, "writing the initramfs");
    Check(RemoveFile(InitramfsFile) == SyscallErrorReadOnly, "removing from the initramfs");

    uint64_t descriptors[MaximumOpenFiles];
    descriptors[0] = archive_file;
    bool all_opened = true;
    for (uint64_t index = 1; index < MaximumOpenFiles; ++index)
    {
        descriptors[index] = OpenFile(InitramfsFile, OpenRead);
        all_opened = all_opened && Valid(descriptors[index]);
    }
    Check(all_opened, "opening up to the descriptor limit");
    Check(OpenFile(InitramfsFile, OpenRead) == SyscallErrorTooManyOpenFiles,
          "opening past the descriptor limit");
    for (const uint64_t descriptor : descriptors)
    {
        CloseFile(descriptor);
    }
}

} // namespace

constexpr char Help[] =
    "usage: filetest\n"
    "Runs checks of the file syscalls on the disk: descriptors, sharing rules,\n"
    "bad pointers, the open-file limit, and cleanup when a process exits.";

extern "C" [[noreturn]] void _start(const ProgramArguments* arguments)
{
    ExitWithHelpIfRequested(arguments, Help);
    // The child the main run spawns: it exits with a writer still open.
    if (arguments != nullptr && arguments->argument_count == 2 &&
        StringEquals(arguments->argument_values[1], "hold"))
    {
        Exit(Valid(OpenFile(TestFile, OpenWrite)) ? 0 : 1);
    }

    for (uint64_t index = 0; index < PatternSize; ++index)
    {
        pattern[index] = static_cast<uint8_t>('A' + index % 26);
    }
    CheckArgumentsAndCreation();
    CheckReading();
    CheckInitramfsAndLimits();

    const uint64_t child = Spawn("filetest hold");
    uint64_t status = 1;
    Check(!IsSyscallError(child) && !IsSyscallError(Wait(child, &status)) && status == 0,
          "a child that exits with a file open");
    const uint64_t writer = OpenFile(TestFile, OpenWrite);
    Check(Valid(writer) && CloseFile(writer) == 0, "exiting closes descriptors");

    Check(RemoveFile(TestFile) == 0, "removing the file");
    Check(OpenFile(TestFile, OpenRead) == SyscallErrorNotFound, "the removed file is gone");
    Check(RemoveFile(TestFile) == SyscallErrorNotFound, "removing it twice");

    WriteText("filetest: ");
    WriteNumber(passed_checks);
    WriteLine(" checks passed");
    Exit(0);
}
