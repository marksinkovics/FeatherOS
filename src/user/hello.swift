import FeatherLibC

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

func arguments() -> [String] {
    let values = featheros_argument_values()!
    return (1..<Int(featheros_argument_count())).map { String(cString: values[$0]!) }
}

print("Hello from Embedded Swift")
var line = "hello: arguments"
for (index, argument) in arguments().enumerated() {
    line += index == 0 ? " " : ", "
    line += argument
}
print(line)
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
let message: StaticString = "hello: written with write\n"
_ = write(STDOUT_FILENO, message.utf8Start, message.utf8CodeUnitCount)
