#include "libc/exit_handlers.h"
#include "libc/format.h"
#include "libc/scan.h"
#include "libc/syscall_result.h"
#include "user/syscall.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

namespace
{

enum : unsigned
{
    Readable = 1U << 0,
    Writable = 1U << 1,
    // Every write goes to the end of the file, wherever the stream was moved.
    Append = 1U << 2,
    AtEnd = 1U << 3,
    Failed = 1U << 4,
    InUse = 1U << 5,
    Console = 1U << 6,
};

constexpr int NoPushback = -1;

} // namespace

// buffer[position, length) is unread input while the stream is reading, and buffer[0, length)
// is output not yet written while it is writing. offset is the descriptor's own position.
struct FeatherFile
{
    int descriptor;
    unsigned flags;
    int buffering;
    bool reading;
    int pushback;
    size_t length;
    size_t position;
    long offset;
    unsigned char buffer[BUFSIZ];
};

namespace
{

FeatherFile standard_streams[3] = {
    {0, Readable | Writable | InUse | Console, _IOLBF, false, NoPushback, 0, 0, 0, {}},
    {1, Readable | Writable | InUse | Console, _IOLBF, false, NoPushback, 0, 0, 0, {}},
    {2, Readable | Writable | InUse | Console, _IONBF, false, NoPushback, 0, 0, 0, {}},
};
FeatherFile opened_streams[FOPEN_MAX - 3];

bool WriteAll(FeatherFile* stream, const unsigned char* bytes, size_t count)
{
    while (count > 0)
    {
        const ssize_t written = write(stream->descriptor, bytes, count);
        if (written <= 0)
        {
            stream->flags |= Failed;
            return false;
        }
        bytes += written;
        count -= static_cast<size_t>(written);
        stream->offset += written;
    }
    return true;
}

bool SeekDescriptor(FeatherFile* stream, long offset)
{
    if (lseek(stream->descriptor, offset, SEEK_SET) < 0)
    {
        return false;
    }
    stream->offset = offset;
    return true;
}

long FileSize(FeatherFile* stream)
{
    struct stat status;
    return fstat(stream->descriptor, &status) == 0 ? static_cast<long>(status.st_size) : -1;
}

// Writes out buffered output, or drops buffered input after moving the descriptor back to
// where the caller has read up to.
bool Settle(FeatherFile* stream)
{
    if (stream->reading)
    {
        const long unread = static_cast<long>(stream->length - stream->position) +
                            (stream->pushback != NoPushback ? 1 : 0);
        stream->reading = false;
        stream->length = 0;
        stream->position = 0;
        stream->pushback = NoPushback;
        return unread == 0 || (stream->flags & Console) != 0 ||
               SeekDescriptor(stream, stream->offset - unread);
    }
    if (stream->length == 0)
    {
        return true;
    }
    if ((stream->flags & Append) != 0)
    {
        const long size = FileSize(stream);
        if (size < 0 || !SeekDescriptor(stream, size))
        {
            stream->flags |= Failed;
            return false;
        }
    }
    const size_t length = stream->length;
    stream->length = 0;
    return WriteAll(stream, stream->buffer, length);
}

bool PutByte(FeatherFile* stream, unsigned char byte)
{
    if ((stream->flags & Writable) == 0)
    {
        stream->flags |= Failed;
        errno = EBADF;
        return false;
    }
    if (stream->reading && !Settle(stream))
    {
        return false;
    }
    stream->buffer[stream->length++] = byte;
    if (stream->length == sizeof(stream->buffer) || (stream->buffering == _IOLBF && byte == '\n'))
    {
        return Settle(stream);
    }
    return true;
}

// Unbuffered streams still collect one call's output and write it at once.
bool FinishOutput(FeatherFile* stream)
{
    return stream->buffering != _IONBF || Settle(stream);
}

int GetByte(FeatherFile* stream)
{
    if ((stream->flags & Readable) == 0)
    {
        stream->flags |= Failed;
        errno = EBADF;
        return EOF;
    }
    if (stream->pushback != NoPushback)
    {
        const int byte = stream->pushback;
        stream->pushback = NoPushback;
        return byte;
    }
    if (!stream->reading)
    {
        if (!Settle(stream))
        {
            return EOF;
        }
        stream->reading = true;
    }
    if (stream->position == stream->length)
    {
        if ((stream->flags & Console) != 0)
        {
            fflush(stdout);
        }
        const ssize_t count = read(stream->descriptor, stream->buffer, sizeof(stream->buffer));
        if (count <= 0)
        {
            stream->flags |= count == 0 ? AtEnd : Failed;
            return EOF;
        }
        stream->offset += count;
        stream->length = static_cast<size_t>(count);
        stream->position = 0;
    }
    return stream->buffer[stream->position++];
}

// The open flags for a mode string, or false for a malformed one.
bool ParseMode(const char* mode, unsigned* flags, uint64_t* open_flags)
{
    if (mode == nullptr)
    {
        return false;
    }
    switch (mode[0])
    {
    case 'r':
        *flags = Readable;
        *open_flags = FeatherOS::User::OpenRead;
        break;
    case 'w':
        *flags = Writable;
        *open_flags = FeatherOS::User::OpenWrite | FeatherOS::User::OpenCreate |
                      FeatherOS::User::OpenTruncate;
        break;
    case 'a':
        *flags = Writable | Append;
        *open_flags =
            FeatherOS::User::OpenWrite | FeatherOS::User::OpenCreate | FeatherOS::User::OpenAppend;
        break;
    default:
        return false;
    }
    for (const char* option = mode + 1; *option != '\0'; ++option)
    {
        if (*option == '+')
        {
            *flags |= Readable | Writable;
            *open_flags |= FeatherOS::User::OpenRead | FeatherOS::User::OpenWrite;
        }
        else if (*option != 'b')
        {
            return false;
        }
    }
    return true;
}

FeatherFile* NewStream(int descriptor, unsigned flags)
{
    for (FeatherFile& stream : opened_streams)
    {
        if ((stream.flags & InUse) != 0)
        {
            continue;
        }
        struct stat status;
        const bool console = fstat(descriptor, &status) == 0 && S_ISCHR(status.st_mode);
        stream = {descriptor,
                  flags | InUse | (console ? Console : 0U),
                  console ? _IOLBF : _IOFBF,
                  false,
                  NoPushback,
                  0,
                  0,
                  0,
                  {}};
        return &stream;
    }
    errno = EMFILE;
    return nullptr;
}

void FlushAll()
{
    fflush(nullptr);
}

__attribute__((constructor)) void RegisterExitFlush()
{
    FeatherOS::LibC::SetExitFlush(FlushAll);
}

struct StreamSink
{
    FeatherFile* stream;
    bool failed = false;

