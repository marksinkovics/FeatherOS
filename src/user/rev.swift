import FeatherSupport

let help = """
    usage: rev [-n] [file...]
    Prints each line of the files, or of its input without any, with its characters reversed.
    An accented letter made of several code points stays one character. -n numbers the lines.
    """

var numbered = false
var lineNumber = 0

func reverseLines(of input: InputFile) throws(SystemError) {
    while let line = try input.readLine() {
        let reversed = String(String(decoding: line, as: UTF8.self).reversed())
        if numbered {
            lineNumber += 1
            print(String(format: "%6d  %s", lineNumber, reversed))
        } else {
            print(reversed)
        }
    }
}

if Arguments.helpRequested {
    print(help)
    exit(.success)
}
var paths = Array(CommandLine.arguments.dropFirst())
if let first = paths.first, first.utf8.elementsEqual("-n".utf8) {
    numbered = true
    paths.removeFirst()
}
if paths.isEmpty {
    do throws(SystemError) {
        try reverseLines(of: .standard)
    } catch {
        fail("standard input: \(error)")
    }
}
var status = ExitCode.success
for path in paths {
    do throws(SystemError) {
        try reverseLines(of: InputFile(path: path))
    } catch {
        warn("\(path): \(error)")
        status = .failure
    }
}
exit(status)
