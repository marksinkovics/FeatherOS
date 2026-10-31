#include <catch2/catch_test_macros.hpp>

#include "user/http_message.h"

#include <string>
#include <string_view>

namespace Http = FeatherOS::User::Http;

namespace
{

Http::Url Parse(const char* text)
{
    Http::Url url{};
    REQUIRE(Http::ParseUrl(text, &url) == Http::UrlStatus::Ok);
    return url;
}

std::string Resolve(const char* base, const char* reference)
{
    Http::Url resolved{};
    REQUIRE(Http::ResolveUrl(Parse(base), reference, &resolved) == Http::UrlStatus::Ok);
    return std::string(resolved.host) + " " + resolved.port + " " + resolved.path;
}

struct Response
{
    Http::ResponseParser parser;
    std::string body;
};

// Feeds text in pieces of step bytes, as a connection might deliver it.
void Feed(Response& response, std::string_view text, size_t step)
{
    for (size_t offset = 0; offset < text.size(); offset += step)
    {
        std::string piece(text.substr(offset, step));
        const size_t body = response.parser.Feed(piece.data(), piece.size());
        response.body.append(piece.data(), body);
    }
}

} // namespace

TEST_CASE("http URLs give a host, a port and a request target")
{
    Http::Url url = Parse("http://example.com");
    REQUIRE(std::string(url.host) == "example.com");
    REQUIRE(std::string(url.port) == "80");
    REQUIRE(std::string(url.path) == "/");

    url = Parse("HTTP://10.0.2.2:8000/files/a.txt?x=1#top");
    REQUIRE(std::string(url.host) == "10.0.2.2");
    REQUIRE(std::string(url.port) == "8000");
    REQUIRE(std::string(url.path) == "/files/a.txt?x=1");

    url = Parse("http://[fec0::2]:8080?q");
    REQUIRE(std::string(url.host) == "fec0::2");
    REQUIRE(std::string(url.port) == "8080");
    REQUIRE(std::string(url.path) == "/?q");

    url = Parse("http://host:/a/./b/../c");
    REQUIRE(std::string(url.port) == "80");
    REQUIRE(std::string(url.path) == "/a/c");
}

TEST_CASE("unsupported and malformed URLs are refused")
{
    Http::Url url{};
    REQUIRE(Http::ParseUrl("https://example.com/", &url) == Http::UrlStatus::UnsupportedScheme);
    REQUIRE(Http::ParseUrl("example.com/", &url) == Http::UrlStatus::Malformed);
    REQUIRE(Http::ParseUrl("http:///path", &url) == Http::UrlStatus::Malformed);
    REQUIRE(Http::ParseUrl("http://host:0/", &url) == Http::UrlStatus::Malformed);
    REQUIRE(Http::ParseUrl("http://host:65536/", &url) == Http::UrlStatus::Malformed);
    REQUIRE(Http::ParseUrl("http://host:80x/", &url) == Http::UrlStatus::Malformed);
    REQUIRE(Http::ParseUrl("http://[::1/", &url) == Http::UrlStatus::Malformed);
    REQUIRE(Http::ParseUrl("http://[::1]x/", &url) == Http::UrlStatus::Malformed);
    REQUIRE(Http::ParseUrl("http://user@host/", &url) == Http::UrlStatus::Malformed);
    REQUIRE(Http::ParseUrl("http://host/a b", &url) == Http::UrlStatus::Malformed);
    const std::string long_host = "http://" + std::string(300, 'a') + "/";
    REQUIRE(Http::ParseUrl(long_host.c_str(), &url) == Http::UrlStatus::TooLong);
    const std::string long_path = "http://host/" + std::string(1100, 'a');
    REQUIRE(Http::ParseUrl(long_path.c_str(), &url) == Http::UrlStatus::TooLong);
}

