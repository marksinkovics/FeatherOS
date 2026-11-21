let wellWidth = 10
let wellRows = 22
let hiddenRows = 2
let previewCount = 3
let lockDelay = 500
let maximumLockResets = 15
let repeatDelay = 170
let repeatRate = 50
let softDropInterval = 40
let lineScores = [0, 100, 300, 500, 800]
let gravityIntervals = [
    800, 720, 640, 560, 480, 400, 320, 240, 180, 130, 100, 80, 60, 50, 40, 30, 20, 10,
]
let kicks = [Cell(x: 0, y: 0), Cell(x: -1, y: 0), Cell(x: 1, y: 0), Cell(x: 0, y: -1)]

func gravityInterval(level: Int) -> Int {
    gravityIntervals[min(level, gravityIntervals.count) - 1]
}

struct Inputs {
    var left = false
    var right = false
    var softDrop = false
    var rotateClockwise = false
    var rotateCounterclockwise = false
    var hardDrop = false
    var hold = false
    var pause = false
}

enum Phase {
    case playing
    case paused
    case over
}

struct ActivePiece {
    var kind: PieceKind
    var rotation: Int
    var x: Int
    var y: Int

    var cells: [Cell] {
        pieceCells(kind, rotation: rotation).map { Cell(x: $0.x + x, y: $0.y + y) }
    }
}

struct Game {
    var well = [UInt8](repeating: 0, count: wellWidth * wellRows)
    var random: Random
    var upcoming: [PieceKind] = []
    var active = ActivePiece(kind: .bar, rotation: 0, x: 0, y: 0)
    var held: PieceKind?
    var holdUsed = false
    var phase = Phase.playing
    var score = 0
    var lines = 0
    var pieceCount = 0
    var gravityTimer = 0
    var lockTimer = 0
    var lockResets = 0
    var lowestRow = 0
    var repeatDirection = 0
    var repeatTimer = 0

    var level: Int {
        lines / 10 + 1
    }

    var preview: [PieceKind] {
        Array(upcoming[0..<previewCount])
    }

    init(seed: UInt64) {
        random = Random(seed: seed)
        spawn(takeNext())
    }

    mutating func takeNext() -> PieceKind {
        if upcoming.count <= previewCount {
            appendBag()
        }
        let kind = upcoming.removeFirst()
        if upcoming.count <= previewCount {
            appendBag()
        }
        return kind
    }

    private mutating func appendBag() {
        var bag = (0..<PieceKind.count).map { PieceKind(rawValue: $0)! }
        var index = bag.count - 1
        while index > 0 {
            bag.swapAt(index, random.below(index + 1))
            index -= 1
        }
        upcoming.append(contentsOf: bag)
    }

    func fits(_ piece: ActivePiece) -> Bool {
        Game.fits(well, piece)
    }

    static func fits(_ well: [UInt8], _ piece: ActivePiece) -> Bool {
        for cell in piece.cells {
            if cell.x < 0 || cell.x >= wellWidth || cell.y < 0 || cell.y >= wellRows {
                return false
            }
            if well[cell.y * wellWidth + cell.x] != 0 {
                return false
            }
        }
        return true
    }

    func isGrounded() -> Bool {
        var below = active
        below.y += 1
        return !fits(below)
    }

    func ghostRow() -> Int {
        Game.landing(well, active)
    }

    static func landing(_ well: [UInt8], _ piece: ActivePiece) -> Int {
        var row = piece.y
        while Game.fits(well, ActivePiece(kind: piece.kind, rotation: piece.rotation, x: piece.x, y: row + 1)) {
            row += 1
        }
        return row
    }

    mutating func spawn(_ kind: PieceKind) {
        active = ActivePiece(kind: kind, rotation: 0, x: (wellWidth - kind.boxSize) / 2, y: 0)
        gravityTimer = 0
        lockTimer = 0
        lockResets = 0
        lowestRow = 0
        pieceCount += 1
        if !fits(active) {
            phase = .over
        }
    }

    private mutating func moved() {
        if isGrounded() && lockResets < maximumLockResets {
            lockResets += 1
            lockTimer = 0
        }
    }

