import FeatherLibC
import FeatherSupport

struct Checker {
    var name = ""
    var failure: String?

    mutating func begin(_ name: String) {
        self.name = name
    }

    mutating func expect(_ condition: Bool, _ detail: @autoclosure () -> String) {
        if failure == nil && !condition {
            failure = "\(name): \(detail())"
        }
    }
}

struct TestMap {
    var cells: [UInt8]
    var width: Int
    var height: Int
    var viewerX = 0
    var viewerY = 0

    init(_ rows: [String]) {
        height = rows.count
        width = rows[0].utf8.count
        cells = []
        for (y, row) in rows.enumerated() {
            for (x, character) in row.utf8.enumerated() {
                switch character {
                case UInt8(ascii: "#"):
                    cells.append(Terrain.wall.rawValue)
                case UInt8(ascii: "@"):
                    viewerX = x
                    viewerY = y
                    cells.append(Terrain.floor.rawValue)
                default:
                    cells.append(Terrain.floor.rawValue)
                }
            }
        }
    }

    func rendered(radius: Int) -> [String] {
        let lit = fieldOfView(
            cells, width: width, height: height, x: viewerX, y: viewerY, radius: radius)
        var rows: [String] = []
        for y in 0..<height {
            var row: [UInt8] = []
            for x in 0..<width {
                let index = y * width + x
                if x == viewerX && y == viewerY {
                    row.append(UInt8(ascii: "@"))
                } else if lit[index] == 0 {
                    row.append(UInt8(ascii: " "))
                } else {
                    row.append(
                        cells[index] == Terrain.wall.rawValue
                            ? UInt8(ascii: "#") : UInt8(ascii: "."))
                }
            }
            rows.append(String(decoding: row, as: UTF8.self))
        }
        return rows
    }
}

func joined(_ rows: [String]) -> String {
    rows.joined(separator: "|")
}

func openRoomGame(oil: Int) -> Game {
    var game = Game(seed: 1)
    game.cells = [UInt8](repeating: Terrain.wall.rawValue, count: mapWidth * mapHeight)
    for y in 1..<mapHeight - 1 {
        for x in 1..<mapWidth - 1 {
            game.cells[y * mapWidth + x] = Terrain.floor.rawValue
        }
    }
    game.x = 30
    game.y = 20
    game.oil = oil
    game.remembered = [UInt8](repeating: 0, count: mapWidth * mapHeight)
    game.refreshSight()
    return game
}

func litCount(_ game: Game) -> Int {
    game.lit.reduce(0) { $0 + Int($1) }
}

func checkRandom(_ check: inout Checker) {
    check.begin("random")
    var first = Random(seed: 1)
    check.expect(first.next() == 10_451_216_379_200_822_465, "first value of seed 1")
    check.expect(first.next() == 13_757_245_211_066_428_519, "second value of seed 1")
    var left = Random(seed: 77)
    var right = Random(seed: 77)
    var same = true
    var inRange = true
    for _ in 0..<100 {
        same = same && left.next() == right.next()
        inRange = inRange && left.below(6) < 6
        _ = right.below(6)
    }
    check.expect(same, "equal seeds diverged")
    check.expect(inRange, "below left its range")
}

func floorHash(_ floor: Floor) -> UInt64 {
    var hash = Fnv1a()
    hash.add(floor.cells)
    return hash.value
}

