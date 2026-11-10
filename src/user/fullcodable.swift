// Codable in full Swift: the standard library's Encoder and Decoder protocols and the conformances
// the compiler synthesizes, over FeatherSupport's JSONEncoder and JSONDecoder.
import FeatherSupport

func text(_ bytes: [UInt8]) -> String {
    String(decoding: bytes, as: UTF8.self)
}

// The types Embedded Swift's hello checks, which must encode to the same text.
enum Phase: String, Codable {
    case planned, done
}

enum Step: Codable, Equatable {
    case build(arch: String)
    case boot(seconds: Double?)
}

struct Milestone: Codable, Equatable {
    var number: Int
    var phase: Phase
    var steps: [Step]
    var note: String?
    var counts: [String: UInt8]
}

let milestone = Milestone(
    number: 25, phase: .done, steps: [.build(arch: "aarch64"), .boot(seconds: 1.5)], note: nil,
    counts: ["parts": 8])
do {
    let json = try JSONEncoder().encode(milestone)
    let decoded = try JSONDecoder().decode(Milestone.self, from: json)
    print("fullcodable: encoded \(text(json)), alike \(decoded == milestone)")
} catch {
    print("fullcodable: \(error)")
}

// Errors carry the coding path and, in full Swift, the type expected.
func report<T: Decodable>(_ type: T.Type, from source: String) {
    do {
        let value = try JSONDecoder().decode(type, from: Array(source.utf8))
        print("fullcodable: decoded \(value)")
    } catch {
        print("fullcodable: \(error)")
    }
}

report(Milestone.self, from: #"{"number":25,"phase":"late"}"#)
report(Milestone.self, from: #"{"phase":"done"}"#)
report(
    Milestone.self,
    from: #"{"number":1,"phase":"done","steps":[{"boot":{"seconds":"slow"}}],"counts":{}}"#)
report(Milestone.self, from: #"{"number":null,"phase":"done","steps":[],"counts":{}}"#)
report([Int8].self, from: "[1,300]")
report(Step.self, from: #"{"boot":{}}"#)

// Dictionary keys through CodingKeyRepresentable, which Dictionary finds with a cast.
enum Architecture: String, Codable, CodingKeyRepresentable {
    case x86_64, aarch64
}

let sorted = {
    var encoder = JSONEncoder()
    encoder.outputFormatting = .sortedKeys
    return encoder
}()
do {
    let byArchitecture: [Architecture: Int] = [.x86_64: 64, .aarch64: 8]
    let json = try sorted.encode(byArchitecture)
    let decoded = try JSONDecoder().decode([Architecture: Int].self, from: json)
    let byNumber = try sorted.encode([2: "two", 1: "one"])
    let byDouble = try sorted.encode([0.5: true])
    print(
        "fullcodable: keys \(text(json)) alike \(decoded == byArchitecture) \(text(byNumber)) "
            + text(byDouble))
} catch {
    print("fullcodable: \(error)")
}

// Values of any Encodable type, opened from the existential to call the generic encode.
struct Mixed: Encodable {
    let items: [any Encodable]

    func encode(to encoder: any Encoder) throws {
        var container = encoder.unkeyedContainer()
        for item in items {
            try container.encode(item)
        }
    }
}

struct Opaque {}

let values: [Any] = [milestone.phase, 7, "seven", Opaque(), Step.build(arch: "x86_64"), 0.5]
let encodable = values.compactMap { $0 as? any Encodable }
do {
    print(
        "fullcodable: existentials \(encodable.count) of \(values.count) "
            + text(try JSONEncoder().encode(Mixed(items: encodable))))
} catch {
    print("fullcodable: \(error)")
}

// A subclass that keeps its superclass's values apart, through superEncoder and superDecoder.
class Animal: Codable {
    var name: String

    init(name: String) {
        self.name = name
    }
}

final class Dog: Animal {
    var tricks: [String]

    private enum CodingKeys: String, CodingKey {
        case tricks
    }

    init(name: String, tricks: [String]) {
        self.tricks = tricks
        super.init(name: name)
    }

    required init(from decoder: any Decoder) throws {
        let container = try decoder.container(keyedBy: CodingKeys.self)
        tricks = try container.decode([String].self, forKey: .tricks)
        try super.init(from: container.superDecoder())
    }

    override func encode(to encoder: any Encoder) throws {
        var container = encoder.container(keyedBy: CodingKeys.self)
        try container.encode(tricks, forKey: .tricks)
        try super.encode(to: container.superEncoder())
    }
}

do {
    let json = try sorted.encode(Dog(name: "Rex", tricks: ["sit", "roll"]))
    let dog = try JSONDecoder().decode(Dog.self, from: json)
    print("fullcodable: subclass \(text(json)) \(dog.name) \(dog.tricks)")
} catch {
    print("fullcodable: \(error)")
}

// A generic type, pretty-printed with sorted keys, and integers wider than JSON's 64 bits.
struct Pair<Value: Codable & Equatable>: Codable, Equatable {
    var first: Value
    var second: Value
}

do {
    var pretty = JSONEncoder()
    pretty.outputFormatting = [.prettyPrinted, .sortedKeys]
    let pair = Pair(first: Int128(-5), second: Int128(Int64.max))
    let json = try pretty.encode(pair)
    print(text(json))
    let back = try JSONDecoder().decode(Pair<Int128>.self, from: json)
    print("fullcodable: pair alike \(back == pair)")
    _ = try JSONEncoder().encode(Pair(first: UInt128(1) << 64, second: 0))
} catch {
    print("fullcodable: \(error)")
}
