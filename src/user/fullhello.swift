// Full Swift: the standard library's unspecialized generic code runs on this program's own types
// through the metadata the runtime builds for them.

struct Version: Comparable, Hashable, CustomStringConvertible {
    let major: Int
    let minor: Int

    static func < (left: Version, right: Version) -> Bool {
        (left.major, left.minor) < (right.major, right.minor)
    }

    var description: String { "\(major).\(minor)" }
}

struct Stack<Element> {
    private var items: [Element] = []

    mutating func push(_ item: Element) {
        items.append(item)
    }

    mutating func pop() -> Element? {
        items.popLast()
    }
}

func largest<Element: Comparable>(_ items: [Element]) -> Element? {
    items.max()
}

let arguments = CommandLine.arguments
if arguments.count == 2 && arguments[1] == "fail" {
    fatalError("fullhello: failed on purpose")
}

print("Hello from full Swift")
print("fullhello: arguments", arguments.dropFirst().joined(separator: ", "))

let versions = [
    Version(major: 6, minor: 4), Version(major: 5, minor: 10), Version(major: 6, minor: 0),
]
print("fullhello: sorted", versions.sorted().map { $0.description }.joined(separator: " "))

var counts: [Version: Int] = [:]
for version in versions + versions.prefix(1) {
    counts[version, default: 0] += 1
}
print("fullhello: 6.4 counted", counts[Version(major: 6, minor: 4)] ?? 0, "times of",
      counts.count, "versions")

var stack = Stack<Version>()
versions.forEach { stack.push($0) }
var popped: [String] = []
while let version = stack.pop() {
    popped.append(version.description)
}
print("fullhello: popped", popped.joined(separator: " "))

print("fullhello: largest", largest(versions)!, largest(["feather", "os"])!,
      largest([2.5, -1.0])!)
let words = Set(["feather", "os", "feather"])
print("fullhello: \(words.count) distinct words, sum \((1...100).reduce(0, +))")

// The capacity comes from malloc_usable_size, through the patched SwiftShims.
final class Buffer: ManagedBuffer<Int, UInt64> {}
let buffer = Buffer.create(minimumCapacity: 10) { _ in 0 }
print("fullhello: buffer capacity at least 10", buffer.capacity >= 10)
