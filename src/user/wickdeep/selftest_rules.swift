func place(
    _ game: inout Game, _ kind: MonsterKind, _ x: Int, _ y: Int, awake: Bool = true,
    hp: Int? = nil
) {
    game.monsters.append(
        Monster(kind: kind, x: x, y: y, hp: hp ?? stats(of: kind).hitPoints, awake: awake))
    game.resetTurns()
}

func holding(_ game: Game, _ kinds: [ItemKind]) -> Game {
    var game = game
    game.pack = kinds
    return game
}

func count(_ events: [Event], _ code: MessageCode) -> Int {
    events.reduce(0) { $0 + ($1.code == code ? 1 : 0) }
}

func checkHitChance(_ check: inout Checker) {
    check.begin("hit chance")
    let table: [(Int, Int)] = [
        (-20, 20), (-11, 20), (-10, 20), (-9, 25), (-8, 30), (-1, 65), (0, 70), (1, 75),
        (4, 90), (5, 95), (6, 95), (9, 95), (20, 95),
    ]
    for (difference, chance) in table {
        let actual = hitChance(attack: 10 + difference, defense: 10)
        check.expect(actual == chance, "attack - defense \(difference) gave \(actual)%")
    }
    var bounded = true
    var monotonic = true
    for defense in 0...14 {
        var last = 0
        for attack in 0...14 {
            let chance = hitChance(attack: attack, defense: defense)
            bounded = bounded && chance >= 20 && chance <= 95 && chance % 5 == 0
            monotonic = monotonic && chance >= last
            last = chance
        }
    }
    check.expect(bounded, "a chance left 20...95 or is not a multiple of 5")
    check.expect(monotonic, "a higher attack lowered the chance")
    var game = openRoomGame(oil: 1000)
    let trials = 10_000
    let samples: [(Int, Int, Int)] = [(12, 3, 95), (3, 9, 40), (1, 13, 20), (5, 5, 70)]
    for (attack, defense, chance) in samples {
        var hits = 0
        for _ in 0..<trials where game.rollHit(attack: attack, defense: defense) {
            hits += 1
        }
        let expected = trials * chance / 100
        check.expect(
            hits >= expected - 200 && hits <= expected + 200,
            "\(attack) against \(defense) hit \(hits) of \(trials), expected about \(expected)")
    }
}

func checkDamage(_ check: inout Checker) {
    check.begin("damage")
    let table: [(Int, Int, [Int])] = [
        (3, 1, [2, 3, 4]), (3, 4, [1, 1, 2]), (1, 9, [1, 1, 1]), (9, 2, [7, 8, 9]),
        (6, 5, [3, 4, 5]), (4, 0, [3, 4, 5]),
    ]
    for (attack, defense, expected) in table {
        var actual: [Int] = []
        for roll in -1...1 {
            actual.append(damage(attack: attack, defense: defense, roll: roll))
        }
        check.expect(actual == expected, "attack \(attack) defense \(defense) gave \(actual)")
    }
    var game = openRoomGame(oil: 1000)
    var low = Int.max
    var high = 0
    for _ in 0..<500 {
        let dealt = game.rollDamage(attack: 3, defense: 1)
        low = min(low, dealt)
        high = max(high, dealt)
    }
    check.expect(low == 2 && high == 4, "rolled damage ranged \(low)...\(high), not 2...4")
}

func expectedStrike(_ game: Game, attack: Int, defense: Int) -> (Bool, Int) {
    var draws = game.random
    guard draws.below(100) < hitChance(attack: attack, defense: defense) else {
        return (false, 0)
    }
    return (true, damage(attack: attack, defense: defense, roll: draws.below(3) - 1))
}

