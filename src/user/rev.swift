import FeatherSupport

let help = """
    usage: rev [file...]
    Prints each line of the files, or of its input without any, with its characters reversed.
    An accented letter made of several code points stays one character.
    """

func reverseLines(of input: InputFile) throws(SystemError) {
    while let line = try input.readLine() {
        print(String(String(decoding: line, as: UTF8.self).reversed()))
    }
}

if Arguments.helpRequested {
    print(help)
    exit(.success)
}
let paths = Arguments.operands
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
