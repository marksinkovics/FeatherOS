let demoPieces = 300
let demoTickLimit = 100_000
let demoPauseAt = 20
let demoPauseTicks = 5
let demoAttemptLimit = 40

// A fixed script: a greedy player that picks a placement for each piece and sends it as inputs,
// with scripted holds, a pause, soft drops to the lock and pieces left to gravity.
struct DemoBot {
    var plannedPiece = 0
    var targetRotation = 0
    var targetColumn = 0
    var attempts = 0
    var aligned = false
    var idleTurn = false
    var holdTriedFor = 0
    var pauseStage = 0
    var waitTicks = 0

    mutating func next(_ game: Game) -> Inputs {
        var inputs = Inputs()
        if game.pieceCount == demoPauseAt && pauseStage == 0 {
            pauseStage = 1
            inputs.pause = true
            return inputs
        }
        if pauseStage >= 1 && pauseStage <= demoPauseTicks {
            pauseStage += 1
            inputs.left = true
            inputs.hardDrop = true
            return inputs
        }
        if pauseStage == demoPauseTicks + 1 {
            pauseStage += 1
            inputs.pause = true
            return inputs
        }
        if game.pieceCount != plannedPiece {
            plan(game)
        }
        if game.pieceCount % 9 == 4 && !game.holdUsed && holdTriedFor != game.pieceCount {
            holdTriedFor = game.pieceCount
            inputs.hold = true
            return inputs
        }
        if !aligned {
            if idleTurn {
                idleTurn = false
                return inputs
            }
            let piece = game.active
            if attempts < demoAttemptLimit
                && (piece.rotation != targetRotation || piece.x != targetColumn)
            {
                attempts += 1
                idleTurn = true
                if piece.rotation != targetRotation {
                    inputs.rotateCounterclockwise = targetRotation == 3
                    inputs.rotateClockwise = targetRotation != 3
                } else {
                    inputs.left = piece.x > targetColumn
                    inputs.right = piece.x < targetColumn
                }
                return inputs
            }
            aligned = true
        }
        if game.pieceCount % 5 == 2 {
            inputs.softDrop = true
        } else if game.pieceCount % 13 == 6 && waitTicks < 100 {
            waitTicks += 1
        } else {
            inputs.hardDrop = true
        }
        return inputs
    }

    private mutating func plan(_ game: Game) {
        plannedPiece = game.pieceCount
        attempts = 0
        aligned = false
        idleTurn = false
        waitTicks = 0
        var best = Int.min
        targetRotation = 0
        targetColumn = game.active.x
        for rotation in 0..<4 {
            for column in -2..<wellWidth {
                var piece = ActivePiece(kind: game.active.kind, rotation: rotation, x: column, y: 0)
                guard game.fits(piece) else {
                    continue
                }
                piece.y = Game.landing(game.well, piece)
                let value = evaluate(game.well, piece)
                if value > best {
                    best = value
                    targetRotation = rotation
                    targetColumn = column
                }
            }
        }
    }

    private func evaluate(_ well: [UInt8], _ piece: ActivePiece) -> Int {
        var cells = well
        for cell in piece.cells {
            cells[cell.y * wellWidth + cell.x] = 1
        }
        var lines = 0
        for row in 0..<wellRows
        where (0..<wellWidth).allSatisfy({ cells[row * wellWidth + $0] != 0 }) {
            lines += 1
        }
        var holes = 0
        var height = 0
        var bumpiness = 0
        var previous = 0
        for column in 0..<wellWidth {
            var top = wellRows
            for row in 0..<wellRows where cells[row * wellWidth + column] != 0 {
                top = row
                break
            }
            for row in top..<wellRows where cells[row * wellWidth + column] == 0 {
                holes += 1
            }
            let columnHeight = wellRows - top
            height += columnHeight
            if column > 0 {
                bumpiness += abs(columnHeight - previous)
            }
            previous = columnHeight
        }
        return lines * 80 - holes * 100 - bumpiness * 2 - height * 3
    }
}

func runDemo(seed: UInt64) {
    var game = Game(seed: seed)
    var bot = DemoBot()
    var ticks = 0
    while game.phase != .over && game.pieceCount <= demoPieces && ticks < demoTickLimit {
        game.step(bot.next(game), milliseconds: 10)
        ticks += 1
    }
    print(
        "featherfall: demo seed \(seed) score \(game.score) lines \(game.lines) "
            + "level \(game.level) hash \(game.wellHash())")
}