    void Put(char character)
    {
        failed = !PutByte(stream, static_cast<unsigned char>(character)) || failed;
    }
};

// Collects output in chunks and writes each with one write call.
struct DescriptorSink
{
    int descriptor;
    char chunk[128];
    size_t length = 0;
    bool failed = false;

    void Put(char character)
    {
        chunk[length++] = character;
        if (length == sizeof(chunk))
        {
            Flush();
        }
    }

    void Flush()
    {
        const char* cursor = chunk;
        while (length > 0)
        {
            const ssize_t written = write(descriptor, cursor, length);
            if (written <= 0)
            {
                failed = true;
                length = 0;
                return;
            }
            cursor += written;
            length -= static_cast<size_t>(written);
        }
    }
};

} // namespace

FILE* stdin = &standard_streams[0];
FILE* stdout = &standard_streams[1];
FILE* stderr = &standard_streams[2];

FILE* fopen(const char* path, const char* mode)
{
    unsigned flags;
    uint64_t open_flags;
    if (!ParseMode(mode, &flags, &open_flags))
    {
        errno = EINVAL;
        return nullptr;
    }
    const int descriptor = FeatherOS::LibC::Check<int>(FeatherOS::User::OpenFile(path, open_flags));
    if (descriptor < 0)
    {
        return nullptr;
    }
    FeatherFile* stream = NewStream(descriptor, flags);
    if (stream == nullptr)
    {
        close(descriptor);
        return nullptr;
    }
    if ((flags & Append) != 0)
    {
        stream->offset = FileSize(stream);
    }
    return stream;
}

