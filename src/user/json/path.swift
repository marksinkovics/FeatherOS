struct JSONPathError: Error, CustomStringConvertible {
    let message: String

    var description: String {
        message
    }
}

enum JSONPathStep: Equatable {
    case key([UInt8])
    // Negative indexes count from the end.
    case index(Int)
    case each
}

// A jq path: . alone, or steps of .name, ."name", ["name"], [n] and [], with the dot before a
// bracket optional.
struct JSONPath: Equatable {
    let steps: [JSONPathStep]

    init(steps: [JSONPathStep]) {
        self.steps = steps
    }

    init(parsing text: [UInt8]) throws(JSONPathError) {
        guard text.first == UInt8(ascii: ".") else {
            throw JSONPathError(message: "a path starts with '.'")
        }
        if text.count == 1 {
            steps = []
            return
        }
        var steps: [JSONPathStep] = []
        var offset = 0
        while offset < text.count {
            if text[offset] == UInt8(ascii: "[") {
                steps.append(try Self.parseBracket(text, offset: &offset))
                continue
            }
            guard text[offset] == UInt8(ascii: ".") else {
                throw Self.error("expected '.' or '['", in: text, at: offset)
            }
            offset += 1
            guard offset < text.count else {
                throw Self.error("expected a name after '.'", in: text, at: offset)
            }
            let byte = text[offset]
            if byte == UInt8(ascii: "[") {
                continue
            }
            if byte == UInt8(ascii: "\"") {
                steps.append(.key(try Self.parseQuoted(text, offset: &offset)))
                continue
            }
            guard isNameStart(byte) else {
                throw Self.error("expected a name after '.'", in: text, at: offset)
            }
            let start = offset
            while offset < text.count, isNameStart(text[offset]) || isDigit(text[offset]) {
                offset += 1
            }
            steps.append(.key(Array(text[start..<offset])))
        }
        self.steps = steps
    }

    private static func parseBracket(_ text: [UInt8], offset: inout Int) throws(JSONPathError)
        -> JSONPathStep
    {
        offset += 1
        guard offset < text.count else {
            throw error("expected ']'", in: text, at: offset)
        }
        let step: JSONPathStep
        if text[offset] == UInt8(ascii: "]") {
            step = .each
        } else if text[offset] == UInt8(ascii: "\"") {
            step = .key(try parseQuoted(text, offset: &offset))
        } else {
            step = .index(try parseIndex(text, offset: &offset))
        }
        guard offset < text.count, text[offset] == UInt8(ascii: "]") else {
            throw error("expected ']'", in: text, at: offset)
        }
        offset += 1
        return step
    }

    private static func parseQuoted(_ text: [UInt8], offset: inout Int) throws(JSONPathError)
        -> [UInt8]
    {
        var parser = JSONParser(text, offset: offset)
        do throws(JSONParseError) {
            let key = try parser.parseString()
            offset = parser.offset
            return key
        } catch {
            throw Self.error(error.message, in: text, at: error.offset)
        }
    }

    private static func parseIndex(_ text: [UInt8], offset: inout Int) throws(JSONPathError) -> Int
    {
        let negative = text[offset] == UInt8(ascii: "-")
        if negative {
            offset += 1
        }
        let start = offset
        var value = 0
        while offset < text.count, isDigit(text[offset]) {
            value = value * 10 + Int(text[offset] - UInt8(ascii: "0"))
            offset += 1
            if offset - start > 9 {
                throw error("index too large", in: text, at: start)
            }
        }
        guard offset > start else {
            throw error("expected an index, a quoted name or ']'", in: text, at: offset)
        }
        return negative ? -value : value
    }

    // jq's rules: a missing member or element, or any step into null, gives null.
    func evaluate(_ value: JSONValue) throws(JSONPathError) -> [JSONValue] {
        var results = [value]
        for step in steps {
            var next: [JSONValue] = []
            for result in results {
                try Self.apply(step, to: result, into: &next)
            }
            results = next
        }
        return results
    }

    private static func apply(_ step: JSONPathStep, to value: JSONValue, into results: inout [JSONValue])
        throws(JSONPathError)
    {
        switch (step, value) {
        case (.key, .null), (.index, .null):
            results.append(.null)
        case (.key(let key), .object):
            results.append(value.member(key) ?? .null)
        case (.key(let key), _):
            let name = String(decoding: key, as: UTF8.self)
            throw JSONPathError(message: "cannot index \(value.kindName) with \"\(name)\"")
        case (.index(let index), .array(let elements)):
            let position = index < 0 ? elements.count + index : index
            results.append(elements.indices.contains(position) ? elements[position] : .null)
        case (.index, _):
            throw JSONPathError(message: "cannot index \(value.kindName) with a number")
        case (.each, .array(let elements)):
            results += elements
        case (.each, .object(let members)):
            results += members.map { $0.value }
        case (.each, _):
            throw JSONPathError(message: "cannot iterate over \(value.kindName)")
        }
    }

    private static func error(_ message: String, in text: [UInt8], at offset: Int) -> JSONPathError
    {
        let path = String(decoding: text, as: UTF8.self)
        return JSONPathError(message: "\(path): column \(offset + 1): \(message)")
    }
}

private func isNameStart(_ byte: UInt8) -> Bool {
    isLetter(byte) || byte == UInt8(ascii: "_")
}