func checkGenerator(_ check: inout Checker) {
    check.begin("generator")
    let expected: [(UInt64, Int, Int, Int, Int, Int, Int, UInt64)] = [
        (1, 1, 37, 35, 8, 5, 491, 5_195_491_783_893_924_868),
        (1, 8, 2, 16, 49, 6, 569, 8_305_767_626_976_444_327),
        (2, 1, 8, 8, 5, 34, 373, 12_810_979_750_081_832_602),
        (50, 5, 56, 22, 10, 8, 507, 10_313_512_887_310_125_770),
    ]
    for row in expected {
        let floor = generateFloor(seed: row.0, depth: row.1)
        let actual =
            "start \(floor.startX),\(floor.startY) exit \(floor.exitX),\(floor.exitY) "
            + "passable \(passableCount(floor.cells)) hash \(floorHash(floor))"
        let wanted =
            "start \(row.2),\(row.3) exit \(row.4),\(row.5) passable \(row.6) hash \(row.7)"
        check.expect(actual == wanted, "seed \(row.0) depth \(row.1): \(actual)")
    }
    let again = generateFloor(seed: 1, depth: 1)
    check.expect(floorHash(again) == floorHash(generateFloor(seed: 1, depth: 1)), "not repeatable")
    check.expect(floorHash(again) != floorHash(generateFloor(seed: 1, depth: 2)), "depths equal")
    check.expect(floorHash(again) != floorHash(generateFloor(seed: 2, depth: 1)), "seeds equal")
}

func checkFloor(_ check: inout Checker, seed: UInt64, depth: Int) {
    let floor = generateFloor(seed: seed, depth: depth)
    let label = "seed \(seed) depth \(depth)"
    check.expect(floor.cells.count == mapWidth * mapHeight, "\(label): size")
    var border = true
    for x in 0..<mapWidth {
        border = border && floor.cells[x] == 0 && floor.cells[(mapHeight - 1) * mapWidth + x] == 0
    }
    for y in 0..<mapHeight {
        border = border && floor.cells[y * mapWidth] == 0
            && floor.cells[y * mapWidth + mapWidth - 1] == 0
    }
    check.expect(border, "\(label): the border is not wall")
    check.expect(floor.rooms.count == sectorColumns * sectorRows, "\(label): room count")
    var shapes = true
    for (number, room) in floor.rooms.enumerated() {
        let column = number % sectorColumns
        let row = number / sectorColumns
        let sizes =
            room.junction
            ? room.width == 1 && room.height == 1
            : room.width >= 4 && room.width <= 11 && room.height >= 3 && room.height <= 7
        shapes = shapes && sizes
            && room.x > column * sectorWidth && room.x + room.width < (column + 1) * sectorWidth
            && room.y > row * sectorHeight && room.y + room.height < (row + 1) * sectorHeight
    }
    check.expect(shapes, "\(label): a room is out of its sector or size")
    let start = floor.rooms[floor.startRoom]
    check.expect(!start.junction && !floor.rooms[floor.exitRoom].junction, "\(label): junction")
    check.expect(floor.startRoom != floor.exitRoom, "\(label): start room is the exit room")
    check.expect(
        floor.startX >= start.x && floor.startX < start.x + start.width
            && floor.startY >= start.y && floor.startY < start.y + start.height,
        "\(label): start outside its room")
    let distance = distances(
        floor.cells, width: mapWidth, height: mapHeight, fromX: floor.startX, fromY: floor.startY)
    check.expect(
        reachableCount(distance) == passableCount(floor.cells), "\(label): a cell is cut off")
    let withoutRubble = floor.cells.map {
        $0 == Terrain.rubble.rawValue ? Terrain.floor.rawValue : $0
    }
    let open = distances(
        withoutRubble, width: mapWidth, height: mapHeight, fromX: floor.startX,
        fromY: floor.startY)
    check.expect(
        distance[floor.exitY * mapWidth + floor.exitX] > 0, "\(label): the exit is unreachable")
    var exits = 0
    var rubble = 0
    for cell in floor.cells {
        exits += cell == Terrain.stairs.rawValue || cell == Terrain.plinth.rawValue ? 1 : 0
        rubble += cell == Terrain.rubble.rawValue ? 1 : 0
    }
    let wanted = depth == finalDepth ? Terrain.plinth.rawValue : Terrain.stairs.rawValue
    check.expect(
        exits == 1 && floor.cells[floor.exitY * mapWidth + floor.exitX] == wanted,
        "\(label): exit cell")
    check.expect(rubble <= 5, "\(label): \(rubble) rubble")
    let center = floor.rooms[floor.exitRoom]
    var farthest = true
    for (number, room) in floor.rooms.enumerated() where !room.junction && number != floor.startRoom {
        farthest =
            farthest
            && open[room.centerY * mapWidth + room.centerX]
                <= open[center.centerY * mapWidth + center.centerX]
    }
    check.expect(farthest, "\(label): the exit room is not the farthest")
}

