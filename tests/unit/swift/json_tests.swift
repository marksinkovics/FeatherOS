// Host tests for src/user/json, compiled into one module with its sources but main.swift.

#if canImport(Darwin)
    import Darwin
#elseif canImport(Glibc)
    import Glibc
#endif

nonisolated(unsafe) var failures = 0
nonisolated(unsafe) var checks = 0

func check(_ condition: Bool, _ what: @autoclosure () -> String, line: Int = #line) {
    checks += 1
    if !condition {
        failures += 1
        print("json_tests.swift:\(line): FAILED: \(what())")
    }
}

func bytes(_ text: String) -> [UInt8] {
    Array(text.utf8)
}

func text(_ bytes: [UInt8]) -> String {
    String(decoding: bytes, as: UTF8.self)
}

func parse(_ input: String) -> [JSONValue]? {
    try? JSONParser.parseAll(bytes(input))
}

func parseOne(_ input: String) -> JSONValue? {
    guard let values = parse(input), values.count == 1 else {
        return nil
    }
    return values[0]
}

func parseError(_ input: String) -> JSONParseError? {
    do throws(JSONParseError) {
        _ = try JSONParser.parseAll(bytes(input))
        return nil
    } catch {
        return error
    }
}

func compact(_ value: JSONValue) -> String {
    var output: [UInt8] = []
    CompactPrinter().write(value, into: &output)
    return text(output)
}

func pretty(_ value: JSONValue) -> String {
    var output: [UInt8] = []
    PrettyPrinter().write(value, into: &output)
    return text(output)
}

func path(_ text: String) -> JSONPath? {
    try? JSONPath(parsing: bytes(text))
}

func pathError(_ text: String) -> String? {
    do throws(JSONPathError) {
        _ = try JSONPath(parsing: bytes(text))
        return nil
    } catch {
        return error.message
    }
}

// The compact text of each result, or the error message.
func query(_ pathText: String, _ input: String) -> [String] {
    guard let path = path(pathText), let value = parseOne(input) else {
        return ["unparsed"]
    }
    do throws(JSONPathError) {
        return try path.evaluate(value).map(compact)
    } catch {
        return [error.message]
    }
}

func checkParseError(
    _ input: String, _ message: String, line: Int, column: Int, sourceLine: Int = #line
) {
    guard let error = parseError(input) else {
        check(false, "\(input) parsed", line: sourceLine)
        return
    }
    check(
        error.message == message && error.line == line && error.column == column,
        "\(input): \(error)", line: sourceLine)
}

func testLiteralsAndNumbers() {
    check(parseOne("null") == .null, "null")
    check(parseOne(" true ") == .bool(true), "true")
    check(parseOne("false") == .bool(false), "false")
    for number in ["0", "-0", "12", "-3.25", "1e5", "1E+5", "2.5e-3", "123456789012345678901234"] {
        check(parseOne(number) == .number(bytes(number)), "number \(number)")
    }
    checkParseError("01", "leading zero in a number", line: 1, column: 2)
    checkParseError("-", "expected a digit after a minus sign, found the end of the input",
        line: 1, column: 2)
    checkParseError("1.", "expected a digit after a decimal point, found the end of the input",
        line: 1, column: 3)
    checkParseError("1e+", "expected a digit after an exponent, found the end of the input",
        line: 1, column: 4)
    checkParseError(".5", "expected a value, found '.'", line: 1, column: 1)
    checkParseError("+1", "expected a value, found '+'", line: 1, column: 1)
    checkParseError("12a", "unexpected 'a' after a value", line: 1, column: 3)
    checkParseError("truex", "unexpected 'x' after a value", line: 1, column: 5)
    checkParseError("nul", "expected a value, found 'n'", line: 1, column: 1)
}