TEST_CASE("Location targets resolve against the request's URL")
{
    const char* base = "http://host:8000/docs/guide/page?x=1";
    REQUIRE(Resolve(base, "http://other/") == "other 80 /");
    REQUIRE(Resolve(base, "//other:81/a") == "other 81 /a");
    REQUIRE(Resolve(base, "/top") == "host 8000 /top");
    REQUIRE(Resolve(base, "next") == "host 8000 /docs/guide/next");
    REQUIRE(Resolve(base, "../up?y=2#frag") == "host 8000 /docs/up?y=2");
    REQUIRE(Resolve(base, "../../../../root") == "host 8000 /root");
    REQUIRE(Resolve(base, "?y=2") == "host 8000 /docs/guide/page?y=2");
    REQUIRE(Resolve(base, "./") == "host 8000 /docs/guide/");
    REQUIRE(Resolve(base, "..") == "host 8000 /docs/");

    Http::Url url{};
    REQUIRE(Http::ResolveUrl(Parse(base), "https://secure/", &url) ==
            Http::UrlStatus::UnsupportedScheme);
}

TEST_CASE("requests name the host, with the port when it is not 80, and ask to close")
{
    char buffer[512];
    size_t length = Http::BuildRequest(Parse("http://example.com/a?b"), buffer, sizeof(buffer));
    REQUIRE(std::string(buffer, length) == "GET /a?b HTTP/1.1\r\n"
                                           "Host: example.com\r\n"
                                           "User-Agent: FeatherOS-fetch/1\r\n"
                                           "Accept: */*\r\n"
                                           "Connection: close\r\n"
                                           "\r\n");
    length = Http::BuildRequest(Parse("http://[fec0::2]:8080/"), buffer, sizeof(buffer));
    REQUIRE(std::string(buffer, length).find("\r\nHost: [fec0::2]:8080\r\n") != std::string::npos);
    REQUIRE(Http::BuildRequest(Parse("http://example.com/"), buffer, 40) == 0);
}

TEST_CASE("a Content-Length body ends after its length, whatever follows")
{
    for (const size_t step : {1, 7, 1000})
    {
        Response response;
        Feed(response,
             "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\ncontent-length:  5 \r\n\r\nhelloEXTRA",
             step);
        REQUIRE(response.parser.HeadersComplete());
        REQUIRE(response.parser.Done());
        REQUIRE(response.parser.Status() == 200);
        REQUIRE(std::string(response.parser.Reason()) == "OK");
        REQUIRE(response.parser.ContentLength() == 5);
        REQUIRE(response.body == "hello");
        REQUIRE(response.parser.Finish());
    }
}

TEST_CASE("chunked bodies are decoded, with extensions and trailers skipped")
{
    for (const size_t step : {1, 3, 1000})
    {
        Response response;
        Feed(response,
             "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nContent-Length: 99\r\n\r\n"
             "5\r\nhello\r\n"
             "7;name=value\r\n, world\r\n"
             "A \r\n0123456789\r\n"
             "0\r\nExpires: never\r\n\r\n",
             step);
        REQUIRE(response.parser.Done());
        REQUIRE(response.body == "hello, world0123456789");
    }
}

TEST_CASE("a body without a length runs until the connection closes")
{
    Response response;
    Feed(response, "HTTP/1.0 200 OK\nServer: old\n\nfirst ", 4);
    Feed(response, "second", 4);
    REQUIRE(response.parser.HeadersComplete());
    REQUIRE_FALSE(response.parser.Done());
    REQUIRE(response.parser.Finish());
    REQUIRE(response.parser.Done());
    REQUIRE(response.body == "first second");
}

TEST_CASE("interim responses are skipped and some statuses have no body")
{
    Response response;
    Feed(response,
         "HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 103 Early Hints\r\nLink: </a>\r\n\r\n"
         "HTTP/1.1 204 No Content\r\nContent-Length: 10\r\n\r\n",
         1000);
    REQUIRE(response.parser.Done());
    REQUIRE(response.parser.Status() == 204);
    REQUIRE(response.parser.ContentLength() == 10);
    REQUIRE(response.body.empty());
}

