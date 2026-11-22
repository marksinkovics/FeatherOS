let saveMagic: [UInt8] = [
    UInt8(ascii: "W"), UInt8(ascii: "D"), UInt8(ascii: "S"), UInt8(ascii: "V"),
]
let saveVersion = 1
let saveHeaderSize = 20
let saveSizeLimit = 64 * 1024
let saveEnergyLimit = 40

struct SaveWriter {
    var bytes: [UInt8] = []

    mutating func put(_ value: Int, bytes count: Int) {
        for index in 0..<count {
            bytes.append(UInt8(truncatingIfNeeded: value >> (8 * index)))
        }
    }

    mutating func put(_ value: UInt64) {
        for index in 0..<8 {
            bytes.append(UInt8(truncatingIfNeeded: value >> UInt64(8 * index)))
        }
    }
}

// The first failure sticks and every later read returns 0, so a decoder reads straight through.
struct SaveReader {
    let bytes: [UInt8]
    var index: Int
    var refusal: String?

    init(_ bytes: [UInt8], from start: Int) {
        self.bytes = bytes
        index = start
    }

    mutating func refuse(_ reason: String) {
        if refusal == nil {
            refusal = reason
        }
    }

    mutating func get(bytes count: Int) -> Int {
        guard index + count <= bytes.count else {
            refuse("length")
            index = bytes.count
            return 0
        }
        var value = 0
        for offset in 0..<count {
            value |= Int(bytes[index + offset]) << (8 * offset)
        }
        index += count
        return value
    }

    mutating func getSigned16() -> Int {
        let value = get(bytes: 2)
        return value >= 0x8000 ? value - 0x1_0000 : value
    }

    mutating func getU64() -> UInt64 {
        var value: UInt64 = 0
        for offset in 0..<8 {
            value |= UInt64(get(bytes: 1)) << UInt64(8 * offset)
        }
        return value
    }

    mutating func getBytes(_ count: Int) -> [UInt8] {
        guard index + count <= bytes.count else {
            refuse("length")
            index = bytes.count
            return []
        }
        let slice = Array(bytes[index..<index + count])
        index += count
        return slice
    }
}

enum SaveResult {
    case game(Game)
    case refused(String)
}

func checksum(of bytes: ArraySlice<UInt8>) -> UInt64 {
    var hash = Fnv1a()
    for byte in bytes {
        hash.add(byte)
    }
    return hash.value
}

extension Game {
    func encode() -> [UInt8] {
        var payload = SaveWriter()
        payload.put(seed)
        payload.put(depth, bytes: 1)
        payload.put(turn, bytes: 4)
        payload.put(random.state)
        payload.put(x, bytes: 1)
        payload.put(y, bytes: 1)
        payload.put(oil, bytes: 2)
        payload.put(hp, bytes: 2)
        payload.put(embers, bytes: 2)
        payload.put(Int(outcome.rawValue), bytes: 1)
        payload.put(Int(weapon?.rawValue ?? 0), bytes: 1)
        payload.put(Int(coat?.rawValue ?? 0), bytes: 1)
        payload.put(pack.count, bytes: 1)
        for kind in pack {
            payload.put(Int(kind.rawValue), bytes: 1)
        }
        payload.bytes += cells
        payload.bytes += remembered
        payload.put(monsters.count, bytes: 1)
        for monster in monsters {
            payload.put(Int(monster.kind.rawValue), bytes: 1)
            payload.put(monster.x, bytes: 1)
            payload.put(monster.y, bytes: 1)
            payload.put(monster.hp, bytes: 2)
            payload.put(monster.awake ? 1 : 0, bytes: 1)
        }
        payload.put(items.count, bytes: 1)
        for item in items {
            payload.put(Int(item.kind.rawValue), bytes: 1)
            payload.put(item.x, bytes: 1)
            payload.put(item.y, bytes: 1)
        }
        for energy in turns.energy {
            payload.put(energy, bytes: 1)
        }
        payload.put(turns.cursor, bytes: 1)
        payload.put(recent.count, bytes: 1)
        for event in recent {
            payload.put(Int(event.code.rawValue), bytes: 1)
            payload.put(event.first, bytes: 2)
            payload.put(event.second, bytes: 2)
        }

        var file = SaveWriter()
        file.bytes += saveMagic
        file.put(saveVersion, bytes: 2)
        file.put(0, bytes: 2)
        file.put(payload.bytes.count, bytes: 4)
        file.put(checksum(of: payload.bytes[...]))
        file.bytes += payload.bytes
        return file.bytes
    }

    static func decode(_ bytes: [UInt8]) -> SaveResult {
        if bytes.count > saveSizeLimit {
            return .refused("size")
        }
        if bytes.count < saveHeaderSize {
            return .refused("short")
        }
        var header = SaveReader(bytes, from: 0)
        guard header.getBytes(4) == saveMagic else {
            return .refused("magic")
        }
        guard header.get(bytes: 2) == saveVersion else {
            return .refused("version")
        }
        guard header.get(bytes: 2) == 0 else {
            return .refused("reserved")
        }
        let length = header.get(bytes: 4)
        let expected = header.getU64()
        guard length == bytes.count - saveHeaderSize else {
            return .refused("length")
        }
        guard checksum(of: bytes[saveHeaderSize...]) == expected else {
            return .refused("checksum")
        }
        var reader = SaveReader(bytes, from: saveHeaderSize)
        let game = readGame(&reader)
        if let reason = reader.refusal {
            return .refused(reason)
        }
        if reader.index != bytes.count {
            return .refused("length")
        }
        return .game(game!)
    }
}