func checkConnectivity(_ check: inout Checker) {
    check.begin("floors")
    for seed in 1...50 {
        for depth in 1...finalDepth {
            checkFloor(&check, seed: UInt64(seed), depth: depth)
            if check.failure != nil {
                return
            }
        }
    }
    var open = [UInt8](repeating: 1, count: 25)
    open[12] = Terrain.rubble.rawValue
    let around = distances(open, width: 5, height: 5, fromX: 0, fromY: 0)
    check.expect(around[24] == 5 && around[12] == -1, "rubble does not block distances")
    let pinched = TestMap(["#####", "#@#.#", "##..#", "#####"])
    check.expect(
        !canStep(
            pinched.cells, width: 5, height: 4, x: 1, y: 1, dx: 1, dy: 1),
        "a diagonal squeezed past a wall")
}

func checkShadowcasting(_ check: inout Checker) {
    check.begin("pillar")
    let pillar = TestMap([
        "###############",
        "#.............#",
        "#.............#",
        "#.............#",
        "#.@..#........#",
        "#.............#",
        "#.............#",
        "#.............#",
        "###############",
    ])
    let shadow = pillar.rendered(radius: 6)
    check.expect(
        shadow == [
            "########       ",
            "#.......       ",
            "#........      ",
            "#........      ",
            "#.@..#         ",
            "#........      ",
            "#........      ",
            "#.......       ",
            "########       ",
        ], joined(shadow))

    check.begin("corridor")
    let corridor = TestMap([
        "##########",
        "#@.......#",
        "########.#",
        "########.#",
        "##########",
    ])
    let short = corridor.rendered(radius: 3)
    check.expect(
        short == [
            "#####     ",
            "#@...     ",
            "#####     ",
            "          ",
            "          ",
        ], joined(short))
    let bend = TestMap([
        "#####",
        "#@..#",
        "###.#",
        "###.#",
        "#####",
    ])
    let around = bend.rendered(radius: 6)
    check.expect(
        around == ["#####", "#@..#", "###.#", "     ", "     "], joined(around))

    check.begin("open room")
    let expectedCounts = [9, 21, 37, 69, 97, 137]
    var rows = [String](repeating: "#" + String(repeating: ".", count: 21) + "#", count: 21)
    rows.insert(String(repeating: "#", count: 23), at: 0)
    rows.append(String(repeating: "#", count: 23))
    var room = TestMap(rows)
    room.viewerX = 11
    room.viewerY = 11
    for radius in 1...6 {
        let lit = fieldOfView(
            room.cells, width: room.width, height: room.height, x: 11, y: 11, radius: radius)
        let count = lit.reduce(0) { $0 + Int($1) }
        check.expect(count == expectedCounts[radius - 1], "radius \(radius) lit \(count) cells")
        var exact = true
        for y in 0..<room.height {
            for x in 0..<room.width {
                let dx = x - 11
                let dy = y - 11
                let inside = dx * dx + dy * dy <= radius * radius + radius
                exact = exact && (lit[y * room.width + x] != 0) == inside
            }
        }
        check.expect(exact, "radius \(radius) is not the disc dx*dx + dy*dy <= r*r + r")
    }
    var views = [[UInt8]](repeating: [], count: room.width * room.height)
    for y in 1...21 {
        for x in 1...21 {
            views[y * room.width + x] = fieldOfView(
                room.cells, width: room.width, height: room.height, x: x, y: y, radius: 6)
        }
    }
    var symmetric = true
    for first in 0..<room.width * room.height where !views[first].isEmpty {
        for second in 0..<room.width * room.height where !views[second].isEmpty {
            symmetric = symmetric && views[first][second] == views[second][first]
        }
    }
    check.expect(symmetric, "a sees b but b does not see a in an open room")
    let center = views[11 * room.width + 11]
    var mirrored = true
    for y in 0..<room.height {
        for x in 0..<room.width {
            let here = center[y * room.width + x]
            mirrored = mirrored && here == center[y * room.width + 22 - x]
                && here == center[(22 - y) * room.width + x] && here == center[x * room.width + y]
        }
    }
    check.expect(mirrored, "the view from the center is not mirrored")
}

