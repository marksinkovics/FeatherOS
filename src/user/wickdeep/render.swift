import FeatherGraphics
import FeatherSupport
import WickdeepArt

typealias Surface = FeatherOS.Graphics.Surface
typealias Font = FeatherOS.Graphics.Font
typealias TileSheet = FeatherOS.Graphics.TileSheet
typealias Art = FeatherOS.User.WickdeepArt

let cellSize = 16
let layoutWidth = 1024
let layoutHeight = 768
let mapX = 32
let mapY = 8
let panelX = 32
let panelY = 656
let panelWidth = 960
let panelHeight = 112
let messageX = 440
let boxWidth = 640
let boxHeight = 480
let boxX = mapX + (cellSize * mapWidth - boxWidth) / 2
let boxY = mapY + (cellSize * mapHeight - boxHeight) / 2

let backgroundColor: UInt32 = 0x0B0D16
let unseenColor: UInt32 = 0x000000
let panelFill: UInt32 = 0x1A1620
let panelBorder: UInt32 = 0xB07A3C
let barBack: UInt32 = 0x2E2838
let hitPointColor: UInt32 = 0xD83C3C
let oilColor: UInt32 = 0xF0882A
let labelColor: UInt32 = 0x9C9484
let dimColor: UInt32 = 0x6E6858
let textColor: UInt32 = 0xE8E0D0
let accentColor: UInt32 = 0xFFC857
let overlayFill: UInt32 = 0x1E1A2A
let selectionFill: UInt32 = 0x3A2F4A

let monsterNames = [
    "dustmite", "gnawling", "oilmoth", "sootcrawler", "shardback", "rootgrip", "bellwraith",
    "Hearthwarden",
]
let itemNames = [
    "oil flask", "salve", "flare stone", "blink feather", "cartographer's moth", "hooked knife",
    "iron crook", "warden's pike", "quilted coat", "tin plate",
]
let itemEffects = [
    "refills 400 oil", "heals 8 and the floor's number", "burns monsters in view within 4 cells",
    "moves you to a cell you have seen", "marks the whole floor as seen", "+1 attack when wielded",
    "+3 attack when wielded", "+5 attack when wielded", "+1 defense when worn",
    "+3 defense when worn",
]
let helpLines = [
    "You are the last wick-keeper. Your lamp's oil is your sight and your",
    "clock: the light shrinks as it burns, and only flasks refill it. Eight",
    "floors down, the Hearthwarden guards the Heartwick. Take it to win;",
    "fall on the way and the run ends. Esc saves the run and leaves, and",
    "the next start resumes it.",
    "",
    "Arrows or keypad 8 4 6 2    move or attack",
    "Keypad 7 9 1 3              move diagonally",
    "Space or keypad 5           wait a turn",
    "Enter                       take the stairs down",
    "I                           open the pack",
    "H                           this help",
    "Esc                         save and quit",
]

func monsterName(_ code: Int) -> String {
    monsterNames[min(max(code, 1), monsterNames.count) - 1]
}

func itemName(_ code: Int) -> String {
    itemNames[min(max(code, 1), itemNames.count) - 1]
}

func messageText(_ event: Event) -> String {
    let first = event.first
    let second = event.second
    switch event.code {
    case .wallAhead: return "A wall blocks the way."
    case .rubbleAhead: return "Rubble blocks the way."
    case .noStairs: return "There are no stairs here."
    case .descended: return "You descend to floor \(first)."
    case .lampDimmed: return "The lamp dims. You see \(first) cells."
    case .lampOut: return "The lamp gutters out."
    case .heartwickTaken: return "You lift the Heartwick from its plinth."
    case .playerHit: return "You hit the \(monsterName(first)) for \(second)."
    case .playerMissed: return "You miss the \(monsterName(first))."
    case .monsterHit: return "The \(monsterName(first)) hits you for \(second)."
    case .monsterMissed: return "The \(monsterName(first)) misses you."
    case .monsterKilled: return "The \(monsterName(first)) dies. +\(second) embers."
    case .leveledUp: return "You reach level \(first)."
    case .oilDrunk: return "The oilmoth drinks \(first) oil."
    case .pickedUp: return "You pick up the \(itemName(first))."
    case .packFull: return "Your pack is full."
    case .dropped: return "You drop the \(itemName(first))."
    case .noRoomHere: return "There is no room to drop it here."
    case .healed: return "You feel better: +\(first) hit points."
    case .oilRefilled: return "The lamp burns bright. Oil \(first)."
    case .flareBurst: return "The flare bursts and strikes \(first)."
    case .blinked: return "The feather carries you away."
    case .mapped: return "The moth maps the whole floor."
    case .nothingHappens: return "Nothing happens."
    case .equipped: return "You equip the \(itemName(first))."
    case .cannotUse: return "That is gear. Equip it instead."
    case .cannotEquip: return "That cannot be equipped."
    case .died: return "The \(monsterName(first)) kills you."
    }
}

