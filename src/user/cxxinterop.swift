import CxxInterop
import Cxx

// Calls C++ from cxx_interop.h, built by GCC, and libc++ types from cxx_interop_std.h, and
// prints what each kind of C++ declaration does in Swift.

typealias Interop = FeatherOS.User.Interop

let next = Interop.NextColor(.Red)
print("cxxinterop: constant \(Interop.Answer), the color after red is green \(next == .Green)")

let sum = Interop.Point(x: 1, y: 2) + Interop.Point(x: 3, y: 4)
print("cxxinterop: point sum (\(sum.x), \(sum.y)), equal \(sum == Interop.Point(x: 4, y: 6))")

let box = Interop.IntBox(value: 7)
print("cxxinterop: template box holds \(box.Get())")

do {
    let first = Interop.Tracked(1)
    var second = first
    second = Interop.Tracked(2)
    print("cxxinterop: tracked \(first.Id()) and \(second.Id()), live \(Interop.Tracked.Live())")
}
print("cxxinterop: tracked live after the scope \(Interop.Tracked.Live())")

func bumped(_ counter: consuming Interop.Counter) -> Interop.Counter {
    counter.Bump()
    return counter
}
do {
    var counter = Interop.Counter()
    counter.Bump()
    counter = bumped(counter)
    print("cxxinterop: move-only counter \(counter.Count())")
}

let square = Interop.Square(3)
print("cxxinterop: square area \(square.Area())")

do {
    let node = Interop.Node.Create(5)!
    let other = node
    print("cxxinterop: node value \(other.Value()), live \(Interop.Node.Live())")
}
print("cxxinterop: nodes live after the scope \(Interop.Node.Live())")

let half = Interop.Half(8).value
let none = Interop.Half(7).value
print("cxxinterop: optional half of 8 \(half ?? -1), of 7 nil \(none == nil)")

let values = Array(Interop.Squares(5))
let total = values.withUnsafeBufferPointer { Interop.Sum(Interop.IntSpan($0)) }
var listed = ""
for value in Interop.Squares(3) {
    listed += "\(value) "
}
print("cxxinterop: vector \(listed)of \(values.count), sum through a span \(total)")

print("cxxinterop: vowels in featheros \(Interop.CountVowels(std.string_view("featheros")))")
