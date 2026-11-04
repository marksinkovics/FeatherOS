import FeatherLibC

// Writes through the C library's streams, so the text keeps its order with print, which goes
// through putchar on stdout.
public struct Output: TextOutputStream {
    private let stream: OpaquePointer

    public static var standard: Output {
        Output(stream: stdout)
    }

    public static var error: Output {
        Output(stream: stderr)
    }

    public func write(_ string: String) {
        var string = string
        string.withUTF8 { write(bytes: $0) }
    }

    public func write(bytes: UnsafeBufferPointer<UInt8>) {
        if let base = bytes.baseAddress {
            fwrite(base, 1, bytes.count, stream)
        }
    }

    public func write(bytes: [UInt8]) {
        bytes.withUnsafeBufferPointer { write(bytes: $0) }
    }

    public func flush() {
        fflush(stream)
    }
}

// The standard library's own print takes a single item under Embedded Swift. This one is here
// rather than in swift.patch because a non-generic function there changes how the standard
// library's code inlines into every program.
@_disfavoredOverload
public func print(
    _ items: any CustomStringConvertible..., separator: String = " ", terminator: String = "\n"
) {
    let output = Output.standard
    var prefix = ""
    for item in items {
        output.write(prefix)
        output.write(item.description)
        prefix = separator
    }
    output.write(terminator)
}