func checkLamp(_ check: inout Checker) {
    check.begin("lamp")
    let radii: [(Int, Int)] = [
        (1000, 6), (500, 6), (499, 5), (200, 5), (199, 3), (50, 3), (49, 2), (1, 2), (0, 1),
    ]
    for (oil, radius) in radii {
        check.expect(lampRadius(oil: oil) == radius, "oil \(oil) gave radius \(lampRadius(oil: oil))")
    }
    var game = openRoomGame(oil: 501)
    check.expect(litCount(game) == 137, "radius 6 lit \(litCount(game))")
    _ = game.step(.wait)
    check.expect(game.oil == 500 && litCount(game) == 137, "at 500 oil")
    let dimmed = game.step(.wait)
    check.expect(
        game.oil == 499 && litCount(game) == 97
            && dimmed == [Event(code: .lampDimmed, first: 5)],
        "at 499 oil: \(litCount(game)) lit, \(dimmed.count) events")
    game.oil = 200
    _ = game.step(.wait)
    check.expect(game.oil == 199 && litCount(game) == 37, "at 199 oil")
    game.oil = 50
    _ = game.step(.wait)
    check.expect(game.oil == 49 && litCount(game) == 21, "at 49 oil")
    game.oil = 2
    _ = game.step(.wait)
    check.expect(game.oil == 1 && litCount(game) == 21, "at 1 oil")
    let out = game.step(.wait)
    check.expect(
        game.oil == 0 && litCount(game) == 9 && out == [Event(code: .lampOut)], "lamp out")
    let after = game.step(.wait)
    check.expect(game.oil == 0 && after.isEmpty, "oil went below 0 or the lamp went out twice")
    check.expect(
        game.remembered.reduce(0) { $0 + Int($1) } > litCount(game), "nothing was remembered")
}

func checkMovement(_ check: inout Checker) {
    check.begin("movement")
    var game = openRoomGame(oil: 100)
    let start = (game.x, game.y)
    let moved = game.step(.move(.northEast))
    check.expect(
        moved.isEmpty && game.x == start.0 + 1 && game.y == start.1 - 1 && game.turn == 1
            && game.oil == 99,
        "a diagonal move: \(game.x),\(game.y) turn \(game.turn) oil \(game.oil)")
    game.x = 30
    game.y = 20
    game.cells[20 * mapWidth + 31] = Terrain.wall.rawValue
    let wall = game.step(.move(.east))
    check.expect(
        wall == [Event(code: .wallAhead, first: 31, second: 20)] && game.x == 30 && game.turn == 1
            && game.oil == 99,
        "a wall took a turn or moved")
    let squeeze = game.step(.move(.northEast))
    check.expect(
        squeeze == [Event(code: .wallAhead, first: 31, second: 19)] && game.x == 30,
        "a diagonal went past a wall")
    game.cells[20 * mapWidth + 29] = Terrain.rubble.rawValue
    let rubble = game.step(.move(.west))
    check.expect(
        rubble == [Event(code: .rubbleAhead, first: 29, second: 20)] && game.x == 30,
        "rubble was entered")
    let pastRubble = game.step(.move(.northWest))
    check.expect(pastRubble.isEmpty && game.x == 29 && game.y == 19, "a diagonal past rubble")
    game.x = 30
    game.y = 20
    let noStairs = game.step(.descend)
    check.expect(
        noStairs == [Event(code: .noStairs)] && game.depth == 1 && game.turn == 2,
        "descending without stairs")
    check.expect(game.recent.count == 3 && game.recent[2] == Event(code: .noStairs), "recent")
    let waited = game.step(.wait)
    check.expect(waited.isEmpty && game.turn == 3 && game.oil == 97, "waiting")
    game.cells[20 * mapWidth + 30] = Terrain.stairs.rawValue
    let down = game.step(.descend)
    let next = generateFloor(seed: 1, depth: 2)
    check.expect(
        down == [Event(code: .descended, first: 2)] && game.depth == 2 && game.score == 100
            && game.x == next.startX && game.y == next.startY && game.cells == next.cells
            && game.turn == 4 && game.oil == 96,
        "descending: depth \(game.depth) at \(game.x),\(game.y)")
    check.expect(
        litCount(game) > 1 && game.remembered == game.lit, "the new floor remembers its view")
    game.depth = finalDepth
    game.cells[game.y * mapWidth + game.x + 1] = Terrain.plinth.rawValue
    let bottom = game.step(.move(.east))
    check.expect(
        bottom == [Event(code: .plinthReached, first: finalDepth)] && game.outcome == .bottom
            && game.score == 600,
        "reaching the plinth")
    let turn = game.turn
    check.expect(game.step(.wait).isEmpty && game.turn == turn, "a finished game took a turn")
}

