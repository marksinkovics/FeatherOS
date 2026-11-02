import FeatherLibC

// Top-level code does not see main's parameters, and Embedded Swift has no CommandLine.
public enum Arguments {
    public static var all: [String] {
        let values = featheros_argument_values()!
        return (0..<Int(featheros_argument_count())).map { String(cString: values[$0]!) }
    }

    public static var operands: [String] {
        Array(all.dropFirst())
    }

    public static var programName: String {
        guard let path = featheros_argument_values()?[0] else {
            return "program"
        }
        let name = String(cString: path)
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
