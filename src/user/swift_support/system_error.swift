import FeatherLibC

// errno is a C macro over a function call, which Swift does not import.
public var errno: Int32 {
    get { __errno_location().pointee }
    set { __errno_location().pointee = newValue }
}

public struct SystemError: Error, CustomStringConvertible {
    public let code: Int32

    public init(code: Int32) {
        self.code = code
    }

    public static var current: SystemError {
        SystemError(code: errno)
    }

    public var description: String {
        String(cString: strerror(code))
    }
}