FILE* fdopen(int descriptor, const char* mode)
{
    unsigned flags;
    uint64_t open_flags;
    if (!ParseMode(mode, &flags, &open_flags))
    {
        errno = EINVAL;
        return nullptr;
    }
    struct stat status;
    if (fstat(descriptor, &status) != 0)
    {
        return nullptr;
    }
    return NewStream(descriptor, flags);
}

int fclose(FILE* stream)
{
    const bool flushed = Settle(stream);
    const bool closed = close(stream->descriptor) == 0;
    stream->flags = 0;
    return flushed && closed ? 0 : EOF;
}

int fflush(FILE* stream)
{
    if (stream != nullptr)
    {
        return stream->reading || Settle(stream) ? 0 : EOF;
    }
    int result = 0;
    for (FeatherFile& standard : standard_streams)
    {
        result = fflush(&standard) == 0 ? result : EOF;
    }
    for (FeatherFile& opened : opened_streams)
    {
        if ((opened.flags & InUse) != 0)
        {
            result = fflush(&opened) == 0 ? result : EOF;
        }
    }
    return result;
}

int setvbuf(FILE* stream, char*, int mode, size_t)
{
    if (mode != _IOFBF && mode != _IOLBF && mode != _IONBF)
    {
        errno = EINVAL;
        return -1;
    }
    if (!Settle(stream))
    {
        return -1;
    }
    stream->buffering = mode;
    return 0;
}

void setbuf(FILE* stream, char* buffer)
{
    setvbuf(stream, buffer, buffer == nullptr ? _IONBF : _IOFBF, BUFSIZ);
}

size_t fread(void* buffer, size_t size, size_t count, FILE* stream)
{
    if (size == 0 || count == 0)
    {
        return 0;
    }
    auto* bytes = static_cast<unsigned char*>(buffer);
    for (size_t element = 0; element < count; ++element)
    {
        for (size_t index = 0; index < size; ++index)
        {
            const int byte = GetByte(stream);
            if (byte == EOF)
            {
                return element;
            }
            bytes[element * size + index] = static_cast<unsigned char>(byte);
        }
    }
    return count;
}

size_t fwrite(const void* buffer, size_t size, size_t count, FILE* stream)
{
    if (size == 0 || count == 0)
    {
        return 0;
    }
    const auto* bytes = static_cast<const unsigned char*>(buffer);
    size_t element = 0;
    for (; element < count; ++element)
    {
        bool written = true;
        for (size_t index = 0; index < size && written; ++index)
        {
            written = PutByte(stream, bytes[element * size + index]);
        }
        if (!written)
        {
            break;
        }
    }
    return FinishOutput(stream) ? element : 0;
}

int fgetc(FILE* stream)
{
    return GetByte(stream);
}

int getc(FILE* stream)
{
    return GetByte(stream);
}

int getchar()
{
    return GetByte(stdin);
}

int ungetc(int character, FILE* stream)
{
    if (character == EOF || stream->pushback != NoPushback)
    {
        return EOF;
    }
    if (!stream->reading && !Settle(stream))
    {
        return EOF;
    }
    stream->reading = true;
    stream->pushback = static_cast<unsigned char>(character);
    stream->flags &= ~static_cast<unsigned>(AtEnd);
    return stream->pushback;
}