func padded(_ text: String, width: Int) -> String {
    var result = text
    while result.utf8.count < width {
        result = " " + result
    }
    return result
}

enum Overlay {
    case none
    case pack(Int)
    case help
    case over(Hall, Int?)

    var key: Int {
        switch self {
        case .none: return 0
        case .pack(let selected): return 1 + selected
        case .help: return 100
        case .over(_, let rank): return 200 + (rank ?? -1) + 1
        }
    }

    var isNone: Bool {
        if case .none = self {
            return true
        }
        return false
    }
}

struct Region {
    var x: Int
    var y: Int
    var width: Int
    var height: Int
}

// The fonts the game draws with, each loaded once; one that cannot be read becomes the console font.
struct Fonts {
    let body: UnsafePointer<Font>
    let heading: UnsafePointer<Font>
    private let owned: [UnsafeMutablePointer<Font>]

    init() {
        var owned: [UnsafeMutablePointer<Font>] = []
        func load(_ name: String) -> UnsafePointer<Font> {
            let storage = UnsafeMutablePointer<Font>.allocate(capacity: 1)
            storage.initialize(to: Font())
            owned.append(storage)
            let font = FeatherOS.Graphics.LoadFontOrConsole(name, storage)!
            if font != UnsafePointer(storage) {
                warn("cannot load the font \(name), using the console font")
            }
            return font
        }
        body = load("mono-16")
        heading = load("sans-32")
        self.owned = owned
    }

    func release() {
        for font in owned {
            font.deinitialize(count: 1)
            font.deallocate()
        }
    }
}

struct Canvas {
    let surface: UnsafeMutablePointer<Surface>

    func fill(_ x: Int, _ y: Int, _ width: Int, _ height: Int, _ color: UInt32) {
        surface.pointee.FillRectangle(Int32(x), Int32(y), Int32(width), Int32(height), color)
    }

    func outline(_ x: Int, _ y: Int, _ width: Int, _ height: Int, _ color: UInt32) {
        surface.pointee.DrawRectangle(Int32(x), Int32(y), Int32(width), Int32(height), color)
    }

    func text(_ x: Int, _ y: Int, _ text: String, _ color: UInt32, _ font: UnsafePointer<Font>) {
        _ = surface.pointee.DrawText(font, Int32(x), Int32(y), text, color)
    }

    func textWidth(_ text: String, _ font: UnsafePointer<Font>) -> Int {
        Int(font.pointee.MeasureText(text))
    }

    func centered(
        _ x: Int, _ width: Int, _ y: Int, _ text: String, _ color: UInt32, _ font: UnsafePointer<Font>
    ) {
        let offset = (width - textWidth(text, font)) / 2
        self.text(x + max(offset, 0), y, text, color, font)
    }
}

// The art of every cell: 0 unseen, else 1 remembered or 2 lit, the terrain tile from bit 2 and
// the occupant's tile plus one from bit 10, only ever on a lit cell.
func terrainTile(_ terrain: UInt8, _ x: Int, _ y: Int) -> Int {
    switch terrain {
    case Terrain.floor.rawValue:
        return Int((x * 7 + y * 11) % 5 == 0 ? Art.TileFloorCracked : Art.TileFloor)
    case Terrain.rubble.rawValue:
        return Int(Art.TileRubble)
    case Terrain.stairs.rawValue:
        return Int(Art.TileStairs)
    case Terrain.plinth.rawValue:
        return Int(Art.TilePlinth)
    default:
        return Int((x * 5 + y * 3) % 7 == 0 ? Art.TileWallMossy : Art.TileWall)
    }
}

