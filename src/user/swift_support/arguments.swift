import FeatherLibC

// Embedded Swift leaves CommandLine out, and top-level code does not see main's parameters, so
// this one reads them from the C library.
public enum CommandLine {
    public static var argc: Int32 {
        featheros_argument_count()
    }

    public static var unsafeArgv: UnsafeMutablePointer<UnsafeMutablePointer<CChar>?> {
        featheros_argument_values()!
    }

    public static var arguments: [String] = (0..<Int(argc)).map {
        String(cString: unsafeArgv[$0]!)
    }
}

public enum Arguments {
    public static var operands: [String] {
        Array(CommandLine.arguments.dropFirst())
    }

    public static var programName: String {
        guard let name = CommandLine.arguments.first else {
            return "program"
        }
        guard let slash = name.utf8.lastIndex(of: UInt8(ascii: "/")) else {
            return name
        }
        return String(name[name.utf8.index(after: slash)...])
    }

    public static var helpRequested: Bool {
        let operands = operands
        return operands.count == 1
            && (operands[0].utf8.elementsEqual("-h".utf8)
                || operands[0].utf8.elementsEqual("--help".utf8))
    }
}
