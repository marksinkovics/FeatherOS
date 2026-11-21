import FeatherLibC

let payloadX = saveHeaderSize + 21
let payloadY = saveHeaderSize + 22
let payloadHitPoints = saveHeaderSize + 25
let payloadPackCount = saveHeaderSize + 32

func decodeVerdict(_ bytes: [UInt8]) -> String {
    switch Game.decode(bytes) {
    case .game: return "ok"
    case .refused(let reason): return reason
    }
}

func resealed(_ file: [UInt8], payload: [UInt8]) -> [UInt8] {
    var writer = SaveWriter()
    writer.bytes += Array(file[0..<8])
    writer.put(payload.count, bytes: 4)
    writer.put(checksum(of: payload[...]))
    writer.bytes += payload
    return writer.bytes
}

func patched(_ file: [UInt8], at offset: Int, to value: UInt8) -> [UInt8] {
    var changed = file
    changed[offset] = value
    return changed
}

func botGame(seed: UInt64, turns: Int) -> Game {
    var game = Game(seed: seed)
    let bot = DemoBot()
    while game.outcome == .playing && game.turn < turns {
        _ = game.step(bot.next(game))
    }
    return game
}

func checkSaveRoundTrip(_ check: inout Checker) {
    check.begin("save round trip")
    var straight = Game(seed: 1)
    var resumed: Game?
    let bot = DemoBot()
    while straight.turn < 600 {
        _ = straight.step(bot.next(straight))
        if straight.turn == 300 && resumed == nil {
            let bytes = straight.encode()
            guard case .game(let loaded) = Game.decode(bytes) else {
                check.expect(false, "a state at turn 300 was refused: \(decodeVerdict(bytes))")
                return
            }
            check.expect(loaded.encode() == bytes, "the loaded state encodes differently")
            check.expect(loaded.stateHash() == straight.stateHash(), "the hash changed")
            check.expect(
                loaded.lit == straight.lit && loaded.turns.energy == straight.turns.energy
                    && loaded.turns.cursor == straight.turns.cursor,
                "the lit cells or the turn order changed")
            resumed = loaded
        }
    }
    guard var loaded = resumed else {
        check.expect(false, "no state at turn 300")
        return
    }
    while loaded.turn < 600 {
        _ = loaded.step(bot.next(loaded))
    }
    check.expect(
        loaded.stateHash() == straight.stateHash() && loaded.encode() == straight.encode(),
        "300 turns, save, load and 300 more differ from 600 straight")

    let late = botGame(seed: 1, turns: 1500)
    check.expect(
        late.pack.count > 0 && (late.weapon != nil || late.coat != nil),
        "the turn 1500 state holds no pack or gear")
    for game in [late, botGame(seed: 2, turns: demoTurnLimit), Game(seed: 9)] {
        let bytes = game.encode()
        if case .game(let again) = Game.decode(bytes) {
            check.expect(again.encode() == bytes, "a state at turn \(game.turn) changed")
        } else {
            check.expect(false, "a state at turn \(game.turn) was refused: \(decodeVerdict(bytes))")
        }
    }
    check.expect(
        botGame(seed: 2, turns: demoTurnLimit).outcome == .dead, "seed 2 did not end in death")
}