func mapView(_ game: Game) -> [UInt32] {
    var occupants = [UInt32](repeating: 0, count: mapWidth * mapHeight)
    for item in game.items {
        occupants[item.y * mapWidth + item.x] =
            UInt32(Int(Art.TileFirstItem) + Int(item.kind.rawValue) - 1 + 1)
    }
    for monster in game.monsters {
        occupants[monster.y * mapWidth + monster.x] =
            UInt32(Int(Art.TileFirstMonster) + Int(monster.kind.rawValue) - 1 + 1)
    }
    occupants[game.y * mapWidth + game.x] = UInt32(Int(Art.TileKeeper) + 1)
    var view = [UInt32](repeating: 0, count: mapWidth * mapHeight)
    for index in 0..<view.count {
        let lit = game.lit[index] != 0
        guard lit || game.remembered[index] != 0 else {
            continue
        }
        let tile = terrainTile(game.cells[index], index % mapWidth, index / mapWidth)
        view[index] = (lit ? 2 : 1) | UInt32(tile) << 2 | (lit ? occupants[index] << 10 : 0)
    }
    return view
}

struct Renderer {
    let canvas: Canvas
    let fonts: Fonts
    let sheet: UnsafeMutablePointer<TileSheet>
    let width: Int
    let height: Int
    let originX: Int
    let originY: Int
    var previousView: [UInt32]
    var previousOverlay = -1
    var previousStats: [Int] = []
    var previousMessages: [String] = []
    var needsFull = true

    init?(_ surface: UnsafeMutablePointer<Surface>, _ fonts: Fonts) {
        canvas = Canvas(surface: surface)
        self.fonts = fonts
        sheet = UnsafeMutablePointer<TileSheet>.allocate(capacity: 1)
        sheet.initialize(to: TileSheet())
        guard Art.LoadWickdeepTiles(sheet) == .Ok else {
            return nil
        }
        width = Int(surface.pointee.Width())
        height = Int(surface.pointee.Height())
        originX = (width - layoutWidth) / 2
        originY = (height - layoutHeight) / 2
        previousView = [UInt32](repeating: .max, count: mapWidth * mapHeight)
    }

    var mapRegion: Region {
        Region(x: originX + mapX, y: originY + mapY, width: cellSize * mapWidth, height: cellSize * mapHeight)
    }

    var boxRegion: Region {
        Region(x: originX + boxX, y: originY + boxY, width: boxWidth, height: boxHeight)
    }

    var statsRegion: Region {
        Region(x: originX + panelX + 2, y: originY + panelY + 2, width: messageX - 6, height: 88)
    }

    var messagesRegion: Region {
        Region(
            x: originX + panelX + messageX, y: originY + panelY + 2,
            width: panelWidth - messageX - 2, height: 88)
    }

    mutating func invalidateAll() {
        needsFull = true
    }

    mutating func render(_ game: Game, _ overlay: Overlay, notice: String?) -> [Region] {
        var regions: [Region] = []
        if needsFull {
            needsFull = false
            drawFrame()
            regions.append(Region(x: 0, y: 0, width: width, height: height))
            previousView = [UInt32](repeating: .max, count: previousView.count)
            previousStats = []
            previousMessages = []
            previousOverlay = -1
        }
        let key = overlayKey(game, overlay)
        let overlayChanged = key != previousOverlay
        if overlayChanged {
            previousOverlay = key
            forgetCells(under: boxRegion)
        }
        let changed = drawCells(mapView(game))
        var redrawBox = overlayChanged
        if let area = changed {
            regions.append(area)
            redrawBox = redrawBox || intersects(area, boxRegion)
        }
        if redrawBox && !overlay.isNone {
            drawOverlay(game, overlay)
            regions.append(boxRegion)
        } else if overlayChanged {
            regions.append(boxRegion)
        }
        let stats = [
            game.hp, game.maxHitPoints, game.oil, game.depth, game.level, game.embers, game.attack,
            game.defense, game.score,
        ]
        if stats != previousStats {
            previousStats = stats
            drawStats(game)
            regions.append(statsRegion)
        }
        var messages = game.recent.map { messageText($0) }
        if let notice {
            messages.append(notice)
            messages.removeFirst(max(0, messages.count - recentLimit))
        }
        if messages != previousMessages {
            previousMessages = messages
            drawMessages(messages)
            regions.append(messagesRegion)
        }
        return regions
    }

    // The pack's box shows what is carried, so a change there counts as a new overlay.
    private func overlayKey(_ game: Game, _ overlay: Overlay) -> Int {
        guard case .pack(let selected) = overlay else {
            return overlay.key
        }
        var hash = selected
        for kind in game.pack {
            hash = hash &* 11 &+ Int(kind.rawValue)
        }
        hash = hash &* 11 &+ Int(game.weapon?.rawValue ?? 0)
        hash = hash &* 11 &+ Int(game.coat?.rawValue ?? 0)
        return 1000 + (hash & 0xFF_FFFF)
    }

