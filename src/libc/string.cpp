#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <strings.h>

namespace
{

bool Contains(const char* set, char character)
{
    for (; *set != '\0'; ++set)
    {
        if (*set == character)
        {
            return true;
        }
    }
    return false;
}

char* tokenizer_state = nullptr;

} // namespace

void* memcpy(void* destination, const void* source, size_t count)
{
    auto* to = static_cast<unsigned char*>(destination);
    const auto* from = static_cast<const unsigned char*>(source);
    for (size_t index = 0; index < count; ++index)
    {
        to[index] = from[index];
    }
    return destination;
}

void* memmove(void* destination, const void* source, size_t count)
{
    auto* to = static_cast<unsigned char*>(destination);
    const auto* from = static_cast<const unsigned char*>(source);
    // Copying backwards is safe when the destination overlaps the end of the source.
    if (reinterpret_cast<uintptr_t>(to) <= reinterpret_cast<uintptr_t>(from))
    {
        for (size_t index = 0; index < count; ++index)
        {
            to[index] = from[index];
        }
    }
    else
    {
        for (size_t index = count; index > 0; --index)
        {
            to[index - 1] = from[index - 1];
        }
    }
    return destination;
}

void* memset(void* destination, int value, size_t count)
{
    auto* to = static_cast<unsigned char*>(destination);
    for (size_t index = 0; index < count; ++index)
    {
        to[index] = static_cast<unsigned char>(value);
    }
    return destination;
}

int memcmp(const void* left, const void* right, size_t count)
{
    const auto* first = static_cast<const unsigned char*>(left);
    const auto* second = static_cast<const unsigned char*>(right);
    for (size_t index = 0; index < count; ++index)
    {
        if (first[index] != second[index])
        {
            return first[index] < second[index] ? -1 : 1;
        }
    }
    return 0;
}

void* memchr(const void* bytes, int value, size_t count)
{
    const auto* cursor = static_cast<const unsigned char*>(bytes);
    for (size_t index = 0; index < count; ++index)
    {
        if (cursor[index] == static_cast<unsigned char>(value))
        {
            return const_cast<unsigned char*>(cursor + index);
        }
    }
    return nullptr;
}

size_t strlen(const char* text)
{
    size_t length = 0;
    while (text[length] != '\0')
    {
        ++length;
    }
    return length;
}

size_t strnlen(const char* text, size_t maximum)
{
    size_t length = 0;
    while (length < maximum && text[length] != '\0')
    {
        ++length;
    }
    return length;
}

char* strcpy(char* destination, const char* source)
{
    size_t index = 0;
    for (; source[index] != '\0'; ++index)
    {
        destination[index] = source[index];
    }
    destination[index] = '\0';
    return destination;
}

// Pads with terminators up to count, and leaves the result unterminated if source is that long.
char* strncpy(char* destination, const char* source, size_t count)
{
    size_t index = 0;
    for (; index < count && source[index] != '\0'; ++index)
    {
        destination[index] = source[index];
    }
    for (; index < count; ++index)
    {
        destination[index] = '\0';
    }
    return destination;
}

char* strcat(char* destination, const char* source)
{
    strcpy(destination + strlen(destination), source);
    return destination;
}

char* strncat(char* destination, const char* source, size_t count)
{
    char* end = destination + strlen(destination);
    size_t index = 0;
    for (; index < count && source[index] != '\0'; ++index)
    {
        end[index] = source[index];
    }
    end[index] = '\0';
    return destination;
}

int strcmp(const char* left, const char* right)
{
    for (; *left != '\0' && *left == *right; ++left, ++right)
    {
    }
    return static_cast<unsigned char>(*left) - static_cast<unsigned char>(*right);
}

int strncmp(const char* left, const char* right, size_t count)
{
    for (size_t index = 0; index < count; ++index)
    {
        if (left[index] != right[index] || left[index] == '\0')
        {
            return static_cast<unsigned char>(left[index]) -
                   static_cast<unsigned char>(right[index]);
        }
    }
    return 0;
}

// The terminator counts as part of the string, so strchr(text, '\0') finds the end.
char* strchr(const char* text, int character)
{
    for (;; ++text)
    {
        if (*text == static_cast<char>(character))
        {
            return const_cast<char*>(text);
        }
        if (*text == '\0')
        {
            return nullptr;
        }
    }
}

char* strrchr(const char* text, int character)
{
    const char* found = nullptr;
    for (;; ++text)
    {
        if (*text == static_cast<char>(character))
        {
            found = text;
        }
        if (*text == '\0')
        {
            return const_cast<char*>(found);
        }
    }
}

char* strstr(const char* text, const char* pattern)
{
    const size_t length = strlen(pattern);
    for (; *text != '\0' || length == 0; ++text)
    {
        if (strncmp(text, pattern, length) == 0)
        {
            return const_cast<char*>(text);
        }
    }
    return nullptr;
}

