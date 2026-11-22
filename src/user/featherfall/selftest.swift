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

extension Game {
    mutating func fill(row: Int, columns: Range<Int>) {
        for column in columns {
            well[row * wellWidth + column] = 9
        }
    }

    mutating func place(_ kind: PieceKind, rotation: Int = 0, x: Int, y: Int) {
        active = ActivePiece(kind: kind, rotation: rotation, x: x, y: y)
        gravityTimer = 0
        lockTimer = 0
        lockResets = 0
        lowestRow = y
    }

    mutating func run(_ inputs: Inputs, ticks: Int) {
        for _ in 0..<ticks {
            step(inputs, milliseconds: 10)
        }
    }

    var filledCells: Int {
        well.reduce(0) { $0 + ($1 != 0 ? 1 : 0) }
    }
}

func sameCells(_ first: [Cell], _ second: [Cell]) -> Bool {
    first.count == second.count && first.allSatisfy { second.contains($0) }
}

func cellList(_ pairs: [(Int, Int)]) -> [Cell] {
    pairs.map { Cell(x: $0.0, y: $0.1) }
}

func checkRandom(_ check: inout Checker) {
    check.begin("random")
    var first = Random(seed: 1)
    check.expect(first.next() == 10_451_216_379_200_822_465, "first value of seed 1")
    check.expect(first.next() == 13_757_245_211_066_428_519, "second value of seed 1")
    var left = Random(seed: 77)
    var right = Random(seed: 77)
    var other = Random(seed: 78)
    var same = true
    var differs = false
    for _ in 0..<100 {
        let value = left.next()
        same = same && value == right.next()
        differs = differs || value != other.next()
    }
    check.expect(same, "equal seeds diverged")
    check.expect(differs, "different seeds gave the same sequence")
    var inRange = true
    for _ in 0..<1000 {
        let value = left.below(7)
        inRange = inRange && value >= 0 && value < 7
    }
    check.expect(inRange, "below(7) left its range")
}

func checkShapes(_ check: inout Checker) {
    check.begin("shapes")
    for index in 0..<PieceKind.count {
        let kind = PieceKind(rawValue: index)!
        let size = kind.boxSize
        let base = pieceCells(kind, rotation: 0)
        for rotation in 0..<4 {
            let turned = pieceCells(kind, rotation: rotation)
            let inside = turned.allSatisfy { $0.x >= 0 && $0.x < size && $0.y >= 0 && $0.y < size }
            check.expect(turned.count == 4 && inside, "piece \(index) rotation \(rotation)")
            let distinct = turned.allSatisfy { cell in turned.filter { $0 == cell }.count == 1 }
            check.expect(distinct, "piece \(index) rotation \(rotation) overlaps")
        }
        let reflected = base.map { Cell(x: size - 1 - $0.x, y: size - 1 - $0.y) }
        check.expect(
            sameCells(pieceCells(kind, rotation: 2), reflected), "piece \(index) half turn")
        let again = pieceCells(kind, rotation: 3).map { Cell(x: size - 1 - $0.y, y: $0.x) }
        check.expect(sameCells(again, base), "piece \(index) four quarter turns")
        var spawned = Game(seed: 1)
        spawned.spawn(kind)
        check.expect(
            spawned.active.cells.allSatisfy { $0.y < hiddenRows }
                && spawned.active.x == (wellWidth - size) / 2,
            "piece \(index) spawn")
    }
    let bar = pieceCells(.bar, rotation: 0)
    check.expect(bar.allSatisfy { $0.y == 1 }, "the bar starts flat")
    check.expect(pieceCells(.bar, rotation: 1).allSatisfy { $0.x == 2 }, "the bar turns upright")
}

func checkBag(_ check: inout Checker) {
    check.begin("bag")
    var game = Game(seed: 3)
    let shown = game.preview
    var order = [game.active.kind.rawValue]
    for _ in 0..<20 {
        order.append(game.takeNext().rawValue)
    }
    for group in 0..<3 {
        let bag = order[(group * 7)..<(group * 7 + 7)]
        check.expect((0..<7).allSatisfy { bag.contains($0) }, "bag \(group) is not a permutation")
    }
    check.expect(
        order[1] == shown[0].rawValue && order[2] == shown[1].rawValue
            && order[3] == shown[2].rawValue,
        "the preview does not match what comes next")
    var neighbor = Game(seed: 4)
    var other = [neighbor.active.kind.rawValue]
    for _ in 0..<20 {
        other.append(neighbor.takeNext().rawValue)
    }
    check.expect(order != other, "seeds 3 and 4 give the same order")
    var repeated = Game(seed: 3)
    var again = [repeated.active.kind.rawValue]
    for _ in 0..<20 {
        again.append(repeated.takeNext().rawValue)
    }
    check.expect(order == again, "one seed gave two orders")
}

