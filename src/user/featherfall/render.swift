import FeatherGraphics

typealias Surface = FeatherOS.Graphics.Surface

let visibleRows = wellRows - hiddenRows
let cellSize = 32
let previewCellSize = 24
let panelWidth = 216
let panelGap = 24
let boxWidth = 120
let boxHeight = 72
let minimumScreenWidth = 2 * (panelWidth + panelGap) + cellSize * wellWidth
let minimumScreenHeight = cellSize * visibleRows + 2 * 64

let backgroundColor: UInt32 = 0x12141F
let panelFill: UInt32 = 0x1B1F2E
let panelBorder: UInt32 = 0x3A4160
let wellFill: UInt32 = 0x0A0C14
let gridColor: UInt32 = 0x131724
let wellBorder: UInt32 = 0x6C7596
let labelColor: UInt32 = 0x8E96B8
let textColor: UInt32 = 0xE8EAF2
let accentColor: UInt32 = 0xFFC857
let overlayFill: UInt32 = 0x1E2338
let pieceColors: [UInt32] = [
    0x35C6D4, 0xF2C94C, 0xA66BDB, 0x6FCF73, 0xEB5757, 0x4F7BE8, 0xF2994A,
]

let helpLines = [
    "Left Right  move", "Down        soft drop", "Space       hard drop", "Up or X      turn right",
    "Z           turn left", "C or Shift  hold", "P           pause", "Esc         quit",
]

enum Overlay {
    case none
    case paused
    case entry([UInt8])
    case table(HighScores, Int?)

    var key: Int {
        switch self {
        case .none:
            return 0
        case .paused:
            return 1
        case .entry(let initials):
            var key = initials.count
            for letter in initials {
                key = key * 27 + Int(letter - UInt8(ascii: "A")) + 1
            }
            return 2 + key
        case .table(_, let rank):
            return 1_000_000 + (rank ?? -1) + 1
        }
    }
}

struct Region {
    var x: Int
    var y: Int
    var width: Int
    var height: Int
}

func shade(_ color: UInt32, percent: UInt32) -> UInt32 {
    let red = min(((color >> 16) & 0xFF) * percent / 100, 255)
    let green = min(((color >> 8) & 0xFF) * percent / 100, 255)
    let blue = min((color & 0xFF) * percent / 100, 255)
    return red << 16 | green << 8 | blue
}

func padded(_ text: String, width: Int) -> String {
    var result = text
    while result.utf8.count < width {
        result = " " + result
    }
    return result
}

struct Canvas {
    let surface: UnsafeMutablePointer<Surface>

    func fill(_ x: Int, _ y: Int, _ width: Int, _ height: Int, _ color: UInt32) {
        surface.pointee.FillRectangle(Int32(x), Int32(y), Int32(width), Int32(height), color)
    }

    func outline(_ x: Int, _ y: Int, _ width: Int, _ height: Int, _ color: UInt32) {
        surface.pointee.DrawRectangle(Int32(x), Int32(y), Int32(width), Int32(height), color)
    }

    func text(_ x: Int, _ y: Int, _ text: String, _ color: UInt32, scale: Int = 1) {
        surface.pointee.DrawText(Int32(x), Int32(y), text, color, Int32(scale))
    }

    func textWidth(_ text: String, scale: Int) -> Int {
        Int(Surface.TextWidth(text, Int32(scale)))
    }

    func centered(_ x: Int, _ width: Int, _ y: Int, _ text: String, _ color: UInt32, scale: Int) {
        let offset = (width - textWidth(text, scale: scale)) / 2
        self.text(x + max(offset, 0), y, text, color, scale: scale)
    }

    func block(_ x: Int, _ y: Int, _ size: Int, _ color: UInt32) {
        fill(x, y, size, size, shade(color, percent: 55))
        fill(x + 2, y + 2, size - 4, size - 4, color)
        fill(x + 2, y + 2, size - 4, 3, shade(color, percent: 135))
    }

    func ghost(_ x: Int, _ y: Int, _ size: Int, _ color: UInt32) {
        let dim = shade(color, percent: 70)
        outline(x + 2, y + 2, size - 4, size - 4, dim)
        outline(x + 3, y + 3, size - 6, size - 6, dim)
    }
}

// The cells a well shows: 0 empty, 1...7 locked, 8...14 the falling piece, 15...21 its ghost.
func wellView(_ game: Game) -> [UInt8] {
    var view = [UInt8](repeating: 0, count: wellWidth * visibleRows)
    for row in hiddenRows..<wellRows {
        for column in 0..<wellWidth {
            view[(row - hiddenRows) * wellWidth + column] = game.well[row * wellWidth + column]
        }
    }
    if game.phase == .over {
        return view
    }
    let kind = UInt8(game.active.kind.rawValue)
    let drop = game.ghostRow() - game.active.y
    for cell in game.active.cells {
        let row = cell.y + drop - hiddenRows
        if row >= 0 && view[row * wellWidth + cell.x] == 0 {
            view[row * wellWidth + cell.x] = 15 + kind
        }
    }
    for cell in game.active.cells {
        let row = cell.y - hiddenRows
        if row >= 0 {
            view[row * wellWidth + cell.x] = 8 + kind
        }
    }
    return view
}

