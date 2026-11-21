let mapWidth = 60
let mapHeight = 40
let sectorColumns = 4
let sectorRows = 3
let sectorWidth = 15
let sectorHeight = 13
let finalDepth = 8
let junctionChance = 6
let extraLinks = 2

enum Terrain: UInt8 {
    case wall = 0
    case floor
    case rubble
    case stairs
    case plinth
}

enum Direction: Int {
    case north = 0
    case northEast
    case east
    case southEast
    case south
    case southWest
    case west
    case northWest

    var dx: Int {
        switch self {
        case .northEast, .east, .southEast: return 1
        case .southWest, .west, .northWest: return -1
        default: return 0
        }
    }

    var dy: Int {
        switch self {
        case .northWest, .north, .northEast: return -1
        case .southEast, .south, .southWest: return 1
        default: return 0
        }
    }
}

struct Room {
    var x: Int
    var y: Int
    var width: Int
    var height: Int
    var junction: Bool

    var centerX: Int { x + width / 2 }
    var centerY: Int { y + height / 2 }
}

struct Floor {
    var cells: [UInt8]
    var rooms: [Room]
    var startRoom: Int
    var exitRoom: Int
    var startX: Int
    var startY: Int
    var exitX: Int
    var exitY: Int
}

func blocksMovement(_ cell: UInt8) -> Bool {
    cell == Terrain.wall.rawValue || cell == Terrain.rubble.rawValue
}

// A diagonal step may not squeeze between two walls or past one.
func canStep(_ cells: [UInt8], width: Int, height: Int, x: Int, y: Int, dx: Int, dy: Int) -> Bool {
    let targetX = x + dx
    let targetY = y + dy
    guard targetX >= 0 && targetY >= 0 && targetX < width && targetY < height else {
        return false
    }
    if blocksMovement(cells[targetY * width + targetX]) {
        return false
    }
    if dx != 0 && dy != 0 {
        return cells[y * width + targetX] != Terrain.wall.rawValue
            && cells[targetY * width + x] != Terrain.wall.rawValue
    }
    return true
}

func distances(_ cells: [UInt8], width: Int, height: Int, fromX: Int, fromY: Int) -> [Int] {
    var distance = [Int](repeating: -1, count: width * height)
    var queue = [fromY * width + fromX]
    distance[queue[0]] = 0
    var head = 0
    while head < queue.count {
        let index = queue[head]
        head += 1
        let x = index % width
        let y = index / width
        for step in 0..<8 {
            let direction = Direction(rawValue: step)!
            guard
                canStep(
                    cells, width: width, height: height, x: x, y: y, dx: direction.dx,
                    dy: direction.dy)
            else {
                continue
            }
            let next = (y + direction.dy) * width + x + direction.dx
            if distance[next] < 0 {
                distance[next] = distance[index] + 1
                queue.append(next)
            }
        }
    }
    return distance
}

func passableCount(_ cells: [UInt8]) -> Int {
    var count = 0
    for cell in cells where !blocksMovement(cell) {
        count += 1
    }
    return count
}

func reachableCount(_ distance: [Int]) -> Int {
    var count = 0
    for value in distance where value >= 0 {
        count += 1
    }
    return count
}

private func makeRoom(_ random: inout Random, column: Int, row: Int, junction: Bool) -> Room {
    let width = junction ? 1 : 4 + random.below(8)
    let height = junction ? 1 : 3 + random.below(5)
    let x = column * sectorWidth + 1 + random.below(sectorWidth - 2 - width + 1)
    let y = row * sectorHeight + 1 + random.below(sectorHeight - 2 - height + 1)
    return Room(x: x, y: y, width: width, height: height, junction: junction)
}

private func carveRoom(_ cells: inout [UInt8], _ room: Room) {
    for y in room.y..<room.y + room.height {
        for x in room.x..<room.x + room.width {
            cells[y * mapWidth + x] = Terrain.floor.rawValue
        }
    }
}

private func carveSegment(_ cells: inout [UInt8], fromX: Int, fromY: Int, toX: Int, toY: Int) {
    var x = fromX
    var y = fromY
    cells[y * mapWidth + x] = Terrain.floor.rawValue
    while x != toX || y != toY {
        if x != toX {
            x += x < toX ? 1 : -1
        } else {
            y += y < toY ? 1 : -1
        }
        cells[y * mapWidth + x] = Terrain.floor.rawValue
    }
}

private func carveCorridor(
    _ cells: inout [UInt8], _ random: inout Random, from first: Room, to second: Room
) {
    let fromX = first.x + random.below(first.width)
    let fromY = first.y + random.below(first.height)
    let toX = second.x + random.below(second.width)
    let toY = second.y + random.below(second.height)
    if random.below(2) == 0 {
        carveSegment(&cells, fromX: fromX, fromY: fromY, toX: toX, toY: fromY)
        carveSegment(&cells, fromX: toX, fromY: fromY, toX: toX, toY: toY)
    } else {
        carveSegment(&cells, fromX: fromX, fromY: fromY, toX: fromX, toY: toY)
        carveSegment(&cells, fromX: fromX, fromY: toY, toX: toX, toY: toY)
    }
}