func checkSaveRefusals(_ check: inout Checker) {
    check.begin("save refusals")
    let game = botGame(seed: 1, turns: 300)
    let good = game.encode()
    check.expect(good.count < 16 * 1024, "a state takes \(good.count) bytes")
    check.expect(decodeVerdict(good) == "ok", "the good save was refused")
    check.expect(Array(good[0..<4]) == saveMagic, "the magic is not WDSV")

    check.expect(decodeVerdict(patched(good, at: 0, to: good[0] ^ 1)) == "magic", "header magic")
    check.expect(decodeVerdict(patched(good, at: 4, to: 2)) == "version", "version 2")
    check.expect(decodeVerdict(patched(good, at: 5, to: 1)) == "version", "version 257")
    check.expect(decodeVerdict(patched(good, at: 6, to: 1)) == "reserved", "reserved byte")
    check.expect(decodeVerdict(patched(good, at: 8, to: good[8] ^ 1)) == "length", "length byte")
    check.expect(
        decodeVerdict(patched(good, at: 12, to: good[12] ^ 0x80)) == "checksum", "checksum byte")
    for offset in [saveHeaderSize, saveHeaderSize + 100, good.count / 2, good.count - 1] {
        check.expect(
            decodeVerdict(patched(good, at: offset, to: good[offset] ^ 0x10)) == "checksum",
            "payload byte \(offset)")
    }

    check.expect(decodeVerdict(Array(good.dropLast())) == "length", "one byte short")
    check.expect(decodeVerdict(Array(good[0..<saveHeaderSize])) == "length", "header only")
    check.expect(decodeVerdict(Array(good[0..<saveHeaderSize - 1])) == "short", "short header")
    check.expect(decodeVerdict([]) == "short", "empty file")
    check.expect(decodeVerdict(good + [0]) == "length", "one byte appended")
    check.expect(
        decodeVerdict([UInt8](repeating: 0, count: saveSizeLimit + 1)) == "size", "over 64 KiB")
    check.expect(
        decodeVerdict(resealed(good, payload: Array(good[saveHeaderSize...]) + [0])) == "length",
        "trailing byte inside the payload")
    check.expect(
        decodeVerdict(resealed(good, payload: Array(good[saveHeaderSize..<good.count - 1])))
            == "length",
        "payload ending inside a field")

    check.expect(decodeVerdict(patched(good, at: payloadX, to: good[payloadX] ^ 1)) == "checksum", "unsealed edit")
    func sealedPatch(_ offset: Int, _ value: UInt8) -> String {
        decodeVerdict(
            resealed(good, payload: Array(patched(good, at: offset, to: value)[saveHeaderSize...])))
    }
    check.expect(sealedPatch(payloadX, UInt8(mapWidth)) == "position", "x off the map")
    check.expect(sealedPatch(payloadY, 255) == "position", "y off the map")
    var wall = patched(good, at: payloadX, to: 0)
    wall[payloadY] = 0
    check.expect(
        decodeVerdict(resealed(good, payload: Array(wall[saveHeaderSize...]))) == "position",
        "the player on a wall")
    check.expect(sealedPatch(payloadPackCount, UInt8(packLimit + 1)) == "count", "pack of 11")
    check.expect(sealedPatch(payloadHitPoints, 255) == "range", "hit points over the maximum")
    check.expect(sealedPatch(saveHeaderSize + 8, 0) == "range", "depth 0")
    check.expect(sealedPatch(saveHeaderSize + 8, UInt8(finalDepth + 1)) == "range", "depth 9")
    check.expect(sealedPatch(saveHeaderSize + 29, 3) == "range", "outcome 3")
    check.expect(sealedPatch(saveHeaderSize + 30, 1) == "kind", "a flask worn as a weapon")

    let cellsStart = saveHeaderSize + 33 + game.pack.count
    check.expect(sealedPatch(cellsStart, 9) == "range", "terrain 9")
    let monstersStart = cellsStart + 2 * mapWidth * mapHeight
    check.expect(game.monsters.count > 0, "no monsters at turn 300")
    check.expect(sealedPatch(monstersStart, UInt8(monsterLimit + 1)) == "count", "65 monsters")
    check.expect(sealedPatch(monstersStart + 1, 0) == "kind", "monster kind 0")
    check.expect(sealedPatch(monstersStart + 1, 99) == "kind", "monster kind 99")
    check.expect(sealedPatch(monstersStart + 2, UInt8(mapWidth)) == "position", "monster off map")
    check.expect(sealedPatch(monstersStart + 4, 0) == "range", "monster with 0 hit points")
    check.expect(sealedPatch(monstersStart + 6, 2) == "range", "monster awake 2")
    let itemsStart = monstersStart + 1 + 6 * game.monsters.count
    check.expect(sealedPatch(itemsStart, UInt8(floorItemLimit + 1)) == "count", "65 items")
    check.expect(game.items.count > 0, "no items at turn 300")
    check.expect(sealedPatch(itemsStart + 1, 11) == "kind", "item kind 11")
    check.expect(sealedPatch(itemsStart + 3, UInt8(mapHeight)) == "position", "item off the map")
    let energyStart = itemsStart + 1 + 3 * game.items.count
    check.expect(sealedPatch(energyStart, 200) == "range", "energy 200")
    let cursorAt = energyStart + game.monsters.count + 1
    check.expect(sealedPatch(cursorAt, UInt8(game.monsters.count + 2)) == "range", "cursor")
    check.expect(sealedPatch(cursorAt + 1, UInt8(recentLimit + 1)) == "count", "4 messages")
    if !game.recent.isEmpty {
        check.expect(sealedPatch(cursorAt + 2, 0) == "kind", "message code 0")
    }
}

