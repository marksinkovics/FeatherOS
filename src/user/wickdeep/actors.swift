let chaseRange = 24
let wakeOneIn = 3
let monsterLimit = 64
let oilDrunkPerBite = 25

enum MonsterKind: UInt8 {
    case dustmite = 1
    case gnawling
    case oilmoth
    case sootcrawler
    case shardback
    case rootgrip
    case bellwraith
    case hearthwarden
}

struct MonsterStats {
    var hitPoints: Int
    var attack: Int
    var defense: Int
    var speed: Int
    var worth: Int
    var moves = true
}

struct Monster: Equatable {
    var kind: MonsterKind
    var x: Int
    var y: Int
    var hp: Int
    var awake = false
}

func stats(of kind: MonsterKind) -> MonsterStats {
    switch kind {
    case .dustmite: return MonsterStats(hitPoints: 3, attack: 2, defense: 0, speed: 12, worth: 1)
    case .gnawling: return MonsterStats(hitPoints: 5, attack: 3, defense: 0, speed: 18, worth: 3)
    case .oilmoth: return MonsterStats(hitPoints: 6, attack: 1, defense: 0, speed: 12, worth: 4)
    case .sootcrawler: return MonsterStats(hitPoints: 10, attack: 4, defense: 1, speed: 12, worth: 6)
    case .shardback: return MonsterStats(hitPoints: 14, attack: 4, defense: 5, speed: 6, worth: 9)
    case .rootgrip:
        return MonsterStats(
            hitPoints: 12, attack: 6, defense: 2, speed: 12, worth: 8, moves: false)
    case .bellwraith: return MonsterStats(hitPoints: 18, attack: 7, defense: 2, speed: 6, worth: 14)
    case .hearthwarden:
        return MonsterStats(hitPoints: 45, attack: 9, defense: 4, speed: 12, worth: 30)
    }
}

let placedMonsters: [MonsterKind] = [
    .dustmite, .gnawling, .oilmoth, .sootcrawler, .shardback, .rootgrip, .bellwraith,
]

func monsterWeight(_ kind: MonsterKind, depth: Int) -> Int {
    switch kind {
    case .dustmite: return max(1, 8 - depth)
    case .gnawling: return depth >= 2 ? 3 : 0
    case .oilmoth: return depth >= 2 ? 2 : 0
    case .sootcrawler: return depth >= 3 ? 3 : 0
    case .shardback: return depth >= 4 ? 2 : 0
    case .rootgrip: return depth >= 3 ? 2 : 0
    case .bellwraith: return depth >= 5 ? depth - 3 : 0
    case .hearthwarden: return 0
    }
}

func chooseMonster(_ random: inout Random, depth: Int) -> MonsterKind {
    var total = 0
    for kind in placedMonsters {
        total += monsterWeight(kind, depth: depth)
    }
    var pick = random.below(total)
    for kind in placedMonsters {
        pick -= monsterWeight(kind, depth: depth)
        if pick < 0 {
            return kind
        }
    }
    return .dustmite
}

let levelThresholds = [10, 25, 50, 90, 140, 200, 270]
let startingHitPoints = 20
let hitPointsPerLevel = 4

func levelFor(embers: Int) -> Int {
    var level = 1
    for threshold in levelThresholds where embers >= threshold {
        level += 1
    }
    return level
}

func hitChance(attack: Int, defense: Int) -> Int {
    min(95, max(20, 70 + 5 * (attack - defense)))
}

func damage(attack: Int, defense: Int, roll: Int) -> Int {
    max(1, attack + roll - defense / 2)
}

extension Game {
    func monsterIndex(x: Int, y: Int) -> Int? {
        for (index, monster) in monsters.enumerated() where monster.x == x && monster.y == y {
            return index
        }
        return nil
    }

    func reaches(fromX: Int, fromY: Int, toX: Int, toY: Int) -> Bool {
        let dx = toX - fromX
        let dy = toY - fromY
        guard abs(dx) <= 1 && abs(dy) <= 1 && (dx != 0 || dy != 0) else {
            return false
        }
        return canStep(cells, width: mapWidth, height: mapHeight, x: fromX, y: fromY, dx: dx, dy: dy)
    }

    mutating func rollHit(attack: Int, defense: Int) -> Bool {
        random.below(100) < hitChance(attack: attack, defense: defense)
    }

