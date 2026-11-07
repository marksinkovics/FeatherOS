import FeatherLibC

public enum ExitCode: Int32 {
    case success = 0
    case failure = 1
}

public func exit(_ code: ExitCode) -> Never {
    FeatherLibC.exit(code.rawValue)
}

// Messages on standard error start with the program's name, like those of the C++ programs.
public func warn(_ message: String) {
    Output.error.write("\(Arguments.programName): \(message)\n")
}

public func fail(_ message: String) -> Never {
    warn(message)
    exit(.failure)
}