func checkRotation(_ check: inout Checker) {
    check.begin("rotation")
    var game = Game(seed: 1)
    game.place(.spur, x: 3, y: 5)
    check.expect(game.rotate(clockwise: true), "no clockwise turn in the open")
    check.expect(
        sameCells(game.active.cells, cellList([(4, 5), (4, 6), (4, 7), (5, 6)])),
        "clockwise cells")
    check.expect(game.rotate(clockwise: false), "no counterclockwise turn in the open")
    check.expect(
        sameCells(game.active.cells, pieceCells(.spur, rotation: 0).map { Cell(x: $0.x + 3, y: $0.y + 5) }),
        "counterclockwise turn did not undo it")
    for _ in 0..<4 {
        game.rotate(clockwise: true)
    }
    check.expect(game.active.rotation == 0 && game.active.x == 3, "four turns are not the identity")

    game.place(.block, x: 4, y: 5)
    check.expect(!game.rotate(clockwise: true) && game.active.rotation == 0, "the block turned")
}

func checkKicks(_ check: inout Checker) {
    check.begin("kicks")
    var game = Game(seed: 1)
    game.place(.spur, rotation: 1, x: -1, y: 5)
    check.expect(game.rotate(clockwise: true), "right kick refused")
    check.expect(game.active.x == 0 && game.active.rotation == 2, "right kick landed at \(game.active.x)")

    game.place(.spur, rotation: 3, x: 8, y: 5)
    check.expect(game.rotate(clockwise: true), "left kick refused")
    check.expect(game.active.x == 7 && game.active.rotation == 0, "left kick landed at \(game.active.x)")

    game.place(.spur, x: 3, y: 5)
    game.well[7 * wellWidth + 4] = 9
    check.expect(game.rotate(clockwise: true), "blocked turn refused")
    check.expect(game.active.x == 2 && game.active.y == 5, "left was not tried before right")

    game.well[7 * wellWidth + 4] = 0
    game.place(.spur, x: 3, y: 20)
    check.expect(game.rotate(clockwise: true), "up kick refused")
    check.expect(game.active.x == 3 && game.active.y == 19, "up kick landed at \(game.active.y)")

    game.place(.bar, rotation: 1, x: -2, y: 5)
    check.expect(!game.rotate(clockwise: true), "a turn with no room succeeded")
    check.expect(
        game.active.rotation == 1 && game.active.x == -2 && game.active.y == 5,
        "a refused turn moved the piece")
}

func checkLineClears(_ check: inout Checker) {
    check.begin("line clears")
    let scores = [0, 100, 300, 500, 800]
    for lines in 1...4 {
        var game = Game(seed: 1)
        for row in (wellRows - lines)..<wellRows {
            game.fill(row: row, columns: 0..<9)
        }
        game.place(.bar, rotation: 1, x: 7, y: 0)
        game.run(Inputs(hardDrop: true), ticks: 1)
        check.expect(game.lines == lines, "\(lines) rows gave \(game.lines) lines")
        check.expect(
            game.score == scores[lines] + 36, "\(lines) rows scored \(game.score)")
        check.expect(game.filledCells == 4 - lines, "\(lines) rows left \(game.filledCells) cells")
        var stacked = true
        for row in (wellRows - (4 - lines))..<wellRows {
            stacked = stacked && game.well[row * wellWidth + 9] != 0
        }
        check.expect(stacked, "\(lines) rows: the bar's rest did not drop")
    }

    var game = Game(seed: 1)
    game.lines = 10
    for row in 18..<wellRows {
        game.fill(row: row, columns: 0..<9)
    }
    game.place(.bar, rotation: 1, x: 7, y: 0)
    game.run(Inputs(hardDrop: true), ticks: 1)
    check.expect(game.score == 1600 + 36, "a four-row clear at level 2 scored \(game.score)")
    check.expect(game.level == 2 && game.lines == 14, "level \(game.level), lines \(game.lines)")

    game = Game(seed: 1)
    game.lines = 9
    game.fill(row: wellRows - 1, columns: 0..<9)
    game.place(.bar, rotation: 1, x: 7, y: 0)
    game.run(Inputs(hardDrop: true), ticks: 1)
    check.expect(game.score == 100 + 36 && game.level == 2, "the tenth line scored at level 1 only")

    game = Game(seed: 1)
    game.fill(row: 21, columns: 0..<9)
    game.fill(row: 20, columns: 0..<8)
    game.fill(row: 19, columns: 0..<9)
    game.place(.bar, rotation: 1, x: 7, y: 0)
    game.run(Inputs(hardDrop: true), ticks: 1)
    var shifted = game.lines == 2 && game.filledCells == 10
    shifted = shifted && game.well[21 * wellWidth + 7] != 0 && game.well[21 * wellWidth + 8] == 0
    shifted = shifted && game.well[21 * wellWidth + 9] != 0 && game.well[20 * wellWidth + 9] != 0
    check.expect(shifted, "two separate rows did not close up")
}