    private func intersects(_ first: Region, _ second: Region) -> Bool {
        first.x < second.x + second.width && second.x < first.x + first.width
            && first.y < second.y + second.height && second.y < first.y + first.height
    }

    private mutating func forgetCells(under region: Region) {
        let first = (region.x - originX - mapX) / cellSize
        let last = (region.x + region.width - originX - mapX - 1) / cellSize
        let top = (region.y - originY - mapY) / cellSize
        let bottom = (region.y + region.height - originY - mapY - 1) / cellSize
        for row in top...bottom {
            for column in first...last {
                previousView[row * mapWidth + column] = .max
            }
        }
    }

    private mutating func drawCells(_ view: [UInt32]) -> Region? {
        var left = mapWidth
        var right = -1
        var top = mapHeight
        var bottom = -1
        for index in 0..<view.count where view[index] != previousView[index] {
            let column = index % mapWidth
            let row = index / mapWidth
            drawCell(column, row, view[index])
            left = min(left, column)
            right = max(right, column)
            top = min(top, row)
            bottom = max(bottom, row)
        }
        previousView = view
        guard right >= 0 else {
            return nil
        }
        return Region(
            x: originX + mapX + left * cellSize, y: originY + mapY + top * cellSize,
            width: (right - left + 1) * cellSize, height: (bottom - top + 1) * cellSize)
    }

    private func drawCell(_ column: Int, _ row: Int, _ value: UInt32) {
        let x = originX + mapX + column * cellSize
        let y = originY + mapY + row * cellSize
        guard value != 0 else {
            canvas.fill(x, y, cellSize, cellSize, unseenColor)
            return
        }
        let shade: FeatherOS.Graphics.Shade = value & 3 == 2 ? .Lit : .Remembered
        sheet.pointee.Draw(canvas.surface, Int32(value >> 2 & 0xFF), Int32(x), Int32(y), shade, false)
        let occupant = value >> 10
        if occupant != 0 {
            sheet.pointee.Draw(canvas.surface, Int32(occupant - 1), Int32(x), Int32(y), .Lit, true)
        }
    }

    private func drawIcon(_ tile: Int32, _ x: Int, _ y: Int) {
        sheet.pointee.Draw(canvas.surface, tile, Int32(x), Int32(y), .Lit, true)
    }

    private func drawFrame() {
        canvas.fill(0, 0, width, height, backgroundColor)
        canvas.fill(originX + mapX, originY + mapY, cellSize * mapWidth, cellSize * mapHeight, unseenColor)
        let x = originX + panelX
        let y = originY + panelY
        canvas.fill(x, y, panelWidth, panelHeight, panelFill)
        canvas.outline(x, y, panelWidth, panelHeight, panelBorder)
        canvas.outline(x + 1, y + 1, panelWidth - 2, panelHeight - 2, panelBorder)
        canvas.text(
            x + 8, y + 92,
            "Arrows move  Space wait  Enter stairs  I pack  H help  Esc save and quit", dimColor, fonts.body)
    }

    private func drawBar(_ x: Int, _ y: Int, _ value: Int, _ maximum: Int, _ color: UInt32) {
        let length = 160
        canvas.fill(x, y, length, 8, barBack)
        canvas.fill(x, y, max(0, min(value, maximum)) * length / max(maximum, 1), 8, color)
    }

    private func drawStats(_ game: Game) {
        let region = statsRegion
        canvas.fill(region.x, region.y, region.width, region.height, panelFill)
        let x = originX + panelX + 10
        let y = originY + panelY + 8
        drawIcon(Int32(Art.TileHeart), x, y)
        canvas.text(x + 24, y, "\(game.hp)/\(game.maxHitPoints)", textColor, fonts.body)
        drawBar(x + 112, y + 4, game.hp, game.maxHitPoints, hitPointColor)
        drawIcon(Int32(Art.TileLamp), x, y + 22)
        canvas.text(x + 24, y + 22, "\(game.oil)", textColor, fonts.body)
        drawBar(x + 112, y + 26, game.oil, maximumOil, oilColor)
        canvas.text(
            x, y + 46, "Floor \(game.depth) of \(finalDepth)   Level \(game.level)   Embers \(game.embers)",
            labelColor, fonts.body)
        canvas.text(
            x, y + 64, "Attack \(game.attack)   Defense \(game.defense)   Score \(game.score)", labelColor, fonts.body)
    }