size_t strspn(const char* text, const char* accept)
{
    size_t length = 0;
    while (text[length] != '\0' && Contains(accept, text[length]))
    {
        ++length;
    }
    return length;
}

size_t strcspn(const char* text, const char* reject)
{
    size_t length = 0;
    while (text[length] != '\0' && !Contains(reject, text[length]))
    {
        ++length;
    }
    return length;
}

char* strpbrk(const char* text, const char* accept)
{
    text += strcspn(text, accept);
    return *text == '\0' ? nullptr : const_cast<char*>(text);
}

char* strtok_r(char* text, const char* separators, char** state)
{
    char* cursor = text != nullptr ? text : *state;
    if (cursor == nullptr)
    {
        return nullptr;
    }
    cursor += strspn(cursor, separators);
    if (*cursor == '\0')
    {
        *state = nullptr;
        return nullptr;
    }
    char* end = cursor + strcspn(cursor, separators);
    if (*end == '\0')
    {
        *state = nullptr;
    }
    else
    {
        *end = '\0';
        *state = end + 1;
    }
    return cursor;
}

char* strtok(char* text, const char* separators)
{
    return strtok_r(text, separators, &tokenizer_state);
}

char* strdup(const char* text)
{
    return strndup(text, SIZE_MAX);
}

char* strndup(const char* text, size_t maximum)
{
    const size_t length = strnlen(text, maximum);
    auto* copy = static_cast<char*>(malloc(length + 1));
    if (copy != nullptr)
    {
        memcpy(copy, text, length);
        copy[length] = '\0';
    }
    return copy;
}

int strncasecmp(const char* left, const char* right, size_t count)
{
    for (; count > 0; --count, ++left, ++right)
    {
        const int difference = tolower(static_cast<unsigned char>(*left)) -
                               tolower(static_cast<unsigned char>(*right));
        if (difference != 0 || *left == '\0')
        {
            return difference;
        }
    }
    return 0;
}

int strcasecmp(const char* left, const char* right)
{
    return strncasecmp(left, right, SIZE_MAX);
}

char* strsignal(int signal)
{
    const char* names[] = {
        "Unknown signal",
        "Hangup",
        "Interrupt",
        "Quit",
        "Illegal instruction",
        "Trace/breakpoint trap",
        "Aborted",
        "Bus error",
        "Floating point exception",
        "Killed",
        "User defined signal 1",
        "Segmentation fault",
        "User defined signal 2",
        "Broken pipe",
        "Alarm clock",
        "Terminated",
        "Stack fault",
        "Child exited",
        "Continued",
    };
    const bool known = signal > 0 && signal < static_cast<int>(sizeof(names) / sizeof(names[0]));
    return const_cast<char*>(names[known ? signal : 0]);
}

char* strerror(int error)
{
    const char* message = "Unknown error";
    switch (error)
    {
    case 0:
        message = "Success";
        break;
    case EPERM:
        message = "Operation not permitted";
        break;
    case ENOENT:
        message = "No such file or directory";
        break;
    case ESRCH:
        message = "No such process";
        break;
    case EINTR:
        message = "Interrupted";
        break;
    case EIO:
        message = "Input/output error";
        break;
    case E2BIG:
        message = "Argument list too long";
        break;
    case ENOEXEC:
        message = "Exec format error";
        break;
    case EBADF:
        message = "Bad file descriptor";
        break;
    case ECHILD:
        message = "No child processes";
        break;
    case EAGAIN:
        message = "Resource temporarily unavailable";
        break;
    case ENOMEM:
        message = "Out of memory";
        break;
    case EFAULT:
        message = "Bad address";
        break;
    case EBUSY:
        message = "Device or resource busy";
        break;
    case EEXIST:
        message = "File exists";
        break;
    case ENOTDIR:
        message = "Not a directory";
        break;
    case EISDIR:
        message = "Is a directory";
        break;
    case EINVAL:
        message = "Invalid argument";
        break;
    case EMFILE:
        message = "Too many open files";
        break;
    case ENOTTY:
        message = "Not a terminal";
        break;
    case ENOSPC:
        message = "No space left on device";
        break;
    case ESPIPE:
        message = "Illegal seek";
        break;
    case EROFS:
        message = "Read-only file system";
        break;
    case EPIPE:
        message = "Broken pipe";
        break;
    case EDOM:
        message = "Argument out of domain";
        break;
    case ERANGE:
        message = "Result out of range";
        break;
    case ENAMETOOLONG:
        message = "File name too long";
        break;
    case ETIMEDOUT:
        message = "Connection timed out";
        break;
    case EDEADLK:
        message = "Resource deadlock avoided";
        break;
    case ENOSYS:
        message = "Function not implemented";
        break;
    case ENOTEMPTY:
        message = "Directory not empty";
        break;
    case EOVERFLOW:
        message = "Value too large for defined data type";
        break;
    case EILSEQ:
        message = "Illegal byte sequence";
        break;
    default:
        break;
    }
    // The standard's return type is not const, but callers must not modify the message.
    return const_cast<char*>(message);
}