private func root(_ parent: inout [Int], _ node: Int) -> Int {
    var current = node
    while parent[current] != current {
        current = parent[current]
    }
    return current
}

private func sectorLinks(_ random: inout Random) -> [(Int, Int)] {
    var edges: [(Int, Int)] = []
    for row in 0..<sectorRows {
        for column in 0..<sectorColumns {
            let sector = row * sectorColumns + column
            if column + 1 < sectorColumns {
                edges.append((sector, sector + 1))
            }
            if row + 1 < sectorRows {
                edges.append((sector, sector + sectorColumns))
            }
        }
    }
    for index in stride(from: edges.count - 1, to: 0, by: -1) {
        edges.swapAt(index, random.below(index + 1))
    }
    var parent = [Int](0..<sectorColumns * sectorRows)
    var links: [(Int, Int)] = []
    var extra = 0
    for edge in edges {
        let first = root(&parent, edge.0)
        let second = root(&parent, edge.1)
        if first != second {
            parent[first] = second
            links.append(edge)
        } else if extra < extraLinks {
            extra += 1
            links.append(edge)
        }
    }
    return links
}

private func placeRubble(
    _ floor: inout Floor, _ random: inout Random
) {
    let wanted = 2 + random.below(4)
    let total = passableCount(floor.cells)
    var placed = 0
    for _ in 0..<wanted * 4 where placed < wanted {
        let room = floor.rooms[random.below(floor.rooms.count)]
        let x = room.x + random.below(room.width)
        let y = room.y + random.below(room.height)
        let index = y * mapWidth + x
        guard floor.cells[index] == Terrain.floor.rawValue else {
            continue
        }
        guard !(x == floor.startX && y == floor.startY) else {
            continue
        }
        floor.cells[index] = Terrain.rubble.rawValue
        let distance = distances(
            floor.cells, width: mapWidth, height: mapHeight, fromX: floor.startX,
            fromY: floor.startY)
        if reachableCount(distance) == total - placed - 1 {
            placed += 1
        } else {
            floor.cells[index] = Terrain.floor.rawValue
        }
    }
}

// Monsters and items go in the rooms but the start room.
private func placeOccupants(_ floor: inout Floor, _ random: inout Random, depth: Int) {}

func generateFloor(seed: UInt64, depth: Int) -> Floor {
    var random = Random(seed: seed ^ (UInt64(depth) &* 0x9E37_79B9_7F4A_7C15))
    var cells = [UInt8](repeating: Terrain.wall.rawValue, count: mapWidth * mapHeight)
    var rooms: [Room] = []
    for row in 0..<sectorRows {
        for column in 0..<sectorColumns {
            let junction = random.below(junctionChance) == 0
            rooms.append(makeRoom(&random, column: column, row: row, junction: junction))
        }
    }
    var index = 0
    while rooms.reduce(0, { $0 + ($1.junction ? 0 : 1) }) < 2 {
        if rooms[index].junction {
            rooms[index] = makeRoom(
                &random, column: index % sectorColumns, row: index / sectorColumns,
                junction: false)
        }
        index += 1
    }
    for room in rooms {
        carveRoom(&cells, room)
    }
    for link in sectorLinks(&random) {
        carveCorridor(&cells, &random, from: rooms[link.0], to: rooms[link.1])
    }

    var candidates: [Int] = []
    for (number, room) in rooms.enumerated() where !room.junction {
        candidates.append(number)
    }
    let startRoom = candidates[random.below(candidates.count)]
    let start = rooms[startRoom]
    let startX = start.x + random.below(start.width)
    let startY = start.y + random.below(start.height)
    let distance = distances(cells, width: mapWidth, height: mapHeight, fromX: startX, fromY: startY)
    var exitRoom = startRoom
    var farthest = -1
    for number in candidates where number != startRoom {
        let far = distance[rooms[number].centerY * mapWidth + rooms[number].centerX]
        if far > farthest {
            farthest = far
            exitRoom = number
        }
    }
    let exit = rooms[exitRoom]
    let exitX = exit.x + random.below(exit.width)
    let exitY = exit.y + random.below(exit.height)
    cells[exitY * mapWidth + exitX] =
        depth == finalDepth ? Terrain.plinth.rawValue : Terrain.stairs.rawValue

    var floor = Floor(
        cells: cells, rooms: rooms, startRoom: startRoom, exitRoom: exitRoom, startX: startX,
        startY: startY, exitX: exitX, exitY: exitY)
    placeRubble(&floor, &random)
    placeOccupants(&floor, &random, depth: depth)
    return floor
}
