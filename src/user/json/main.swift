import FeatherSupport

let help = """
    usage: json [-c] [-r] [-t type] [path [file...]]
    Reads the JSON values in the files, or in its input without any, and prints what the
    path selects from each, as jq does. A path is . for the whole value, or steps of .name
    or ."name" for a member, [n] for an element (negative from the end) and [] for every
    element or member value. A missing member or element is null.
      -c  print each result on one line
      -r  print strings without quotes or escapes
      -t  decode each result as a Swift type with JSONDecoder and print it encoded again:
          bool, string, double, float, int, int8 to int64, uint, uint8 to uint64,
          [T] for an array, {T} for an object and T? for an optional, as in -t "{int?}"
    """

struct Options {
    var compact = false
    var raw = false
    var type: TypeExpression?
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
            case UInt8(ascii: "t"):
                guard let name = operands.popFirst() else {
                    fail("-t needs a type; see json -h")
                }
                guard let type = TypeExpression(parsing: name) else {
                    fail("unknown type \(name); see json -h")
                }
                options.type = type
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
            if let type = options.type {
                do {
                    try writeTyped(result, as: type, options: options, into: &output)
                } catch let error as DecodingError {
                    Output.standard.write(bytes: output)
                    output = []
                    warn("\(name): \(error)")
                    status = .failure
                    continue
                } catch {
                    fail("\(name): cannot encode the value")
                }
            } else if options.raw, case .string(let text) = result {
                output += text
            } else {
                printer.write(result, into: &output)
            }
            output.append(UInt8(ascii: "\n"))
        }
        Output.standard.write(bytes: output)
    }
}

func writeTyped(
    _ value: JSONValue, as type: TypeExpression, options: Options, into output: inout [UInt8]
) throws {
    var decoder = JSONDecoder()
    decoder.userInfo[TypedRoot.expressionKey] = type
    let typed = try decoder.decode(TypedRoot.self, from: value).value
    if options.raw, case .string(let text) = typed {
        output += text.utf8
        return
    }
    var encoder = JSONEncoder()
    encoder.outputFormatting = options.compact ? [] : [.prettyPrinted]
    output += try encoder.encode(typed)
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