func checkEnergy(_ check: inout Checker) {
    check.begin("energy")
    var turns = Turns(speeds: [12, 6, 18])
    var order: [Int] = []
    for _ in 0..<12 {
        order.append(turns.next())
    }
    check.expect(order == [0, 2, 0, 1, 2, 2, 0, 2, 0, 1, 2, 2], "order \(order)")
    var fast = Turns(speeds: [18, 12])
    order = []
    for _ in 0..<10 {
        order.append(fast.next())
    }
    check.expect(order == [0, 1, 0, 1, 0, 0, 1, 0, 1, 0], "a fast player: \(order)")
    var counts = [0, 0, 0]
    var long = Turns(speeds: [12, 6, 18])
    for _ in 0..<600 {
        counts[long.next()] += 1
    }
    check.expect(counts == [200, 100, 300], "shares \(counts)")
    var game = openRoomGame(oil: 1000)
    for _ in 0..<100 {
        _ = game.step(.wait)
    }
    check.expect(game.turn == 100 && game.turns.cursor == 1, "the player alone acts each step")
}

let scriptedDirections: [Direction] = [
    .east, .east, .southEast, .south, .west, .northWest, .north, .northEast,
]

func scriptedRun(seed: UInt64, steps: Int) -> (UInt64, [Event]) {
    var game = Game(seed: seed)
    var log: [Event] = []
    for number in 0..<steps {
        let command: Command =
            number % 7 == 3 ? .wait : (number % 11 == 5 ? .descend : .move(scriptedDirections[number % 8]))
        log.append(contentsOf: game.step(command))
    }
    return (game.stateHash(), log)
}

func checkDeterminism(_ check: inout Checker) {
    check.begin("determinism")
    let first = scriptedRun(seed: 5, steps: 300)
    let second = scriptedRun(seed: 5, steps: 300)
    let other = scriptedRun(seed: 6, steps: 300)
    check.expect(first.0 == second.0 && first.1 == second.1, "one seed gave two games")
    check.expect(first.0 != other.0, "two seeds gave the same game")
}

func runSelfTest() -> Bool {
    var check = Checker()
    checkRandom(&check)
    checkGenerator(&check)
    checkConnectivity(&check)
    checkShadowcasting(&check)
    checkLamp(&check)
    checkMovement(&check)
    checkEnergy(&check)
    checkDeterminism(&check)
    if let failure = check.failure {
        print("wickdeep: selftest failed: \(failure)")
        return false
    }
    print("wickdeep: selftest ok")
    return true
}