char* fgets(char* buffer, int size, FILE* stream)
{
    if (size <= 0)
    {
        return nullptr;
    }
    int length = 0;
    while (length < size - 1)
    {
        const int byte = GetByte(stream);
        if (byte == EOF)
        {
            break;
        }
        buffer[length++] = static_cast<char>(byte);
        if (byte == '\n')
        {
            break;
        }
    }
    if (length == 0 && size > 1)
    {
        return nullptr;
    }
    buffer[length] = '\0';
    return buffer;
}

ssize_t getline(char** line, size_t* capacity, FILE* stream)
{
    if (line == nullptr || capacity == nullptr)
    {
        errno = EINVAL;
        return -1;
    }
    size_t length = 0;
    for (;;)
    {
        if (*line == nullptr || length + 1 >= *capacity)
        {
            const size_t grown = *capacity < 64 ? 64 : *capacity * 2;
            auto* larger = static_cast<char*>(realloc(*line, grown));
            if (larger == nullptr)
            {
                return -1;
            }
            *line = larger;
            *capacity = grown;
        }
        const int byte = GetByte(stream);
        if (byte == EOF)
        {
            break;
        }
        (*line)[length++] = static_cast<char>(byte);
        if (byte == '\n')
        {
            break;
        }
    }
    (*line)[length] = '\0';
    return length == 0 ? -1 : static_cast<ssize_t>(length);
}

int fputc(int character, FILE* stream)
{
    const auto byte = static_cast<unsigned char>(character);
    return PutByte(stream, byte) && FinishOutput(stream) ? byte : EOF;
}

int putc(int character, FILE* stream)
{
    return fputc(character, stream);
}

int putchar(int character)
{
    return fputc(character, stdout);
}

int fputs(const char* text, FILE* stream)
{
    for (; *text != '\0'; ++text)
    {
        if (!PutByte(stream, static_cast<unsigned char>(*text)))
        {
            return EOF;
        }
    }
    return FinishOutput(stream) ? 0 : EOF;
}

int puts(const char* text)
{
    return fputs(text, stdout) == 0 && fputc('\n', stdout) != EOF ? 0 : EOF;
}

int fseek(FILE* stream, long offset, int whence)
{
    if ((stream->flags & Console) != 0)
    {
        errno = ESPIPE;
        return -1;
    }
    long base = 0;
    if (whence == SEEK_CUR)
    {
        base = ftell(stream);
    }
    else if (whence == SEEK_END)
    {
        if (!Settle(stream))
        {
            return -1;
        }
        base = FileSize(stream);
    }
    else if (whence != SEEK_SET)
    {
        errno = EINVAL;
        return -1;
    }
    if (base < 0 || !Settle(stream))
    {
        return -1;
    }
    const long target = base + offset;
    if (target < 0)
    {
        errno = EINVAL;
        return -1;
    }
    if (!SeekDescriptor(stream, target))
    {
        return -1;
    }
    stream->flags &= ~static_cast<unsigned>(AtEnd);
    return 0;
}

long ftell(FILE* stream)
{
    if ((stream->flags & Console) != 0)
    {
        errno = ESPIPE;
        return -1;
    }
    if (stream->reading)
    {
        return stream->offset - static_cast<long>(stream->length - stream->position) -
               (stream->pushback != NoPushback ? 1 : 0);
    }
    if ((stream->flags & Append) != 0 && stream->length > 0)
    {
        return FileSize(stream) + static_cast<long>(stream->length);
    }
    return stream->offset + static_cast<long>(stream->length);
}

void rewind(FILE* stream)
{
    fseek(stream, 0, SEEK_SET);
    stream->flags &= ~static_cast<unsigned>(Failed);
}

int feof(FILE* stream)
{
    return (stream->flags & AtEnd) != 0;
}

int ferror(FILE* stream)
{
    return (stream->flags & Failed) != 0;
}

void clearerr(FILE* stream)
{
    stream->flags &= ~static_cast<unsigned>(AtEnd | Failed);
}

int fileno(FILE* stream)
{
    return stream->descriptor;
}

