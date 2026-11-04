public struct JSONParseError: Error, CustomStringConvertible {
    public let message: String
    public let offset: Int
    public let line: Int
    public let column: Int

    public var description: String {
        "line \(line), column \(column): \(message)"
    }
}

// RFC 8259 over bytes. The input may hold several values separated by whitespace, as jq reads.
public struct JSONParser {
    // Each level of nesting is a recursive call on a user stack of 252 KiB.
    static let maximumDepth = 256

    private let bytes: [UInt8]
    public private(set) var offset: Int
    private var depth = 0

    public init(_ bytes: [UInt8], offset: Int = 0) {
        self.bytes = bytes
        self.offset = offset
    }

    public static func parseAll(_ bytes: [UInt8]) throws(JSONParseError) -> [JSONValue] {
        var parser = JSONParser(bytes)
        var values: [JSONValue] = []
        while let value = try parser.next() {
            values.append(value)
        }
        return values
    }

    // The next value, or nil at the end of the input.
    public mutating func next() throws(JSONParseError) -> JSONValue? {
        skipWhitespace()
        if offset == bytes.count {
            return nil
        }
        return try parseValue()
    }

    private mutating func parseValue() throws(JSONParseError) -> JSONValue {
        skipWhitespace()
        guard let byte = peek() else {
            throw error("expected a value, found \(describeNext())")
        }
        switch byte {
        case UInt8(ascii: "{"):
            return try parseObject()
        case UInt8(ascii: "["):
            return try parseArray()
        case UInt8(ascii: "\""):
            return .string(try parseString())
        case UInt8(ascii: "t"):
            try expectLiteral("true")
            return .bool(true)
        case UInt8(ascii: "f"):
            try expectLiteral("false")
            return .bool(false)
        case UInt8(ascii: "n"):
            try expectLiteral("null")
            return .null
        case UInt8(ascii: "-"), UInt8(ascii: "0")...UInt8(ascii: "9"):
            return .number(try parseNumber())
        default:
            throw error("expected a value, found \(describeNext())")
        }
    }

    private mutating func parseObject() throws(JSONParseError) -> JSONValue {
        try enter()
        offset += 1
        var collector = JSONMemberCollector()
        skipWhitespace()
        if peek() == UInt8(ascii: "}") {
            offset += 1
            depth -= 1
            return .object([])
        }
        while true {
            skipWhitespace()
            guard peek() == UInt8(ascii: "\"") else {
                throw error("expected a member name, found \(describeNext())")
            }
            let key = try parseString()
            skipWhitespace()
            guard peek() == UInt8(ascii: ":") else {
                throw error("expected ':' after a member name, found \(describeNext())")
            }
            offset += 1
            collector.add(key: key, value: try parseValue())
            skipWhitespace()
            switch peek() {
            case UInt8(ascii: ","):
                offset += 1
            case UInt8(ascii: "}"):
                offset += 1
                depth -= 1
                return .object(collector.members)
            default:
                throw error("expected ',' or '}' after a member, found \(describeNext())")
            }
        }
    }

    private mutating func parseArray() throws(JSONParseError) -> JSONValue {
        try enter()
        offset += 1
        var elements: [JSONValue] = []
        skipWhitespace()
        if peek() == UInt8(ascii: "]") {
            offset += 1
            depth -= 1
            return .array([])
        }
        while true {
            elements.append(try parseValue())
            skipWhitespace()
            switch peek() {
            case UInt8(ascii: ","):
                offset += 1
            case UInt8(ascii: "]"):
                offset += 1
                depth -= 1
                return .array(elements)
            default:
                throw error("expected ',' or ']' after an element, found \(describeNext())")
            }
        }
    }

    private mutating func enter() throws(JSONParseError) {
        depth += 1
        if depth > Self.maximumDepth {
            throw error("nested deeper than \(Self.maximumDepth) levels")
        }
    }

    // The bytes of the string at the offset, with its escapes decoded.
    public mutating func parseString() throws(JSONParseError) -> [UInt8] {
        offset += 1
        var text: [UInt8] = []
        while true {
            let start = offset
            while offset < bytes.count {
                let byte = bytes[offset]
                if byte == UInt8(ascii: "\"") || byte == UInt8(ascii: "\\") || byte < 0x20
                    || byte >= 0x80
                {
                    break
                }
                offset += 1
            }
            text += bytes[start..<offset]
            guard let byte = peek() else {
                throw error("unterminated string")
            }
            switch byte {
            case UInt8(ascii: "\""):
                offset += 1
                return text
            case UInt8(ascii: "\\"):
                try parseEscape(into: &text)
            case 0x80...:
                let length = try validateUTF8Sequence()
                text += bytes[offset..<offset + length]
                offset += length
            default:
                throw error("control character in a string")
            }
        }
    }