TEST_CASE("redirects keep their Location")
{
    Response response;
    Feed(response,
         "HTTP/1.1 302 Found\r\nLocation: /elsewhere\r\nContent-Length: 0\r\n\r\n", 1000);
    REQUIRE(response.parser.Done());
    REQUIRE(response.parser.Status() == 302);
    REQUIRE(std::string(response.parser.Location()) == "/elsewhere");
}

TEST_CASE("long header lines are cut, not refused")
{
    Response response;
    Feed(response,
         "HTTP/1.1 200 OK\r\nSet-Cookie: " + std::string(5000, 'c') +
             "\r\nContent-Length: 2\r\n\r\nok",
         100);
    REQUIRE(response.parser.Done());
    REQUIRE(response.body == "ok");
}

TEST_CASE("malformed responses fail")
{
    const std::pair<const char*, Http::ResponseError> cases[] = {
        {"SPDY/3 200 OK\r\n", Http::ResponseError::BadStatusLine},
        {"HTTP/1.1 20 OK\r\n", Http::ResponseError::BadStatusLine},
        {"HTTP/1.1 200OK\r\n", Http::ResponseError::BadStatusLine},
        {"HTTP/1.1 700 Odd\r\n", Http::ResponseError::BadStatusLine},
        {"HTTP/1.1 200 OK\r\nNo colon here\r\n", Http::ResponseError::BadHeader},
        {"HTTP/1.1 200 OK\r\nContent-Length : 1\r\n", Http::ResponseError::BadHeader},
        {"HTTP/1.1 200 OK\r\nContent-Length: 1x\r\n", Http::ResponseError::BadHeader},
        {"HTTP/1.1 200 OK\r\nContent-Length: 1\r\nContent-Length: 2\r\n",
         Http::ResponseError::BadHeader},
        {"HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip, chunked\r\n",
         Http::ResponseError::UnsupportedCoding},
        {"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nzz\r\n",
         Http::ResponseError::BadChunk},
        {"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n2\r\nabX\r\n",
         Http::ResponseError::BadChunk},
        {"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n1000000000000000\r\n",
         Http::ResponseError::BadChunk},
    };
    for (const auto& [text, error] : cases)
    {
        Response response;
        Feed(response, text, 1000);
        REQUIRE(response.parser.Error() == error);
        REQUIRE_FALSE(response.parser.Finish());
        REQUIRE(response.parser.Error() == error);
    }
}

TEST_CASE("a response cut short is truncated")
{
    for (const char* text : {"", "HTTP/1.1 200 OK\r\n",
                             "HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nabc",
                             "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabc\r\n"})
    {
        Response response;
        Feed(response, text, 1000);
        REQUIRE_FALSE(response.parser.Finish());
        REQUIRE(response.parser.Error() == Http::ResponseError::Truncated);
    }
}

namespace
{

// Feeds text in pieces of step bytes, keeping what follows the head.
std::string FeedRequest(Http::RequestParser& parser, std::string_view text, size_t step = 1000)
{
    size_t offset = 0;
    while (offset < text.size() && !parser.Done())
    {
        const std::string_view piece = text.substr(offset, step);
        offset += parser.Feed(piece.data(), piece.size());
    }
    return std::string(text.substr(offset));
}

std::string Path(const char* target)
{
    char path[Http::MaximumPathLength + 1];
    REQUIRE(Http::RequestPath(target, path, sizeof(path)));
    return path;
}

} // namespace

