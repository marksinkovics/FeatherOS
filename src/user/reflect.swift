// Full Swift: everything here needs the type records the compiler writes and the runtime reads
// while the program runs, so Embedded Swift rejects it.

protocol Shape {
    var area: Double { get }
    static var sides: Int { get }
}

protocol Named {
    var name: String { get }
}

struct Square: Shape, Named {
    let side: Double
    var area: Double { side * side }
    static var sides: Int { 4 }
    var name: String { "square" }
}

struct Circle: Shape {
    let radius: Double
    var area: Double { 3 * radius * radius }
    static var sides: Int { 0 }
}

struct Pair<Element> {
    let first: Element
    let second: Element
}

extension Pair: Shape where Element: Shape {
    var area: Double { first.area + second.area }
    static var sides: Int { Element.sides * 2 }
}

enum Direction: Hashable {
    case north, south
}

enum ParseError: Error {
    case empty
    case badCharacter(Character, at: Int)
}

struct Point: Hashable {
    var x: Int
    var y: Int
}

struct Line {
    var start: Point
    var end: Point
    var label: String?
}

struct Temperature: CustomReflectable {
    let kelvin: Int
    var customMirror: Mirror {
        Mirror(self, children: ["celsius": kelvin - 273], displayStyle: .struct)
    }
}

class Animal: Named {
    let name: String
    var score = 0

    required init(name: String) {
        self.name = name
    }

    func greeting<Listener>(to listener: Listener) -> String {
        "\(name) looks at \(listener)"
    }
}

final class Dog: Animal {
    var tricks = ["sit", "roll"]

    override func greeting<Listener>(to listener: Listener) -> String {
        "\(name) wags at \(listener)"
    }
}

class Container<Element> {
    var items: [Element] = []

    class var sorts: Bool { false }

    func add(_ item: Element) {
        items.append(item)
    }

    func summary() -> String {
        "\(type(of: self)) of \(items)\(Self.sorts ? ", sorted" : "")"
    }
}

final class SortedContainer<Element: Comparable>: Container<Element> {
    override class var sorts: Bool { true }

    override func add(_ item: Element) {
        items.insert(item, at: items.firstIndex { $0 > item } ?? items.endIndex)
    }
}

final class Node {
    let name: String
    weak var parent: Node?
    weak var friend: (any Named & AnyObject)?
    var children: [Node] = []

    init(name: String) {
        self.name = name
    }

    deinit {
        print("reflect: deinit \(name)")
    }
}

final class Owner {
    let name: String
    var pet: Pet?

    init(name: String) {
        self.name = name
    }
}

final class Pet {
    unowned let owner: Owner

    init(owner: Owner) {
        self.owner = owner
    }
}

@inline(never)
func classify(_ value: Any) -> String {
    switch value {
    case let shape as any Shape & Named:
        return "\(shape.name) of \(shape.area)"
    case let shape as any Shape:
        return "\(type(of: shape)) of \(shape.area), \(type(of: shape).sides) sides"
    case let numbers as [Int]:
        return "numbers summing to \(numbers.reduce(0, +))"
    case let number as Int:
        return "int \(number)"
    case let collection as any Collection:
        return "collection of \(collection.count)"
    default:
        return "other \(type(of: value))"
    }
}

func parse(_ text: String) throws -> Int {
    if text.isEmpty {
        throw ParseError.empty
    }
    for (offset, character) in text.enumerated() where !character.isNumber {
        throw ParseError.badCharacter(character, at: offset)
    }
    return Int(text)!
}

func children(_ mirror: Mirror) -> String {
    mirror.children.map { "\($0.label ?? "_")=\($0.value)" }.joined(separator: " ")
}

func make(_ kind: Animal.Type, named name: String) -> Animal {
    kind.init(name: name)
}

// Casts to protocol types, concrete generic types, arrays and metatypes, and errors.
let values: [Any] = [
    Square(side: 2), Circle(radius: 1), Pair(first: Square(side: 1), second: Square(side: 3)),
    Pair(first: 1, second: 2), [1, 2, 3], [1, 2] as [Any], Int?.some(7) as Any, "feather",
]
for value in values {
    print("reflect: cast \(classify(value))")
}
let kinds: [Any.Type] = [Square.self, Int.self, Pair<Circle>.self, Pair<Int>.self]
print("reflect: sides", kinds.map { kind in
    "\(kind) \((kind as? any Shape.Type).map { "\($0.sides)" } ?? "none")"
}.joined(separator: ", "))
let animals: [AnyObject] = [Dog(name: "Rex"), Animal(name: "Tom"), Node(name: "lone")]
print("reflect: named", animals.map { ($0 as? any Named)?.name ?? "\(type(of: $0))" },
      "dogs", animals.filter { $0 is Dog }.count)