    // Surrogate errors point at the escape that is wrong.
    private mutating func parseEscape(into text: inout [UInt8]) throws(JSONParseError) {
        let start = offset
        offset += 1
        guard let byte = peek() else {
            throw error("unterminated string")
        }
        let simple: UInt8? =
            switch byte {
            case UInt8(ascii: "\""): UInt8(ascii: "\"")
            case UInt8(ascii: "\\"): UInt8(ascii: "\\")
            case UInt8(ascii: "/"): UInt8(ascii: "/")
            case UInt8(ascii: "b"): 0x08
            case UInt8(ascii: "f"): 0x0c
            case UInt8(ascii: "n"): UInt8(ascii: "\n")
            case UInt8(ascii: "r"): UInt8(ascii: "\r")
            case UInt8(ascii: "t"): UInt8(ascii: "\t")
            default: nil
            }
        if let simple {
            text.append(simple)
            offset += 1
            return
        }
        guard byte == UInt8(ascii: "u") else {
            throw error("unknown escape: \\ followed by \(describeNext())")
        }
        offset = start
        var scalar = try parseHexEscape()
        if scalar >= 0xdc00, scalar <= 0xdfff {
            offset = start
            throw error("low surrogate without a high one")
        }
        if scalar >= 0xd800, scalar <= 0xdbff {
            let lowStart = offset
            guard offset + 1 < bytes.count, bytes[offset] == UInt8(ascii: "\\"),
                bytes[offset + 1] == UInt8(ascii: "u")
            else {
                offset = start
                throw error("high surrogate without a low one")
            }
            let low = try parseHexEscape()
            guard low >= 0xdc00, low <= 0xdfff else {
                offset = lowStart
                throw error("high surrogate without a low one")
            }
            scalar = 0x10000 + ((scalar - 0xd800) << 10) + (low - 0xdc00)
        }
        appendUTF8(scalar, to: &text)
    }

    // Reads \uXXXX at the offset.
    private mutating func parseHexEscape() throws(JSONParseError) -> UInt32 {
        offset += 2
        var value: UInt32 = 0
        for _ in 0..<4 {
            guard let byte = peek(), let digit = hexDigitValue(byte) else {
                throw error("expected four hex digits after \\u, found \(describeNext())")
            }
            value = value << 4 | UInt32(digit)
            offset += 1
        }
        return value
    }

    // The length of the well-formed UTF-8 sequence at the offset (Unicode table 3-7).
    private func validateUTF8Sequence() throws(JSONParseError) -> Int {
        let lead = bytes[offset]
        let length: Int
        var lowest: UInt8 = 0x80
        var highest: UInt8 = 0xbf
        switch lead {
        case 0xc2...0xdf:
            length = 2
        case 0xe0:
            length = 3
            lowest = 0xa0
        case 0xe1...0xec, 0xee...0xef:
            length = 3
        case 0xed:
            length = 3
            highest = 0x9f
        case 0xf0:
            length = 4
            lowest = 0x90
        case 0xf1...0xf3:
            length = 4
        case 0xf4:
            length = 4
            highest = 0x8f
        default:
            throw error("invalid UTF-8 byte 0x\(hex(lead))")
        }
        for index in 1..<length {
            let position = offset + index
            let limits = index == 1 ? lowest...highest : 0x80...0xbf
            guard position < bytes.count, limits.contains(bytes[position]) else {
                throw error("invalid UTF-8 sequence")
            }
        }
        return length
    }

    private mutating func parseNumber() throws(JSONParseError) -> [UInt8] {
        let start = offset
        if peek() == UInt8(ascii: "-") {
            offset += 1
        }
        if peek() == UInt8(ascii: "0") {
            offset += 1
            if let byte = peek(), isDigit(byte) {
                throw error("leading zero in a number")
            }
        } else {
            try skipDigits(after: "a minus sign")
        }
        if peek() == UInt8(ascii: ".") {
            offset += 1
            try skipDigits(after: "a decimal point")
        }
        if peek() == UInt8(ascii: "e") || peek() == UInt8(ascii: "E") {
            offset += 1
            if peek() == UInt8(ascii: "+") || peek() == UInt8(ascii: "-") {
                offset += 1
            }
            try skipDigits(after: "an exponent")
        }
        try expectEndOfWord()
        return Array(bytes[start..<offset])
    }

