#ifndef FEATHEROS_USER_HTTP_MESSAGE_H
#define FEATHEROS_USER_HTTP_MESSAGE_H

#include <stddef.h>
#include <stdint.h>

// HTTP/1.1 messages (RFC 9110, RFC 9112) for clients and servers, and http URLs (RFC 3986).
namespace FeatherOS::User::Http
{

constexpr size_t MaximumHostLength = 255;
constexpr size_t MaximumPathLength = 1023;
constexpr size_t MaximumLineLength = 1023;
constexpr size_t MaximumRequestHeadLength = 8192;
constexpr size_t MaximumMethodLength = 15;

struct Url
{
    // An IPv6 literal without its brackets.
    char host[MaximumHostLength + 1];
    char port[6];
    // The path and query, starting with '/'; the fragment is left out.
    char path[MaximumPathLength + 1];
};

enum class UrlStatus
{
    Ok,
    // A scheme other than http, such as https.
    UnsupportedScheme,
    Malformed,
    TooLong,
};

UrlStatus ParseUrl(const char* text, Url* url);
// A Location header's target, absolute or relative to base.
UrlStatus ResolveUrl(const Url& base, const char* reference, Url* url);

// A GET request for url that asks the server to close the connection after its response.
// Returns its length, or 0 when it does not fit in size bytes.
size_t BuildRequest(const Url& url, char* buffer, size_t size);

enum class ResponseError
{
    None,
    BadStatusLine,
    BadHeader,
    UnsupportedCoding,
    BadChunk,
    // The connection closed before the body did.
    Truncated,
};

const char* ResponseErrorText(ResponseError error);

// Reads a response as it arrives, through 1xx responses to the final one, and takes the body out
// of its framing: a Content-Length, chunks, or everything until the connection closes.
class ResponseParser
{
public:
    ResponseParser();

    // Parses length bytes of data, moving the body bytes among them to the front of data, and
    // returns how many there are. Once the parser fails or finishes, the rest is ignored.
    size_t Feed(char* data, size_t length);
    // Called when the connection closes; false when the response was cut short.
    bool Finish();

    bool HeadersComplete() const;
    bool Done() const;
    ResponseError Error() const;
    int Status() const;
    const char* Reason() const;
    // Empty without a Location header.
    const char* Location() const;
    // -1 without a Content-Length.
    int64_t ContentLength() const;

private:
    enum class State
    {
        StatusLine,
        Headers,
        Body,
        BodyUntilClose,
        ChunkSize,
        ChunkData,
        ChunkDataEnd,
        Trailers,
        Done,
        Failed,
    };

    // Collects a line; true once one ends, with m_line holding it without its line ending.
    bool CollectLine(char character);
    void ParseStatusLine();
    void ParseHeader();
    void StartBody();
    void ParseChunkSize();
    void Fail(ResponseError error);

    State m_state;
    ResponseError m_error;
    bool m_headers_complete;
    char m_line[MaximumLineLength + 1];
    size_t m_line_length;
    int m_status;
    char m_reason[64];
    char m_location[MaximumLineLength + 1];
    int64_t m_content_length;
    bool m_chunked;
    uint64_t m_remaining;
};

enum class Method
{
    Get,
    Head,
    Other,
};

// Reads a request's head as it arrives: the request line and the headers a file server needs.
class RequestParser
{
public:
    RequestParser();

    // Parses at most length bytes and returns how many it took. It stops at the end of the head,
    // so the bytes after it belong to the request's body or to the next request.
    size_t Feed(const char* data, size_t length);

    // True once the head is complete or refused.
    bool Done() const;
    // 0 for a well-formed head, otherwise the status to refuse it with: 400, 414, 431 or 505.
    int ErrorStatus() const;
    Method RequestMethod() const;
    // Empty until the request line arrives, and cut to MaximumMethodLength.
    const char* MethodName() const;
    const char* Target() const;
    // 0 for HTTP/1.0 and 1 for HTTP/1.1 or a later 1.x.
    int MinorVersion() const;
    // The client lets the connection stay open after the response.
    bool KeepAlive() const;
    // A body follows the head.
    bool HasBody() const;

private:
    enum class State
    {
        RequestLine,
        Headers,
        Done,
    };

    void ParseRequestLine();
    void ParseHeader();
    void FinishHead();
    void Fail(int status);

    State m_state;
    int m_error_status;
    char m_line[MaximumLineLength + 1];
    size_t m_line_length;
    bool m_line_too_long;
    size_t m_head_length;
    char m_method_name[MaximumMethodLength + 1];
    Method m_method;
    char m_target[MaximumLineLength + 1];
    int m_minor_version;
    int m_host_count;
    bool m_close;
    bool m_keep_alive;
    int64_t m_content_length;
    bool m_transfer_coding;
};

// The file path a request target names: its path percent-decoded and without dot segments, so it
// starts with '/' and never climbs above it. False for a malformed target or one longer than size.
bool RequestPath(const char* target, char* path, size_t size);
// Percent-encodes every byte of path but letters, digits, "-._~" and '/', so the result is safe
// in a request target, a Location header and an HTML attribute.
bool EncodePath(const char* path, char* encoded, size_t size);
const char* StatusReason(int status);
// A media type for a file from its name's extension.
const char* ContentType(const char* name);

} // namespace FeatherOS::User::Http

#endif
