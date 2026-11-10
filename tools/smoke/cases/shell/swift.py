from smoke.cases.shell.part import Part
from smoke.script import Lines, TypeWhen

# Programs written in Embedded Swift, which the build skips without a Swift
# toolchain.
SWIFT_SHELL_INPUT = (
    b'hello one "two words"\n'
    b'echo "Embedded Swift" | rev\n'
    b"write /tmp/rev.txt abc def\n"
    b"rev /tmp/rev.txt /tmp/none\n"
    b'echo "ab cd" | rev -n\n'
    b"rev -h\n"
    # regex reports subexpressions through FeatherSupport's
    # RegularExpression over the C library's regex.h.
    b"write /tmp/s.txt key=42 id=7\n"
    b'regex -a "([a-z]+)=([0-9]+)( )?" /tmp/s.txt\n'
    b"echo AxB a.b | regex -B -i a.\\(b\\)\n"
    b"regex ( /tmp/s.txt\nregex -h\n"
    b"json .name /system/sample.json\n"
    b"json -r .motto /system/sample.json\n"
    b"json -c .milestones /system/sample.json\n"
    b"json .motto /system/sample.json\n"
    b"json -c .architectures[] /system/sample.json\n"
    b"echo [10,[20,30]] | json .[-1]\n"
    b"echo [1,,2] | json\n"
    b"json .a..b /system/sample.json\n"
    b"json .name[0] /system/sample.json /none\n"
    b'json -c -t "{double}" .versions /system/sample.json\n'
    b"json -t [string] .architectures /system/sample.json\n"
    b'json -c -t "{int?}" .milestones /system/sample.json\n'
    b"echo [1,300] | json -t [int8]\n"
    b'echo [1,null] | json -c -t "[uint16?]"\n'
    b"json -t ratio .ratio /system/sample.json\n"
    b"json -h\n"
    b"life -p glider -s 6x6 -n 4\n"
    b"life -p blinker -s 5x5 -n 1\n"
    b"life -n 2 | wc -c\n"
    b"life -p nothing\n"
    b"life -s 2x2 -n 1\n"
    b"life < /system/hello.txt\n"
    b"life -h\n"
    # The check values are cppbench's, so both did the same work.
    b"swiftbench -n 2\n"
    b"swiftbench -t x\n"
)
# life animates on the alternate screen until q; only a glider has 5 alive.
SWIFT_LIFE_INPUT = (b"life -p glider\n", TypeWhen("5 alive; q quits", b"q"))
EXPECTED_SWIFT_OUTPUT = (
    "Hello from Embedded Swift",
    "hello: arguments one, two words",
    "hello: argc 3, the last argument two words",
    "hello: formatted [ab   ] [003.1] [0xff] [+42] [A] "
    "[-9223372036854775808] [%d]",
    "hello: read feather, close-on-exec true then false, "
    "ioctl on a file Not a terminal, on the terminal true",
    "hello: largest square of 25, rectangle of 12",
    "hello: tracker released",
    "hello: tracker was shared true",
    "hello: 2 distinct words, 4 characters, roll in range true",
    "hello: running as process ",
    "hello: parsed 0.0025 and 0.1",
    "hello: printed items|3|2.5|true|6.4.2",
    'hello: 3 hashables, "1" is a String, base 7',
    "hello: withVaList 1 2 3 4 5 6 7 8, 0.5 1 1.5 2 2.5 3 3.5 4 4.5",
    "hello: written with write",
    "hello: /etc/hosts read whole and by line alike true",
    "hello: /none: No such file or directory",
    'hello: encoded {"number":25,"phase":"done","steps":'
    '[{"build":{"arch":"aarch64"}},{"boot":{"seconds":1.5}}],'
    '"counts":{"parts":8}}, alike true',
    "hello: DecodingError.dataCorrupted: Data was corrupted. "
    "Path: phase. Debug description: Cannot initialize from invalid "
    "String value late",
    "tfiwS deddebmE",
    "fed cba",
    "rev: /tmp/none: No such file or directory",
    "     1  dc ba",
    "usage: rev [-n] [file...]",
    "1: key=42  [key] [42] [ ]",
    "1: id=7 [id] [7] -",
    "1: AxB [B]",
    "regex: Unmatched ( or \\(",
    "usage: regex [-Bia] <pattern> [file...]",
    '"FeatherOS"',
    # The serial console shows the tab as a space.
    'light as a "feather"',
    '{"done":17,"next":"json"}',
    '"light\\tas a \\"feather\\""',
    '"x86_64"\r\n"aarch64"',
    "[\r\n  20,\r\n  30\r\n]",
    "json: standard input: line 1, column 4: expected a value, found ','",
    "json: invalid path .a..b: column 4: expected a name after '.'",
    "json: /system/sample.json: cannot index a string with a number",
    "json: /none: No such file or directory",
    # -t decodes into Swift types with JSONDecoder and encodes them again.
    '{"swift":6.4,"limine":9}',
    '[\r\n  "x86_64",\r\n  "aarch64"\r\n]',
    "json: /system/sample.json: DecodingError.typeMismatch. "
    "Path: next. Debug description: Expected a number but found a string "
    "instead.",
    "json: standard input: DecodingError.dataCorrupted: Data was "
    "corrupted. Path: [1]. Debug description: Parsed number <300> does not "
    "fit in Int8.",
    "[1,null]",
    "json: unknown type ratio; see json -h",
    "usage: json [-c] [-r] [-t type] [path [file...]]",
    "......\r\n......\r\n...#..\r\n....#.\r\n..###.\r\n......\r\n",
    ".....\r\n..#..\r\n..#..\r\n..#..\r\n.....\r\n",
    # 23 rows of 80 cells, the size without a terminal.
    "\n1863\r\n",
    "life: unknown pattern nothing; see life -h",
    "life: invalid size 2x2; see life -h",
    "life: standard input is not a terminal; use -n to print the grid",
    "usage: life [-p glider|blinker|random] [-n generations]",
    "life: generation 0, 5 alive; q quits",
    "life: check 691, 2 runs in ",
    "sieve: check 78498, 2 runs in ",
    "quicksort: check 2963021034, 2 runs in ",
    "sort: check 2963021034, 2 runs in ",
    "json: check 138999, 2 runs in ",
    "swiftbench: -t needs a positive count; see swiftbench -h",
)

PART = Part(
    Lines(SWIFT_SHELL_INPUT) + list(SWIFT_LIFE_INPUT),
    EXPECTED_SWIFT_OUTPUT,
    requires="hello",
    missing_note="Swift programs were not built; skipping their checks",
)