TEST_CASE("request heads give the method, target and version, and stop at their end")
{
    for (const size_t step : {size_t{1}, size_t{5}, size_t{1000}})
    {
        Http::RequestParser parser;
        const std::string rest = FeedRequest(
            parser, "\r\nGET /docs/a%20b.txt?x=1 HTTP/1.1\r\nHost: example\r\n\r\nGET / HTTP/1.1",
            step);
        REQUIRE(parser.Done());
        REQUIRE(parser.ErrorStatus() == 0);
        REQUIRE(parser.RequestMethod() == Http::Method::Get);
        REQUIRE(std::string(parser.MethodName()) == "GET");
        REQUIRE(std::string(parser.Target()) == "/docs/a%20b.txt?x=1");
        REQUIRE(parser.MinorVersion() == 1);
        REQUIRE(parser.KeepAlive());
        REQUIRE_FALSE(parser.HasBody());
        REQUIRE(rest == "GET / HTTP/1.1");
    }

    Http::RequestParser head;
    FeedRequest(head, "HEAD / HTTP/1.1\nHost: x\n\n");
    REQUIRE(head.ErrorStatus() == 0);
    REQUIRE(head.RequestMethod() == Http::Method::Head);

    Http::RequestParser other;
    FeedRequest(other, "get / HTTP/1.1\r\nHost: x\r\n\r\n");
    REQUIRE(other.ErrorStatus() == 0);
    REQUIRE(other.RequestMethod() == Http::Method::Other);
}

TEST_CASE("the connection stays open by the version's default unless a request says otherwise")
{
    const std::pair<const char*, bool> cases[] = {
        {"GET / HTTP/1.1\r\nHost: x\r\n\r\n", true},
        {"GET / HTTP/1.1\r\nHost: x\r\nConnection: Upgrade, close\r\n\r\n", false},
        {"GET / HTTP/1.0\r\n\r\n", false},
        {"GET / HTTP/1.0\r\nConnection: keep-alive\r\n\r\n", true},
        {"GET / HTTP/1.0\r\nConnection: keep-alive, close\r\n\r\n", false},
        {"GET / HTTP/1.9\r\nHost: x\r\n\r\n", true},
    };
    for (const auto& [text, keep_alive] : cases)
    {
        Http::RequestParser parser;
        FeedRequest(parser, text);
        REQUIRE(parser.ErrorStatus() == 0);
        REQUIRE(parser.KeepAlive() == keep_alive);
    }
}

TEST_CASE("a request with a length or a transfer coding has a body")
{
    for (const char* text : {"POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 3\r\n\r\n",
                             "POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n"})
    {
        Http::RequestParser parser;
        FeedRequest(parser, text);
        REQUIRE(parser.ErrorStatus() == 0);
        REQUIRE(parser.HasBody());
    }
    Http::RequestParser empty;
    FeedRequest(empty, "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\n\r\n");
    REQUIRE_FALSE(empty.HasBody());
}

