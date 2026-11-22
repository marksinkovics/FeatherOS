from smoke.cases.shell.part import Part
from smoke.script import Lines

# Swift programs that call C++, built where deps/ holds the Cxx module's
# sources; gfxcheck draws with the graphics library and compares checksums;
# featherfall's rules run from the GRAPHICS mode and wickdeep's from DUNGEON,
# here only their arguments.
SWIFT_CXX_INPUT = (
    b"cxxinterop\n"
    b"gfxcheck\n"
    b"http url http://example.com:8080/a/b/c?q ../d //[::1]/x ?r\n"
    b"http url https://example.com\n"
    b"http request /system/http-request.txt\n"
    b"http response /system/http-response.txt\n"
    b'echo "HTTP/1.1 200 OK" | http response\n'
    b"http -h\n"
    b"featherfall -h\n"
    b"featherfall --demo --seed x\n"
    b"wickdeep -h\n"
    b"wickdeep --demo --seed x\n"
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
    "cxxinterop: string hello, swift, 5 words, equal true, same hash true",
    "cxxinterop: chrono 90 seconds, true",
    "gfxcheck: surface 64x56, sprite 8x8",
    "gfxcheck: checksum 569184433, match true",
    "gfxcheck: tiles 2 of 8x8, loaded true",
    "gfxcheck: tile checksum 1753845943, match true",
    "gfxcheck: font checksum 1648750494, match true",
    "gfxcheck: font sans 16, line 16, text ends at 157, console 8x16",
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
    "usage: featherfall [--seed N] | --selftest | --demo [--seed N]\r\n"
    "A falling-block puzzle for a virtual console. Keys: Left and Right "
    "move, Down drops\r\n"
    "softly, Space drops at once, Up or X turns clockwise, Z anticlockwise, "
    "C or Shift holds\r\n"
    "a piece, P pauses, Esc quits. A top-10 table is kept in "
    "$HOME/.featherfall (/tmp\r\n"
    "without HOME); at game over type three initials, then Enter plays "
    "again.\r\n"
    "--seed N fixes the order of pieces. --selftest checks the rules; --demo "
    "plays a fixed\r\n"
    "script without a display and prints the score, lines, level and a hash "
    "of the well.",
    "featherfall: invalid seed x; see featherfall -h",
    "usage: wickdeep [--seed N] | --selftest | --check-save PATH | "
    "--demo [--seed N] [--save-at TURN PATH]\r\n"
    "A turn-based dungeon crawler for a virtual console: eight "
    "floors lit by a lamp that runs\r\n"
    "down, and the Heartwick at the bottom. Keys: arrows or keypad "
    "8 4 6 2 move or attack,\r\n"
    "keypad 7 9 1 3 move diagonally, Space waits, Enter takes the "
    "stairs, I opens the pack (Up\r\n"
    "and Down choose, Enter uses or equips, D drops, Esc closes), H "
    "shows help, Esc or Ctrl+C\r\n"
    "saves the run to $HOME/.wickdeep-save (/tmp without HOME) and "
    "quits; the next start\r\n"
    "resumes it, and a save that does not check out is set aside as "
    ".wickdeep-save.bad. Runs\r\n"
    "that end are ranked in $HOME/.wickdeep-runs. --seed N fixes a "
    "new run's seed.\r\n"
    "--selftest checks the rules and the save format; --check-save "
    "reads a save and says whether\r\n"
    "it is sound; --demo plays a fixed bot from the first floor to "
    "its death or the Heartwick\r\n"
    "without a display and prints the turns, depth, level, score, "
    "outcome and a hash of the\r\n"
    "encoded save, which --save-at writes to PATH when the bot "
    "reaches that turn.",
    "wickdeep: invalid seed x; see wickdeep -h",
)

PART = Part(
    Lines(SWIFT_CXX_INPUT),
    EXPECTED_SWIFT_CXX_OUTPUT,
    requires="http",
    missing_note="Swift's C++ programs were not built; skipping them",
)