    private mutating func skipDigits(after what: String) throws(JSONParseError) {
        guard let byte = peek(), isDigit(byte) else {
            throw error("expected a digit after \(what), found \(describeNext())")
        }
        while let byte = peek(), isDigit(byte) {
            offset += 1
        }
    }

    private mutating func expectLiteral(_ literal: StaticString) throws(JSONParseError) {
        let start = offset
        for expected in UnsafeBufferPointer(start: literal.utf8Start, count: literal.utf8CodeUnitCount)
        {
            guard peek() == expected else {
                offset = start
                throw error("expected a value, found \(describeNext())")
            }
            offset += 1
        }
        try expectEndOfWord()
    }

    // So that "truex" or "12a" are not read as a value followed by another.
    private func expectEndOfWord() throws(JSONParseError) {
        if let byte = peek(), isDigit(byte) || isLetter(byte) || byte == UInt8(ascii: ".")
            || byte == UInt8(ascii: "+") || byte == UInt8(ascii: "-")
        {
            throw error("unexpected \(describeNext()) after a value")
        }
    }

    private mutating func skipWhitespace() {
        while let byte = peek(),
            byte == UInt8(ascii: " ") || byte == UInt8(ascii: "\n") || byte == UInt8(ascii: "\t")
                || byte == UInt8(ascii: "\r")
        {
            offset += 1
        }
    }

    private func peek() -> UInt8? {
        offset < bytes.count ? bytes[offset] : nil
    }

    private func describeNext() -> String {
        guard let byte = peek() else {
            return "the end of the input"
        }
        if byte > 0x20, byte < 0x7f {
            return "'\(String(decoding: [byte], as: UTF8.self))'"
        }
        return "byte 0x\(hex(byte))"
    }

    // Columns count characters, not bytes: UTF-8 continuation bytes are skipped.
    private func error(_ message: String) -> JSONParseError {
        var line = 1
        var column = 1
        for byte in bytes[..<min(offset, bytes.count)] {
            if byte == UInt8(ascii: "\n") {
                line += 1
                column = 1
            } else if byte & 0xc0 != 0x80 {
                column += 1
            }
        }
        return JSONParseError(message: message, offset: offset, line: line, column: column)
    }
}

func appendUTF8(_ scalar: UInt32, to text: inout [UInt8]) {
    switch scalar {
    case ..<0x80:
        text.append(UInt8(scalar))
    case ..<0x800:
        text.append(UInt8(0xc0 | scalar >> 6))
        text.append(UInt8(0x80 | scalar & 0x3f))
    case ..<0x10000:
        text.append(UInt8(0xe0 | scalar >> 12))
        text.append(UInt8(0x80 | scalar >> 6 & 0x3f))
        text.append(UInt8(0x80 | scalar & 0x3f))
    default:
        text.append(UInt8(0xf0 | scalar >> 18))
        text.append(UInt8(0x80 | scalar >> 12 & 0x3f))
        text.append(UInt8(0x80 | scalar >> 6 & 0x3f))
        text.append(UInt8(0x80 | scalar & 0x3f))
    }
}

private func isDigit(_ byte: UInt8) -> Bool {
    byte >= UInt8(ascii: "0") && byte <= UInt8(ascii: "9")
}

private func isLetter(_ byte: UInt8) -> Bool {
    (byte | 0x20) >= UInt8(ascii: "a") && (byte | 0x20) <= UInt8(ascii: "z")
}

func hexDigitValue(_ byte: UInt8) -> UInt8? {
    if isDigit(byte) {
        return byte - UInt8(ascii: "0")
    }
    let lower = byte | 0x20
    if lower >= UInt8(ascii: "a"), lower <= UInt8(ascii: "f") {
        return lower - UInt8(ascii: "a") + 10
    }
    return nil
}

func hex(_ byte: UInt8) -> String {
    let digits: StaticString = "0123456789abcdef"
    let text = [digits.utf8Start[Int(byte >> 4)], digits.utf8Start[Int(byte & 0xf)]]
    return String(decoding: text, as: UTF8.self)
}
