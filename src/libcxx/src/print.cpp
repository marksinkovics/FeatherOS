// Programs see the library's headers as the system headers they are; the host tests build this
// file against their own copy, in a namespace of its own beside the host's library.
#ifdef FEATHEROS_HOST_TEST
#include "libcxx/include/print"
#else
#include <print>
#endif

// This is the standard library, so it defines what std declares.
// NOLINTBEGIN(bugprone-std-namespace-modification)
namespace FEATHEROS_STD_NAMESPACE
{

namespace
{

class FileFormatBuffer final : public __detail::FormatBuffer
{
public:
    explicit FileFormatBuffer(FILE* stream)
        : FormatBuffer(m_storage, sizeof(m_storage), &DrainTo), m_stream(stream)
    {
    }

private:
    static void DrainTo(FormatBuffer& buffer, const char* data, size_t size)
    {
        fwrite(data, 1, size, static_cast<FileFormatBuffer&>(buffer).m_stream);
    }

    char m_storage[512];
    FILE* m_stream;
};

void Print(FILE* stream, string_view pattern, format_args args, bool newline)
{
    flockfile(stream);
    FileFormatBuffer buffer(stream);
    __detail::VFormat(buffer, pattern, args);
    if (newline)
    {
        buffer.Put('\n');
    }
    buffer.Flush();
    funlockfile(stream);
}

} // namespace

void vprint_unicode(FILE* stream, string_view pattern, format_args args)
{
    Print(stream, pattern, args, false);
}

void vprint_unicode(string_view pattern, format_args args)
{
    Print(stdout, pattern, args, false);
}

void vprint_nonunicode(FILE* stream, string_view pattern, format_args args)
{
    Print(stream, pattern, args, false);
}

void vprint_nonunicode(string_view pattern, format_args args)
{
    Print(stdout, pattern, args, false);
}

namespace __detail
{
void VPrintLine(FILE* stream, string_view pattern, format_args args)
{
    Print(stream, pattern, args, true);
}
} // namespace __detail

} // namespace FEATHEROS_STD_NAMESPACE
// NOLINTEND(bugprone-std-namespace-modification)
