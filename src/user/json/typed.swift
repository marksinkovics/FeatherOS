// json -t: a value decoded with JSONDecoder into the Swift types a type expression names, such as
// [uint8] or {double?}, then encoded again with JSONEncoder. The expression is only known while
// the program runs, so containers decode their elements through superDecoder.
#if canImport(FeatherSupport)
    import FeatherSupport
#endif

indirect enum TypeExpression: Equatable {
    case bool
    case string
    case double
    case float
    case int, int8, int16, int32, int64
    case uint, uint8, uint16, uint32, uint64
    case array(TypeExpression)
    case dictionary(TypeExpression)
    case optional(TypeExpression)

    // A name, [T] for an array, {T} for an object with string keys, and T? for an optional.
    init?(parsing text: String) {
        var rest = Substring(text)
        guard let expression = Self.parse(&rest), rest.isEmpty else {
            return nil
        }
        self = expression
    }

    private static func parse(_ text: inout Substring) -> TypeExpression? {
        var expression: TypeExpression
        if text.first == "[" || text.first == "{" {
            let isArray = text.removeFirst() == "["
            guard let element = parse(&text), text.first == (isArray ? "]" : "}") else {
                return nil
            }
            text.removeFirst()
            expression = isArray ? .array(element) : .dictionary(element)
        } else {
            let name = text.prefix { $0 != "?" && $0 != "]" && $0 != "}" }
            guard let found = named(name) else {
                return nil
            }
            text.removeFirst(name.count)
            expression = found
        }
        while text.first == "?" {
            text.removeFirst()
            expression = .optional(expression)
        }
        return expression
    }

    private static func named(_ name: Substring) -> TypeExpression? {
        switch name {
        case "bool": .bool
        case "string": .string
        case "double": .double
        case "float": .float
        case "int": .int
        case "int8": .int8
        case "int16": .int16
        case "int32": .int32
        case "int64": .int64
        case "uint": .uint
        case "uint8": .uint8
        case "uint16": .uint16
        case "uint32": .uint32
        case "uint64": .uint64
        default: nil
        }
    }
}

// The decoded value. Integers are checked against their own type, then kept wide.
indirect enum TypedValue {
    case null
    case bool(Bool)
    case string(String)
    case double(Double)
    case float(Float)
    case signed(Int64)
    case unsigned(UInt64)
    case array([TypedValue])
    case dictionary([(String, TypedValue)])

    init(decoding expression: TypeExpression, from decoder: any Decoder) throws {
        switch expression {
        case .array(let element):
            var container = try decoder.unkeyedContainer()
            var elements: [TypedValue] = []
            while !container.isAtEnd {
                elements.append(try TypedValue(decoding: element, from: container.superDecoder()))
            }
            self = .array(elements)
        case .dictionary(let element):
            self = .dictionary(try Self.decodeMembers(element, from: decoder))
        case .optional(let wrapped):
            if try decoder.singleValueContainer().decodeNil() {
                self = .null
            } else {
                self = try TypedValue(decoding: wrapped, from: decoder)
            }
        default:
            self = try Self.decodePrimitive(expression, from: decoder.singleValueContainer())
        }
    }

    // Embedded Swift warns about container(keyedBy:) on any Decoder, although the call works.
    @diagnose(EmbeddedRestrictions, as: ignored)
    private static func decodeMembers(_ element: TypeExpression, from decoder: any Decoder) throws
        -> [(String, TypedValue)]
    {
        let container = try decoder.container(keyedBy: MemberName.self)
        return try container.allKeys.map { key in
            let decoder = try container.superDecoder(forKey: key)
            return (key.stringValue, try TypedValue(decoding: element, from: decoder))
        }
    }

    private static func decodePrimitive(
        _ expression: TypeExpression, from container: SingleValueDecodingContainer
    ) throws -> TypedValue {
        switch expression {
        case .bool: .bool(try container.decode(Bool.self))
        case .string: .string(try container.decode(String.self))
        case .double: .double(try container.decode(Double.self))
        case .float: .float(try container.decode(Float.self))
        case .int: .signed(Int64(try container.decode(Int.self)))
        case .int8: .signed(Int64(try container.decode(Int8.self)))
        case .int16: .signed(Int64(try container.decode(Int16.self)))
        case .int32: .signed(Int64(try container.decode(Int32.self)))
        case .int64: .signed(try container.decode(Int64.self))
        case .uint: .unsigned(UInt64(try container.decode(UInt.self)))
        case .uint8: .unsigned(UInt64(try container.decode(UInt8.self)))
        case .uint16: .unsigned(UInt64(try container.decode(UInt16.self)))
        case .uint32: .unsigned(UInt64(try container.decode(UInt32.self)))
        case .uint64: .unsigned(try container.decode(UInt64.self))
        case .array, .dictionary, .optional: preconditionFailure("not a primitive")
        }
    }
}

extension TypedValue: Encodable {
    @diagnose(EmbeddedRestrictions, as: ignored)
    func encode(to encoder: any Encoder) throws {
        switch self {
        case .array(let elements):
            var container = encoder.unkeyedContainer()
            try container.encode(contentsOf: elements)
        case .dictionary(let members):
            var container = encoder.container(keyedBy: MemberName.self)
            for (name, value) in members {
                try container.encode(value, forKey: MemberName(stringValue: name))
            }
        default:
            var container = encoder.singleValueContainer()
            switch self {
            case .null: try container.encodeNil()
            case .bool(let value): try container.encode(value)
            case .string(let value): try container.encode(value)
            case .double(let value): try container.encode(value)
            case .float(let value): try container.encode(value)
            case .signed(let value): try container.encode(value)
            case .unsigned(let value): try container.encode(value)
            case .array, .dictionary: break
            }
        }
    }
}

// The root value, which finds its type expression in the decoder's userInfo.
struct TypedRoot: Decodable {
    static let expressionKey = CodingUserInfoKey(rawValue: "json.typeExpression")!

    let value: TypedValue

    init(from decoder: any Decoder) throws {
        let expression = decoder.userInfo[Self.expressionKey] as! TypeExpression
        value = try TypedValue(decoding: expression, from: decoder)
    }
}

struct MemberName: CodingKey {
    let stringValue: String

    init(stringValue: String) {
        self.stringValue = stringValue
    }

    init?(intValue: Int) {
        return nil
    }

    var intValue: Int? { nil }
}
