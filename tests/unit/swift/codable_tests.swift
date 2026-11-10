// Host tests for FeatherOS's Codable (src/user/swift_stdlib/codable.swift) and FeatherSupport's
// JSON coders, built with Embedded Swift against FeatherOS's standard library module, and in full
// Swift against the host's library with FeatherSupport's codable_containers.swift.

@_silgen_name("exit")
func exit(_ status: Int32) -> Never

nonisolated(unsafe) var failures = 0
nonisolated(unsafe) var checks = 0

func check(_ condition: Bool, _ what: @autoclosure () -> String, line: Int = #line) {
    checks += 1
    if !condition {
        failures += 1
        print("codable_tests.swift:\(line): FAILED: \(what())")
    }
}

func text(_ bytes: [UInt8]) -> String {
    String(decoding: bytes, as: UTF8.self)
}

func encoded<T: Encodable>(_ value: T, _ formatting: JSONEncoder.OutputFormatting = [])
    -> String
{
    var encoder = JSONEncoder()
    encoder.outputFormatting = formatting
    do {
        return text(try encoder.encode(value))
    } catch let error as EncodingError {
        return "error: \(describe(error))"
    } catch {
        return "unexpected error"
    }
}

func decoded<T: Decodable>(_ type: T.Type, _ json: String) -> T? {
    try? JSONDecoder().decode(type, from: Array(json.utf8))
}

// The error's description, or the value encoded again if decoding succeeded.
func decodingError<T: Codable>(_ type: T.Type, _ json: String) -> String {
    do {
        return "decoded \(encoded(try JSONDecoder().decode(type, from: Array(json.utf8))))"
    } catch let error as DecodingError {
        return describe(error)
    } catch {
        return "unexpected error"
    }
}

#if $Embedded
    func describe(_ error: EncodingError) -> String {
        error.description
    }

    func describe(_ error: DecodingError) -> String {
        error.description
    }
#else
    // The host's library words its descriptions as its own release does, and names types, so the
    // checks compare what Embedded Swift's description is made of.
    func describe(_ error: EncodingError) -> String {
        switch error {
        case .invalidValue(_, let context):
            describe("EncodingError.invalidValue", context.codingPath, context.debugDescription)
        @unknown default:
            "unknown error"
        }
    }

    func describe(_ error: DecodingError) -> String {
        switch error {
        case .typeMismatch(_, let context):
            describe("DecodingError.typeMismatch", context.codingPath, context.debugDescription)
        case .valueNotFound(_, let context):
            describe("DecodingError.valueNotFound", context.codingPath, context.debugDescription)
        case .keyNotFound(let key, let context):
            describe(
                "DecodingError.keyNotFound: Key '\(describe([key]))' not found in keyed decoding "
                    + "container", context.codingPath, context.debugDescription)
        case .dataCorrupted(let context):
            describe(
                "DecodingError.dataCorrupted: Data was corrupted", context.codingPath,
                context.debugDescription)
        @unknown default:
            "unknown error"
        }
    }

    func describe(_ message: String, _ path: [any CodingKey], _ debugDescription: String)
        -> String
    {
        var output = message
        if !path.isEmpty {
            output += ". Path: \(describe(path))"
        }
        if !debugDescription.isEmpty {
            output += ". Debug description: \(debugDescription)"
        }
        return output
    }

    func describe(_ path: [any CodingKey]) -> String {
        var output = ""
        for (position, key) in path.enumerated() {
            if let index = key.intValue {
                output += "[\(index)]"
                continue
            }
            if position > 0 {
                output += "."
            }
            let name = key.stringValue
            guard name.contains(where: { $0 == "." || $0 == "`" || $0 == "\\" }) else {
                output += name
                continue
            }
            output += "`"
            for character in name {
                if character == "`" || character == "\\" {
                    output += "\\"
                }
                output.append(character)
            }
            output += "`"
        }
        return output
    }
#endif

