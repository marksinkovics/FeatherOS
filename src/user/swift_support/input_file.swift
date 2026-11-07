import FeatherLibC

public final class InputFile {
    private let stream: OpaquePointer
    private let owned: Bool
    private var line: UnsafeMutablePointer<CChar>?
    private var capacity = 0

    public static var standard: InputFile {
        InputFile(stream: stdin, owned: false)
    }

    public init(path: String) throws(SystemError) {
        guard let stream = fopen(path, "r") else {
            throw SystemError.current
        }
        self.stream = stream
        owned = true
    }

    private init(stream: OpaquePointer, owned: Bool) {
        self.stream = stream
        self.owned = owned
    }

    deinit {
        free(line)
        if owned {
            fclose(stream)
        }
    }

    public func readAll() throws(SystemError) -> [UInt8] {
        var contents: [UInt8] = []
        var chunk = [UInt8](repeating: 0, count: Int(BUFSIZ))
        while true {
            let count = chunk.withUnsafeMutableBytes { fread($0.baseAddress, 1, $0.count, stream) }
            contents += chunk[..<count]
            if count < chunk.count {
                try checkError()
                return contents
            }
        }
    }

    // A line without its newline, or nil at the end of the file.
    public func readLine() throws(SystemError) -> [UInt8]? {
        let length = getline(&line, &capacity, stream)
        guard length >= 0, let line else {
            try checkError()
            return nil
        }
        let bytes = UnsafeRawBufferPointer(start: line, count: length)
        return Array(bytes.last == UInt8(ascii: "\n") ? bytes.dropLast() : bytes[...])
    }

    private func checkError() throws(SystemError) {
        if ferror(stream) != 0 {
            throw SystemError.current
        }
    }
}
