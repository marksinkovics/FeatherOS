let energyToAct = 12
let startingOil = 1000
let maximumOil = 1000
let descentScore = 100
let plinthScore = 500
let recentLimit = 3

enum Command {
    case move(Direction)
    case wait
    case descend
}

enum MessageCode: UInt8 {
    case wallAhead = 1
    case rubbleAhead
    case noStairs
    case descended
    case lampDimmed
    case lampOut
    case plinthReached
}

struct Event: Equatable {
    var code: MessageCode
    var first = 0
    var second = 0
}

enum Outcome: UInt8 {
    case playing = 0
    case bottom
}

// Actor 0 is the player. Passes within a tick go through the actors in order, so a fast actor
// acts again only after the others that are ready.
struct Turns {
    var speeds: [Int]
    var energy: [Int]
    var cursor = 0

    init(speeds: [Int]) {
        self.speeds = speeds
        energy = [Int](repeating: 0, count: speeds.count)
    }

    mutating func next() -> Int {
        while true {
            while cursor < speeds.count {
                let actor = cursor
                cursor += 1
                if energy[actor] >= energyToAct {
                    energy[actor] -= energyToAct
                    return actor
                }
            }
            cursor = 0
            if !energy.contains(where: { $0 >= energyToAct }) {
                for actor in 0..<speeds.count {
                    energy[actor] += speeds[actor]
                }
            }
        }
    }
}

struct Game {
    var seed: UInt64
    var depth = 1
    var turn = 0
    var random: Random
    var x = 0
    var y = 0
    var oil = startingOil
    var level = 1
    var score = 0
    var outcome = Outcome.playing
    var cells: [UInt8] = []
    var remembered = [UInt8](repeating: 0, count: mapWidth * mapHeight)
    var lit = [UInt8](repeating: 0, count: mapWidth * mapHeight)
    var turns = Turns(speeds: [12])
    var recent: [Event] = []

    init(seed: UInt64) {
        self.seed = seed
        random = Random(seed: seed)
        enterFloor()
        _ = turns.next()
    }

    mutating func enterFloor() {
        let floor = generateFloor(seed: seed, depth: depth)
        cells = floor.cells
        x = floor.startX
        y = floor.startY
        remembered = [UInt8](repeating: 0, count: mapWidth * mapHeight)
        refreshSight()
    }

    mutating func refreshSight() {
        lit = fieldOfView(
            cells, width: mapWidth, height: mapHeight, x: x, y: y, radius: lampRadius(oil: oil))
        for index in 0..<lit.count where lit[index] != 0 {
            remembered[index] = 1
        }
    }

    func cell(_ x: Int, _ y: Int) -> Terrain {
        Terrain(rawValue: cells[y * mapWidth + x])!
    }

    mutating func step(_ command: Command) -> [Event] {
        guard outcome == .playing else {
            return []
        }
        var events: [Event] = []
        switch command {
        case .move(let direction):
            let targetX = x + direction.dx
            let targetY = y + direction.dy
            guard
                canStep(
                    cells, width: mapWidth, height: mapHeight, x: x, y: y, dx: direction.dx,
                    dy: direction.dy)
            else {
                let rubble =
                    targetX >= 0 && targetY >= 0 && targetX < mapWidth && targetY < mapHeight
                    && cell(targetX, targetY) == .rubble
                let event = Event(
                    code: rubble ? .rubbleAhead : .wallAhead, first: targetX, second: targetY)
                remember([event])
                return [event]
            }
            x = targetX
            y = targetY
            if cell(x, y) == .plinth {
                outcome = .bottom
                score += plinthScore
                events.append(Event(code: .plinthReached, first: depth))
            }
        case .wait:
            break
        case .descend:
            guard cell(x, y) == .stairs else {
                let event = Event(code: .noStairs)
                remember([event])
                return [event]
            }
            depth += 1
            score += descentScore
            events.append(Event(code: .descended, first: depth))
            enterFloor()
        }
        burnLamp(&events)
        turn += 1
        while turns.next() != 0 {}
        refreshSight()
        remember(events)
        return events
    }

    private mutating func burnLamp(_ events: inout [Event]) {
        let before = lampRadius(oil: oil)
        let wasLit = oil > 0
        oil = max(0, oil - 1)
        let after = lampRadius(oil: oil)
        if wasLit && oil == 0 {
            events.append(Event(code: .lampOut))
        } else if after != before {
            events.append(Event(code: .lampDimmed, first: after))
        }
    }

    private mutating func remember(_ events: [Event]) {
        recent.append(contentsOf: events)
        if recent.count > recentLimit {
            recent.removeFirst(recent.count - recentLimit)
        }
    }

    func stateHash() -> UInt64 {
        var hash = Fnv1a()
        hash.add(seed, bytes: 8)
        hash.add(depth, bytes: 1)
        hash.add(turn, bytes: 4)
        hash.add(random.state, bytes: 8)
        hash.add(x, bytes: 1)
        hash.add(y, bytes: 1)
        hash.add(oil, bytes: 2)
        hash.add(level, bytes: 1)
        hash.add(score, bytes: 4)
        hash.add(outcome.rawValue)
        hash.add(turns.energy.count, bytes: 1)
        for energy in turns.energy {
            hash.add(energy, bytes: 2)
        }
        hash.add(turns.cursor, bytes: 1)
        hash.add(cells)
        hash.add(remembered)
        hash.add(recent.count, bytes: 1)
        for event in recent {
            hash.add(event.code.rawValue)
            hash.add(event.first, bytes: 2)
            hash.add(event.second, bytes: 2)
        }
        return hash.value
    }
}