    mutating func rollDamage(attack: Int, defense: Int) -> Int {
        damage(attack: attack, defense: defense, roll: random.below(3) - 1)
    }

    mutating func playerAttacks(_ index: Int, _ events: inout [Event]) {
        let kind = monsters[index].kind
        let target = stats(of: kind)
        guard rollHit(attack: attack, defense: target.defense) else {
            events.append(Event(code: .playerMissed, first: Int(kind.rawValue)))
            return
        }
        let dealt = rollDamage(attack: attack, defense: target.defense)
        events.append(Event(code: .playerHit, first: Int(kind.rawValue), second: dealt))
        hurtMonster(index, by: dealt, &events)
    }

    mutating func hurtMonster(_ index: Int, by amount: Int, _ events: inout [Event]) {
        monsters[index].awake = true
        monsters[index].hp -= amount
        guard monsters[index].hp <= 0 else {
            return
        }
        let kind = monsters[index].kind
        let worth = stats(of: kind).worth
        let before = level
        embers += worth
        events.append(Event(code: .monsterKilled, first: Int(kind.rawValue), second: worth))
        monsters.remove(at: index)
        turns.remove(actor: index + 1)
        if level > before {
            hp += hitPointsPerLevel * (level - before)
            events.append(Event(code: .leveledUp, first: level))
        }
    }

    mutating func monsterAttacks(_ index: Int, _ events: inout [Event]) {
        let kind = monsters[index].kind
        let striker = stats(of: kind)
        guard rollHit(attack: striker.attack, defense: defense) else {
            events.append(Event(code: .monsterMissed, first: Int(kind.rawValue)))
            return
        }
        let dealt = rollDamage(attack: striker.attack, defense: defense)
        hp -= dealt
        events.append(Event(code: .monsterHit, first: Int(kind.rawValue), second: dealt))
        if hp <= 0 {
            hp = 0
            outcome = .dead
            events.append(Event(code: .died, first: Int(kind.rawValue)))
        }
    }

    func isFree(x: Int, y: Int) -> Bool {
        !(x == self.x && y == self.y) && monsterIndex(x: x, y: y) == nil
    }

    mutating func moveMonster(_ index: Int, chase: [Int]) {
        let monster = monsters[index]
        let here = chase[monster.y * mapWidth + monster.x]
        var best = -1
        if here > 0 && here <= chaseRange {
            var bestDistance = here
            for step in 0..<8 {
                let direction = Direction(rawValue: step)!
                let nx = monster.x + direction.dx
                let ny = monster.y + direction.dy
                guard
                    reaches(fromX: monster.x, fromY: monster.y, toX: nx, toY: ny),
                    isFree(x: nx, y: ny)
                else {
                    continue
                }
                let distance = chase[ny * mapWidth + nx]
                if distance >= 0 && distance < bestDistance {
                    bestDistance = distance
                    best = step
                }
            }
        } else {
            let step = random.below(8)
            let direction = Direction(rawValue: step)!
            let nx = monster.x + direction.dx
            let ny = monster.y + direction.dy
            if reaches(fromX: monster.x, fromY: monster.y, toX: nx, toY: ny)
                && isFree(x: nx, y: ny)
            {
                best = step
            }
        }
        if best >= 0 {
            let direction = Direction(rawValue: best)!
            monsters[index].x += direction.dx
            monsters[index].y += direction.dy
        }
    }

    mutating func monsterActs(_ index: Int, chase: inout [Int], _ events: inout [Event]) {
        let monster = monsters[index]
        guard monster.awake else {
            if monster.kind != .hearthwarden && lit[monster.y * mapWidth + monster.x] != 0
                && random.below(wakeOneIn) == 0
            {
                monsters[index].awake = true
            }
            return
        }
        if reaches(fromX: monster.x, fromY: monster.y, toX: x, toY: y) {
            if monster.kind == .oilmoth {
                let drunk = min(oil, oilDrunkPerBite)
                changeOil(by: -drunk, &events)
                events.append(Event(code: .oilDrunk, first: drunk))
            } else {
                monsterAttacks(index, &events)
            }
            return
        }
        guard stats(of: monster.kind).moves else {
            return
        }
        if chase.isEmpty {
            chase = distances(cells, width: mapWidth, height: mapHeight, fromX: x, fromY: y)
        }
        moveMonster(index, chase: chase)
    }
}
