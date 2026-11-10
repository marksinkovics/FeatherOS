from smoke.cases.shell.part import Part
from smoke.cases.shell.swift_concurrency import (
    EXPECTED_SWIFT_CONCURRENCY_OUTPUT,
)
from smoke.cases.shell.swift_cxx import EXPECTED_SWIFT_CXX_OUTPUT
from smoke.script import Lines, TypeWhen

# Full Swift, with runtime type information, built where deps/ holds the
# runtime's sources.
SWIFT_FULL_INPUT = (
    b'fullhello one "two words"\nfullhello fail\nreflect\n'
    b"fulltasks\nfullexecutors\nfullcxxinterop\nfullregex\n"
    # Codable over the standard library's protocols, and programs ported
    # from Embedded Swift with FeatherSupport built in full mode. Their
    # arguments differ from the Embedded checks', whose output would match.
    b"fullcodable\n"
    b"echo [1,300] | fulljson -t [int8]\n"
    b'fulljson -c -t "{int?}" .milestones /system/sample.json\n'
    b'echo [[1,2.5],null] | fulljson -c -t "[[double]?]"\n'
    b"fulllife -p glider -s 7x5 -n 3\n"
    b"fullhttp url http://[::1]:81/x/y ./z ?q\n"
    b"fullhttp url https://example.com\n"
    b"fullswiftbench -n 1\n"
)
# life animates until q; Embedded Swift's check animates a glider instead.
SWIFT_FULL_LIFE_INPUT = (
    b"fulllife -p blinker\n",
    TypeWhen("3 alive; q quits", b"q"),
)
EXPECTED_SWIFT_FULL_OUTPUT = (
    "Hello from full Swift",
    "fullhello: arguments one, two words",
    "fullhello: sorted 5.10 6.0 6.4",
    "fullhello: 6.4 counted 2 times of 3 versions",
    "fullhello: popped 6.0 5.10 6.4",
    "fullhello: largest 6.4 os 2.5",
    "fullhello: 2 distinct words, sum 5050",
    "fullhello: buffer capacity at least 10 true",
    "fullhello/fullhello.swift:33: "
    "Fatal error: fullhello: failed on purpose\r\n"
    "shell: program terminated by a CPU exception",
    "reflect: cast square of 4.0\r\n"
    "reflect: cast Circle of 3.0, 0 sides\r\n"
    "reflect: cast Pair<Square> of 10.0, 8 sides\r\n"
    "reflect: cast other Pair<Int>\r\n"
    "reflect: cast numbers summing to 6\r\n"
    "reflect: cast numbers summing to 3\r\n"
    "reflect: cast int 7\r\n"
    "reflect: cast collection of 7",
    "reflect: sides Square 4, Int none, Pair<Circle> 0, Pair<Int> none",
    'reflect: named ["Rex", "Tom", "Node"] dogs 1',
    'reflect: parsed 42, empty, badCharacter("x", at: 1)',
    "reflect: counts [2, 1, 4] sum 16",
    "reflect: mirror struct start=Point(x: 0, y: 0) end=Point(x: 3, y: 4) "
    'label=Optional("diagonal")',
    'reflect: mirror class tricks=["sit", "roll"] super name=Rex score=0',
    'reflect: mirror badCharacter=("x", at: 1) .0=1 label=a celsius=27',
    # The terminal shows bytes above 0x7E, here dump's UTF-8 triangles, as '?'.
    "??? reflect.Dog #0\r\n"
    "  ??? super: reflect.Animal\r\n"
    '    - name: "Rex"\r\n'
    "    - score: 0\r\n"
    "  ??? tricks: 2 elements\r\n"
    '    - "sit"\r\n'
    '    - "roll"',
    'reflect: keypaths Point(x: 10, y: 20) 4 ["Point(x: 0, y: 0)", '
    '"Optional(\\"diagonal\\")", "Optional(8)"] [3, -1]',
    "reflect: keypaths score 5 distinct 2 equal true",
    "reflect: weak parent root friend Rex\r\n"
    "reflect: deinit root\r\n"
    "reflect: weak parent nil",
    "reflect: unowned owner Ann captured score 1",
    "reflect: generic Tom looks at Point(x: 10, y: 20), "
    "Rex wags at Point(x: 10, y: 20)",
    "reflect: generic Container<Int> of [3, 1, 2]",
    "reflect: generic SortedContainer<Int> of [1, 2, 3], sorted",
    'reflect: made Dog Fido ["sit", "roll"]',
    'reflect: describing north | Point(x: 10, y: 20) | (1, "two") | '
    "Optional(3) | nil",
    "reflect: describing Pair<Double>(first: 1.5, second: 2.5) | "
    "[reflect.Direction.south: [1]] | Int | Pair<String>",
    "reflect: describing reflect.Node | (Function) | south | empty",
    "reflect: reflecting reflect.Point(x: 10, y: 20) reflect.Direction.south "
    "reflect.Pair<reflect.Point>",
    "fullregex: literal 2026-11-09 year 2026 month 11 day 09\r\n"
    'fullregex: matches ["2026-11-09=disk full", '
    '"2026-12-01=network down"]\r\n'
    "fullregex: whole true false\r\n"
    "fullregex: optional - a\r\n"
    "fullregex: replacing a#b#c#\r\n"
    "fullregex: replacing 09/11/2026\r\n"
    'fullregex: split ["one", "two", "three", "four"]\r\n'
    "fullregex: ranges 3 contains true\r\n"
    "fullregex: trimming indented\r\n"
    "fullregex: backreference hello\r\n"
    'fullregex: lookahead ["100"]\r\n'
    "fullregex: reluctant <a> greedy <a><b>\r\n"
    "fullregex: ignoring case true\r\n"
    'fullregex: line anchors ["2026-12-01 error network down"]\r\n'
    "fullregex: graphemes true scalars true\r\n"
    "fullregex: script [3]\r\n"
    "fullregex: prefix move 3 north steps 3\r\n"
    "fullregex: starts true false",
    'fullregex: runtime ["a:1", "b:", "c:33"] captures 3\r\n'
    "fullregex: typed major 6 minor 4\r\n"
    "fullregex: invalid rejected\r\n"
    'fullregex: builder ["apples 12 green", "pears 3 -"]\r\n'
    "fullregex: builder total 15",
    'fullcodable: encoded {"number":25,"phase":"done","steps":'
    '[{"build":{"arch":"aarch64"}},{"boot":{"seconds":1.5}}],'
    '"counts":{"parts":8}}, alike true',
    "fullcodable: DecodingError.dataCorrupted: Data was corrupted. "
    "Path: phase. Debug description: Cannot initialize Phase from invalid "
    "String value late",
    "fullcodable: DecodingError.keyNotFound: Key 'number' not found in "
    "keyed decoding container. Debug description: No value associated "
    'with key "number".',
    "fullcodable: DecodingError.typeMismatch: Expected value of type "
    "Double. Path: steps[0].boot.seconds. Debug description: Expected a "
    "number but found a string instead.",
    "fullcodable: DecodingError.valueNotFound: Expected value of type Int. "
    "Path: number. Debug description: Expected a number but found null "
    "instead.",
    "fullcodable: DecodingError.dataCorrupted: Data was corrupted. "
    "Path: [1]. Debug description: Parsed number <300> does not fit in "
    "Int8.",
    "fullcodable: decoded boot(seconds: nil)",
    'fullcodable: keys {"aarch64":8,"x86_64":64} alike true '
    '{"1":"one","2":"two"} [0.5,true]',
    "fullcodable: existentials 5 of 6 "
    '["done",7,"seven",{"build":{"arch":"x86_64"}},0.5]',
    'fullcodable: subclass {"super":{"name":"Rex"},"tricks":["sit","roll"]} '
    'Rex ["sit", "roll"]',
    '{\r\n  "first": -5,\r\n  "second": 9223372036854775807\r\n}\r\n'
    "fullcodable: pair alike true",
    "fullcodable: EncodingError.invalidValue: 18446744073709551616 "
    "(UInt128). Path: first. Debug description: Integer does not fit in 64 "
    "bits.",
    "fulljson: standard input: DecodingError.dataCorrupted: Data was "
    "corrupted. Path: [1]. Debug description: Parsed number <300> does not "
    "fit in Int8.",
    "fulljson: /system/sample.json: DecodingError.typeMismatch: Expected "
    "value of type Int. Path: next. Debug description: Expected a number "
    "but found a string instead.",
    "[[1,2.5],null]\r\n",
    ".......\r\n.......\r\n...#...\r\n....##.\r\n...##..\r\n",
    "host ::1\r\nport 81\r\npath /x/y\r\n"
    "./z resolves to http://[::1]:81/x/z\r\n"
    "?q resolves to http://[::1]:81/x/y?q",
    "fullhttp: https://example.com: only http URLs are supported",
    "life: check 691, 1 run in ",
    "sieve: check 78498, 1 run in ",
    "quicksort: check 2963021034, 1 run in ",
    "sort: check 2963021034, 1 run in ",
    "json: check 138999, 1 run in ",
    "life: generation 0, 3 alive; q quits",
) + tuple(
    # tasks, executors and cxxinterop built in full Swift, over upstream's
    # _Concurrency and Cxx.
    "full" + text
    for text in EXPECTED_SWIFT_CONCURRENCY_OUTPUT + EXPECTED_SWIFT_CXX_OUTPUT
    if text.startswith(("tasks:", "executors:", "cxxinterop:"))
)

PART = Part(
    Lines(SWIFT_FULL_INPUT) + list(SWIFT_FULL_LIFE_INPUT),
    EXPECTED_SWIFT_FULL_OUTPUT,
    requires="fullhello",
    missing_note="full Swift was not built; skipping its checks",
)
