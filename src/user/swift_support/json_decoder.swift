// Foundation's JSONDecoder for FeatherOS's Codable, over the json program's parser. The input is
// one JSON value, as bytes or already parsed.
public struct JSONDecoder {
    public var userInfo: [CodingUserInfoKey: Any] = [:]

    public init() {}

    public func decode<T: Decodable>(_ type: T.Type, from bytes: [UInt8]) throws -> T {
        try decode(type, from: try Self.parse(bytes))
    }

    public func decode<T: Decodable>(_ type: T.Type, from value: JSONValue) throws -> T {
        try T(from: JSONValueDecoder(value: value, codingPath: [], userInfo: userInfo))
    }

    private static func parse(_ bytes: [UInt8]) throws -> JSONValue {
        var parser = JSONParser(bytes)
        let problem: String
        do throws(JSONParseError) {
            if let value = try parser.next() {
                if try parser.next() == nil {
                    return value
                }
                problem = "more than one value"
            } else {
                problem = "no value"
            }
        } catch {
            problem = error.description
        }
        throw DecodingError.dataCorrupted(
            DecodingError.Context(
                codingPath: [], debugDescription: "The given data was not valid JSON: \(problem)."))
    }
}

final class JSONValueDecoder: DecoderBase {
    private static let searchLimit = 8
    private let value: JSONValue
    private let path: [any CodingKey]
    private let info: [CodingUserInfoKey: Any]
    private var index = 0
    private var positions: [[UInt8]: Int]?

    init(value: JSONValue, codingPath: [any CodingKey], userInfo: [CodingUserInfoKey: Any]) {
        self.value = value
        self.path = codingPath
        self.info = userInfo
    }

    override var codingPath: [any CodingKey] { path }

    override var userInfo: [CodingUserInfoKey: Any] { info }

    override func beginKeyedContainer() throws {
        if case .object(let members) = value {
            if positions == nil, members.count > Self.searchLimit {
                var positions: [[UInt8]: Int] = [:]
                for (index, member) in members.enumerated() {
                    positions[member.key] = index
                }
                self.positions = positions
            }
            return
        }
        throw mismatch(value, expected: "an object", type: [String: Any].self, at: .value)
    }

    override func beginUnkeyedContainer() throws {
        guard case .array = value else {
            throw mismatch(value, expected: "an array", type: [Any].self, at: .value)
        }
        index = 0
    }

    override var allKeys: [String] {
        guard case .object(let members) = value else {
            return []
        }
        return members.map { String(decoding: $0.key, as: UTF8.self) }
    }

    override func contains(_ key: any CodingKey) -> Bool {
        member(key) != nil
    }

    override var elementCount: Int? {
        if case .array(let elements) = value {
            return elements.count
        }
        return nil
    }

    override var currentIndex: Int { index }

    override func moveToNextElement() {
        index += 1
    }

    override func decodeNil(at slot: CodingSlot) throws -> Bool {
        try lookUp(slot, type: Any?.self) == .null
    }

    override func decode(_ type: Bool.Type, at slot: CodingSlot) throws -> Bool {
        let found = try lookUp(slot, type: type)
        guard case .bool(let truth) = found else {
            throw mismatch(found, expected: "a boolean", type: type, at: slot)
        }
        return truth
    }

    override func decode(_ type: String.Type, at slot: CodingSlot) throws -> String {
        let found = try lookUp(slot, type: type)
        guard case .string(let text) = found else {
            throw mismatch(found, expected: "a string", type: type, at: slot)
        }
        return String(decoding: text, as: UTF8.self)
    }

    override func decode(_ type: Double.Type, at slot: CodingSlot) throws -> Double {
        let text = try number(at: slot, type: type)
        guard let number = Double(String(decoding: text, as: UTF8.self)), number.isFinite else {
            throw doesNotFit(text, "Double", at: slot)
        }
        return number
    }

    override func decode(_ type: Float.Type, at slot: CodingSlot) throws -> Float {
        let text = try number(at: slot, type: type)
        guard let number = Float(String(decoding: text, as: UTF8.self)), number.isFinite else {
            throw doesNotFit(text, "Float", at: slot)
        }
        return number
    }