    @discardableResult
    mutating func shift(_ columns: Int) -> Bool {
        var piece = active
        piece.x += columns
        guard fits(piece) else {
            return false
        }
        active = piece
        moved()
        return true
    }

    @discardableResult
    mutating func rotate(clockwise: Bool) -> Bool {
        if active.kind == .block {
            return false
        }
        var turned = active
        turned.rotation = (turned.rotation + (clockwise ? 1 : 3)) & 3
        for kick in kicks {
            var candidate = turned
            candidate.x += kick.x
            candidate.y += kick.y
            if fits(candidate) {
                active = candidate
                moved()
                return true
            }
        }
        return false
    }

    private mutating func moveDown(soft: Bool) -> Bool {
        var piece = active
        piece.y += 1
        guard fits(piece) else {
            return false
        }
        active = piece
        if soft {
            score += 1
        }
        if active.y > lowestRow {
            lowestRow = active.y
            lockTimer = 0
            lockResets = 0
        }
        return true
    }

    mutating func hold() {
        if holdUsed {
            return
        }
        let current = active.kind
        if let previous = held {
            held = current
            spawn(previous)
        } else {
            held = current
            spawn(takeNext())
        }
        holdUsed = true
    }

    private mutating func lock() {
        var allHidden = true
        for cell in active.cells {
            well[cell.y * wellWidth + cell.x] = UInt8(active.kind.rawValue + 1)
            if cell.y >= hiddenRows {
                allHidden = false
            }
        }
        let cleared = clearLines()
        score += lineScores[cleared] * level
        lines += cleared
        holdUsed = false
        if allHidden {
            phase = .over
            return
        }
        spawn(takeNext())
    }

    private mutating func clearLines() -> Int {
        var cleared = 0
        var target = wellRows - 1
        var source = wellRows - 1
        while source >= 0 {
            let full = (0..<wellWidth).allSatisfy { well[source * wellWidth + $0] != 0 }
            if full {
                cleared += 1
            } else {
                if target != source {
                    for column in 0..<wellWidth {
                        well[target * wellWidth + column] = well[source * wellWidth + column]
                    }
                }
                target -= 1
            }
            source -= 1
        }
        while target >= 0 {
            for column in 0..<wellWidth {
                well[target * wellWidth + column] = 0
            }
            target -= 1
        }
        return cleared
    }

    private mutating func hardDrop() {
        var rows = 0
        while moveDown(soft: false) {
            rows += 1
        }
        score += 2 * rows
        lock()
    }

    private mutating func repeatHorizontal(_ inputs: Inputs, milliseconds: Int) {
        let direction = inputs.left == inputs.right ? 0 : (inputs.left ? -1 : 1)
        if direction != repeatDirection {
            repeatDirection = direction
            repeatTimer = 0
            if direction != 0 {
                shift(direction)
            }
        } else if direction != 0 {
            repeatTimer += milliseconds
            while repeatTimer >= repeatDelay {
                shift(direction)
                repeatTimer -= repeatRate
            }
        }
    }

    mutating func step(_ inputs: Inputs, milliseconds: Int) {
        if phase == .over {
            return
        }
        if inputs.pause {
            phase = phase == .paused ? .playing : .paused
        }
        if phase != .playing {
            return
        }
        if inputs.hold {
            hold()
        }
        if inputs.rotateClockwise {
            rotate(clockwise: true)
        }
        if inputs.rotateCounterclockwise {
            rotate(clockwise: false)
        }
        repeatHorizontal(inputs, milliseconds: milliseconds)
        if inputs.hardDrop {
            hardDrop()
            return
        }
        if isGrounded() {
            gravityTimer = 0
            lockTimer += milliseconds
            if lockTimer >= lockDelay {
                lock()
            }
            return
        }
        let interval = inputs.softDrop
            ? min(gravityInterval(level: level), softDropInterval)
            : gravityInterval(level: level)
        gravityTimer += milliseconds
        while gravityTimer >= interval {
            gravityTimer -= interval
            if !moveDown(soft: inputs.softDrop) {
                break
            }
        }
    }

    func wellHash() -> UInt64 {
        var hash: UInt64 = 0xCBF2_9CE4_8422_2325
        for cell in well {
            hash = (hash ^ UInt64(cell)) &* 0x0000_0100_0000_01B3
        }
        return hash
    }
}