private func inMap(_ x: Int, _ y: Int) -> Bool {
    x < mapWidth && y < mapHeight
}

private func readGame(_ reader: inout SaveReader) -> Game? {
    var game = Game(seed: reader.getU64())
    game.depth = reader.get(bytes: 1)
    game.turn = reader.get(bytes: 4)
    game.random = Random(seed: reader.getU64())
    game.x = reader.get(bytes: 1)
    game.y = reader.get(bytes: 1)
    game.oil = reader.get(bytes: 2)
    game.hp = reader.get(bytes: 2)
    game.embers = reader.get(bytes: 2)
    let outcome = Outcome(rawValue: UInt8(reader.get(bytes: 1)))
    let weapon = reader.get(bytes: 1)
    let coat = reader.get(bytes: 1)
    if reader.refusal != nil {
        return nil
    }
    guard game.depth >= 1 && game.depth <= finalDepth, game.oil <= maximumOil, let outcome,
        game.hp <= game.maxHitPoints, (game.hp == 0) == (outcome == .dead)
    else {
        reader.refuse("range")
        return nil
    }
    game.outcome = outcome
    guard weapon == 0 || ItemKind(rawValue: UInt8(weapon))?.isWeapon == true,
        coat == 0 || ItemKind(rawValue: UInt8(coat))?.isCoat == true
    else {
        reader.refuse("kind")
        return nil
    }
    game.weapon = ItemKind(rawValue: UInt8(weapon))
    game.coat = ItemKind(rawValue: UInt8(coat))

    let packCount = reader.get(bytes: 1)
    guard packCount <= packLimit else {
        reader.refuse("count")
        return nil
    }
    game.pack = []
    for _ in 0..<packCount {
        guard let kind = ItemKind(rawValue: UInt8(reader.get(bytes: 1))) else {
            reader.refuse("kind")
            return nil
        }
        game.pack.append(kind)
    }

    game.cells = reader.getBytes(mapWidth * mapHeight)
    game.remembered = reader.getBytes(mapWidth * mapHeight)
    if reader.refusal != nil {
        return nil
    }
    guard game.cells.allSatisfy({ $0 <= Terrain.plinth.rawValue }),
        game.remembered.allSatisfy({ $0 <= 1 })
    else {
        reader.refuse("range")
        return nil
    }
    guard inMap(game.x, game.y), !blocksMovement(game.cells[game.y * mapWidth + game.x]) else {
        reader.refuse("position")
        return nil
    }

    let monsterCount = reader.get(bytes: 1)
    guard monsterCount <= monsterLimit else {
        reader.refuse("count")
        return nil
    }
    game.monsters = []
    for _ in 0..<monsterCount {
        let kind = MonsterKind(rawValue: UInt8(reader.get(bytes: 1)))
        let x = reader.get(bytes: 1)
        let y = reader.get(bytes: 1)
        let hp = reader.get(bytes: 2)
        let awake = reader.get(bytes: 1)
        guard let kind else {
            reader.refuse("kind")
            return nil
        }
        guard inMap(x, y), !blocksMovement(game.cells[y * mapWidth + x]) else {
            reader.refuse("position")
            return nil
        }
        guard hp >= 1 && hp <= stats(of: kind).hitPoints && awake <= 1 else {
            reader.refuse("range")
            return nil
        }
        game.monsters.append(Monster(kind: kind, x: x, y: y, hp: hp, awake: awake == 1))
    }

    let itemCount = reader.get(bytes: 1)
    guard itemCount <= floorItemLimit else {
        reader.refuse("count")
        return nil
    }
    game.items = []
    for _ in 0..<itemCount {
        let kind = ItemKind(rawValue: UInt8(reader.get(bytes: 1)))
        let x = reader.get(bytes: 1)
        let y = reader.get(bytes: 1)
        guard let kind else {
            reader.refuse("kind")
            return nil
        }
        guard inMap(x, y), !blocksMovement(game.cells[y * mapWidth + x]) else {
            reader.refuse("position")
            return nil
        }
        game.items.append(FloorItem(kind: kind, x: x, y: y))
    }

    game.turns = Turns(speeds: [12] + game.monsters.map { stats(of: $0.kind).speed })
    for actor in 0...monsterCount {
        let energy = reader.get(bytes: 1)
        guard energy <= saveEnergyLimit else {
            reader.refuse("range")
            return nil
        }
        game.turns.energy[actor] = energy
    }
    game.turns.cursor = reader.get(bytes: 1)
    guard game.turns.cursor <= monsterCount + 1 else {
        reader.refuse("range")
        return nil
    }

    let recentCount = reader.get(bytes: 1)
    guard recentCount <= recentLimit else {
        reader.refuse("count")
        return nil
    }
    game.recent = []
    for _ in 0..<recentCount {
        let code = MessageCode(rawValue: UInt8(reader.get(bytes: 1)))
        let first = reader.getSigned16()
        let second = reader.getSigned16()
        guard let code else {
            reader.refuse("kind")
            return nil
        }
        game.recent.append(Event(code: code, first: first, second: second))
    }
    if reader.refusal != nil {
        return nil
    }
    game.refreshSight()
    return game
}