func roundTrips<T: Codable & Equatable>(_ value: T, line: Int = #line) {
    var encoder = JSONEncoder()
    encoder.outputFormatting = [.sortedKeys]
    do {
        let json = try encoder.encode(value)
        let back = try JSONDecoder().decode(T.self, from: json)
        check(back == value, "round trip of \(text(json))", line: line)
    } catch let error as EncodingError {
        check(false, "round trip: \(error)", line: line)
    } catch let error as DecodingError {
        check(false, "round trip: \(error)", line: line)
    } catch {
        check(false, "round trip: unexpected error", line: line)
    }
}

// MARK: Synthesized conformances

enum Planet: String, Codable { case mercury, venus }
enum Level: Int, Codable { case low = 1, high = 9 }
enum Byte: UInt8, Codable { case zero, one }
enum Ratio: Double, Codable { case half = 0.5, whole = 1 }
enum Region: String, Codable, CodingKeyRepresentable, Hashable { case north, south }

enum Event: Codable, Equatable {
    case started
    case moved(x: Int, y: Int)
    case named(String, note: String?)
}

struct Inner: Codable, Equatable {
    var name: String
    var tags: [String]
}

struct Numbers: Codable, Equatable {
    var int: Int
    var int8: Int8
    var int16: Int16
    var int32: Int32
    var int64: Int64
    var uint: UInt
    var uint8: UInt8
    var uint16: UInt16
    var uint32: UInt32
    var uint64: UInt64
    var double: Double
    var float: Float
    var wide: Int128
    var unsignedWide: UInt128
}

struct Everything: Codable, Equatable {
    var flag: Bool
    var title: String
    var note: String?
    var absent: Int?
    var inner: Inner
    var inners: [Inner]
    var set: Set<Int>
    var contiguous: ContiguousArray<UInt8>
    var byName: [String: Int]
    var byNumber: [Int: String]
    var byRegion: [Region: Bool]
    var byFlag: [Bool: String]
    var planet: Planet
    var level: Level
    var byte: Byte
    var ratio: Ratio
    var events: [Event]
    var range: Range<Int>
    var closed: ClosedRange<Double>
    var duration: Duration
    var vector: SIMD4<Float>
    var nested: [[Int?]]
}

struct Renamed: Codable, Equatable {
    var fullName: String
    var age: Int
    var cached = 0

    enum CodingKeys: String, CodingKey {
        case fullName = "full_name"
        case age
    }
}

struct Wide: Codable, Equatable {
    var a = 1, b = 2, c = 3, d = 4, e = 5, f = 6, g = 7, h = 8, i = 9, j = 10
}