func checkPlayerCombat(_ check: inout Checker) {
    check.begin("player combat")
    var seenHit = false
    var seenMiss = false
    for seed in 1...30 {
        var game = openRoomGame(oil: 1000)
        game.random = Random(seed: UInt64(seed))
        place(&game, .sootcrawler, 31, 20, awake: false)
        let (hit, dealt) = expectedStrike(game, attack: 3, defense: 1)
        let events = game.step(.move(.east))
        check.expect(game.x == 30 && game.turn == 1 && game.oil == 999, "an attack moved or was free")
        if hit {
            seenHit = true
            check.expect(
                events.first == Event(code: .playerHit, first: 4, second: dealt)
                    && game.monsters[0].hp == 10 - dealt && game.monsters[0].awake,
                "seed \(seed): \(events)")
        } else {
            seenMiss = true
            check.expect(
                events.first == Event(code: .playerMissed, first: 4) && game.monsters[0].hp == 10,
                "seed \(seed): a miss hurt the sootcrawler: \(events)")
        }
    }
    check.expect(seenHit && seenMiss, "30 attacks gave only hits or only misses")

    var kill = openRoomGame(oil: 1000)
    place(&kill, .dustmite, 29, 20, awake: false, hp: 1)
    place(&kill, .gnawling, 40, 20, awake: false)
    var events: [Event] = []
    for _ in 0..<40 where kill.monsters.count == 2 {
        events = kill.step(.move(.west))
    }
    check.expect(
        kill.monsters.count == 1 && kill.monsters[0].kind == .gnawling && kill.embers == 1
            && events.contains(Event(code: .monsterKilled, first: 1, second: 1)),
        "killing a dustmite: \(kill.monsters.count) left, \(kill.embers) embers")
    check.expect(
        kill.turns.speeds == [12, 18] && kill.turns.energy.count == 2,
        "a death left the turn order \(kill.turns.speeds)")
    var armed = openRoomGame(oil: 1000)
    armed.weapon = .wardensPike
    place(&armed, .shardback, 31, 20, awake: false)
    var dealt = 0
    for _ in 0..<60 where armed.monsters.count == 1 {
        for event in armed.step(.move(.east)) where event.code == .playerHit {
            dealt = max(dealt, event.second)
        }
    }
    check.expect(armed.attack == 8 && dealt >= 5 && dealt <= 9, "the pike dealt up to \(dealt)")
}

func checkMonsterCombat(_ check: inout Checker) {
    check.begin("monster combat")
    var seenHit = false
    var seenMiss = false
    for seed in 1...30 {
        var game = openRoomGame(oil: 1000)
        game.random = Random(seed: UInt64(seed))
        place(&game, .sootcrawler, 31, 20)
        let (hit, dealt) = expectedStrike(game, attack: 4, defense: 1)
        let events = game.step(.wait)
        if hit {
            seenHit = true
            check.expect(
                events == [Event(code: .monsterHit, first: 4, second: dealt)]
                    && game.hp == 20 - dealt && game.monsters[0].x == 31,
                "seed \(seed): \(events)")
        } else {
            seenMiss = true
            check.expect(
                events == [Event(code: .monsterMissed, first: 4)] && game.hp == 20,
                "seed \(seed): \(events)")
        }
    }
    check.expect(seenHit && seenMiss, "30 strikes gave only hits or only misses")

    var plated = openRoomGame(oil: 1000)
    plated.coat = .tinPlate
    check.expect(plated.defense == 4, "tin plate gave defense \(plated.defense)")
    var bare = openRoomGame(oil: 1000)
    var platedHits = 0
    var bareHits = 0
    for _ in 0..<3000 {
        platedHits += plated.rollHit(attack: 4, defense: plated.defense) ? 1 : 0
        bareHits += bare.rollHit(attack: 4, defense: bare.defense) ? 1 : 0
    }
    check.expect(
        platedHits >= 2000 && platedHits <= 2200 && bareHits >= 2450 && bareHits <= 2650,
        "armour: \(platedHits) and \(bareHits) hits of 3000 at defense 4 and 1")

    var doomed = openRoomGame(oil: 1000)
    doomed.hp = 1
    place(&doomed, .bellwraith, 31, 20)
    var events: [Event] = []
    for _ in 0..<80 where doomed.outcome == .playing {
        events = doomed.step(.wait)
    }
    check.expect(
        doomed.outcome == .dead && doomed.hp == 0
            && events.contains(Event(code: .died, first: 7)) && doomed.score == 0,
        "dying: outcome \(doomed.outcome) hp \(doomed.hp)")
    let turn = doomed.turn
    check.expect(
        doomed.step(.wait).isEmpty && doomed.step(.move(.west)).isEmpty && doomed.turn == turn,
        "a dead player acted")
}

