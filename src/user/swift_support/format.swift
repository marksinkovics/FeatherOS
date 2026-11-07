import FeatherLibC

// What a value passes to String(format:), as C would pass it to printf.
public enum FormatValue {
    case signed(Int64)
    case unsigned(UInt64)
    case double(Double)
    case string(String)
    case pointer(UnsafeRawPointer?)
}

public protocol FormatArgument {
    var formatValue: FormatValue { get }
}

extension FormatArgument where Self: SignedInteger & FixedWidthInteger {
    public var formatValue: FormatValue { .signed(Int64(self)) }
}

extension FormatArgument where Self: UnsignedInteger & FixedWidthInteger {
    public var formatValue: FormatValue { .unsigned(UInt64(self)) }
}

extension Int: FormatArgument {}
extension Int8: FormatArgument {}
extension Int16: FormatArgument {}
extension Int32: FormatArgument {}
extension Int64: FormatArgument {}
extension UInt: FormatArgument {}
extension UInt8: FormatArgument {}
extension UInt16: FormatArgument {}
extension UInt32: FormatArgument {}
extension UInt64: FormatArgument {}

extension Double: FormatArgument {
    public var formatValue: FormatValue { .double(self) }
}

extension Float: FormatArgument {
    public var formatValue: FormatValue { .double(Double(self)) }
}

extension String: FormatArgument {
    public var formatValue: FormatValue { .string(self) }
}

extension Substring: FormatArgument {
    public var formatValue: FormatValue { .string(String(self)) }
}

extension UnsafeRawPointer: FormatArgument {
    public var formatValue: FormatValue { .pointer(self) }
}

extension UnsafeMutableRawPointer: FormatArgument {
    public var formatValue: FormatValue { .pointer(UnsafeRawPointer(self)) }
}

extension UnsafePointer: FormatArgument {
    public var formatValue: FormatValue { .pointer(UnsafeRawPointer(self)) }
}

extension UnsafeMutablePointer: FormatArgument {
    public var formatValue: FormatValue { .pointer(UnsafeRawPointer(self)) }
}

extension OpaquePointer: FormatArgument {
    public var formatValue: FormatValue { .pointer(UnsafeRawPointer(self)) }
}

extension String {
    // printf's conversions, through the C library's engine. Integers keep all 64 bits unless hh
    // or h asks for fewer, a conversion without a suitable argument is written as it appears, and
    // a string ends at its first NUL.
    public init(format: String, _ arguments: any FormatArgument...) {
        let values = arguments.map { $0.formatValue }
        // Every string argument as a C string, one after another.
        var strings: [CChar] = []
        var offsets: [Int] = []
        for case .string(let text) in values {
            offsets.append(strings.count)
            strings.append(contentsOf: text.utf8CString)
        }
        self = strings.withUnsafeBufferPointer { strings in
            var nextString = 0
            let tagged = values.map { value in
                var argument = featheros_format_argument()
                switch value {
                case .signed(let number):
                    argument.type = FEATHEROS_FORMAT_SIGNED
                    argument.value.signed_value = number
                case .unsigned(let number):
                    argument.type = FEATHEROS_FORMAT_UNSIGNED
                    argument.value.unsigned_value = number
                case .double(let number):
                    argument.type = FEATHEROS_FORMAT_DOUBLE
                    argument.value.double_value = number
                case .string:
                    argument.type = FEATHEROS_FORMAT_STRING
                    argument.value.string = strings.baseAddress! + offsets[nextString]
                    nextString += 1
                case .pointer(let pointer):
                    argument.type = FEATHEROS_FORMAT_POINTER
                    argument.value.pointer = pointer
                }
                return argument
            }
            return Self.formatted(format, tagged)
        }
    }

    private static func formatted(_ format: String, _ arguments: [featheros_format_argument])
        -> String
    {
        format.withCString { format in
            arguments.withUnsafeBufferPointer { arguments in
                let length = featheros_format(nil, 0, format, arguments.baseAddress, arguments.count)
                return String(unsafeUninitializedCapacity: Int(length) + 1) { buffer in
                    buffer.withMemoryRebound(to: CChar.self) { buffer in
                        Int(
                            featheros_format(
                                buffer.baseAddress, buffer.count, format, arguments.baseAddress,
                                arguments.count))
                    }
                }
            }
        }
    }
}