func checkScoring(_ check: inout Checker) {
    check.begin("scoring")
    var game = Game(seed: 1)
    game.place(.block, x: 4, y: 0)
    game.run(Inputs(softDrop: true), ticks: 40)
    check.expect(game.active.y == 10 && game.score == 10, "soft drop: row \(game.active.y), score \(game.score)")

    game = Game(seed: 1)
    game.place(.block, x: 4, y: 0)
    game.run(Inputs(), ticks: 80)
    check.expect(game.active.y == 1 && game.score == 0, "gravity: row \(game.active.y), score \(game.score)")

    game = Game(seed: 1)
    game.place(.block, x: 4, y: 0)
    game.run(Inputs(hardDrop: true), ticks: 1)
    check.expect(game.score == 40, "hard drop scored \(game.score)")
    check.expect(game.filledCells == 4 && game.well[20 * wellWidth + 4] != 0, "hard drop did not lock on the floor")
}

func checkLockDelay(_ check: inout Checker) {
    check.begin("lock delay")
    var game = Game(seed: 1)
    game.place(.block, x: 4, y: 20)
    game.run(Inputs(), ticks: 49)
    check.expect(game.filledCells == 0 && game.active.y == 20, "locked after 490 ms")
    game.run(Inputs(), ticks: 1)
    check.expect(game.filledCells == 4, "not locked after 500 ms")

    game = Game(seed: 1)
    game.place(.block, x: 4, y: 20)
    for move in 0..<15 {
        game.run(Inputs(left: move % 2 == 0, right: move % 2 == 1), ticks: 1)
        game.run(Inputs(), ticks: 3)
    }
    check.expect(game.lockResets == 15 && game.filledCells == 0, "resets \(game.lockResets) after 15 moves")
    var ticks = 0
    var move = 0
    while game.filledCells == 0 && ticks < 100 {
        let pressed = ticks % 4 == 0
        game.run(Inputs(left: pressed && move % 2 == 0, right: pressed && move % 2 == 1), ticks: 1)
        move += pressed ? 1 : 0
        ticks += 1
    }
    check.expect(ticks == 46, "locked after \(ticks) more ticks, expected 46 with no reset left")
}

func checkHold(_ check: inout Checker) {
    check.begin("hold")
    var game = Game(seed: 9)
    let first = game.active.kind
    let following = game.preview[0]
    game.run(Inputs(hold: true), ticks: 1)
    check.expect(game.held == first && game.active.kind == following, "first hold")
    check.expect(game.active.y == 0 && game.active.rotation == 0, "the held piece did not respawn")
    let before = game.pieceCount
    game.run(Inputs(hold: true), ticks: 1)
    check.expect(game.held == first && game.active.kind == following && game.pieceCount == before, "second hold on the same piece")
    game.run(Inputs(hardDrop: true), ticks: 1)
    let latest = game.active.kind
    check.expect(!game.holdUsed, "hold not available after a lock")
    game.run(Inputs(hold: true), ticks: 1)
    check.expect(game.held == latest && game.active.kind == first, "swap after a lock")
}

func checkGameOver(_ check: inout Checker) {
    check.begin("game over")
    var game = Game(seed: 1)
    for row in 2..<wellRows {
        game.fill(row: row, columns: 3..<7)
    }
    game.place(.block, x: 4, y: 0)
    game.run(Inputs(), ticks: 50)
    check.expect(game.phase == .over, "a piece locked in the hidden rows did not end the game")
    let hash = game.wellHash()
    let pieces = game.pieceCount
    game.run(Inputs(left: true, hardDrop: true, pause: true), ticks: 10)
    check.expect(game.phase == .over && game.wellHash() == hash && game.pieceCount == pieces, "input after game over")

    game = Game(seed: 2)
    game.fill(row: 0, columns: 3..<7)
    game.fill(row: 1, columns: 3..<7)
    game.place(.block, x: 0, y: 0)
    game.run(Inputs(hardDrop: true), ticks: 1)
    check.expect(game.phase == .over && game.filledCells == 12, "a blocked spawn did not end the game")
}

