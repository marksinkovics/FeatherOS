// Host tests for src/user/life, compiled into one module with its sources but main.swift.

#if canImport(Darwin)
    import Darwin
#elseif canImport(Glibc)
    import Glibc
#endif

nonisolated(unsafe) var failures = 0
nonisolated(unsafe) var checks = 0

func check(_ condition: Bool, _ what: @autoclosure () -> String, line: Int = #line) {
    checks += 1
    if !condition {
        failures += 1
        print("life_tests.swift:\(line): FAILED: \(what())")
    }
}

func bytes(_ text: String) -> [UInt8] {
    Array(text.utf8)
}

// The same numbers on every run, unlike the system generator.
struct SplitMix: RandomNumberGenerator {
    var state: UInt64

    mutating func next() -> UInt64 {
        state &+= 0x9E37_79B9_7F4A_7C15
        var value = state
        value = (value ^ (value >> 30)) &* 0xBF58_476D_1CE4_E5B9
        value = (value ^ (value >> 27)) &* 0x94D0_49BB_1331_11EB
        return value ^ (value >> 31)
    }
}

func grid(_ pattern: Pattern, _ columns: Int, _ rows: Int, seed: UInt64 = 1) -> Grid {
    var grid = Grid(GridSize(columns: columns, rows: rows)!)
    var generator = SplitMix(state: seed)
    pattern.place(on: &grid, using: &generator)
    return grid
}

func picture(_ grid: Grid) -> String {
    var output: [UInt8] = []
    for row in 0..<grid.rows {
        grid.appendRow(row, live: UInt8(ascii: "#"), dead: UInt8(ascii: "."), to: &output)
        output.append(UInt8(ascii: "\n"))
    }
    return String(decoding: output, as: UTF8.self)
}

func advanced(_ grid: Grid, by generations: Int) -> Grid {
    var grid = grid
    for _ in 0..<generations {
        grid.advance()
    }
    return grid
}

func testSizesAndCounts() {
    check(GridSize(parsing: bytes("80x23")) == GridSize(columns: 80, rows: 23), "80x23")
    check(GridSize(parsing: bytes("3x1000")) != nil, "smallest and largest")
    check(GridSize(parsing: bytes("2x10")) == nil, "too narrow")
    check(GridSize(parsing: bytes("10x1001")) == nil, "too tall")
    check(GridSize(parsing: bytes("10")) == nil, "no cross")
    check(GridSize(parsing: bytes("x10")) == nil, "no columns")
    check(GridSize(parsing: bytes("10x")) == nil, "no rows")
    check(GridSize(parsing: bytes("10x10x10")) == nil, "two crosses")
    check(GridSize(parsing: bytes("-5x10")) == nil, "negative")
    check(parseCount(bytes("0")[...]) == 0, "zero")
    check(parseCount(bytes("1000000000")[...]) == 1_000_000_000, "a billion")
    check(parseCount(bytes("1000000001")[...]) == nil, "over a billion")
    check(parseCount(bytes("99999999999")[...]) == nil, "too many digits")
    check(parseCount(bytes("")[...]) == nil, "empty")
    check(parseCount(bytes("+1")[...]) == nil, "sign")
}

func testPatterns() {
    check(Pattern(named: bytes("glider")) == .glider, "glider")
    check(Pattern(named: bytes("blinker")) == .blinker, "blinker")
    check(Pattern(named: bytes("random")) == .random, "random")
    check(Pattern(named: bytes("Glider")) == nil, "names are lower case")
    check(Pattern(named: bytes("")) == nil, "empty name")
    check(
        picture(grid(.glider, 5, 5)) == ".....\n..#..\n...#.\n.###.\n.....\n",
        "glider in the middle")
    check(picture(grid(.blinker, 4, 3)) == "....\n###.\n....\n", "blinker left of the middle")
    let random = grid(.random, 100, 100)
    check((2000...3000).contains(random.population), "about a quarter alive at random")
    check(
        picture(random) == picture(grid(.random, 100, 100)), "the same generator, the same cells")
    check(
        picture(random) != picture(grid(.random, 100, 100, seed: 2)),
        "another seed, other cells")
}

func testGenerations() {
    let blinker = grid(.blinker, 5, 5)
    check(picture(advanced(blinker, by: 1)) == ".....\n..#..\n..#..\n..#..\n.....\n", "blinker turns")
    check(picture(advanced(blinker, by: 2)) == picture(blinker), "blinker back after two")
    var block = Grid(GridSize(columns: 4, rows: 4)!)
    for (column, row) in [(1, 1), (2, 1), (1, 2), (2, 2)] {
        block[column, row] = true
    }
    check(picture(advanced(block, by: 5)) == picture(block), "a block stays")
    var lonely = Grid(GridSize(columns: 4, rows: 4)!)
    lonely[1, 1] = true
    lonely[2, 1] = true
    check(advanced(lonely, by: 1).population == 0, "two cells die")
    check(
        picture(advanced(grid(.glider, 6, 6), by: 4))
            == "......\n......\n...#..\n....#.\n..###.\n......\n",
        "a glider moves one cell down and right in four")
    let glider = grid(.glider, 7, 7)
    check(picture(advanced(glider, by: 28)) == picture(glider), "a glider wraps around")
    check(advanced(glider, by: 13).population == 5, "a glider keeps five cells across edges")
}

func testEdgesAndRows() {
    var corner = Grid(GridSize(columns: 5, rows: 4)!)
    corner[-1, -1] = true
    check(corner[4, 3], "negative coordinates wrap")
    corner[5, 4] = true
    check(corner[0, 0] && corner.population == 2, "coordinates past the end wrap")
    // Three cells around the corner make a fourth, each a neighbor across both edges.
    var across = Grid(GridSize(columns: 5, rows: 5)!)
    for (column, row) in [(4, 4), (0, 4), (4, 0)] {
        across[column, row] = true
    }
    check(advanced(across, by: 1)[0, 0], "a birth across both edges")
    let blinker = grid(.blinker, 5, 5)
    let turned = advanced(blinker, by: 1)
    check(blinker.rowEquals(0, in: turned) && blinker.rowEquals(4, in: turned), "empty rows")
    check(!blinker.rowEquals(1, in: turned) && !blinker.rowEquals(2, in: turned), "changed rows")
    check(!blinker.rowEquals(0, in: Grid(GridSize(columns: 5, rows: 6)!)), "another size")
}

@main
struct LifeTests {
    static func main() {
        testSizesAndCounts()
        testPatterns()
        testGenerations()
        testEdgesAndRows()
        print("\(checks) Life checks, \(failures) failed")
        exit(failures == 0 ? 0 : 1)
    }
}
