import FeatherSupport

let help = """
    usage: json [-c] [-r] [path [file...]]
    Reads the JSON values in the files, or in its input without any, and prints what the
    path selects from each, as jq does. A path is . for the whole value, or steps of .name
    or ."name" for a member, [n] for an element (negative from the end) and [] for every
    element or member value. A missing member or element is null.
      -c  print each result on one line
      -r  print strings without quotes or escapes
    """

struct Options {
    var compact = false
    var raw = false
    var path = JSONPath(steps: [])
    var files: [String] = []
}

func parseOptions() -> Options {
    var options = Options()
    var operands = Arguments.operands[...]
    while let operand = operands.first, operand.utf8.first == UInt8(ascii: "-"),
        operand.utf8.count > 1
    {
        operands.removeFirst()
        for letter in operand.utf8.dropFirst() {
            switch letter {
            case UInt8(ascii: "c"):
                options.compact = true
            case UInt8(ascii: "r"):
                options.raw = true
            default:
                fail("unknown option \(operand); see json -h")
            }
        }
    }
    if let path = operands.popFirst() {
        do throws(JSONPathError) {
            options.path = try JSONPath(parsing: Array(path.utf8))
        } catch {
            fail("invalid path \(error)")
        }
    }
    options.files = Array(operands)
    return options
}

// Values before a parse error are still printed, and a path error skips only its value.
func printResults<Printer: JSONPrinter>(
    of bytes: [UInt8], named name: String, options: Options, printer: Printer
) -> ExitCode {
    var parser = JSONParser(bytes)
    var status = ExitCode.success
    while true {
        let results: [JSONValue]
        do throws(JSONParseError) {
            guard let value = try parser.next() else {
                return status
            }
            do throws(JSONPathError) {
                results = try options.path.evaluate(value)
            } catch {
                warn("\(name): \(error)")
                status = .failure
                continue
            }
        } catch {
            warn("\(name): \(error)")
            return .failure
        }
        var output: [UInt8] = []
        for result in results {
            if options.raw, case .string(let text) = result {
                output += text
            } else {
                printer.write(result, into: &output)
            }
            output.append(UInt8(ascii: "\n"))
        }
        Output.standard.write(bytes: output)
    }
}

func printResults<Printer: JSONPrinter>(options: Options, printer: Printer) -> ExitCode {
    let inputs = options.files.isEmpty ? [nil] : options.files.map { Optional($0) }
    var status = ExitCode.success
    for path in inputs {
        let name = path ?? "standard input"
        let bytes: [UInt8]
        do throws(SystemError) {
            let input = if let path { try InputFile(path: path) } else { InputFile.standard }
            bytes = try input.readAll()
        } catch {
            warn("\(name): \(error)")
            status = .failure
            continue
        }
        if printResults(of: bytes, named: name, options: options, printer: printer) == .failure {
            status = .failure
        }
    }
    return status
}

if Arguments.helpRequested {
    print(help)
    exit(.success)
}
let options = parseOptions()
exit(
    options.compact
        ? printResults(options: options, printer: CompactPrinter())
        : printResults(options: options, printer: PrettyPrinter()))
