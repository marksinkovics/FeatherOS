#include "http_message.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

namespace FeatherOS::User::Http
{
namespace
{

constexpr char DefaultPort[] = "80";

bool IsDigit(char character)
{
    return character >= '0' && character <= '9';
}

int HexValue(char character)
{
    if (IsDigit(character))
    {
        return character - '0';
    }
    if (character >= 'a' && character <= 'f')
    {
        return character - 'a' + 10;
    }
    if (character >= 'A' && character <= 'F')
    {
        return character - 'A' + 10;
    }
    return -1;
}

bool IsAlphanumeric(char character)
{
    return IsDigit(character) || (character >= 'a' && character <= 'z') ||
           (character >= 'A' && character <= 'Z');
}

// RFC 9110 section 5.6.2.
bool IsTokenCharacter(char character)
{
    return IsAlphanumeric(character) ||
           (character != '\0' && strchr("!#$%&'*+-.^_`|~", character) != nullptr);
}

bool IsSpace(char character)
{
    return character == ' ' || character == '\t';
}

// Whether a comma-separated header value such as Connection's lists token.
bool ListHasToken(const char* list, const char* token)
{
    const size_t token_length = strlen(token);
    while (*list != '\0')
    {
        while (IsSpace(*list) || *list == ',')
        {
            ++list;
        }
        size_t length = strcspn(list, ",");
        const char* next = list + length;
        while (length > 0 && IsSpace(list[length - 1]))
        {
            --length;
        }
        if (length == token_length && strncasecmp(list, token, length) == 0)
        {
            return true;
        }
        list = next;
    }
    return false;
}

// Up to 18 digits, so the value fits; -1 otherwise.
int64_t ParseContentLength(const char* value)
{
    const size_t length = strlen(value);
    if (length == 0 || length > 18)
    {
        return -1;
    }
    int64_t result = 0;
    for (size_t index = 0; index < length; ++index)
    {
        if (!IsDigit(value[index]))
        {
            return -1;
        }
        result = result * 10 + (value[index] - '0');
    }
    return result;
}

// Splits a header line at its colon, trimming the value; nullptr for a malformed one.
char* SplitHeader(char* line)
{
    char* colon = strchr(line, ':');
    if (colon == nullptr || colon == line || IsSpace(colon[-1]))
    {
        return nullptr;
    }
    *colon = '\0';
    char* value = colon + 1;
    while (IsSpace(*value))
    {
        ++value;
    }
    size_t value_length = strlen(value);
    while (value_length > 0 && IsSpace(value[value_length - 1]))
    {
        value[--value_length] = '\0';
    }
    return value;
}

bool CopyText(char* destination, size_t capacity, const char* source, size_t length)
{
    if (length >= capacity)
    {
        return false;
    }
    memcpy(destination, source, length);
    destination[length] = '\0';
    return true;
}

// RFC 3986 section 5.2.4, on the first query bytes of path, which start with '/'. The rest, such
// as a query, is left alone.
void RemoveDotSegments(char* path, size_t query)
{
    size_t input = 0;
    size_t output = 0;
    auto remove_last_segment = [&]()
    {
        while (output > 0 && path[output - 1] != '/')
        {
            --output;
        }
        if (output > 0)
        {
            --output;
        }
    };
    while (input < query)
    {
        const char* rest = path + input;
        const size_t left = query - input;
        if (left >= 3 && strncmp(rest, "/./", 3) == 0)
        {
            input += 2;
        }
        else if (left == 2 && strncmp(rest, "/.", 2) == 0)
        {
            input += 1;
            path[input] = '/';
        }
        else if (left >= 4 && strncmp(rest, "/../", 4) == 0)
        {
            input += 3;
            remove_last_segment();
        }
        else if (left == 3 && strncmp(rest, "/..", 3) == 0)
        {
            input += 2;
            path[input] = '/';
            remove_last_segment();
        }
        else
        {
            path[output++] = path[input++];
            while (input < query && path[input] != '/')
            {
                path[output++] = path[input++];
            }
        }
    }
    memmove(path + output, path + query, strlen(path + query) + 1);
}

// The path and query from text up to its fragment, as a request target.
UrlStatus SetPath(Url* url, const char* text)
{
    const size_t length = strcspn(text, "#");
    for (size_t index = 0; index < length; ++index)
    {
        const auto byte = static_cast<unsigned char>(text[index]);
        if (byte <= ' ' || byte == 0x7F)
        {
            return UrlStatus::Malformed;
        }
    }
    const size_t prefix = text[0] == '/' ? 0 : 1;
    if (prefix + length > MaximumPathLength)
    {
        return UrlStatus::TooLong;
    }
    url->path[0] = '/';
    memcpy(url->path + prefix, text, length);
    url->path[prefix + length] = '\0';
    RemoveDotSegments(url->path, strcspn(url->path, "?"));
    return UrlStatus::Ok;
}

UrlStatus ParseAuthority(const char* text, size_t length, Url* url)
{
    if (memchr(text, '@', length) != nullptr)
    {
        return UrlStatus::Malformed;
    }
    size_t host_start = 0;
    size_t host_end;
    size_t after_host;
    if (length > 0 && text[0] == '[')
    {
        const auto* close = static_cast<const char*>(memchr(text, ']', length));
        if (close == nullptr)
        {
            return UrlStatus::Malformed;
        }
        host_start = 1;
        host_end = static_cast<size_t>(close - text);
        after_host = host_end + 1;
    }
    else
    {
        const auto* colon = static_cast<const char*>(memchr(text, ':', length));
        host_end = colon == nullptr ? length : static_cast<size_t>(colon - text);
        after_host = host_end;
    }
    if (host_end == host_start)
    {
        return UrlStatus::Malformed;
    }
    if (!CopyText(url->host, sizeof(url->host), text + host_start, host_end - host_start))
    {
        return UrlStatus::TooLong;
    }
    if (after_host == length || after_host + 1 == length)
    {
        if (after_host < length && text[after_host] != ':')
        {
            return UrlStatus::Malformed;
        }
        memcpy(url->port, DefaultPort, sizeof(DefaultPort));
        return UrlStatus::Ok;
    }
    if (text[after_host] != ':')
    {
        return UrlStatus::Malformed;
    }
    const char* port = text + after_host + 1;
    const size_t port_length = length - after_host - 1;
    unsigned long value = 0;
    for (size_t index = 0; index < port_length; ++index)
    {
        if (!IsDigit(port[index]) || index >= 5)
        {
            return UrlStatus::Malformed;
        }
        value = value * 10 + static_cast<unsigned long>(port[index] - '0');
    }
    if (value == 0 || value > 0xFFFF)
    {
        return UrlStatus::Malformed;
    }
    snprintf(url->port, sizeof(url->port), "%lu", value);
    return UrlStatus::Ok;
}

} // namespace

UrlStatus ParseUrl(const char* text, Url* url)
{
    const char* separator = strstr(text, "://");
    if (separator == nullptr || separator == text)
    {
        return UrlStatus::Malformed;
    }
    const auto scheme_length = static_cast<size_t>(separator - text);
    if (scheme_length != 4 || strncasecmp(text, "http", 4) != 0)
    {
        for (const char* character = text; character < separator; ++character)
        {
            if (strchr("/?#", *character) != nullptr)
            {
                return UrlStatus::Malformed;
            }
        }
        return UrlStatus::UnsupportedScheme;
    }
    const char* authority = separator + 3;
    const size_t authority_length = strcspn(authority, "/?#");
    const UrlStatus status = ParseAuthority(authority, authority_length, url);
    if (status != UrlStatus::Ok)
    {
        return status;
    }
    return SetPath(url, authority + authority_length);
}

UrlStatus ResolveUrl(const Url& base, const char* reference, Url* url)
{
    const size_t scheme_length = strcspn(reference, ":/?#");
    if (scheme_length > 0 && reference[scheme_length] == ':')
    {
        return ParseUrl(reference, url);
    }
    if (strncmp(reference, "//", 2) == 0)
    {
        char absolute[sizeof("http:") + MaximumHostLength + MaximumPathLength + 8];
        if (snprintf(absolute, sizeof(absolute), "http:%s", reference) >=
            static_cast<int>(sizeof(absolute)))
        {
            return UrlStatus::TooLong;
        }
        return ParseUrl(absolute, url);
    }
    *url = base;
    if (reference[0] == '/')
    {
        return SetPath(url, reference);
    }
    if (reference[0] == '#' || reference[0] == '\0')
    {
        return UrlStatus::Ok;
    }
    // A query replaces the base's; anything else replaces the base path's last segment.
    size_t keep = strcspn(base.path, "?");
    if (reference[0] != '?')
    {
        while (keep > 0 && base.path[keep - 1] != '/')
        {
            --keep;
        }
    }
    char merged[MaximumPathLength + 1];
    if (snprintf(merged, sizeof(merged), "%.*s%s", static_cast<int>(keep), base.path, reference) >=
        static_cast<int>(sizeof(merged)))
    {
        return UrlStatus::TooLong;
    }
    return SetPath(url, merged);
}

size_t BuildRequest(const Url& url, char* buffer, size_t size)
{
    const bool literal_ipv6 = strchr(url.host, ':') != nullptr;
    const bool default_port = strcmp(url.port, DefaultPort) == 0;
    const int length =
        snprintf(buffer, size,
                 "GET %s HTTP/1.1\r\n"
                 "Host: %s%s%s%s%s\r\n"
                 "User-Agent: FeatherOS-fetch/1\r\n"
                 "Accept: */*\r\n"
                 "Connection: close\r\n"
                 "\r\n",
                 url.path, literal_ipv6 ? "[" : "", url.host, literal_ipv6 ? "]" : "",
                 default_port ? "" : ":", default_port ? "" : url.port);
    if (length < 0 || static_cast<size_t>(length) >= size)
    {
        return 0;
    }
    return static_cast<size_t>(length);
}

const char* ResponseErrorText(ResponseError error)
{
    switch (error)
    {
    case ResponseError::None:
        return "no error";
    case ResponseError::BadStatusLine:
        return "bad status line";
    case ResponseError::BadHeader:
        return "bad header";
    case ResponseError::UnsupportedCoding:
        return "unsupported transfer coding";
    case ResponseError::BadChunk:
        return "bad chunk";
    case ResponseError::Truncated:
        return "connection closed before the end of the response";
    }
    return "unknown error";
}

ResponseParser::ResponseParser()
    : m_state(State::StatusLine), m_error(ResponseError::None), m_headers_complete(false), m_line{},
      m_line_length(0), m_status(0), m_reason{}, m_location{}, m_content_length(-1),
      m_chunked(false), m_remaining(0)
{
}

size_t ResponseParser::Feed(char* data, size_t length)
{
    size_t body = 0;
    size_t index = 0;
    while (index < length)
    {
        switch (m_state)
        {
        case State::StatusLine:
        case State::Headers:
        case State::ChunkSize:
        case State::ChunkDataEnd:
        case State::Trailers:
            if (!CollectLine(data[index++]))
            {
                break;
            }
            if (m_state == State::StatusLine)
            {
                ParseStatusLine();
            }
            else if (m_state == State::Headers)
            {
                ParseHeader();
            }
            else if (m_state == State::ChunkSize)
            {
                ParseChunkSize();
            }
            else if (m_state == State::ChunkDataEnd)
            {
                if (m_line_length != 0)
                {
                    Fail(ResponseError::BadChunk);
                }
                else
                {
                    m_state = State::ChunkSize;
                }
            }
            else if (m_line_length == 0)
            {
                m_state = State::Done;
            }
            m_line_length = 0;
            break;
        case State::Body:
        case State::ChunkData:
        case State::BodyUntilClose:
        {
            size_t count = length - index;
            if (m_state != State::BodyUntilClose && count > m_remaining)
            {
                count = static_cast<size_t>(m_remaining);
            }
            memmove(data + body, data + index, count);
            body += count;
            index += count;
            if (m_state != State::BodyUntilClose)
            {
                m_remaining -= count;
                if (m_remaining == 0)
                {
                    m_state = m_state == State::Body ? State::Done : State::ChunkDataEnd;
                }
            }
            break;
        }
        case State::Done:
        case State::Failed:
            return body;
        }
    }
    return body;
}

bool ResponseParser::Finish()
{
    if (m_state == State::BodyUntilClose)
    {
        m_state = State::Done;
    }
    if (m_state != State::Done && m_state != State::Failed)
    {
        Fail(ResponseError::Truncated);
    }
    return m_state == State::Done;
}

bool ResponseParser::HeadersComplete() const
{
    return m_headers_complete;
}

bool ResponseParser::Done() const
{
    return m_state == State::Done;
}

ResponseError ResponseParser::Error() const
{
    return m_error;
}

int ResponseParser::Status() const
{
    return m_status;
}

const char* ResponseParser::Reason() const
{
    return m_reason;
}

const char* ResponseParser::Location() const
{
    return m_location;
}

int64_t ResponseParser::ContentLength() const
{
    return m_content_length;
}

bool ResponseParser::CollectLine(char character)
{
    if (character == '\n')
    {
        if (m_line_length > 0 && m_line[m_line_length - 1] == '\r')
        {
            --m_line_length;
        }
        m_line[m_line_length] = '\0';
        return true;
    }
    // A longer line keeps its start, which holds everything the parser reads.
    if (m_line_length < MaximumLineLength)
    {
        m_line[m_line_length++] = character;
    }
    return false;
}

void ResponseParser::ParseStatusLine()
{
    // HTTP/1.x SSS[ reason]
    if (m_line_length < 12 || strncmp(m_line, "HTTP/1.", 7) != 0 || !IsDigit(m_line[7]) ||
        m_line[8] != ' ' || !IsDigit(m_line[9]) || !IsDigit(m_line[10]) || !IsDigit(m_line[11]) ||
        (m_line_length > 12 && m_line[12] != ' '))
    {
        Fail(ResponseError::BadStatusLine);
        return;
    }
    m_status = (m_line[9] - '0') * 100 + (m_line[10] - '0') * 10 + (m_line[11] - '0');
    if (m_status < 100 || m_status > 599)
    {
        Fail(ResponseError::BadStatusLine);
        return;
    }
    const char* reason = m_line_length > 12 ? m_line + 13 : "";
    const size_t reason_length = strlen(reason);
    CopyText(m_reason, sizeof(m_reason), reason,
             reason_length < sizeof(m_reason) ? reason_length : sizeof(m_reason) - 1);
    m_state = State::Headers;
}

void ResponseParser::ParseHeader()
{
    if (m_line_length == 0)
    {
        if (m_status < 200)
        {
            m_status = 0;
            m_reason[0] = '\0';
            m_location[0] = '\0';
            m_content_length = -1;
            m_chunked = false;
            m_state = State::StatusLine;
            return;
        }
        StartBody();
        return;
    }
    // An obsolete folded continuation, of a header the parser has no use for.
    if (IsSpace(m_line[0]))
    {
        return;
    }
    const char* value = SplitHeader(m_line);
    if (value == nullptr)
    {
        Fail(ResponseError::BadHeader);
        return;
    }

    if (strcasecmp(m_line, "Content-Length") == 0)
    {
        const int64_t length = ParseContentLength(value);
        if (length < 0 || (m_content_length >= 0 && m_content_length != length))
        {
            Fail(ResponseError::BadHeader);
            return;
        }
        m_content_length = length;
    }
    else if (strcasecmp(m_line, "Transfer-Encoding") == 0)
    {
        // Only chunked is decoded, and fetch asks for no other coding.
        if (strcasecmp(value, "chunked") != 0)
        {
            Fail(ResponseError::UnsupportedCoding);
            return;
        }
        m_chunked = true;
    }
    else if (strcasecmp(m_line, "Location") == 0)
    {
        CopyText(m_location, sizeof(m_location), value, strlen(value));
    }
}

void ResponseParser::StartBody()
{
    m_headers_complete = true;
    if (m_status == 204 || m_status == 304)
    {
        m_state = State::Done;
    }
    else if (m_chunked)
    {
        // RFC 9112 section 6.3: chunked framing wins over a Content-Length.
        m_state = State::ChunkSize;
    }
    else if (m_content_length == 0)
    {
        m_state = State::Done;
    }
    else if (m_content_length > 0)
    {
        m_remaining = static_cast<uint64_t>(m_content_length);
        m_state = State::Body;
    }
    else
    {
        m_state = State::BodyUntilClose;
    }
}

void ResponseParser::ParseChunkSize()
{
    uint64_t size = 0;
    size_t index = 0;
    for (; index < m_line_length && HexValue(m_line[index]) >= 0; ++index)
    {
        if (index == 15)
        {
            Fail(ResponseError::BadChunk);
            return;
        }
        size = size * 16 + static_cast<uint64_t>(HexValue(m_line[index]));
    }
    while (index < m_line_length && IsSpace(m_line[index]))
    {
        ++index;
    }
    // Chunk extensions follow a ';' and are ignored.
    if (index == 0 || (index < m_line_length && m_line[index] != ';'))
    {
        Fail(ResponseError::BadChunk);
        return;
    }
    if (size == 0)
    {
        m_state = State::Trailers;
        return;
    }
    m_remaining = size;
    m_state = State::ChunkData;
}

void ResponseParser::Fail(ResponseError error)
{
    m_error = error;
    m_state = State::Failed;
}

RequestParser::RequestParser()
    : m_state(State::RequestLine), m_error_status(0), m_line{}, m_line_length(0),
      m_line_too_long(false), m_head_length(0), m_method_name{}, m_method(Method::Other),
      m_target{}, m_minor_version(1), m_host_count(0), m_close(false), m_keep_alive(false),
      m_content_length(-1), m_transfer_coding(false)
{
}

size_t RequestParser::Feed(const char* data, size_t length)
{
    size_t index = 0;
    while (index < length && m_state != State::Done)
    {
        const char character = data[index++];
        if (++m_head_length > MaximumRequestHeadLength)
        {
            Fail(431);
            break;
        }
        if (character != '\n')
        {
            if (m_line_length < MaximumLineLength)
            {
                m_line[m_line_length++] = character;
            }
            else
            {
                m_line_too_long = true;
            }
            continue;
        }
        if (m_line_length > 0 && m_line[m_line_length - 1] == '\r')
        {
            --m_line_length;
        }
        m_line[m_line_length] = '\0';
        if (m_line_too_long)
        {
            Fail(m_state == State::RequestLine ? 414 : 431);
        }
        else if (m_state == State::RequestLine)
        {
            ParseRequestLine();
        }
        else
        {
            ParseHeader();
        }
        m_line_length = 0;
    }
    return index;
}

bool RequestParser::Done() const
{
    return m_state == State::Done;
}

int RequestParser::ErrorStatus() const
{
    return m_error_status;
}

Method RequestParser::RequestMethod() const
{
    return m_method;
}

const char* RequestParser::MethodName() const
{
    return m_method_name;
}

const char* RequestParser::Target() const
{
    return m_target;
}

int RequestParser::MinorVersion() const
{
    return m_minor_version;
}

bool RequestParser::KeepAlive() const
{
    return !m_close && (m_minor_version >= 1 || m_keep_alive);
}

bool RequestParser::HasBody() const
{
    return m_transfer_coding || m_content_length > 0;
}

void RequestParser::ParseRequestLine()
{
    // RFC 9112 section 2.2: empty lines before a request line are skipped.
    if (m_line_length == 0)
    {
        return;
    }
    // method SP target SP HTTP/d.d
    char* first = strchr(m_line, ' ');
    char* second = first == nullptr ? nullptr : strchr(first + 1, ' ');
    if (second == nullptr || first == m_line || second == first + 1)
    {
        Fail(400);
        return;
    }
    *first = '\0';
    *second = '\0';
    const char* target = first + 1;
    const char* version = second + 1;
    for (const char* character = m_line; *character != '\0'; ++character)
    {
        if (!IsTokenCharacter(*character))
        {
            Fail(400);
            return;
        }
    }
    for (const char* character = target; *character != '\0'; ++character)
    {
        const auto byte = static_cast<unsigned char>(*character);
        if (byte < ' ' || byte == 0x7F)
        {
            Fail(400);
            return;
        }
    }
    const size_t method_length = strlen(m_line);
    CopyText(m_method_name, sizeof(m_method_name), m_line,
             method_length < sizeof(m_method_name) ? method_length : sizeof(m_method_name) - 1);
    CopyText(m_target, sizeof(m_target), target, strlen(target));
    if (strlen(version) != 8 || strncmp(version, "HTTP/", 5) != 0 || !IsDigit(version[5]) ||
        version[6] != '.' || !IsDigit(version[7]))
    {
        Fail(400);
        return;
    }
    if (version[5] != '1')
    {
        Fail(505);
        return;
    }
    m_minor_version = version[7] == '0' ? 0 : 1;
    // Methods are case-sensitive.
    if (strcmp(m_line, "GET") == 0)
    {
        m_method = Method::Get;
    }
    else if (strcmp(m_line, "HEAD") == 0)
    {
        m_method = Method::Head;
    }
    m_state = State::Headers;
}

void RequestParser::ParseHeader()
{
    if (m_line_length == 0)
    {
        FinishHead();
        return;
    }
    // RFC 9112 section 5.2 lets a server refuse obsolete line folding.
    if (IsSpace(m_line[0]))
    {
        Fail(400);
        return;
    }
    const char* value = SplitHeader(m_line);
    if (value == nullptr)
    {
        Fail(400);
        return;
    }
    if (strcasecmp(m_line, "Host") == 0)
    {
        ++m_host_count;
    }
    else if (strcasecmp(m_line, "Connection") == 0)
    {
        m_close = m_close || ListHasToken(value, "close");
        m_keep_alive = m_keep_alive || ListHasToken(value, "keep-alive");
    }
    else if (strcasecmp(m_line, "Content-Length") == 0)
    {
        const int64_t length = ParseContentLength(value);
        if (length < 0 || (m_content_length >= 0 && m_content_length != length))
        {
            Fail(400);
            return;
        }
        m_content_length = length;
    }
    else if (strcasecmp(m_line, "Transfer-Encoding") == 0)
    {
        m_transfer_coding = true;
    }
}

void RequestParser::FinishHead()
{
    // RFC 9112 section 3.2: an HTTP/1.1 request has exactly one Host, and no request has two.
    if (m_host_count > 1 || (m_minor_version >= 1 && m_host_count == 0))
    {
        Fail(400);
        return;
    }
    m_state = State::Done;
}

void RequestParser::Fail(int status)
{
    m_error_status = status;
    m_state = State::Done;
}

bool RequestPath(const char* target, char* path, size_t size)
{
    Url url;
    if (target[0] != '/')
    {
        // The absolute form, which clients send to proxies and every server accepts.
        if (ParseUrl(target, &url) != UrlStatus::Ok)
        {
            return false;
        }
        target = url.path;
    }
    const size_t length = strcspn(target, "?#");
    size_t output = 0;
    for (size_t index = 0; index < length; ++index)
    {
        char character = target[index];
        if (character == '%')
        {
            const int high = index + 2 < length ? HexValue(target[index + 1]) : -1;
            const int low = high < 0 ? -1 : HexValue(target[index + 2]);
            if (low < 0 || (high == 0 && low == 0))
            {
                return false;
            }
            character = static_cast<char>(high * 16 + low);
            index += 2;
        }
        if (output + 1 >= size)
        {
            return false;
        }
        path[output++] = character;
    }
    path[output] = '\0';
    // Decoded, a '?' is part of the path and must not shield what follows it from dot removal.
    RemoveDotSegments(path, output);
    return true;
}

bool EncodePath(const char* path, char* encoded, size_t size)
{
    constexpr char Digits[] = "0123456789ABCDEF";
    size_t output = 0;
    for (; *path != '\0'; ++path)
    {
        const auto byte = static_cast<unsigned char>(*path);
        const bool plain = IsAlphanumeric(*path) || strchr("-._~/", *path) != nullptr;
        if (output + (plain ? 1 : 3) >= size)
        {
            return false;
        }
        if (plain)
        {
            encoded[output++] = *path;
        }
        else
        {
            encoded[output++] = '%';
            encoded[output++] = Digits[byte >> 4];
            encoded[output++] = Digits[byte & 0xF];
        }
    }
    encoded[output] = '\0';
    return true;
}

const char* StatusReason(int status)
{
    switch (status)
    {
    case 200:
        return "OK";
    case 301:
        return "Moved Permanently";
    case 400:
        return "Bad Request";
    case 403:
        return "Forbidden";
    case 404:
        return "Not Found";
    case 414:
        return "URI Too Long";
    case 431:
        return "Request Header Fields Too Large";
    case 500:
        return "Internal Server Error";
    case 501:
        return "Not Implemented";
    case 505:
        return "HTTP Version Not Supported";
    default:
        return "Unknown";
    }
}

const char* ContentType(const char* name)
{
    struct Type
    {
        const char* extension;
        const char* type;
    };
    static constexpr Type Types[] = {
        {"html", "text/html; charset=utf-8"},
        {"htm", "text/html; charset=utf-8"},
        {"txt", "text/plain; charset=utf-8"},
        {"conf", "text/plain; charset=utf-8"},
        {"md", "text/markdown; charset=utf-8"},
        {"css", "text/css"},
        {"js", "text/javascript"},
        {"json", "application/json"},
        {"png", "image/png"},
        {"jpg", "image/jpeg"},
        {"jpeg", "image/jpeg"},
        {"gif", "image/gif"},
        {"svg", "image/svg+xml"},
        {"ico", "image/vnd.microsoft.icon"},
        {"pdf", "application/pdf"},
    };
    const char* dot = strrchr(name, '.');
    const char* slash = strrchr(name, '/');
    if (dot != nullptr && (slash == nullptr || dot > slash))
    {
        for (const Type& type : Types)
        {
            if (strcasecmp(dot + 1, type.extension) == 0)
            {
                return type.type;
            }
        }
    }
    return "application/octet-stream";
}

} // namespace FeatherOS::User::Http