func checkLevels(_ check: inout Checker) {
    check.begin("levels")
    let thresholds: [(Int, Int)] = [
        (0, 1), (9, 1), (10, 2), (24, 2), (25, 3), (49, 3), (50, 4), (89, 4), (90, 5), (139, 5),
        (140, 6), (199, 6), (200, 7), (269, 7), (270, 8), (100_000, 8),
    ]
    for (embers, level) in thresholds {
        check.expect(levelFor(embers: embers) == level, "\(embers) embers gave level \(levelFor(embers: embers))")
    }
    var game = openRoomGame(oil: 1000)
    check.expect(
        game.hp == 20 && game.maxHitPoints == 20 && game.attack == 3 && game.defense == 1
            && game.level == 1,
        "starting numbers")
    game.embers = 9
    game.hp = 10
    place(&game, .dustmite, 31, 20, awake: false, hp: 1)
    var events: [Event] = []
    var before = game.hp
    for _ in 0..<40 where !game.monsters.isEmpty {
        before = game.hp
        events = game.step(.move(.east))
    }
    check.expect(
        game.level == 2 && game.maxHitPoints == 24 && game.hp == before + 4 && game.attack == 4
            && events.contains(Event(code: .leveledUp, first: 2)),
        "level 2: hp \(game.hp)/\(game.maxHitPoints) attack \(game.attack)")
    var jump = openRoomGame(oil: 1000)
    jump.embers = 9
    place(&jump, .hearthwarden, 31, 20, awake: false, hp: 1)
    for _ in 0..<80 where !jump.monsters.isEmpty {
        _ = jump.step(.move(.east))
    }
    check.expect(
        jump.embers == 39 && jump.level == 3 && jump.maxHitPoints == 28 && jump.hp == 28
            && jump.attack == 5,
        "two levels at once: level \(jump.level) hp \(jump.hp)/\(jump.maxHitPoints)")
    check.expect(jump.score == 39, "score \(jump.score)")
    var top = openRoomGame(oil: 1000)
    top.embers = 270
    check.expect(top.level == 8 && top.maxHitPoints == 48 && top.attack == 10, "level 8")
}