int remove(const char* path)
{
    const uint64_t result = FeatherOS::User::RemoveFile(path);
    if (result == FeatherOS::User::SyscallErrorIsDirectory)
    {
        return FeatherOS::LibC::Check<int>(FeatherOS::User::RemoveDirectory(path));
    }
    return FeatherOS::LibC::Check<int>(result);
}

void perror(const char* prefix)
{
    const char* message = strerror(errno);
    if (prefix != nullptr && *prefix != '\0')
    {
        fprintf(stderr, "%s: %s\n", prefix, message);
    }
    else
    {
        fprintf(stderr, "%s\n", message);
    }
}

int vfprintf(FILE* stream, const char* format, va_list arguments)
{
    StreamSink sink = {stream};
    const int count = FeatherOS::LibC::FormatTo(sink, format, arguments);
    return FinishOutput(stream) && !sink.failed ? count : -1;
}

int fprintf(FILE* stream, const char* format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    const int count = vfprintf(stream, format, arguments);
    va_end(arguments);
    return count;
}

int vprintf(const char* format, va_list arguments)
{
    return vfprintf(stdout, format, arguments);
}

int printf(const char* format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    const int count = vfprintf(stdout, format, arguments);
    va_end(arguments);
    return count;
}

int vdprintf(int descriptor, const char* format, va_list arguments)
{
    DescriptorSink sink = {descriptor, {}};
    const int count = FeatherOS::LibC::FormatTo(sink, format, arguments);
    sink.Flush();
    return sink.failed ? -1 : count;
}

int dprintf(int descriptor, const char* format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    const int count = vdprintf(descriptor, format, arguments);
    va_end(arguments);
    return count;
}

int vsnprintf(char* buffer, size_t size, const char* format, va_list arguments)
{
    FeatherOS::LibC::BufferSink sink = {buffer, size};
    const int count = FeatherOS::LibC::FormatTo(sink, format, arguments);
    sink.Terminate();
    return count;
}

int snprintf(char* buffer, size_t size, const char* format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    const int count = vsnprintf(buffer, size, format, arguments);
    va_end(arguments);
    return count;
}

int vsprintf(char* buffer, const char* format, va_list arguments)
{
    return vsnprintf(buffer, SIZE_MAX, format, arguments);
}

int sprintf(char* buffer, const char* format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    const int count = vsnprintf(buffer, SIZE_MAX, format, arguments);
    va_end(arguments);
    return count;
}

namespace
{

struct StreamSource
{
    FeatherFile* stream;

    int Get()
    {
        return GetByte(stream);
    }

    void Unget(int character)
    {
        ungetc(character, stream);
    }
};

struct TextSource
{
    const char* text;

    int Get()
    {
        return *text == '\0' ? EOF : static_cast<unsigned char>(*text++);
    }

    void Unget(int)
    {
        --text;
    }
};

} // namespace

int vfscanf(FILE* stream, const char* format, va_list arguments)
{
    StreamSource source = {stream};
    return FeatherOS::LibC::ScanFrom(source, format, arguments);
}

int fscanf(FILE* stream, const char* format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    const int count = vfscanf(stream, format, arguments);
    va_end(arguments);
    return count;
}

int vscanf(const char* format, va_list arguments)
{
    return vfscanf(stdin, format, arguments);
}

int scanf(const char* format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    const int count = vfscanf(stdin, format, arguments);
    va_end(arguments);
    return count;
}

int vsscanf(const char* text, const char* format, va_list arguments)
{
    TextSource source = {text};
    return FeatherOS::LibC::ScanFrom(source, format, arguments);
}

int sscanf(const char* text, const char* format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    const int count = vsscanf(text, format, arguments);
    va_end(arguments);
    return count;
}

void __assert_fail(const char* expression, const char* file, unsigned int line,
                   const char* function)
{
    fprintf(stderr, "%s:%u: %s: Assertion `%s' failed.\n", file, line, function, expression);
    abort();
}