var outcomes: [String] = []
for text in ["42", "", "4x"] {
    do {
        outcomes.append("\(try parse(text))")
    } catch let error as ParseError {
        outcomes.append("\(error)")
    } catch {
        outcomes.append("unexpected \(error)")
    }
}
print("reflect: parsed", outcomes.joined(separator: ", "))
let collections: [any Collection<Int>] = [[1, 2], Set([3]), 1...4]
print("reflect: counts", collections.map(\.count), "sum",
      collections.map { $0.reduce(0, +) }.reduce(0, +))

// Mirror and dump.
let line = Line(start: Point(x: 0, y: 0), end: Point(x: 3, y: 4), label: "diagonal")
let lineMirror = Mirror(reflecting: line)
print("reflect: mirror", lineMirror.displayStyle!, children(lineMirror))
let rex = Dog(name: "Rex")
let rexMirror = Mirror(reflecting: rex)
print("reflect: mirror", rexMirror.displayStyle!, children(rexMirror), "super",
      rexMirror.superclassMirror.map(children) ?? "none")
print("reflect: mirror", children(Mirror(reflecting: ParseError.badCharacter("x", at: 1))),
      children(Mirror(reflecting: (1, label: "a"))),
      children(Mirror(reflecting: Temperature(kelvin: 300))))
dump(rex)

// Key paths as values.
var point = Point(x: 1, y: 2)
let coordinates: [WritableKeyPath<Point, Int>] = [\.x, \.y]
for path in coordinates {
    point[keyPath: path] *= 10
}
let endY = (\Line.end).appending(path: \Point.y)
let parts: [PartialKeyPath<Line>] = [\.start, \.label, \.label?.count]
let lines = [line, Line(start: point, end: Point(x: -1, y: 5), label: nil)]
print("reflect: keypaths", point, line[keyPath: endY], parts.map { "\(line[keyPath: $0])" },
      lines.map(\.end.x))
let scorePath: ReferenceWritableKeyPath<Animal, Int> = \.score
rex[keyPath: scorePath] += 5
let pathSet: Set<AnyKeyPath> = [\Point.x, \Point.y, (\Point.self).appending(path: \.x)]
print("reflect: keypaths score", rex.score, "distinct", pathSet.count,
      "equal", \Line.end.y == endY)

// weak and unowned references.
var root: Node? = Node(name: "root")
let leaf = Node(name: "leaf")
leaf.parent = root
leaf.friend = rex
root!.children.append(leaf)
print("reflect: weak parent", leaf.parent?.name ?? "nil", "friend", leaf.friend?.name ?? "nil")
root = nil
print("reflect: weak parent", leaf.parent?.name ?? "nil")
var owner: Owner? = Owner(name: "Ann")
owner!.pet = Pet(owner: owner!)
let pet = owner!.pet!
var tick: (() -> Void)? = nil
do {
    let counter = Animal(name: "counter")
    tick = { [weak counter] in counter?.score += 1 }
    tick!()
    print("reflect: unowned owner", pet.owner.name, "captured score", counter.score)
}
tick!()
owner = nil

// Non-final generic class methods.
let listeners: [Animal] = [Animal(name: "Tom"), rex]
print("reflect: generic", listeners.map { $0.greeting(to: point) }.joined(separator: ", "))
let containers: [Container<Int>] = [Container(), SortedContainer()]
for container in containers {
    [3, 1, 2].forEach(container.add)
    print("reflect: generic", container.summary())
}
let made = make(Dog.self, named: "Fido")
print("reflect: made", type(of: made), made.name, (made as? Dog)?.tricks ?? [])

// String(describing:) and String(reflecting:) for any value.
let sampleGroups: [[Any]] = [
    [Direction.north, point, (1, "two"), Int?.some(3) as Any, Int?.none as Any],
    [Pair(first: 1.5, second: 2.5), [Direction.south: [1]], Int.self, Pair<String>.self],
    [leaf, { (number: Int) in number }, AnyHashable(Direction.south), ParseError.empty as Error],
]
for samples in sampleGroups {
    print("reflect: describing", samples.map { String(describing: $0) }.joined(separator: " | "))
}
print("reflect: reflecting", String(reflecting: point), String(reflecting: Direction.south),
      String(reflecting: Pair<Point>.self))