func testSynthesized() {
    let numbers = Numbers(
        int: -1, int8: -128, int16: -32768, int32: -2_147_483_648, int64: .min, uint: 1,
        uint8: 255, uint16: 65535, uint32: 4_294_967_295, uint64: .max, double: -2.5e-300,
        float: 0.1, wide: -9_000_000_000, unsignedWide: 18_000_000_000)
    roundTrips(numbers)
    check(
        encoded(numbers)
            == #"{"int":-1,"int8":-128,"int16":-32768,"int32":-2147483648,"#
            + #""int64":-9223372036854775808,"uint":1,"uint8":255,"uint16":65535,"#
            + #""uint32":4294967295,"uint64":18446744073709551615,"double":-2.5e-300,"#
            + #""float":0.1,"wide":-9000000000,"unsignedWide":18000000000}"#,
        encoded(numbers))

    let everything = Everything(
        flag: true, title: "a \"quoted\"\nline", note: "here", absent: nil,
        inner: Inner(name: "in", tags: ["a", "b"]),
        inners: [Inner(name: "x", tags: []), Inner(name: "y", tags: ["z"])],
        set: [3, 1, 2], contiguous: [7, 8], byName: ["one": 1, "two": 2],
        byNumber: [1: "one", 20: "twenty"], byRegion: [.north: true, .south: false],
        byFlag: [true: "yes"], planet: .venus, level: .high, byte: .one, ratio: .half,
        events: [.started, .moved(x: 1, y: -2), .named("n", note: nil), .named("m", note: "o")],
        range: 1..<5, closed: 0.5...2,
        duration: .milliseconds(1500), vector: SIMD4(1, 2.5, -3, 0), nested: [[1, nil], []])
    roundTrips(everything)

    check(encoded(Inner(name: "n", tags: [])) == #"{"name":"n","tags":[]}"#, "declaration order")
    check(encoded([Planet.mercury]) == #"["mercury"]"#, "string raw value")
    check(encoded(Level.high) == "9", "int raw value")
    check(encoded(Ratio.whole) == "1", "whole double without .0")
    check(encoded(Event.started) == #"{"started":{}}"#, encoded(Event.started))
    check(encoded(Event.moved(x: 1, y: 2)) == #"{"moved":{"x":1,"y":2}}"#, "payload labels")
    check(
        encoded(Event.named("n", note: nil)) == #"{"named":{"_0":"n"}}"#,
        encoded(Event.named("n", note: nil)))
    check(encoded([2: "b"]) == #"{"2":"b"}"#, "Int keys")
    check(encoded([Region.north: 1]) == #"{"north":1}"#, "CodingKeyRepresentable keys")
    check(encoded([true: 1]) == "[true,1]", "other keys alternate with values")
    check(encoded([String: Int]()) == "{}", "empty dictionary with String keys")
    check(encoded([Bool: Int]()) == "[]", "empty dictionary with other keys")
    check(encoded(3..<7) == "[3,7]", "range")
    check(encoded(3...) == "[3]" && decoded(PartialRangeFrom<Int>.self, "[3]")?.lowerBound == 3,
        "partial range from")
    check(encoded(..<4) == "[4]" && decoded(PartialRangeUpTo<Int>.self, "[4]")?.upperBound == 4,
        "partial range up to")
    check(encoded(...6) == "[6]" && decoded(PartialRangeThrough<Int>.self, "[6]")?.upperBound == 6,
        "partial range through")
    check(encoded(Duration.seconds(2)) == "[0,2000000000000000000]", encoded(Duration.seconds(2)))
    check(encoded(SIMD2<Int32>(1, 2)) == "[1,2]", "SIMD")
    check(encoded(Optional<Int>.none) == "null", "nil at the top")
    check(encoded("\u{1}é") == "\"\\u0001é\"", "control characters escaped, UTF-8 kept")

    roundTrips(Renamed(fullName: "Ada", age: 36))
    check(encoded(Renamed(fullName: "Ada", age: 36)) == #"{"full_name":"Ada","age":36}"#, "renamed")
    check(
        decoded(Renamed.self, #"{"age":1,"full_name":"B","cached":5}"#)
            == Renamed(fullName: "B", age: 1), "unknown keys ignored, defaults kept")

    roundTrips(Wide())
    check(
        decoded(Wide.self, #"{"j":0,"i":0,"h":0,"g":0,"f":0,"e":0,"d":0,"c":0,"b":0,"a":-1}"#)?.a
            == -1, "keys found in a large object")
    check(
        encoded(Wide(), [.sortedKeys])
            == #"{"a":1,"b":2,"c":3,"d":4,"e":5,"f":6,"g":7,"h":8,"i":9,"j":10}"#,
        "sorted keys")
    check(
        encoded(Inner(name: "n", tags: ["t"]), [.prettyPrinted])
            == "{\n  \"name\": \"n\",\n  \"tags\": [\n    \"t\"\n  ]\n}", "pretty printed")
    check(decoded(Inner.self, " { \"tags\" : [ ] , \"name\" : \"x\" } ")?.name == "x", "whitespace")
    check(decoded(Int.self, "1e3") == 1000, "integer written with an exponent")
    check(decoded(Int.self, "2.0") == 2, "integer written with a fraction")
    check(decoded(Float.self, "0.1") == 0.1, "float parsed as a float")
    check(decoded([Int?].self, "[1,null,3]") == [1, nil, 3], "nulls in an array")
    check(decoded(Planet?.self, "null") == .some(nil), "null optional")
}

// MARK: Hand-written conformances

class Animal: Codable {
    var legs: Int

    init(legs: Int) {
        self.legs = legs
    }
}

final class Bird: Animal {
    var canFly: Bool

    enum CodingKeys: CodingKey {
        case canFly
    }

    init(canFly: Bool) {
        self.canFly = canFly
        super.init(legs: 2)
    }

    @diagnose(EmbeddedRestrictions, as: ignored)
    required init(from decoder: any Decoder) throws {
        let container = try decoder.container(keyedBy: CodingKeys.self)
        canFly = try container.decode(Bool.self, forKey: .canFly)
        try super.init(from: container.superDecoder())
    }

    @diagnose(EmbeddedRestrictions, as: ignored)
    override func encode(to encoder: any Encoder) throws {
        var container = encoder.container(keyedBy: CodingKeys.self)
        try container.encode(canFly, forKey: .canFly)
        try super.encode(to: container.superEncoder())
    }
}

struct Name: CodingKey {
    var stringValue: String
    var intValue: Int? { nil }

    init(_ name: String) {
        stringValue = name
    }

    init(stringValue: String) {
        self.stringValue = stringValue
    }

    init?(intValue: Int) {
        return nil
    }
}

// Exercises the containers' other operations and records the coding paths it sees.
struct Layout: Codable, Equatable {
    var points: [[Int]]
    var labels: [String: String]
    var paths: [String]

    static let userInfoKey = CodingUserInfoKey(rawValue: "layout")!

    init(points: [[Int]], labels: [String: String]) {
        self.points = points
        self.labels = labels
        paths = []
    }

    @diagnose(EmbeddedRestrictions, as: ignored)
    func encode(to encoder: any Encoder) throws {
        var root = encoder.container(keyedBy: Name.self)
        var points = root.nestedUnkeyedContainer(forKey: Name("points"))
        for point in self.points {
            var pair = points.nestedUnkeyedContainer()
            try pair.encode(contentsOf: point)
        }
        var labels = root.nestedContainer(keyedBy: Name.self, forKey: Name("labels"))
        for (key, value) in self.labels.sorted(by: { $0.key < $1.key }) {
            try labels.encode(value, forKey: Name(key))
        }
        var nothing = points.nestedContainer(keyedBy: Name.self)
        try nothing.encodeNil(forKey: Name("none"))
        try root.encode(points.count, forKey: Name("count"))
        try root.encode("first", forKey: Name("twice"))
        try root.encode("second", forKey: Name("twice"))
        let info = encoder.userInfo[Self.userInfoKey] as? String ?? "none"
        try root.encode(info, forKey: Name("info"))
        let path = labels.codingPath.map { $0.stringValue }.joined(separator: "/")
        try root.encode(path, forKey: Name("path"))
    }

    @diagnose(EmbeddedRestrictions, as: ignored)
    init(from decoder: any Decoder) throws {
        let root = try decoder.container(keyedBy: Name.self)
        var paths: [String] = []
        var points = try root.nestedUnkeyedContainer(forKey: Name("points"))
        var decodedPoints: [[Int]] = []
        while !points.isAtEnd {
            if try points.decodeNil() {
                continue
            }
            // The keyed element is no array; it stays current to be asked for as an object.
            guard var pair = try? points.nestedUnkeyedContainer() else {
                let object = try points.nestedContainer(keyedBy: Name.self)
                paths.append(object.codingPath.map { $0.stringValue }.joined(separator: "/"))
                paths.append(String(try object.decodeNil(forKey: Name("none"))))
                continue
            }
            var point: [Int] = []
            while let value = try pair.decodeIfPresent(Int.self) {
                point.append(value)
            }
            decodedPoints.append(point)
        }
        let labelContainer = try root.nestedContainer(keyedBy: Name.self, forKey: Name("labels"))
        var labels: [String: String] = [:]
        for key in labelContainer.allKeys {
            labels[key.stringValue] = try labelContainer.decode(String.self, forKey: key)
        }
        paths.append(String(try root.decode(Int.self, forKey: Name("count"))))
        paths.append(try root.decode(String.self, forKey: Name("twice")))
        paths.append(try root.decode(String.self, forKey: Name("info")))
        paths.append(try root.decode(String.self, forKey: Name("path")))
        paths.append(String(root.contains(Name("absent"))))
        self.points = decodedPoints
        self.labels = labels
        self.paths = paths
    }
}

struct Nothing: Encodable {
    func encode(to encoder: any Encoder) throws {}
}

func testHandWritten() {
    check(encoded(Bird(canFly: true)) == #"{"canFly":true,"super":{"legs":2}}"#, "superEncoder")
    let bird = decoded(Bird.self, #"{"super":{"legs":3},"canFly":false}"#)
    check(bird?.legs == 3 && bird?.canFly == false, "superDecoder")

    var encoder = JSONEncoder()
    encoder.userInfo[Layout.userInfoKey] = "given"
    let layout = Layout(points: [[1, 2], [3]], labels: ["b": "B", "a": "A"])
    var json = ""
    do {
        json = text(try encoder.encode(layout))
    } catch {
        check(false, "Layout did not encode")
    }
    check(
        json == #"{"points":[[1,2],[3],{"none":null}],"labels":{"a":"A","b":"B"},"count":3,"#
            + #""twice":"second","info":"given","path":"labels"}"#, json)
    let back = decoded(Layout.self, json)
    check(back?.points == [[1, 2], [3]], "nested unkeyed containers")
    check(back?.labels == ["a": "A", "b": "B"], "nested keyed container")
    check(
        back?.paths == ["points/Index 2", "true", "3", "second", "given", "labels", "false"],
        (back?.paths ?? []).joined(separator: ", "))
    check(
        encoded(Nothing()) == "error: EncodingError.invalidValue. Debug description: Top-level "
            + "value did not encode any values.", encoded(Nothing()))
}

// MARK: Errors

struct Pair: Codable {
    var first: Int
    var second: Int

    init(from decoder: any Decoder) throws {
        var container = try decoder.unkeyedContainer()
        first = try container.decode(Int.self)
        second = try container.decode(Int.self)
    }

    func encode(to encoder: any Encoder) throws {
        var container = encoder.unkeyedContainer()
        try container.encode(first)
        try container.encode(second)
    }
}

struct Holder: Codable {
    var inner: Inner
    var level: Level?
}

func testErrors() {
    check(
        decodingError(Holder.self, #"{"inner":{"tags":[]}}"#)
            == "DecodingError.keyNotFound: Key 'name' not found in keyed decoding container. "
            + "Path: inner. Debug description: No value associated with key \"name\".",
        decodingError(Holder.self, #"{"inner":{"tags":[]}}"#))
    check(
        decodingError(Holder.self, #"{"inner":{"name":"n","tags":["a",2]}}"#)
            == "DecodingError.typeMismatch. Path: inner.tags[1]. Debug description: Expected a "
            + "string but found a number instead.",
        decodingError(Holder.self, #"{"inner":{"name":"n","tags":["a",2]}}"#))
    check(
        decodingError(Holder.self, #"{"inner":null}"#)
            == "DecodingError.valueNotFound. Path: inner. Debug description: Expected an object "
            + "but found null instead.", decodingError(Holder.self, #"{"inner":null}"#))
    // The library writes this message itself, and in full Swift the host's release words it.
    let badLevel = #"{"inner":{"name":"n","tags":[]},"level":5}"#
    #if $Embedded
        check(
            decodingError(Holder.self, badLevel)
                == "DecodingError.dataCorrupted: Data was corrupted. Path: level. Debug "
                + "description: Cannot initialize from invalid Int value 5",
            decodingError(Holder.self, badLevel))
    #else
        check(decoded(Holder.self, badLevel) == nil, "decoding fails")
    #endif
    check(
        decodingError([Int8].self, "[1,300]")
            == "DecodingError.dataCorrupted: Data was corrupted. Path: [1]. Debug description: "
            + "Parsed number <300> does not fit in Int8.", decodingError([Int8].self, "[1,300]"))
    check(
        decodingError([UInt].self, "[-1]")
            == "DecodingError.dataCorrupted: Data was corrupted. Path: [0]. Debug description: "
            + "Parsed JSON number <-1> does not fit in UInt64.", decodingError([UInt].self, "[-1]"))
    check(
        decodingError(Double.self, "1e999")
            == "DecodingError.dataCorrupted: Data was corrupted. Debug description: Parsed JSON "
            + "number <1e999> does not fit in Double.", decodingError(Double.self, "1e999"))
    check(
        decodingError(Pair.self, "[1]")
            == "DecodingError.valueNotFound. Path: [1]. Debug description: Unkeyed container is "
            + "at end.", decodingError(Pair.self, "[1]"))
    // The library writes this message itself, and in full Swift the host's release words it.
    #if $Embedded
        check(
            decodingError(Range<Int>.self, "[5,1]")
                == "DecodingError.dataCorrupted: Data was corrupted. Debug description: Cannot "
                + "initialize a range with a lowerBound greater than upperBound",
            decodingError(Range<Int>.self, "[5,1]"))
    #else
        check(decoded(Range<Int>.self, "[5,1]") == nil, "decoding fails")
    #endif
    check(
        decodingError(SIMD2<Int>.self, "[1,2,3]")
            == "DecodingError.dataCorrupted: Data was corrupted. Debug description: Expected "
            + "vector with exactly 2 elements.", decodingError(SIMD2<Int>.self, "[1,2,3]"))
    check(
        decodingError([Bool: Int].self, "[true]")
            == "DecodingError.dataCorrupted: Data was corrupted. Debug description: Expected "
            + "collection of key-value pairs; encountered odd-length array instead.",
        decodingError([Bool: Int].self, "[true]"))
    // The library writes this message itself, and in full Swift the host's release words it.
    #if $Embedded
        check(
            decodingError([Int: Int].self, #"{"x":1}"#)
                == "DecodingError.dataCorrupted: Data was corrupted. Path: x. Debug description: "
                + "Could not convert key to the dictionary's key type",
            decodingError([Int: Int].self, #"{"x":1}"#))
    #else
        check(decoded([Int: Int].self, #"{"x":1}"#) == nil, "decoding fails")
    #endif
    check(
        decodingError([String: Int].self, #"{"a.b":true}"#)
            == "DecodingError.typeMismatch. Path: `a.b`. Debug description: Expected a number "
            + "but found a boolean instead.", decodingError([String: Int].self, #"{"a.b":true}"#))
    check(
        decodingError(Int.self, "[1")
            == "DecodingError.dataCorrupted: Data was corrupted. Debug description: The given "
            + "data was not valid JSON: line 1, column 3: expected ',' or ']' after an element, "
            + "found the end of the input.",
        decodingError(Int.self, "[1"))
    check(
        decodingError(Int.self, "1 2")
            == "DecodingError.dataCorrupted: Data was corrupted. Debug description: The given "
            + "data was not valid JSON: more than one value.", decodingError(Int.self, "1 2"))
    check(decoded(Bool.self, "") == nil, "no value")

    check(
        encoded([1, Double.nan])
            == "error: EncodingError.invalidValue. Path: [1]. Debug description: Unable to "
            + "encode a non-finite number in JSON.", encoded([1, Double.nan]))
    check(
        encoded(["f": Float.infinity])
            == "error: EncodingError.invalidValue. Path: f. Debug description: Unable to encode "
            + "a non-finite number in JSON.", encoded(["f": Float.infinity]))
    check(
        encoded(Int128.max)
            == "error: EncodingError.invalidValue. Debug description: Integer does not fit in 64 "
            + "bits.", encoded(Int128.max))
    check(decoded(UInt128.self, "18446744073709551615") == UInt128(UInt64.max), "wide decode")

    // Full Swift's description starts with the key's type.
    let key = Name("k")
    #if $Embedded
        let keyDescription = #"CodingKey(stringValue: "k", intValue: nil)"#
    #else
        let keyDescription = #"Name(stringValue: "k", intValue: nil)"#
    #endif
    check(key.description == keyDescription, key.description)
}

@main
struct CodableTests {
    static func main() {
        testSynthesized()
        testHandWritten()
        testErrors()
        print("\(checks) Codable checks, \(failures) failed")
        exit(failures == 0 ? 0 : 1)
    }
}