func checkItems(_ check: inout Checker) {
    check.begin("oil flask")
    let base = openRoomGame(oil: 300)
    var game = holding(base, [.salve, .oilFlask])
    var events = game.step(.use(1))
    check.expect(
        game.oil == 699 && game.pack == [.salve] && game.turn == 1
            && events.contains(Event(code: .oilRefilled, first: 700)),
        "a flask at 300 gave \(game.oil)")
    game = holding(openRoomGame(oil: 800), [.oilFlask])
    _ = game.step(.use(0))
    check.expect(game.oil == 999 && game.pack.isEmpty, "a flask at 800 gave \(game.oil)")
    game = holding(openRoomGame(oil: 100), [.oilFlask])
    _ = game.step(.use(0))
    check.expect(litCount(game) == 97, "the flask did not widen the view: \(litCount(game))")

    check.begin("salve")
    game = holding(openRoomGame(oil: 1000), [.salve])
    game.hp = 5
    events = game.step(.use(0))
    check.expect(
        game.hp == 14 && events == [Event(code: .healed, first: 9)] && game.pack.isEmpty,
        "a salve on floor 1 healed to \(game.hp)")
    game = holding(openRoomGame(oil: 1000), [.salve])
    game.depth = 3
    game.hp = 5
    _ = game.step(.use(0))
    check.expect(game.hp == 16, "a salve on floor 3 healed to \(game.hp)")
    game = holding(openRoomGame(oil: 1000), [.salve])
    game.hp = 18
    events = game.step(.use(0))
    check.expect(game.hp == 20 && events == [Event(code: .healed, first: 2)], "a salve overflowed")

    check.begin("flare stone")
    game = holding(openRoomGame(oil: 1000), [.flareStone])
    game.cells[20 * mapWidth + 28] = Terrain.wall.rawValue
    game.refreshSight()
    place(&game, .oilmoth, 32, 20, awake: false)
    place(&game, .shardback, 34, 20, awake: false)
    place(&game, .dustmite, 36, 20, awake: false)
    place(&game, .dustmite, 30, 22, awake: false, hp: 9)
    place(&game, .sootcrawler, 27, 20, awake: false)
    check.expect(
        game.lit[20 * mapWidth + 27] == 0 && game.lit[20 * mapWidth + 36] != 0,
        "the setup: the wall's shadow or the radius")
    events = game.step(.use(0))
    check.expect(
        game.monsters.map { $0.kind } == [.shardback, .dustmite, .dustmite, .sootcrawler],
        "the oilmoth was not the only kill: \(game.monsters.map { $0.kind })")
    check.expect(
        game.monsters[0].hp == 8 && game.monsters[0].awake && game.monsters[1].hp == 3
            && game.monsters[2].hp == 3 && game.monsters[2].awake && game.monsters[3].hp == 10
            && !game.monsters[3].awake,
        "hit points \(game.monsters.map { $0.hp })")
    check.expect(
        game.embers == 4 && game.pack.isEmpty
            && events.contains(Event(code: .flareBurst, first: 3))
            && events.contains(Event(code: .monsterKilled, first: 3, second: 4)),
        "flare: \(game.embers) embers, \(events)")
    check.expect(game.turns.speeds == [12, 6, 12, 12, 12], "turn order \(game.turns.speeds)")

    check.begin("blink feather")
    var chosen = [Int](repeating: 0, count: mapWidth * mapHeight)
    var inside = true
    for seed in 1...40 {
        game = holding(openRoomGame(oil: 1000), [.blinkFeather])
        game.random = Random(seed: UInt64(seed))
        game.remembered = [UInt8](repeating: 0, count: mapWidth * mapHeight)
        for y in 18...22 {
            for x in 28...34 {
                game.remembered[y * mapWidth + x] = 1
            }
        }
        game.cells[20 * mapWidth + 32] = Terrain.wall.rawValue
        place(&game, .dustmite, 29, 20, awake: false)
        events = game.step(.use(0))
        inside = inside && game.x >= 28 && game.x <= 34 && game.y >= 18 && game.y <= 22
            && !(game.x == 32 && game.y == 20) && !(game.x == 29 && game.y == 20)
            && !(game.x == 30 && game.y == 20) && game.pack.isEmpty
            && count(events, .blinked) == 1
        chosen[game.y * mapWidth + game.x] = 1
    }
    check.expect(inside, "the feather landed outside the remembered free cells")
    let distinct = chosen.reduce(0) { $0 + $1 }
    check.expect(distinct >= 10, "the feather chose only \(distinct) cells")
    game = holding(openRoomGame(oil: 1000), [.blinkFeather])
    game.remembered = [UInt8](repeating: 0, count: mapWidth * mapHeight)
    game.remembered[20 * mapWidth + 30] = 1
    events = game.step(.use(0))
    check.expect(
        events == [Event(code: .nothingHappens, first: 4)] && game.pack == [.blinkFeather]
            && game.turn == 0,
        "a feather with nowhere to go was used: \(events)")

    check.begin("cartographer's moth")
    game = holding(openRoomGame(oil: 1000), [.cartographersMoth])
    let seen = game.remembered.reduce(0) { $0 + Int($1) }
    events = game.step(.use(0))
    check.expect(
        seen < 200 && game.remembered.reduce(0) { $0 + Int($1) } == mapWidth * mapHeight
            && events == [Event(code: .mapped)] && game.pack.isEmpty,
        "the moth marked \(game.remembered.reduce(0) { $0 + Int($1) }) cells")

    check.begin("gear")
    game = holding(openRoomGame(oil: 1000), [.hookedKnife, .oilFlask, .ironCrook, .quiltedCoat])
    _ = game.step(.equip(0))
    check.expect(
        game.weapon == .hookedKnife && game.attack == 4 && game.pack == [.oilFlask, .ironCrook, .quiltedCoat]
            && game.turn == 1,
        "equipping a knife: attack \(game.attack)")
    _ = game.step(.equip(1))
    check.expect(
        game.weapon == .ironCrook && game.attack == 6 && game.pack == [.oilFlask, .hookedKnife, .quiltedCoat],
        "swapping to a crook: \(game.pack)")
    _ = game.step(.equip(2))
    check.expect(game.coat == .quiltedCoat && game.defense == 2 && game.pack.count == 2, "a coat")
    game.pack.append(.tinPlate)
    game.pack.append(.wardensPike)
    _ = game.step(.equip(2))
    _ = game.step(.equip(3))
    check.expect(
        game.coat == .tinPlate && game.defense == 4 && game.weapon == .wardensPike
            && game.attack == 8 && game.pack.count == 4,
        "plate and pike: defense \(game.defense) attack \(game.attack)")
    let turn = game.turn
    let flask = game.pack.firstIndex(of: .oilFlask)!
    events = game.step(.equip(flask))
    check.expect(
        events == [Event(code: .cannotEquip, first: 1)] && game.turn == turn,
        "a flask was worn: \(events)")
    events = game.step(.use(game.pack.firstIndex(of: .hookedKnife)!))
    check.expect(
        events == [Event(code: .cannotUse, first: 6)] && game.turn == turn && game.pack.count == 4,
        "a knife was used: \(events)")
    check.expect(
        game.step(.use(9)).isEmpty && game.step(.equip(-1)).isEmpty && game.step(.drop(4)).isEmpty
            && game.turn == turn,
        "a bad slot took a turn")

    check.begin("pack")
    game = openRoomGame(oil: 1000)
    game.items = [FloorItem(kind: .salve, x: 31, y: 20), FloorItem(kind: .oilFlask, x: 32, y: 20)]
    events = game.step(.move(.east))
    check.expect(
        game.pack == [.salve] && game.items.count == 1
            && events == [Event(code: .pickedUp, first: 2)],
        "stepping onto an item: \(events)")
    game.pack = [ItemKind](repeating: .salve, count: packLimit)
    events = game.step(.move(.east))
    check.expect(
        game.pack.count == packLimit && game.items.count == 1
            && events == [Event(code: .packFull, first: 1)],
        "a full pack took an item: \(events)")
    _ = game.step(.move(.west))
    game.pack = [.tinPlate, .salve]
    events = game.step(.drop(0))
    check.expect(
        game.pack == [.salve] && game.items.contains(FloorItem(kind: .tinPlate, x: 31, y: 20))
            && game.items.count == 2 && events == [Event(code: .dropped, first: 10)],
        "dropping: \(events)")
    game.pack = [.salve]
    let turnBefore = game.turn
    events = game.step(.drop(0))
    check.expect(
        events == [Event(code: .noRoomHere, first: 2)] && game.pack == [.salve]
            && game.turn == turnBefore,
        "two items on one cell: \(events)")
    _ = game.step(.move(.west))
    events = game.step(.move(.east))
    check.expect(
        game.pack == [.salve, .tinPlate] && events == [Event(code: .pickedUp, first: 10)],
        "picking up what was dropped: \(game.pack)")
    game.items = (0..<floorItemLimit).map { FloorItem(kind: .salve, x: 1 + $0 % 50, y: 1 + $0 / 50) }
    events = game.step(.drop(0))
    check.expect(events == [Event(code: .noRoomHere, first: 2)], "a 65th floor item: \(events)")
}