struct Renderer {
    let canvas: Canvas
    let width: Int
    let height: Int
    let wellX: Int
    let wellY: Int
    let leftX: Int
    let rightX: Int
    var previousView: [UInt8]
    var previousOverlay = -1
    var previousLeft: [Int] = []
    var previousRight: [Int] = []
    var needsFull = true

    init(_ surface: UnsafeMutablePointer<Surface>) {
        canvas = Canvas(surface: surface)
        width = Int(surface.pointee.Width())
        height = Int(surface.pointee.Height())
        wellX = (width - cellSize * wellWidth) / 2
        wellY = (height - cellSize * visibleRows) / 2
        leftX = wellX - panelGap - panelWidth
        rightX = wellX + cellSize * wellWidth + panelGap
        previousView = [UInt8](repeating: 255, count: wellWidth * visibleRows)
    }

    var wellRegion: Region {
        Region(x: wellX, y: wellY, width: cellSize * wellWidth, height: cellSize * visibleRows)
    }

    var leftRegion: Region {
        Region(x: leftX, y: wellY, width: panelWidth, height: cellSize * visibleRows)
    }

    var rightRegion: Region {
        Region(x: rightX, y: wellY, width: panelWidth, height: cellSize * visibleRows)
    }

    mutating func invalidateAll() {
        needsFull = true
    }

    mutating func render(_ game: Game, _ overlay: Overlay) -> [Region] {
        var regions: [Region] = []
        if needsFull {
            needsFull = false
            drawFrame()
            regions.append(Region(x: 0, y: 0, width: width, height: height))
            previousView = [UInt8](repeating: 255, count: previousView.count)
            previousLeft = []
            previousRight = []
            previousOverlay = -1
        }
        let left = [game.score, game.level, game.lines, game.held.map { $0.rawValue } ?? -1]
        if left != previousLeft {
            previousLeft = left
            drawLeftPanel(game)
            regions.append(leftRegion)
        }
        let right = game.preview.map { $0.rawValue }
        if right != previousRight {
            previousRight = right
            drawRightPanel(game)
            regions.append(rightRegion)
        }
        let key = overlay.key
        if key != previousOverlay {
            if key != 0 {
                drawOverlay(game, overlay)
                regions.append(wellRegion)
            }
            previousView = [UInt8](repeating: 255, count: previousView.count)
            previousOverlay = key
        }
        if key == 0 {
            drawWell(wellView(game), &regions)
        }
        return regions
    }

    private mutating func drawWell(_ view: [UInt8], _ regions: inout [Region]) {
        var changed: [Int] = []
        for index in view.indices where view[index] != previousView[index] {
            changed.append(index)
        }
        for index in changed {
            drawCell(index % wellWidth, index / wellWidth, view[index])
        }
        previousView = view
        if changed.count > 6 {
            regions.append(wellRegion)
            return
        }
        for index in changed {
            regions.append(
                Region(
                    x: wellX + (index % wellWidth) * cellSize,
                    y: wellY + (index / wellWidth) * cellSize, width: cellSize, height: cellSize))
        }
    }

    private func drawCell(_ column: Int, _ row: Int, _ value: UInt8) {
        let x = wellX + column * cellSize
        let y = wellY + row * cellSize
        canvas.fill(x, y, cellSize, cellSize, wellFill)
        canvas.outline(x, y, cellSize, cellSize, gridColor)
        if value >= 15 {
            canvas.ghost(x, y, cellSize, pieceColors[Int(value) - 15])
        } else if value >= 8 {
            canvas.block(x, y, cellSize, pieceColors[Int(value) - 8])
        } else if value >= 1 {
            canvas.block(x, y, cellSize, pieceColors[Int(value) - 1])
        }
    }

    private func drawFrame() {
        canvas.fill(0, 0, width, height, backgroundColor)
        canvas.centered(0, width, 14, "FEATHERFALL", accentColor, scale: 3)
        canvas.fill(wellX - 4, wellY - 4, cellSize * wellWidth + 8, cellSize * visibleRows + 8, wellBorder)
        canvas.fill(wellX, wellY, cellSize * wellWidth, cellSize * visibleRows, wellFill)
    }

    private func drawPanel(_ region: Region) {
        canvas.fill(region.x, region.y, region.width, region.height, panelFill)
        canvas.outline(region.x, region.y, region.width, region.height, panelBorder)
    }

    private func drawPreview(_ x: Int, _ y: Int, _ kind: PieceKind?) {
        canvas.fill(x, y, boxWidth, boxHeight, wellFill)
        canvas.outline(x, y, boxWidth, boxHeight, panelBorder)
        guard let kind else {
            return
        }
        let cells = pieceCells(kind, rotation: 0)
        let minimumX = cells.map { $0.x }.min() ?? 0
        let maximumX = cells.map { $0.x }.max() ?? 0
        let minimumY = cells.map { $0.y }.min() ?? 0
        let maximumY = cells.map { $0.y }.max() ?? 0
        let originX = x + (boxWidth - (maximumX - minimumX + 1) * previewCellSize) / 2
        let originY = y + (boxHeight - (maximumY - minimumY + 1) * previewCellSize) / 2
        for cell in cells {
            canvas.block(
                originX + (cell.x - minimumX) * previewCellSize,
                originY + (cell.y - minimumY) * previewCellSize, previewCellSize,
                pieceColors[kind.rawValue])
        }
    }

