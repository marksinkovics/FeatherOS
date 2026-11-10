from smoke.cases.shell.part import Part
from smoke.script import Lines

# Swift programs that call C++, built where deps/ holds the Cxx module's
# sources.
SWIFT_CXX_INPUT = (
    b"cxxinterop\n"
    b"http url http://example.com:8080/a/b/c?q ../d //[::1]/x ?r\n"
    b"http url https://example.com\n"
    b"http request /system/http-request.txt\n"
    b"http response /system/http-response.txt\n"
    b'echo "HTTP/1.1 200 OK" | http response\n'
    b"http -h\n"
)
EXPECTED_SWIFT_CXX_OUTPUT = (
    "cxxinterop: constant 42, the color after red is green true",
    "cxxinterop: point sum (4, 6), equal true",
    "cxxinterop: template box holds 7",
    "cxxinterop: tracked 1 and 2, live 2",
    "cxxinterop: tracked live after the scope 0",
    "cxxinterop: move-only counter 2",
    "cxxinterop: square area 9",
    "cxxinterop: node value 5, live 1",
    "cxxinterop: nodes live after the scope 0",
    "cxxinterop: optional half of 8 4, of 7 nil true",
    "cxxinterop: vector 0 1 4 of 5, sum through a span 30",
    "cxxinterop: vowels in featheros 4",
    "host example.com\r\nport 8080\r\npath /a/b/c?q\r\n"
    "../d resolves to http://example.com:8080/a/d\r\n"
    "//[::1]/x resolves to http://[::1]/x\r\n"
    "?r resolves to http://example.com:8080/a/b/c?r",
    "http: https://example.com: only http URLs are supported",
    "GET /docs/a%20b/../index.html names /docs/index.html "
    "(text/html; charset=utf-8), HTTP/1.1, keep-alive\r\n"
    "HEAD /notes.txt?raw names /notes.txt (text/plain; charset=utf-8), "
    "HTTP/1.0, close\r\n"
    "refused with 505 HTTP Version Not Supported",
    "200 OK\r\nFeather OS",
    "http: connection closed before the end of the response",
    "usage: http url <url> [reference...]",
)

PART = Part(
    Lines(SWIFT_CXX_INPUT),
    EXPECTED_SWIFT_CXX_OUTPUT,
    requires="http",
    missing_note="Swift's C++ programs were not built; skipping them",
)
