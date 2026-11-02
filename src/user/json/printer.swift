// A printer chooses the layout; the shared extension walks the value.
protocol JSONPrinter {
    var memberSeparator: StaticString { get }
    // Called before each element or member of a container, and before its closing bracket.
    func writeBreak(depth: Int, into output: inout [UInt8])
}

extension JSONPrinter {
    func write(_ value: JSONValue, into output: inout [UInt8]) {
        write(value, depth: 0, into: &output)
    }

    private func write(_ value: JSONValue, depth: Int, into output: inout [UInt8]) {
        switch value {
        case .null:
            append("null", to: &output)
        case .bool(let truth):
            append(truth ? "true" : "false", to: &output)
        case .number(let text):
            output += text
        case .string(let text):
            appendQuoted(text, to: &output)
        case .array(let elements):
            output.append(UInt8(ascii: "["))
            for (index, element) in elements.enumerated() {
                if index > 0 {
                    output.append(UInt8(ascii: ","))
                }
                writeBreak(depth: depth + 1, into: &output)
                write(element, depth: depth + 1, into: &output)
            }
            if !elements.isEmpty {
                writeBreak(depth: depth, into: &output)
            }
            output.append(UInt8(ascii: "]"))
        case .object(let members):
            output.append(UInt8(ascii: "{"))
            for (index, member) in members.enumerated() {
                if index > 0 {
                    output.append(UInt8(ascii: ","))
                }
                writeBreak(depth: depth + 1, into: &output)
                appendQuoted(member.key, to: &output)
                append(memberSeparator, to: &output)
                write(member.value, depth: depth + 1, into: &output)
            }
            if !members.isEmpty {
                writeBreak(depth: depth, into: &output)
            }
            output.append(UInt8(ascii: "}"))
        }
    }
}

struct CompactPrinter: JSONPrinter {
    var memberSeparator: StaticString { ":" }

    func writeBreak(depth: Int, into output: inout [UInt8]) {}
}

struct PrettyPrinter: JSONPrinter {
    var indent = 2

    var memberSeparator: StaticString { ": " }

    func writeBreak(depth: Int, into output: inout [UInt8]) {
        output.append(UInt8(ascii: "\n"))
        output += repeatElement(UInt8(ascii: " "), count: depth * indent)
    }
}

func append(_ text: StaticString, to output: inout [UInt8]) {
    output += UnsafeBufferPointer(start: text.utf8Start, count: text.utf8CodeUnitCount)
}

// Other bytes, UTF-8 sequences included, are written as they are.
func appendQuoted(_ text: [UInt8], to output: inout [UInt8]) {
    output.append(UInt8(ascii: "\""))
    for byte in text {
        switch byte {
        case UInt8(ascii: "\""):
            append("\\\"", to: &output)
        case UInt8(ascii: "\\"):
            append("\\\\", to: &output)
        case UInt8(ascii: "\n"):
            append("\\n", to: &output)
        case UInt8(ascii: "\r"):
            append("\\r", to: &output)
        case UInt8(ascii: "\t"):
            append("\\t", to: &output)
        case 0x08:
            append("\\b", to: &output)
        case 0x0c:
            append("\\f", to: &output)
        case ..<0x20, 0x7f:
            append("\\u00", to: &output)
            output += hex(byte).utf8
        default:
            output.append(byte)
        }
    }
    output.append(UInt8(ascii: "\""))
}