func chebyshev(_ monster: Monster, _ game: Game) -> Int {
    max(abs(monster.x - game.x), abs(monster.y - game.y))
}

func checkMonsterTable(_ check: inout Checker) {
    check.begin("monster table")
    let table: [(MonsterKind, Int, Int, Int, Int, Int)] = [
        (.dustmite, 3, 2, 0, 12, 1), (.gnawling, 5, 3, 0, 18, 3), (.oilmoth, 6, 1, 0, 12, 4),
        (.sootcrawler, 10, 4, 1, 12, 6), (.shardback, 14, 4, 5, 6, 9),
        (.rootgrip, 12, 6, 2, 12, 8), (.bellwraith, 18, 7, 2, 6, 14),
        (.hearthwarden, 45, 9, 4, 12, 30),
    ]
    for (kind, hitPoints, attack, defense, speed, worth) in table {
        let actual = stats(of: kind)
        check.expect(
            actual.hitPoints == hitPoints && actual.attack == attack && actual.defense == defense
                && actual.speed == speed && actual.worth == worth
                && actual.moves == (kind != .rootgrip),
            "\(kind)")
    }
    let items: [(ItemKind, Int)] = [
        (.hookedKnife, 1), (.ironCrook, 3), (.wardensPike, 5), (.quiltedCoat, 1), (.tinPlate, 3),
        (.oilFlask, 0),
    ]
    for (kind, bonus) in items {
        check.expect(kind.bonus == bonus, "\(kind) bonus \(kind.bonus)")
    }
}

