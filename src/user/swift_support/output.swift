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
