let energyToAct = 12
let startingOil = 1000
let maximumOil = 1000
let descentScore = 100
let heartwickScore = 1000
let recentLimit = 3
let startingAttack = 3
let startingDefense = 1

enum Command {
    case move(Direction)
    case wait
    case descend
    case use(Int)
    case equip(Int)
    case drop(Int)
}

enum MessageCode: UInt8 {
    case wallAhead = 1
    case rubbleAhead
    case noStairs
    case descended
    case lampDimmed
    case lampOut
    case heartwickTaken
    case playerHit
    case playerMissed
    case monsterHit
    case monsterMissed
    case monsterKilled
    case leveledUp
    case oilDrunk
    case pickedUp
    case packFull
    case dropped
    case noRoomHere
    case healed
    case oilRefilled
    case flareBurst
    case blinked
    case mapped
    case nothingHappens
    case equipped
    case cannotUse
    case cannotEquip
    case died
}

struct Event: Equatable {
    var code: MessageCode
    var first = 0
    var second = 0
}

enum Outcome: UInt8 {
    case playing = 0
    case dead
    case victory
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

    mutating func remove(actor: Int) {
        speeds.remove(at: actor)
        energy.remove(at: actor)
        if actor < cursor {
            cursor -= 1
        }
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
    var hp = startingHitPoints
    var embers = 0
    var outcome = Outcome.playing
    var pack: [ItemKind] = []
    var weapon: ItemKind?
    var coat: ItemKind?
    var cells: [UInt8] = []
    var remembered = [UInt8](repeating: 0, count: mapWidth * mapHeight)
    var lit = [UInt8](repeating: 0, count: mapWidth * mapHeight)
    var monsters: [Monster] = []
    var items: [FloorItem] = []
    var turns = Turns(speeds: [12])
    var recent: [Event] = []

    init(seed: UInt64) {
        self.seed = seed
        random = Random(seed: seed)
        enterFloor()
    }

    var level: Int { levelFor(embers: embers) }
    var maxHitPoints: Int { startingHitPoints + hitPointsPerLevel * (level - 1) }
    var attack: Int { startingAttack + level - 1 + (weapon?.bonus ?? 0) }
    var defense: Int { startingDefense + (coat?.bonus ?? 0) }
    var score: Int {
        (depth - 1) * descentScore + embers + (outcome == .victory ? heartwickScore : 0)
    }

    mutating func enterFloor() {
        let floor = generateFloor(seed: seed, depth: depth)
        cells = floor.cells
        monsters = floor.monsters
        items = floor.items
        x = floor.startX
        y = floor.startY
        remembered = [UInt8](repeating: 0, count: mapWidth * mapHeight)
        resetTurns()
        refreshSight()
    }

    mutating func resetTurns() {
        turns = Turns(speeds: [12] + monsters.map { stats(of: $0.kind).speed })
        _ = turns.next()
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
        var spent = true
        switch command {
        case .move(let direction):
            spent = move(direction, &events)
        case .wait:
            break
        case .descend:
            spent = descend(&events)
        case .use(let slot):
            spent = use(slot: slot, &events)
        case .equip(let slot):
            spent = equip(slot: slot, &events)
        case .drop(let slot):
            spent = drop(slot: slot, &events)
        }
        guard spent else {
            remember(events)
            return events
        }
        burnLamp(&events)
        turn += 1
        if outcome == .playing {
            refreshSight()
            let oilBefore = oil
            var chase: [Int] = []
            while outcome == .playing {
                let actor = turns.next()
                if actor == 0 {
                    break
                }
                monsterActs(actor - 1, chase: &chase, &events)
            }
            if oil != oilBefore {
                refreshSight()
            }
        } else {
            refreshSight()
        }
        remember(events)
        return events
    }

    private mutating func move(_ direction: Direction, _ events: inout [Event]) -> Bool {
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
            events.append(
                Event(code: rubble ? .rubbleAhead : .wallAhead, first: targetX, second: targetY))
            return false
        }
        if let target = monsterIndex(x: targetX, y: targetY) {
            playerAttacks(target, &events)
            return true
        }
        x = targetX
        y = targetY
        arrive(&events)
        return true
    }

    private mutating func descend(_ events: inout [Event]) -> Bool {
        guard cell(x, y) == .stairs else {
            events.append(Event(code: .noStairs))
            return false
        }
        depth += 1
        events.append(Event(code: .descended, first: depth))
        enterFloor()
        return true
    }

    mutating func arrive(_ events: inout [Event]) {
        if cell(x, y) == .plinth {
            outcome = .victory
            events.append(Event(code: .heartwickTaken, first: depth))
            for index in monsters.indices where monsters[index].kind == .hearthwarden {
                monsters[index].awake = true
            }
            return
        }
        pickUp(&events)
    }

    mutating func changeOil(by amount: Int, _ events: inout [Event]) {
        let before = lampRadius(oil: oil)
        let wasLit = oil > 0
        oil = min(maximumOil, max(0, oil + amount))
        let after = lampRadius(oil: oil)
        if wasLit && oil == 0 {
            events.append(Event(code: .lampOut))
        } else if after < before {
            events.append(Event(code: .lampDimmed, first: after))
        }
    }

    private mutating func burnLamp(_ events: inout [Event]) {
        changeOil(by: -1, &events)
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
        hash.add(hp, bytes: 2)
        hash.add(embers, bytes: 2)
        hash.add(outcome.rawValue)
        hash.add(weapon?.rawValue ?? 0)
        hash.add(coat?.rawValue ?? 0)
        hash.add(pack.count, bytes: 1)
        for kind in pack {
            hash.add(kind.rawValue)
        }
        hash.add(turns.energy.count, bytes: 1)
        for energy in turns.energy {
            hash.add(energy, bytes: 2)
        }
        hash.add(turns.cursor, bytes: 1)
        hash.add(cells)
        hash.add(remembered)
        hash.add(monsters.count, bytes: 1)
        for monster in monsters {
            hash.add(monster.kind.rawValue)
            hash.add(monster.x, bytes: 1)
            hash.add(monster.y, bytes: 1)
            hash.add(monster.hp, bytes: 2)
            hash.add(monster.awake ? 1 : 0, bytes: 1)
        }
        hash.add(items.count, bytes: 1)
        for item in items {
            hash.add(item.kind.rawValue)
            hash.add(item.x, bytes: 1)
            hash.add(item.y, bytes: 1)
        }
        hash.add(recent.count, bytes: 1)
        for event in recent {
            hash.add(event.code.rawValue)
            hash.add(event.first, bytes: 2)
            hash.add(event.second, bytes: 2)
        }
        return hash.value
    }
}