func checkPlacement(_ check: inout Checker) {
    check.begin("placement")
    var kinds = [Int](repeating: 0, count: 9)
    for seed in 1...30 {
        for depth in 1...finalDepth {
            let floor = generateFloor(seed: UInt64(seed), depth: depth)
            let label = "seed \(seed) depth \(depth)"
            for monster in floor.monsters {
                kinds[Int(monster.kind.rawValue)] += 1
            }
            let regular = floor.monsters.filter { $0.kind != .hearthwarden }
            check.expect(
                regular.count == min(16, 3 + depth) && floor.items.count == 4 + depth / 2,
                "\(label): \(regular.count) monsters, \(floor.items.count) items")
            check.expect(
                floor.monsters.count <= monsterLimit && floor.items.count <= floorItemLimit,
                "\(label): over the limits")
            var taken = [Int](repeating: 0, count: mapWidth * mapHeight)
            var gnawlings = 0
            var wardens = 0
            var legal = true
            let start = floor.rooms[floor.startRoom]
            for monster in floor.monsters {
                let index = monster.y * mapWidth + monster.x
                taken[index] += 1
                gnawlings += monster.kind == .gnawling ? 1 : 0
                wardens += monster.kind == .hearthwarden ? 1 : 0
                legal = legal && floor.cells[index] == Terrain.floor.rawValue && !monster.awake
                    && monster.hp == stats(of: monster.kind).hitPoints
                    && (monsterWeight(monster.kind, depth: depth) > 0) == (monster.kind != .hearthwarden)
                    && !(monster.x >= start.x && monster.x < start.x + start.width
                        && monster.y >= start.y && monster.y < start.y + start.height)
            }
            for item in floor.items {
                let index = item.y * mapWidth + item.x
                taken[index] += 1
                legal = legal && floor.cells[index] == Terrain.floor.rawValue
                    && itemWeight(item.kind, depth: depth) > 0
                    && !(item.x >= start.x && item.x < start.x + start.width
                        && item.y >= start.y && item.y < start.y + start.height)
            }
            check.expect(legal, "\(label): a monster or item is misplaced or out of its depth")
            check.expect(taken.max()! <= 1, "\(label): two things on one cell")
            check.expect(gnawlings % 2 == 0, "\(label): \(gnawlings) gnawlings")
            check.expect(wardens == (depth == finalDepth ? 1 : 0), "\(label): \(wardens) wardens")
            if depth == finalDepth {
                let room = floor.rooms[floor.exitRoom]
                let warden = floor.monsters.first { $0.kind == .hearthwarden }!
                check.expect(
                    warden.x >= room.x && warden.x < room.x + room.width && warden.y >= room.y
                        && warden.y < room.y + room.height,
                    "\(label): the Hearthwarden is outside the plinth's room")
            }
            if depth == 1 {
                check.expect(
                    floor.monsters.allSatisfy { $0.kind == .dustmite }, "\(label): only dustmites")
            }
            if check.failure != nil {
                return
            }
        }
    }
    check.expect(kinds[1...].allSatisfy { $0 > 0 }, "a kind never appears: \(kinds)")
    var game = Game(seed: 3)
    let second = generateFloor(seed: 3, depth: 2)
    game.cells[game.y * mapWidth + game.x] = Terrain.stairs.rawValue
    _ = game.step(.descend)
    check.expect(
        game.monsters == second.monsters && game.items == second.items
            && game.turns.speeds == [12] + second.monsters.map { stats(of: $0.kind).speed },
        "descending did not bring in the new floor's occupants")
}