func testStrings() {
    check(parseOne(#""plain""#) == .string(bytes("plain")), "plain string")
    check(
        parseOne(#""\"\\\/\b\f\n\r\t""#) == .string([0x22, 0x5c, 0x2f, 0x08, 0x0c, 0x0a, 0x0d, 0x09]),
        "simple escapes")
    check(parseOne(#""caf\u00e9""#) == .string(bytes("café")), "two-byte escape")
    check(parseOne(#""\u20AC""#) == .string(bytes("€")), "three-byte escape")
    check(parseOne(#""\ud83d\ude00""#) == .string(bytes("😀")), "surrogate pair")
    check(parseOne("\"raw é 😀\"") == .string(bytes("raw é 😀")), "raw UTF-8")
    checkParseError(#""\ude00""#, "low surrogate without a high one", line: 1, column: 2)
    checkParseError(#""\ud83d x""#, "high surrogate without a low one", line: 1, column: 2)
    checkParseError(#""\ud83d\u0041""#, "high surrogate without a low one", line: 1, column: 8)
    checkParseError(#""\u12g4""#,
        "expected four hex digits after \\u, found 'g'", line: 1, column: 6)
    checkParseError(#""\x""#, "unknown escape: \\ followed by 'x'", line: 1, column: 3)
    checkParseError("\"tab\there\"", "control character in a string", line: 1, column: 5)
    checkParseError("\"open", "unterminated string", line: 1, column: 6)
    checkParseError("\"\\", "unterminated string", line: 1, column: 3)
    for invalid: [UInt8] in [[0xff], [0xc0, 0xaf], [0xe0, 0x80, 0xaf], [0xed, 0xa0, 0x80],
        [0xf4, 0x90, 0x80, 0x80], [0xc3]]
    {
        let accepted = try? JSONParser.parseAll([0x22] + invalid + [0x22])
        check(accepted == nil, "invalid UTF-8 \(invalid) accepted")
    }
}

func testContainers() {
    check(parseOne("[]") == .array([]), "empty array")
    check(parseOne("{ }") == .object([]), "empty object")
    let nested = parseOne(#" { "a" : [1, {"b": null}], "c": "d" } "#)
    let expected = JSONValue.object([
        JSONMember(key: bytes("a"), value: .array([
            .number(bytes("1")), .object([JSONMember(key: bytes("b"), value: .null)]),
        ])),
        JSONMember(key: bytes("c"), value: .string(bytes("d"))),
    ])
    check(nested == expected, "nested containers")
    check(
        parseOne(#"{"k": 1, "j": 2, "k": 3}"#)
            == .object([
                JSONMember(key: bytes("k"), value: .number(bytes("3"))),
                JSONMember(key: bytes("j"), value: .number(bytes("2"))),
            ]),
        "a repeated key keeps its first place and last value")
    let many = (0..<20).map { "\"k\($0)\": \($0)" }.joined(separator: ", ")
    if case .object(let members)? = parseOne("{\(many), \"k3\": \"again\", \"k15\": true}") {
        check(members.count == 20, "many keys: \(members.count) members")
        check(members[3].value == .string(bytes("again")), "repeat among many keys")
        check(members[15].value == .bool(true), "repeat after the dictionary is built")
    } else {
        check(false, "many keys did not parse")
    }
    checkParseError("[1,]", "expected a value, found ']'", line: 1, column: 4)
    checkParseError("[1 2]", "expected ',' or ']' after an element, found '2'", line: 1, column: 4)
    checkParseError(#"{"a" 1}"#, "expected ':' after a member name, found '1'", line: 1, column: 6)
    checkParseError(#"{"a":1,}"#, "expected a member name, found '}'", line: 1, column: 8)
    checkParseError(#"{"a":1"#, "expected ',' or '}' after a member, found the end of the input",
        line: 1, column: 7)
    checkParseError("{\n  \"é\": [\n    1,\n    x\n  ]\n}", "expected a value, found 'x'",
        line: 4, column: 5)
    checkParseError("[\"é\", ?]", "expected a value, found '?'", line: 1, column: 7)
    checkParseError("[\u{1}]", "expected a value, found byte 0x01", line: 1, column: 2)
    let deepest = String(repeating: "[", count: JSONParser.maximumDepth)
        + String(repeating: "]", count: JSONParser.maximumDepth)
    check(parse(deepest) != nil, "nesting at the limit")
    checkParseError("[" + deepest + "]", "nested deeper than 256 levels", line: 1, column: 257)
}

func testSequences() {
    check(parse("") == [], "empty input")
    check(parse(" \n\t ") == [], "only whitespace")
    check(
        parse("1 [2]{\"a\":3}\n\"x\"null")
            == [.number(bytes("1")), .array([.number(bytes("2"))]),
                .object([JSONMember(key: bytes("a"), value: .number(bytes("3")))]),
                .string(bytes("x")), .null],
        "a sequence of values")
    var parser = JSONParser(bytes("{\"a\":1} ]"))
    check((try? parser.next()) != nil, "the value before an error")
    checkParseError("{\"a\":1} ]", "expected a value, found ']'", line: 1, column: 9)
}

func testPrinters() {
    let value = parseOne(#"{"a": [1, {"b": null}, [], {}], "s": "line\nnext", "t": true}"#)!
    check(
        compact(value) == #"{"a":[1,{"b":null},[],{}],"s":"line\nnext","t":true}"#,
        "compact: \(compact(value))")
    let expected = """
        {
          "a": [
            1,
            {
              "b": null
            },
            [],
            {}
          ],
          "s": "line\\nnext",
          "t": true
        }
        """
    check(pretty(value) == expected, "pretty:\n\(pretty(value))")
    check(compact(.array([])) == "[]", "empty array")
    check(pretty(.object([])) == "{}", "empty object")
    check(
        compact(.string([0x22, 0x5c, 0x08, 0x0c, 0x0d, 0x09, 0x01, 0x1f, 0x7f] + bytes("/é")))
            == #""\"\\\b\f\r\t\u0001\u001f\u007f/é""#,
        "escapes: \(compact(.string([0x01, 0x7f])))")
    let original = #"[0, -1.5e3, "😀\u0000", {"k": [true, false, null]}]"#
    let parsed = parseOne(original)!
    check(parseOne(compact(parsed)) == parsed, "compact output parses back")
    check(parseOne(pretty(parsed)) == parsed, "pretty output parses back")
}

func testPaths() {
    check(path(".") == JSONPath(steps: []), "identity")
    check(
        path(".a.b[0]") == JSONPath(steps: [.key(bytes("a")), .key(bytes("b")), .index(0)]),
        ".a.b[0]")
    check(path(".a[-2]") == JSONPath(steps: [.key(bytes("a")), .index(-2)]), "negative index")
    check(path(".[]") == JSONPath(steps: [.each]), ".[]")
    check(path(".a.[1][]") == JSONPath(steps: [.key(bytes("a")), .index(1), .each]), ".a.[1][]")
    check(
        path(#".["two words"]."x\u0041y".z_9"#)
            == JSONPath(steps: [.key(bytes("two words")), .key(bytes("xAy")), .key(bytes("z_9"))]),
        "quoted names")
    check(pathError("a") == "a path starts with '.'", "no dot")
    check(pathError("") == "a path starts with '.'", "empty path")
    check(pathError("..a") == "..a: column 2: expected a name after '.'", "two dots")
    check(pathError(".a.") == ".a.: column 4: expected a name after '.'", "trailing dot")
    check(pathError(".a b") == ".a b: column 3: expected '.' or '['", "a space")
    check(pathError(".[x]") == ".[x]: column 3: expected an index, a quoted name or ']'", ".[x]")
    check(pathError(".[1") == ".[1: column 4: expected ']'", "unclosed bracket")
    check(pathError(".[12345678901]") == ".[12345678901]: column 3: index too large", "big index")
    check(pathError(#"."open"#) == #"."open: column 7: unterminated string"#, "unclosed quote")
}

func testEvaluation() {
    let document = #"{"a": {"b": [10, 20, 30]}, "n": null, "s": "text", "o": {"x": 1, "y": 2}}"#
    check(query(".", "[1, 2]") == ["[1,2]"], "identity")
    check(query(".a.b[0]", document) == ["10"], ".a.b[0]")
    check(query(".a.b[-1]", document) == ["30"], "last element")
    check(query(".a.b[3]", document) == ["null"], "past the end")
    check(query(".a.b[-4]", document) == ["null"], "before the start")
    check(query(".missing", document) == ["null"], "missing member")
    check(query(".n.deeper[2]", document) == ["null"], "steps into null")
    check(query(".a.b[]", document) == ["10", "20", "30"], "every element")
    check(query(".o[]", document) == ["1", "2"], "every member value")
    check(query(".[].x", #"[{"x": 1}, {"y": 2}, null]"#) == ["1", "null", "null"], "fan out")
    check(query(".[]", "[]") == [], "nothing to iterate")
    check(query(".s.t", document) == [#"cannot index a string with "t""#], "member of a string")
    check(query(".a[0]", document) == ["cannot index an object with a number"], "index object")
    check(query(".s[]", document) == ["cannot iterate over a string"], "iterate string")
    check(query(".[]", "true") == ["cannot iterate over a boolean"], "iterate boolean")
}

@main
struct JSONTests {
    static func main() {
        testLiteralsAndNumbers()
        testStrings()
        testContainers()
        testSequences()
        testPrinters()
        testPaths()
        testEvaluation()
        print("\(checks) JSON checks, \(failures) failed")
        exit(failures == 0 ? 0 : 1)
    }
}