    private func drawLeftPanel(_ game: Game) {
        drawPanel(leftRegion)
        let x = leftX + 16
        canvas.text(x, wellY + 14, "HOLD", labelColor, scale: 2)
        drawPreview(leftX + (panelWidth - boxWidth) / 2, wellY + 54, game.held)
        let values = [("SCORE", game.score), ("LEVEL", game.level), ("LINES", game.lines)]
        var y = wellY + 170
        for (name, value) in values {
            canvas.text(x, y, name, labelColor, scale: 2)
            canvas.text(x, y + 32, String(min(value, highScoreMaximum)), textColor, scale: 2)
            y += 90
        }
    }

    private func drawRightPanel(_ game: Game) {
        drawPanel(rightRegion)
        canvas.text(rightX + 16, wellY + 14, "NEXT", labelColor, scale: 2)
        for (index, kind) in game.preview.enumerated() {
            drawPreview(rightX + (panelWidth - boxWidth) / 2, wellY + 54 + index * 84, kind)
        }
        var y = wellY + 330
        for line in helpLines {
            canvas.text(rightX + 16, y, line, labelColor)
            y += 20
        }
    }

    private func drawOverlay(_ game: Game, _ overlay: Overlay) {
        let region = wellRegion
        canvas.fill(region.x, region.y, region.width, region.height, overlayFill)
        canvas.outline(region.x, region.y, region.width, region.height, accentColor)
        canvas.outline(region.x + 1, region.y + 1, region.width - 2, region.height - 2, accentColor)
        switch overlay {
        case .none:
            break
        case .paused:
            canvas.centered(region.x, region.width, region.y + 270, "PAUSED", accentColor, scale: 3)
            canvas.centered(region.x, region.width, region.y + 330, "P resumes", labelColor, scale: 2)
        case .entry(let initials):
            drawGameOver(game, region)
            drawEntry(initials, region)
        case .table(let scores, let rank):
            drawGameOver(game, region)
            drawTable(scores, rank, region)
        }
    }

    private func drawGameOver(_ game: Game, _ region: Region) {
        canvas.centered(region.x, region.width, region.y + 24, "GAME OVER", accentColor, scale: 3)
        let score = "SCORE " + String(min(game.score, highScoreMaximum))
        canvas.centered(region.x, region.width, region.y + 72, score, textColor, scale: 2)
    }

    private func drawEntry(_ initials: [UInt8], _ region: Region) {
        canvas.centered(region.x, region.width, region.y + 130, "NEW HIGH SCORE", accentColor, scale: 2)
        let slotWidth = 56
        let slotGap = 16
        let originX = region.x + (region.width - 3 * slotWidth - 2 * slotGap) / 2
        for slot in 0..<highScoreInitialCount {
            let x = originX + slot * (slotWidth + slotGap)
            let y = region.y + 180
            canvas.fill(x, y, slotWidth, 80, wellFill)
            canvas.outline(x, y, slotWidth, 80, slot == initials.count ? accentColor : panelBorder)
            if slot < initials.count {
                canvas.text(x + 12, y + 8, String(decoding: [initials[slot]], as: UTF8.self), textColor, scale: 4)
            }
        }
        let lines = ["Type your initials", "Backspace erases", "Enter saves", "Esc skips"]
        var y = region.y + 300
        for line in lines {
            let ready = line == "Enter saves" && initials.count == highScoreInitialCount
            canvas.centered(region.x, region.width, y, line, ready ? accentColor : labelColor, scale: 1)
            y += 22
        }
    }

    private func drawTable(_ scores: HighScores, _ rank: Int?, _ region: Region) {
        canvas.centered(region.x, region.width, region.y + 120, "HIGH SCORES", textColor, scale: 2)
        if scores.entries.isEmpty {
            canvas.centered(region.x, region.width, region.y + 180, "None yet", labelColor, scale: 2)
        }
        for (index, entry) in scores.entries.enumerated() {
            let y = region.y + 160 + index * 34
            let color = index == rank ? accentColor : textColor
            canvas.text(region.x + 20, y, padded(String(index + 1), width: 2), color, scale: 2)
            canvas.text(region.x + 68, y, String(decoding: entry.initials, as: UTF8.self), color, scale: 2)
            let score = String(entry.score)
            canvas.text(region.x + region.width - 20 - canvas.textWidth(score, scale: 2), y, score, color, scale: 2)
        }
        canvas.centered(region.x, region.width, region.y + 560, "Enter plays again", accentColor, scale: 1)
        canvas.centered(region.x, region.width, region.y + 582, "Esc quits", labelColor, scale: 1)
    }
}
