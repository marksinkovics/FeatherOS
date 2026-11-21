let packLimit = 10
let floorItemLimit = 64
let flaskOil = 400
let flareDamage = 6
let flareRange = 4

enum ItemKind: UInt8 {
    case oilFlask = 1
    case salve
    case flareStone
    case blinkFeather
    case cartographersMoth
    case hookedKnife
    case ironCrook
    case wardensPike
    case quiltedCoat
    case tinPlate

    var isWeapon: Bool {
        self == .hookedKnife || self == .ironCrook || self == .wardensPike
    }

    var isCoat: Bool {
        self == .quiltedCoat || self == .tinPlate
    }

    var isGear: Bool {
        isWeapon || isCoat
    }

    var bonus: Int {
        switch self {
        case .hookedKnife, .quiltedCoat: return 1
        case .ironCrook, .tinPlate: return 3
        case .wardensPike: return 5
        default: return 0
        }
    }
}

struct FloorItem: Equatable {
    var kind: ItemKind
    var x: Int
    var y: Int
}

let placedItems: [ItemKind] = [
    .oilFlask, .salve, .flareStone, .blinkFeather, .cartographersMoth, .hookedKnife, .ironCrook,
    .wardensPike, .quiltedCoat, .tinPlate,
]

func itemWeight(_ kind: ItemKind, depth: Int) -> Int {
    switch kind {
    case .oilFlask, .salve: return 4
    case .flareStone, .blinkFeather: return depth >= 2 ? 2 : 0
    case .cartographersMoth: return depth >= 2 ? 1 : 0
    case .hookedKnife: return depth <= 4 ? 2 : 0
    case .ironCrook: return depth >= 3 ? 2 : 0
    case .wardensPike: return depth >= 5 ? 1 : 0
    case .quiltedCoat: return depth <= 4 ? 2 : 0
    case .tinPlate: return depth >= 3 ? 2 : 0
    }
}

func chooseItem(_ random: inout Random, depth: Int) -> ItemKind {
    var total = 0
    for kind in placedItems {
        total += itemWeight(kind, depth: depth)
    }
    var pick = random.below(total)
    for kind in placedItems {
        pick -= itemWeight(kind, depth: depth)
        if pick < 0 {
            return kind
        }
    }
    return .oilFlask
}

extension Game {
    func itemIndex(x: Int, y: Int) -> Int? {
        for (index, item) in items.enumerated() where item.x == x && item.y == y {
            return index
        }
        return nil
    }

    mutating func pickUp(_ events: inout [Event]) {
        guard let index = itemIndex(x: x, y: y) else {
            return
        }
        let kind = items[index].kind
        if pack.count >= packLimit {
            events.append(Event(code: .packFull, first: Int(kind.rawValue)))
            return
        }
        pack.append(kind)
        items.remove(at: index)
        events.append(Event(code: .pickedUp, first: Int(kind.rawValue)))
    }

    mutating func use(slot: Int, _ events: inout [Event]) -> Bool {
        guard slot >= 0 && slot < pack.count else {
            return false
        }
        let kind = pack[slot]
        guard !kind.isGear else {
            events.append(Event(code: .cannotUse, first: Int(kind.rawValue)))
            return false
        }
        pack.remove(at: slot)
        switch kind {
        case .oilFlask:
            changeOil(by: min(flaskOil, maximumOil - oil), &events)
            events.append(Event(code: .oilRefilled, first: oil))
        case .salve:
            let healed = min(8 + depth, maxHitPoints - hp)
            hp += healed
            events.append(Event(code: .healed, first: healed))
        case .flareStone:
            flare(&events)
        case .blinkFeather:
            guard blink(&events) else {
                pack.insert(kind, at: slot)
                events.append(Event(code: .nothingHappens, first: Int(kind.rawValue)))
                return false
            }
        case .cartographersMoth:
            remembered = [UInt8](repeating: 1, count: mapWidth * mapHeight)
            events.append(Event(code: .mapped))
        case .hookedKnife, .ironCrook, .wardensPike, .quiltedCoat, .tinPlate:
            break
        }
        return true
    }

    mutating func flare(_ events: inout [Event]) {
        var struck = 0
        var index = monsters.count - 1
        while index >= 0 {
            let dx = monsters[index].x - x
            let dy = monsters[index].y - y
            if dx * dx + dy * dy <= flareRange * flareRange
                && lit[monsters[index].y * mapWidth + monsters[index].x] != 0
            {
                struck += 1
                hurtMonster(index, by: flareDamage, &events)
            }
            index -= 1
        }
        events.append(Event(code: .flareBurst, first: struck))
    }

    mutating func blink(_ events: inout [Event]) -> Bool {
        var targets: [Int] = []
        for index in 0..<mapWidth * mapHeight where remembered[index] != 0 {
            let terrain = cells[index]
            guard terrain == Terrain.floor.rawValue || terrain == Terrain.stairs.rawValue else {
                continue
            }
            let tx = index % mapWidth
            let ty = index / mapWidth
            if isFree(x: tx, y: ty) {
                targets.append(index)
            }
        }
        guard !targets.isEmpty else {
            return false
        }
        let target = targets[random.below(targets.count)]
        x = target % mapWidth
        y = target / mapWidth
        events.append(Event(code: .blinked, first: x, second: y))
        arrive(&events)
        return true
    }

    mutating func equip(slot: Int, _ events: inout [Event]) -> Bool {
        guard slot >= 0 && slot < pack.count else {
            return false
        }
        let kind = pack[slot]
        guard kind.isGear else {
            events.append(Event(code: .cannotEquip, first: Int(kind.rawValue)))
            return false
        }
        if kind.isWeapon {
            if let worn = weapon {
                pack[slot] = worn
            } else {
                pack.remove(at: slot)
            }
            weapon = kind
        } else {
            if let worn = coat {
                pack[slot] = worn
            } else {
                pack.remove(at: slot)
            }
            coat = kind
        }
        events.append(Event(code: .equipped, first: Int(kind.rawValue)))
        return true
    }

    mutating func drop(slot: Int, _ events: inout [Event]) -> Bool {
        guard slot >= 0 && slot < pack.count else {
            return false
        }
        let kind = pack[slot]
        guard items.count < floorItemLimit && itemIndex(x: x, y: y) == nil else {
            events.append(Event(code: .noRoomHere, first: Int(kind.rawValue)))
            return false
        }
        pack.remove(at: slot)
        items.append(FloorItem(kind: kind, x: x, y: y))
        events.append(Event(code: .dropped, first: Int(kind.rawValue)))
        return true
    }
}