TEST_CASE("malformed requests are refused with a status")
{
    const std::string long_target = "GET /" + std::string(Http::MaximumLineLength, 'a') +
                                    " HTTP/1.1\r\nHost: x\r\n\r\n";
    const std::string long_header =
        "GET / HTTP/1.1\r\nX: " + std::string(Http::MaximumLineLength, 'a') + "\r\n\r\n";
    std::string many_headers = "GET / HTTP/1.1\r\nHost: x\r\n";
    while (many_headers.size() <= Http::MaximumRequestHeadLength)
    {
        many_headers += "X-Padding: " + std::string(100, 'p') + "\r\n";
    }
    const std::pair<std::string, int> cases[] = {
        {"GET /\r\n\r\n", 400},
        {"GET  / HTTP/1.1\r\nHost: x\r\n\r\n", 400},
        {"GET / HTTP/1.1 \r\nHost: x\r\n\r\n", 400},
        {"G(T / HTTP/1.1\r\nHost: x\r\n\r\n", 400},
        {"GET /a\x01 HTTP/1.1\r\nHost: x\r\n\r\n", 400},
        {"GET / HTTP/11\r\nHost: x\r\n\r\n", 400},
        {"GET / HTTP/2.0\r\nHost: x\r\n\r\n", 505},
        {"GET / HTTP/1.1\r\n\r\n", 400},
        {"GET / HTTP/1.1\r\nHost: x\r\nHost: y\r\n\r\n", 400},
        {"GET / HTTP/1.1\r\nHost : x\r\n\r\n", 400},
        {"GET / HTTP/1.1\r\nHost: x\r\nX-Folded: a\r\n b\r\n\r\n", 400},
        {"GET / HTTP/1.1\r\nHost: x\r\nContent-Length: -1\r\n\r\n", 400},
        {"GET / HTTP/1.1\r\nHost: x\r\nContent-Length: 1\r\nContent-Length: 2\r\n\r\n", 400},
        {long_target, 414},
        {long_header, 431},
        {many_headers, 431},
    };
    for (const auto& [text, status] : cases)
    {
        Http::RequestParser parser;
        FeedRequest(parser, text);
        REQUIRE(parser.Done());
        REQUIRE(parser.ErrorStatus() == status);
    }

    // A refused version still names the request, for the log.
    Http::RequestParser version;
    FeedRequest(version, "GET /x HTTP/2.0\r\n\r\n");
    REQUIRE(std::string(version.MethodName()) == "GET");
    REQUIRE(std::string(version.Target()) == "/x");
}

TEST_CASE("request paths are decoded and never climb above the root")
{
    REQUIRE(Path("/") == "/");
    REQUIRE(Path("/docs/a%20b.txt?x=1#top") == "/docs/a b.txt");
    REQUIRE(Path("/a/./b/../c") == "/a/c");
    REQUIRE(Path("/../../system/init.conf") == "/system/init.conf");
    REQUIRE(Path("/%2e%2e/%2E%2E/system") == "/system");
    REQUIRE(Path("/a%2F..%2F..%2Fsystem") == "/system");
    // A decoded '?' is part of the path, not the start of a query.
    REQUIRE(Path("/%3F/../../system") == "/system");
    REQUIRE(Path("http://example.com:8080/x/../y?q") == "/y");

    char path[Http::MaximumPathLength + 1];
    for (const char* target : {"*", "/a%2", "/a%zz", "/a%00b", "ftp://example.com/"})
    {
        REQUIRE_FALSE(Http::RequestPath(target, path, sizeof(path)));
    }
    char small[4];
    REQUIRE(Http::RequestPath("/abc", path, sizeof(path)));
    REQUIRE_FALSE(Http::RequestPath("/abcd", small, sizeof(small)));
}

TEST_CASE("encoded paths keep only unreserved characters and slashes")
{
    char encoded[64];
    REQUIRE(Http::EncodePath("/docs/a b&c:d\"<>%.txt", encoded, sizeof(encoded)));
    REQUIRE(std::string(encoded) == "/docs/a%20b%26c%3Ad%22%3C%3E%25.txt");
    REQUIRE(Http::EncodePath("/A-z_0.9~", encoded, sizeof(encoded)));
    REQUIRE(std::string(encoded) == "/A-z_0.9~");
    char small[4];
    REQUIRE(Http::EncodePath("abc", small, sizeof(small)));
    REQUIRE_FALSE(Http::EncodePath("ab ", small, sizeof(small)));
}

TEST_CASE("content types come from the extension")
{
    REQUIRE(std::string(Http::ContentType("/www/index.HTML")) == "text/html; charset=utf-8");
    REQUIRE(std::string(Http::ContentType("/hello.txt")) == "text/plain; charset=utf-8");
    REQUIRE(std::string(Http::ContentType("/logo.png")) == "image/png");
    REQUIRE(std::string(Http::ContentType("/system/vi")) == "application/octet-stream");
    REQUIRE(std::string(Http::ContentType("/a.d/file")) == "application/octet-stream");
    REQUIRE(std::string(Http::StatusReason(404)) == "Not Found");
}