func checkPause(_ check: inout Checker) {
    check.begin("pause")
    var game = Game(seed: 1)
    game.place(.block, x: 4, y: 0)
    game.run(Inputs(pause: true), ticks: 1)
    check.expect(game.phase == .paused, "pause did not pause")
    game.run(Inputs(left: true, rotateClockwise: true, hardDrop: true, hold: true), ticks: 200)
    check.expect(game.active.y == 0 && game.active.x == 4 && game.filledCells == 0, "the piece moved while paused")
    game.run(Inputs(pause: true), ticks: 1)
    check.expect(game.phase == .playing, "pause did not resume")
    game.run(Inputs(), ticks: 79)
    check.expect(game.active.y == 1, "gravity after resuming: row \(game.active.y)")
}

func checkRepeat(_ check: inout Checker) {
    check.begin("auto-repeat")
    var game = Game(seed: 1)
    game.place(.block, x: 4, y: 0)
    let held = Inputs(left: true)
    game.run(held, ticks: 17)
    check.expect(game.active.x == 3, "after 170 ms of the press: \(game.active.x)")
    game.run(held, ticks: 1)
    check.expect(game.active.x == 2, "no first repeat at 170 ms: \(game.active.x)")
    game.run(held, ticks: 4)
    check.expect(game.active.x == 2, "second repeat came early: \(game.active.x)")
    game.run(held, ticks: 1)
    check.expect(game.active.x == 1, "no second repeat 50 ms later: \(game.active.x)")

    game.run(Inputs(), ticks: 1)
    game.run(held, ticks: 1)
    check.expect(game.active.x == 0, "a new press did not move at once")

    game = Game(seed: 1)
    game.place(.block, x: 4, y: 0)
    game.run(Inputs(left: true, right: true), ticks: 30)
    check.expect(game.active.x == 4, "both directions moved the piece")
}

func checkGravity(_ check: inout Checker) {
    check.begin("gravity")
    var game = Game(seed: 1)
    check.expect(game.level == 1 && gravityInterval(level: 1) == 800, "level 1")
    var falling = true
    for level in 2...18 {
        falling = falling && gravityInterval(level: level) < gravityInterval(level: level - 1)
    }
    check.expect(falling, "gravity does not speed up with every level")
    check.expect(gravityInterval(level: 99) == gravityInterval(level: 18), "gravity past the table")
    game.lines = 9
    check.expect(game.level == 1, "level at 9 lines")
    game.lines = 10
    check.expect(game.level == 2, "level at 10 lines")
    game.place(.block, x: 4, y: 0)
    game.run(Inputs(), ticks: 71)
    check.expect(game.active.y == 0, "level 2 dropped before 720 ms")
    game.run(Inputs(), ticks: 1)
    check.expect(game.active.y == 1, "level 2 did not drop at 720 ms")
}

func checkGhost(_ check: inout Checker) {
    check.begin("ghost")
    var game = Game(seed: 1)
    game.place(.block, x: 4, y: 3)
    check.expect(game.ghostRow() == 20, "ghost on an empty floor: \(game.ghostRow())")
    game.well[21 * wellWidth + 5] = 9
    check.expect(game.ghostRow() == 19, "ghost above a block: \(game.ghostRow())")
    game.run(Inputs(hardDrop: true), ticks: 1)
    check.expect(game.well[19 * wellWidth + 4] != 0 && game.well[20 * wellWidth + 5] != 0, "hard drop and ghost disagree")
}

func scoreEntry(_ text: String, _ score: Int) -> HighScore {
    HighScore(initials: Array(text.utf8), score: score)
}