func checkWaking(_ check: inout Checker) {
    check.begin("waking")
    var game = openRoomGame(oil: 1000)
    place(&game, .dustmite, 33, 20, awake: false)
    place(&game, .dustmite, 50, 20, awake: false)
    place(&game, .hearthwarden, 30, 24, awake: false)
    var wokeAt = -1
    for turn in 1...80 {
        _ = game.step(.wait)
        if wokeAt < 0 && game.monsters[0].awake {
            wokeAt = turn
        }
        if game.hp < 20 {
            game.hp = 20
        }
    }
    check.expect(wokeAt > 0 && wokeAt <= 40, "a dustmite in the light woke at \(wokeAt)")
    check.expect(game.monsters[1].x == 50 && !game.monsters[1].awake, "a dark sleeper woke")
    check.expect(
        !game.monsters[2].awake && game.monsters[2].x == 30 && game.monsters[2].y == 24,
        "the Hearthwarden woke in the light")
    var woken = 0
    var trials = 0
    for seed in 1...120 {
        var one = openRoomGame(oil: 1000)
        one.random = Random(seed: UInt64(seed))
        place(&one, .rootgrip, 33, 20, awake: false)
        _ = one.step(.wait)
        woken += one.monsters[0].awake ? 1 : 0
        trials += 1
    }
    check.expect(woken >= 26 && woken <= 56, "\(woken) of \(trials) woke on one turn, expected a third")

    var hit = openRoomGame(oil: 1000)
    place(&hit, .hearthwarden, 31, 20, awake: false)
    var events: [Event] = []
    for _ in 0..<60 where !hit.monsters[0].awake {
        events = hit.step(.move(.east))
        hit.hp = 20
    }
    check.expect(
        hit.monsters[0].awake && events.contains { $0.code == .playerHit },
        "a hit did not wake the Hearthwarden")
}

func checkChase(_ check: inout Checker) {
    check.begin("chase")
    var game = openRoomGame(oil: 1000)
    place(&game, .dustmite, 40, 20)
    var distances: [Int] = []
    for _ in 0..<12 {
        _ = game.step(.wait)
        distances.append(chebyshev(game.monsters[0], game))
    }
    check.expect(
        distances == [9, 8, 7, 6, 5, 4, 3, 2, 1, 1, 1, 1], "dustmite approach \(distances)")
    check.expect(game.hp < 20, "an adjacent dustmite never struck in four turns")

    var maze = openRoomGame(oil: 1000)
    for y in 1..<mapHeight - 1 {
        maze.cells[y * mapWidth + 36] = Terrain.wall.rawValue
    }
    maze.cells[30 * mapWidth + 36] = Terrain.floor.rawValue
    maze.refreshSight()
    place(&maze, .dustmite, 40, 20)
    var best = 99
    var arrived = false
    for _ in 0..<40 where !arrived {
        _ = maze.step(.wait)
        maze.hp = 20
        arrived = chebyshev(maze.monsters[0], maze) <= 1
        best = min(best, abs(maze.monsters[0].x - 37) + abs(maze.monsters[0].y - 30))
    }
    check.expect(arrived, "the dustmite did not find the gap in the wall")

    var far = openRoomGame(oil: 1000)
    far.x = 3
    far.y = 3
    place(&far, .dustmite, 50, 35)
    var path: [Int] = []
    var moves = 0
    var onFloor = true
    for _ in 0..<60 {
        let before = (far.monsters[0].x, far.monsters[0].y)
        _ = far.step(.wait)
        let monster = far.monsters[0]
        moves += (monster.x, monster.y) != before ? 1 : 0
        onFloor = onFloor && far.cells[monster.y * mapWidth + monster.x] == Terrain.floor.rawValue
        path.append(chebyshev(monster, far))
    }
    var steady = true
    for index in 1..<path.count {
        steady = steady && path[index] == path[index - 1] - 1
    }
    check.expect(onFloor && moves > 0, "a far monster stood still or left the floor")
    check.expect(!steady, "a monster out of reach walked straight at the player")

    var crowd = openRoomGame(oil: 1000)
    for index in 0..<6 {
        place(&crowd, .dustmite, 38 + index, 20 + index % 2)
    }
    var unique = true
    for _ in 0..<30 {
        _ = crowd.step(.wait)
        crowd.hp = 20
        var seen = [Int](repeating: 0, count: mapWidth * mapHeight)
        seen[crowd.y * mapWidth + crowd.x] = 1
        for monster in crowd.monsters {
            seen[monster.y * mapWidth + monster.x] += 1
        }
        unique = unique && seen.max()! == 1
    }
    check.expect(unique, "two actors shared a cell")
}

