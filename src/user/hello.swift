import FeatherLibC
import FeatherSupport

protocol Shape {
    var name: String { get }
    var area: Int { get }
}

struct Square: Shape {
    let side: Int
    var name: String { "square" }
    var area: Int { side * side }
}

struct Rectangle: Shape {
    let width: Int
    let height: Int
    var name: String { "rectangle" }
    var area: Int { width * height }
}

func describe<S: Shape>(_ shape: S) -> String {
    "\(shape.name) of \(shape.area)"
}

func largest<S: Shape>(_ shapes: [S]) -> S? {
    shapes.max { $0.area < $1.area }
}

final class Tracker {
    let name: String

    init(_ name: String) {
        self.name = name
    }

    deinit {
        print("hello: \(name) released")
    }
}

func shareTracker() -> Bool {
    var tracker = Tracker("tracker")
    let alias = tracker
    let shared = !isKnownUniquelyReferenced(&tracker)
    withExtendedLifetime(alias) {}
    return shared
}

print("Hello from Embedded Swift")
var line = "hello: arguments"
for (index, argument) in CommandLine.arguments.dropFirst().enumerated() {
    line += index == 0 ? " " : ", "
    line += argument
}
print(line)
let last = String(cString: CommandLine.unsafeArgv[Int(CommandLine.argc) - 1]!)
print("hello: argc \(CommandLine.argc), the last argument \(last)")
if let square = largest([Square(side: 3), Square(side: 5)]) {
    print("hello: largest \(describe(square)), \(describe(Rectangle(width: 3, height: 4)))")
}
print("hello: tracker was shared \(shareTracker())")
let distinct = Set(["feather", "swift", "feather"]).count
// An e followed by a combining acute accent is one Character.
let characters = "cafe\u{301}".count
let rolled = (1...6).contains(Int.random(in: 1...6))
print("hello: \(distinct) distinct words, \(characters) characters, roll in range \(rolled)")
print("hello: running as process \(getpid())")
// Parsing a Double calls the C library's fma on x86-64.
print("hello: parsed \(Double("2.5e-3") ?? -1) and \(Float("0.1") ?? -1)")

func readsAlike(_ path: String) throws(SystemError) -> Bool {
    let whole = try InputFile(path: path).readAll()
    let file = try InputFile(path: path)
    var joined: [UInt8] = []
    while let line = try file.readLine() {
        joined += line + [UInt8(ascii: "\n")]
    }
    return !whole.isEmpty && joined == whole
}

for path in ["/etc/hosts", "/none"] {
    do throws(SystemError) {
        print("hello: \(path) read whole and by line alike \(try readsAlike(path))")
    } catch {
        print("hello: \(path): \(error)")
    }
}
print(
    String(
        format: "hello: formatted [%-5s] [%05.1f] [%#x] [%+d] [%c] [%d] [%d]", "ab", 3.14159, 255,
        42, 65, Int.min, "not a number"))

// open, openat, fcntl and ioctl are variadic in C, so these calls reach FeatherSupport's forms.
func callVariadicFunctions() -> String {
    let path = "/tmp/hello.txt"
    let writer = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0o644)
    _ = write(writer, "feather", 7)
    close(writer)
    let reader = openat(AT_FDCWD, path, O_RDONLY | O_CLOEXEC)
    var bytes = [UInt8](repeating: 0, count: 16)
    let count = read(reader, &bytes, bytes.count)
    let text = String(decoding: bytes[..<max(count, 0)], as: UTF8.self)
    let closesOnExec = fcntl(reader, F_GETFD) == FD_CLOEXEC
    let cleared = fcntl(reader, F_SETFD, 0) == 0 && fcntl(reader, F_GETFD) == 0
    var size = winsize()
    errno = 0
    let onFile = ioctl(reader, UInt(TIOCGWINSZ), &size) == -1 ? "\(SystemError.current)" : "none"
    close(reader)
    unlink(path)
    let onTerminal = ioctl(STDOUT_FILENO, UInt(TIOCGWINSZ), &size) == 0 && size.ws_row > 0
    return "read \(text), close-on-exec \(closesOnExec) then \(!cleared), ioctl on a file \(onFile), "
        + "on the terminal \(onTerminal)"
}

print("hello: \(callVariadicFunctions())")
let message: StaticString = "hello: written with write\n"
_ = write(STDOUT_FILENO, message.utf8Start, message.utf8CodeUnitCount)