func checkSaveFiles(_ check: inout Checker) {
    check.begin("save files")
    let path = "/tmp/.wickdeep-selftest"
    unlink(path)
    unlink(path + ".tmp")
    unlink(path + ".bad")
    if case .none = loadSavedGame(path: path) {
    } else {
        check.expect(false, "a missing save was found")
    }
    let game = botGame(seed: 3, turns: 120)
    check.expect(saveGame(game, path: path) == nil, "the save was not written")
    check.expect(readFile(path: path + ".tmp", limit: 1) == nil, "the temporary file is left")
    if case .game(let loaded) = loadSavedGame(path: path) {
        check.expect(loaded.encode() == game.encode(), "the saved game came back different")
    } else {
        check.expect(false, "the saved game was not loaded")
    }
    var damaged = game.encode()
    damaged[damaged.count - 1] ^= 1
    check.expect(writeFileAtomically(damaged, path: path) == nil, "the damaged save not written")
    if case .refused(let reason) = loadSavedGame(path: path) {
        check.expect(reason == "checksum", "the damaged save gave \(reason)")
    } else {
        check.expect(false, "the damaged save was loaded")
    }
    check.expect(readFile(path: path, limit: 1) == nil, "the refused save is still in place")
    check.expect(
        readFile(path: path + ".bad", limit: saveSizeLimit) == damaged, "the refused save is lost")
    unlink(path + ".bad")
}

func line(_ score: Int, _ depth: Int, _ turns: Int, _ outcome: String) -> [UInt8] {
    Array("\(score) \(depth) \(turns) \(outcome)\n".utf8)
}

func checkHall(_ check: inout Checker) {
    check.begin("hall of runs")
    let text = line(40, 2, 90, "dead") + line(1945, 8, 2690, "victory") + line(40, 3, 80, "dead")
    guard let hall = Hall.parse(text) else {
        check.expect(false, "a sound file did not parse")
        return
    }
    check.expect(
        hall.runs.map { $0.score } == [1945, 40, 40] && hall.runs[1].turns == 90
            && hall.runs[2].turns == 80 && hall.runs[0].outcome == .victory,
        "the runs are not in score order with ties kept")
    check.expect(Hall.parse(hall.serialized()) == hall, "the hall did not round trip")
    check.expect(Hall.parse([]) == Hall(), "an empty file is an empty hall")

    let bad: [[UInt8]] = [
        Array("garbage".utf8), line(5, 0, 1, "dead"), line(5, 9, 1, "dead"),
        line(5, 1, 1, "playing"), line(1_000_000_000, 1, 1, "dead"),
        Array("5 1 1 dead".utf8), Array("5 1 dead\n".utf8), Array("5  1 1 dead\n".utf8),
        line(5, 1, 1, "dead") + Array("x\n".utf8),
        (0..<11).reduce([UInt8]()) { $0 + line(100 - $1, 1, 1, "dead") },
    ]
    for (number, bytes) in bad.enumerated() {
        check.expect(Hall.parse(bytes) == nil, "bad file \(number) parsed")
    }

    var table = Hall()
    for score in 1...10 {
        table.insert(Run(score: score * 10, depth: 1, turns: score, outcome: .dead))
    }
    check.expect(table.runs.count == 10 && table.runs[0].score == 100, "ten runs")
    check.expect(!table.qualifies(10) && table.qualifies(11), "the qualifying score")
    check.expect(
        table.insert(Run(score: 5, depth: 1, turns: 1, outcome: .dead)) == nil, "a low score fit")
    check.expect(
        table.insert(Run(score: 55, depth: 8, turns: 9, outcome: .victory)) == 5,
        "the rank of 55")
    check.expect(table.runs.count == 10 && table.runs[9].score == 20, "the lowest was not dropped")
    check.expect(
        table.insert(Run(score: 100, depth: 2, turns: 2, outcome: .dead)) == 1,
        "a tie goes below the older run")

    let path = "/tmp/.wickdeep-selftest-runs"
    unlink(path)
    unlink(path + ".tmp")
    check.expect(loadHall(path: path) == Hall(), "a missing file is an empty hall")
    check.expect(saveHall(table, path: path) == nil, "the hall was not written")
    check.expect(loadHall(path: path) == table, "the hall did not come back")
    check.expect(readFile(path: path + ".tmp", limit: 1) == nil, "the temporary file is left")
    check.expect(
        writeFileAtomically(Array("not a hall\n".utf8), path: path) == nil, "the junk not written")
    check.expect(loadHall(path: path) == Hall(), "a file that does not parse was used")
    check.expect(saveHall(table, path: path) == nil, "the hall was not replaced")
    check.expect(loadHall(path: path) == table, "the replaced hall did not come back")
    unlink(path)
}