func checkSpeeds(_ check: inout Checker) {
    check.begin("speeds")
    var walked: [Int] = []
    for kind in [MonsterKind.shardback, .dustmite, .gnawling] {
        var game = openRoomGame(oil: 1000)
        game.x = 10
        game.y = 20
        place(&game, kind, 32, 20)
        var moves = 0
        for _ in 0..<12 {
            let before = game.monsters[0].x
            _ = game.step(.wait)
            moves += before - game.monsters[0].x
        }
        walked.append(moves)
    }
    check.expect(walked == [6, 12, 18], "cells walked in 12 turns: \(walked)")
}

func checkOilmoth(_ check: inout Checker) {
    check.begin("oilmoth")
    var game = openRoomGame(oil: 1000)
    place(&game, .oilmoth, 31, 20)
    var events = game.step(.wait)
    check.expect(
        events == [Event(code: .oilDrunk, first: 25)] && game.oil == 974 && game.hp == 20,
        "a moth's first turn: \(events), oil \(game.oil), hp \(game.hp)")
    for _ in 0..<9 {
        _ = game.step(.wait)
    }
    check.expect(game.oil == 740 && game.hp == 20, "ten turns: oil \(game.oil), hp \(game.hp)")
    check.expect(
        game.recent.allSatisfy { $0.code == .oilDrunk },
        "the moth did something but drink: \(game.recent)")
    game.oil = 60
    game.refreshSight()
    events = game.step(.wait)
    check.expect(
        game.oil == 34 && events.contains(Event(code: .oilDrunk, first: 25))
            && events.contains { $0.code == .lampDimmed },
        "a moth at 60 oil: \(events), oil \(game.oil)")
    game.oil = 6
    events = game.step(.wait)
    check.expect(
        game.oil == 0 && events.contains(Event(code: .oilDrunk, first: 5))
            && events.contains(Event(code: .lampOut)),
        "a moth at 6 oil: \(events), oil \(game.oil)")
    check.expect(game.hp == 20, "the moth bit")
    var far = openRoomGame(oil: 1000)
    far.x = 10
    place(&far, .oilmoth, 20, 20)
    for _ in 0..<12 {
        _ = far.step(.wait)
    }
    check.expect(far.monsters[0].x == 11 && far.oil == 1000 - 12 - 3 * 25, "a moth drawn to the lamp: \(far.monsters[0].x) \(far.oil)")
}

func checkRootgrip(_ check: inout Checker) {
    check.begin("rootgrip")
    var game = openRoomGame(oil: 1000)
    place(&game, .rootgrip, 36, 20)
    for _ in 0..<60 {
        _ = game.step(.wait)
    }
    check.expect(
        game.monsters[0].x == 36 && game.monsters[0].y == 20 && game.hp == 20,
        "a rootgrip moved or struck from 6 cells")
    var struck = 0
    var next = game
    for _ in 0..<4 {
        let step = next.step(.move(.east))
        struck += count(step, .monsterHit) + count(step, .monsterMissed)
    }
    check.expect(next.x == 34 && struck == 0, "walking up to the rootgrip: \(next.x), \(struck)")
    let arrival = next.step(.move(.east))
    check.expect(
        next.x == 35 && count(arrival, .monsterHit) + count(arrival, .monsterMissed) == 1,
        "the rootgrip did not strike on arrival: \(arrival)")
    var adjacent = 0
    for _ in 0..<10 {
        let events = next.step(.wait)
        adjacent += count(events, .monsterHit) + count(events, .monsterMissed)
        next.hp = 20
    }
    check.expect(adjacent == 10 && next.monsters[0].x == 36, "the rootgrip struck \(adjacent) of 10 turns")
    var away = next
    for _ in 0..<5 {
        _ = away.step(.move(.west))
    }
    check.expect(
        away.monsters[0].x == 36 && away.monsters[0].y == 20 && away.x == 30,
        "the rootgrip followed the player")
}

func demoRun(seed: UInt64, turns: Int) -> UInt64 {
    var game = Game(seed: seed)
    let bot = DemoBot()
    while game.outcome == .playing && game.turn < turns {
        _ = game.step(bot.next(game))
    }
    return game.stateHash()
}

func checkBotRuns(_ check: inout Checker) {
    check.begin("bot determinism")
    let first = demoRun(seed: 4, turns: 300)
    check.expect(first == demoRun(seed: 4, turns: 300), "one seed, two runs")
    check.expect(first != demoRun(seed: 5, turns: 300), "two seeds, one run")
}