    private func drawMessages(_ messages: [String]) {
        let region = messagesRegion
        canvas.fill(region.x, region.y, region.width, region.height, panelFill)
        let colors = [dimColor, labelColor, textColor]
        let skipped = recentLimit - messages.count
        for (index, message) in messages.enumerated() {
            canvas.text(
                region.x + 8, region.y + 6 + (index + skipped) * 22, message, colors[index + skipped], fonts.body)
        }
    }

    private func drawOverlay(_ game: Game, _ overlay: Overlay) {
        let region = boxRegion
        canvas.fill(region.x, region.y, region.width, region.height, overlayFill)
        canvas.outline(region.x, region.y, region.width, region.height, accentColor)
        canvas.outline(region.x + 1, region.y + 1, region.width - 2, region.height - 2, accentColor)
        switch overlay {
        case .none:
            break
        case .pack(let selected):
            drawPack(game, selected, region)
        case .help:
            drawHelp(region)
        case .over(let hall, let rank):
            drawOver(game, hall, rank, region)
        }
    }

    private func drawPack(_ game: Game, _ selected: Int, _ region: Region) {
        canvas.centered(region.x, region.width, region.y + 14, "PACK", accentColor, fonts.heading)
        let wielding = game.weapon.map { itemName(Int($0.rawValue)) } ?? "nothing"
        let wearing = game.coat.map { itemName(Int($0.rawValue)) } ?? "nothing"
        canvas.text(region.x + 24, region.y + 54, "Wielding: \(wielding)", labelColor, fonts.body)
        canvas.text(region.x + 24, region.y + 74, "Wearing:  \(wearing)", labelColor, fonts.body)
        if game.pack.isEmpty {
            canvas.text(region.x + 24, region.y + 120, "You carry nothing.", textColor, fonts.body)
        }
        for (slot, kind) in game.pack.enumerated() {
            let y = region.y + 104 + slot * 26
            if slot == selected {
                canvas.fill(region.x + 16, y - 4, region.width - 32, 24, selectionFill)
                canvas.outline(region.x + 16, y - 4, region.width - 32, 24, accentColor)
            }
            drawIcon(Int32(Art.TileFirstItem) + Int32(kind.rawValue) - 1, region.x + 24, y)
            canvas.text(region.x + 52, y, itemName(Int(kind.rawValue)), textColor, fonts.body)
        }
        if selected < game.pack.count {
            let kind = Int(game.pack[selected].rawValue)
            canvas.text(
                region.x + 24, region.y + 376, "\(itemName(kind)): \(itemEffects[kind - 1])",
                accentColor, fonts.body)
        }
        canvas.centered(
            region.x, region.width, region.y + 444,
            "Up Down choose   Enter use or equip   D drop   Esc close", labelColor, fonts.body)
    }

    private func drawHelp(_ region: Region) {
        canvas.centered(region.x, region.width, region.y + 14, "WICKDEEP", accentColor, fonts.heading)
        for (index, line) in helpLines.enumerated() {
            canvas.text(region.x + 24, region.y + 64 + index * 22, line, index < 5 ? textColor : labelColor, fonts.body)
        }
        canvas.centered(region.x, region.width, region.y + 444, "Any key closes", dimColor, fonts.body)
    }

    private func drawOver(_ game: Game, _ hall: Hall, _ rank: Int?, _ region: Region) {
        let title = game.outcome == .victory ? "THE HEARTWICK IS YOURS" : "YOU HAVE FALLEN"
        canvas.centered(region.x, region.width, region.y + 14, title, accentColor, fonts.heading)
        let summary = "Floor \(game.depth)   Level \(game.level)   \(game.turn) turns   Score \(game.score)"
        canvas.centered(region.x, region.width, region.y + 56, summary, textColor, fonts.body)
        canvas.centered(region.x, region.width, region.y + 96, "HALL OF RUNS", labelColor, fonts.body)
        for (index, run) in hall.runs.enumerated() {
            let color = index == rank ? accentColor : textColor
            let y = region.y + 124 + index * 24
            let line =
                padded(String(index + 1), width: 2) + "  " + padded(String(run.score), width: 7)
                + "  floor " + String(run.depth) + "  " + padded(String(run.turns), width: 6)
                + " turns  " + outcomeWord(run.outcome)
            canvas.text(region.x + 80, y, line, color, fonts.body)
        }
        canvas.centered(region.x, region.width, region.y + 444, "Enter or Esc leaves", labelColor, fonts.body)
    }
}
