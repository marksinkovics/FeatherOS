// Strings and keys hold UTF-8 bytes rather than Strings, so the program never needs the Unicode
// data tables. Numbers keep their text, so they print as they were written.
public indirect enum JSONValue: Equatable {
    case null
    case bool(Bool)
    case number([UInt8])
    case string([UInt8])
    case array([JSONValue])
    case object([JSONMember])

    public var kindName: String {
        switch self {
        case .null: "null"
        case .bool: "a boolean"
        case .number: "a number"
        case .string: "a string"
        case .array: "an array"
        case .object: "an object"
        }
    }

    public func member(_ key: [UInt8]) -> JSONValue? {
        guard case .object(let members) = self else {
            return nil
        }
        return members.first { $0.key == key }?.value
    }
}

public struct JSONMember: Equatable {
    public var key: [UInt8]
    public var value: JSONValue

    public init(key: [UInt8], value: JSONValue) {
        self.key = key
        self.value = value
    }
}

// A repeated key keeps its first place and its last value, as in jq. Small objects are searched
// in order; larger ones through a dictionary, so parsing stays linear.
struct JSONMemberCollector {
    private static let searchLimit = 8
    private(set) var members: [JSONMember] = []
    private var positions: [[UInt8]: Int]?

    mutating func add(key: [UInt8], value: JSONValue) {
        if let index = position(of: key) {
            members[index].value = value
            return
        }
        positions?[key] = members.count
        members.append(JSONMember(key: key, value: value))
        if positions == nil, members.count > Self.searchLimit {
            var positions: [[UInt8]: Int] = [:]
            for (index, member) in members.enumerated() {
                positions[member.key] = index
            }
            self.positions = positions
        }
    }

    private func position(of key: [UInt8]) -> Int? {
        if let positions {
            return positions[key]
        }
        return members.firstIndex { $0.key == key }
    }
}