func checkHighScores(_ check: inout Checker) {
    check.begin("high scores")
    let valid = Array("AAA 500\nBBB 900\nCCC 20\n".utf8)
    let parsed = HighScores.parse(valid)
    check.expect(parsed?.entries.count == 3, "a valid file parsed to \(parsed?.entries.count ?? -1)")
    check.expect(parsed?.entries.first == scoreEntry("BBB", 900), "entries are not sorted")
    check.expect(parsed?.serialized() == Array("BBB 900\nAAA 500\nCCC 20\n".utf8), "serialized text")
    check.expect(HighScores.parse([])?.entries.count == 0, "an empty file is an empty table")
    let garbage = [
        "hello\n", "AAA\n", "AAA 12", "aaa 12\n", "AAAA 12\n", "AA 12\n", "AAA  12\n",
        "AAA -5\n", "AAA 1234567890\n", "AAA 12\nBBB x\n", "AAA 12\n\n", "\n",
    ]
    for text in garbage {
        check.expect(HighScores.parse(Array(text.utf8)) == nil, "garbage parsed: \(text)")
    }
    var eleven = ""
    for index in 0..<11 {
        eleven += "AAA \(index + 1)\n"
    }
    check.expect(HighScores.parse(Array(eleven.utf8)) == nil, "eleven lines parsed")
    check.expect(HighScores.parse(Array("AAA 999999999\n".utf8))?.entries.count == 1, "nine digits")

    var table = HighScores()
    check.expect(!table.qualifies(0), "a score of 0 qualified")
    check.expect(table.qualifies(1), "the first score did not qualify")
    check.expect(table.insert(scoreEntry("AAA", 100)) == 0, "first rank")
    check.expect(table.insert(scoreEntry("BBB", 300)) == 0, "a higher score ranks first")
    check.expect(table.insert(scoreEntry("CCC", 100)) == 2, "a tie ranks below the older entry")
    check.expect(table.entries.map { $0.score } == [300, 100, 100], "order \(table.entries)")
    for index in 0..<7 {
        table.insert(scoreEntry("ZZZ", 200 + index))
    }
    check.expect(table.entries.count == 10, "full table has \(table.entries.count)")
    check.expect(table.entries[9].score == 100, "the lowest score is \(table.entries[9].score)")
    check.expect(!table.qualifies(100), "a tie with the last place qualified")
    check.expect(table.qualifies(101), "a score above the last place did not qualify")
    check.expect(table.insert(scoreEntry("DDD", 100)) == nil, "a score below the table ranked")
    check.expect(table.insert(scoreEntry("EEE", 101)) == 8, "rank above the last place")
    check.expect(table.entries.count == 10, "top 10 truncation: \(table.entries.count)")
    check.expect(table.entries[8].score == 101 && table.entries[9].score == 100, "dropped entry")
    check.expect(HighScores.parse(table.serialized()) == table, "a round trip through text")

    let path = "/tmp/.featherfall-selftest"
    check.expect(loadHighScores(path: path).entries.isEmpty, "a missing file")
    check.expect(saveHighScores(table, path: path) == nil, "saving failed")
    check.expect(loadHighScores(path: path) == table, "a round trip through a file")
    if let stream = fopen(path, "w") {
        fputs("this is not a high score file\n", stream)
        fclose(stream)
    }
    check.expect(loadHighScores(path: path).entries.isEmpty, "a garbage file was not ignored")
    check.expect(saveHighScores(table, path: path) == nil, "replacing failed")
    check.expect(loadHighScores(path: path) == table, "a garbage file was not replaced")
    unlink(path)
    check.expect(saveHighScores(table, path: "/nonexistent/.featherfall") != nil, "a bad path saved")
}

func checkDeterminism(_ check: inout Checker) {
    check.begin("determinism")
    var hashes: [UInt64] = []
    for seed: UInt64 in [11, 11, 12] {
        var game = Game(seed: seed)
        var bot = DemoBot()
        for _ in 0..<3000 {
            game.step(bot.next(game), milliseconds: 10)
        }
        hashes.append(game.wellHash() ^ UInt64(game.score))
    }
    check.expect(hashes[0] == hashes[1], "one seed gave two games")
    check.expect(hashes[0] != hashes[2], "two seeds gave the same game")
}

func runSelfTest() -> Bool {
    var check = Checker()
    checkRandom(&check)
    checkShapes(&check)
    checkBag(&check)
    checkRotation(&check)
    checkKicks(&check)
    checkLineClears(&check)
    checkScoring(&check)
    checkLockDelay(&check)
    checkHold(&check)
    checkGameOver(&check)
    checkPause(&check)
    checkRepeat(&check)
    checkGravity(&check)
    checkGhost(&check)
    checkHighScores(&check)
    checkDeterminism(&check)
    if let failure = check.failure {
        print("featherfall: selftest failed: \(failure)")
        return false
    }
    print("featherfall: selftest ok")
    return true
}
