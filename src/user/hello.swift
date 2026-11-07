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

struct Version: TextOutputStreamable {
    func write<Target: TextOutputStream>(to target: inout Target) {
        target.write("6.4.2")
    }
}

var printed = ""
print("items", 3, 2.5, true, String(describing: Version()), separator: "|", to: &printed)
print("hello: printed", printed, terminator: "")

let hashables: Set<AnyHashable> = [1, "1", AnyHashable(1), AnyHashable(AnyHashable("1")), 2.5]
let labels: [AnyHashable: String] = [1: "an Int", "1": "a String"]
let base = AnyHashable(7).base as? Int ?? -1
print("hello: \(hashables.count) hashables, \"1\" is \(labels["1"] ?? "missing"), base \(base)")

// More integers and doubles than registers, so some go on the stack. Each element is converted
// on its own, since converting a whole [Int] to [CVarArg] would be a dynamic cast.
let integers = (1...8).map { $0 as CVarArg }
let doubles = (1...9).map { Double($0) / 2 as CVarArg }
let varArgs = withVaList(integers + doubles) { arguments in
    var buffer = [CChar](repeating: 0, count: 128)
    _ = vsnprintf(&buffer, buffer.count, "%d %d %d %d %d %d %d %d, %g %g %g %g %g %g %g %g %g",
                  arguments)
    return String(cString: buffer)
}
print("hello: withVaList \(varArgs)")
let message: StaticString = "hello: written with write\n"
_ = write(STDOUT_FILENO, message.utf8Start, message.utf8CodeUnitCount)

// Conformances the compiler synthesizes, over FeatherOS's Codable.
enum Phase: String, Codable {
    case planned, done
}

enum Step: Codable, Equatable {
    case build(arch: String)
    case boot(seconds: Double?)
}

struct Milestone: Codable, Equatable {
    var number: Int
    var phase: Phase
    var steps: [Step]
    var note: String?
    var counts: [String: UInt8]
}

let milestone = Milestone(
    number: 25, phase: .done, steps: [.build(arch: "aarch64"), .boot(seconds: 1.5)], note: nil,
    counts: ["parts": 8])
do {
    let json = try JSONEncoder().encode(milestone)
    let decoded = try JSONDecoder().decode(Milestone.self, from: json)
    print("hello: encoded \(String(decoding: json, as: UTF8.self)), alike \(decoded == milestone)")
    _ = try JSONDecoder().decode(Milestone.self, from: Array(#"{"number":25,"phase":"late"}"#.utf8))
} catch let error as DecodingError {
    print("hello: \(error)")
} catch {
    print("hello: encoding failed")
}
