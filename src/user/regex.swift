import FeatherLibC
import FeatherSupport

let usage = "usage: regex [-Bia] <pattern> [file...]"
let help = """
    \(usage)
    Prints the first match of an extended regular expression on each line of the files, or of
    its input without any, after the line's number and followed by each subexpression in
    brackets, or "-" for one that took no part: -B takes a basic expression, -i ignores case,
    -a prints every match on a line. Exits with 0 when a line matched, 1 when none did, 2 on
    an error.
    """

if Arguments.helpRequested {
    print(help)
    exit(.success)
}

var options = RegularExpression.Options.extended
var everyMatch = false
var operands = Arguments.operands
while let first = operands.first, first.utf8.count > 1, first.utf8.first == UInt8(ascii: "-") {
    operands.removeFirst()
    for letter in first.utf8.dropFirst() {
        switch letter {
        case UInt8(ascii: "B"):
            options.remove(.extended)
        case UInt8(ascii: "i"):
            options.insert(.ignoreCase)
        case UInt8(ascii: "a"):
            everyMatch = true
        default:
            warn("unknown option -\(String(decoding: [letter], as: UTF8.self))")
            print(usage)
            FeatherLibC.exit(2)
        }
    }
}
guard let pattern = operands.first else {
    print(usage)
    FeatherLibC.exit(2)
}

let expression: RegularExpression
do throws(RegularExpressionError) {
    expression = try RegularExpression(pattern, options: options)
} catch {
    warn("\(error)")
    FeatherLibC.exit(2)
}

var matchedAny = false

func search(_ input: InputFile) throws(SystemError) {
    var number = 0
    while let bytes = try input.readLine() {
        number += 1
        let line = String(decoding: bytes, as: UTF8.self)
        let found = everyMatch ? expression.allMatches(in: line)
            : expression.firstMatch(in: line).map { [$0] } ?? []
        for match in found {
            matchedAny = true
            var text = "\(number): \(match.matched)"
            for group in match.groups {
                text += group.map { " [\(line[$0])]" } ?? " -"
            }
            print(text)
        }
    }
}

let paths = operands.dropFirst()
var failed = false
if paths.isEmpty {
    do throws(SystemError) {
        try search(.standard)
    } catch {
        warn("standard input: \(error)")
        failed = true
    }
}
for path in paths {
    do throws(SystemError) {
        try search(InputFile(path: path))
    } catch {
        warn("\(path): \(error)")
        failed = true
    }
}
FeatherLibC.exit(failed ? 2 : matchedAny ? 0 : 1)