    override func decode(_ type: Int64.Type, at slot: CodingSlot) throws -> Int64 {
        let text = try number(at: slot, type: type)
        guard let number = Self.integer(text, as: Int64.self) else {
            throw doesNotFit(text, "Int64", at: slot)
        }
        return number
    }

    override func decode(_ type: UInt64.Type, at slot: CodingSlot) throws -> UInt64 {
        let text = try number(at: slot, type: type)
        guard let number = Self.integer(text, as: UInt64.self) else {
            throw doesNotFit(text, "UInt64", at: slot)
        }
        return number
    }

    override func nestedDecoder(at slot: CodingSlot) throws -> DecoderBase {
        JSONValueDecoder(
            value: try lookUp(slot, type: Any.self), codingPath: path + keys(for: slot),
            userInfo: info)
    }

    private func lookUp(_ slot: CodingSlot, type: Any.Type) throws -> JSONValue {
        switch slot {
        case .value:
            return value
        case .key(let key):
            guard let found = member(key) else {
                throw DecodingError.keyNotFound(
                    key,
                    DecodingError.Context(
                        codingPath: path,
                        debugDescription: "No value associated with key \"\(key.stringValue)\"."))
            }
            return found
        case .element:
            guard case .array(let elements) = value, index < elements.count else {
                throw DecodingError.valueNotFound(
                    type,
                    DecodingError.Context(
                        codingPath: path + [JSONIndexKey(index)],
                        debugDescription: "Unkeyed container is at end."))
            }
            return elements[index]
        }
    }

    private func member(_ key: any CodingKey) -> JSONValue? {
        guard case .object(let members) = value else {
            return nil
        }
        let name = Array(key.stringValue.utf8)
        if let positions {
            return positions[name].map { members[$0].value }
        }
        return members.first { $0.key == name }?.value
    }

    private func number(at slot: CodingSlot, type: Any.Type) throws -> [UInt8] {
        let found = try lookUp(slot, type: type)
        guard case .number(let text) = found else {
            throw mismatch(found, expected: "a number", type: type, at: slot)
        }
        return text
    }

    private func keys(for slot: CodingSlot) -> [any CodingKey] {
        switch slot {
        case .value: []
        case .key(let key): [key]
        case .element: [JSONIndexKey(index)]
        }
    }

    private func mismatch(_ found: JSONValue, expected: String, type: Any.Type, at slot: CodingSlot)
        -> DecodingError
    {
        let context = DecodingError.Context(
            codingPath: path + keys(for: slot),
            debugDescription: "Expected \(expected) but found \(found.kindName) instead.")
        if found == .null {
            return .valueNotFound(type, context)
        }
        return .typeMismatch(type, context)
    }

    private func doesNotFit(_ text: [UInt8], _ typeName: StaticString, at slot: CodingSlot)
        -> DecodingError
    {
        let number = String(decoding: text, as: UTF8.self)
        return .dataCorrupted(
            DecodingError.Context(
                codingPath: path + keys(for: slot),
                debugDescription: "Parsed JSON number <\(number)> does not fit in \(typeName)."))
    }

    // Digits only, as most integers are written; otherwise a number such as 1e3 or 2.0 that is
    // still a whole number in range.
    private static func integer<T: FixedWidthInteger>(_ text: [UInt8], as type: T.Type) -> T? {
        let negative = text.first == UInt8(ascii: "-")
        var result: T = 0
        var isPlain = true
        for byte in text.dropFirst(negative ? 1 : 0) {
            guard byte >= UInt8(ascii: "0"), byte <= UInt8(ascii: "9") else {
                isPlain = false
                break
            }
            let digit = T(byte - UInt8(ascii: "0"))
            let (shifted, overflowA) = result.multipliedReportingOverflow(by: 10)
            let (next, overflowB) =
                negative
                ? shifted.subtractingReportingOverflow(digit)
                : shifted.addingReportingOverflow(digit)
            if overflowA || overflowB {
                return nil
            }
            result = next
        }
        if isPlain {
            return result
        }
        guard let number = Double(String(decoding: text, as: UTF8.self)) else {
            return nil
        }
        return T(exactly: number)
    }
}
