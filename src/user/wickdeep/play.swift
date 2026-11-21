import FeatherGraphics
import FeatherLibC
import FeatherSupport

typealias Display = FeatherOS.Graphics.Display
typealias DisplayEvent = FeatherOS.Graphics.Event

let overGuardMilliseconds = 750
let interruptStatus: Int32 = 130

var interruptRequested = false

func milliseconds() -> Int {
    var now = timespec()
    clock_gettime(CLOCK_MONOTONIC, &now)
    return Int(now.tv_sec) * 1000 + Int(now.tv_nsec) / 1_000_000
}

func newSeed() -> UInt64 {
    var now = timespec()
    clock_gettime(CLOCK_REALTIME, &now)
    return UInt64(truncatingIfNeeded: now.tv_sec) &* 0x9E37_79B9_7F4A_7C15
        ^ UInt64(truncatingIfNeeded: now.tv_nsec) ^ UInt64(truncatingIfNeeded: getpid())
}

func direction(of code: Int32) -> Direction? {
    switch code {
    case KEY_UP, KEY_KP8: return .north
    case KEY_KP9: return .northEast
    case KEY_RIGHT, KEY_KP6: return .east
    case KEY_KP3: return .southEast
    case KEY_DOWN, KEY_KP2: return .south
    case KEY_KP1: return .southWest
    case KEY_LEFT, KEY_KP4: return .west
    case KEY_KP7: return .northWest
    default: return nil
    }
}

struct Player {
    var game: Game
    var overlay = Overlay.none
    var notice: String?
    let savePath: String
    let hallPath: String
    var hidden = false
    var redraw = false
    var quit = false
    var exitStatus: Int32 = 0
    var guardUntil = 0
    var problems: [String] = []

    init(seed: UInt64?, savePath: String, hallPath: String) {
        self.savePath = savePath
        self.hallPath = hallPath
        game = Game(seed: seed ?? newSeed())
        switch loadSavedGame(path: savePath) {
        case .game(let saved) where saved.outcome == .playing:
            game = saved
        case .game:
            unlink(savePath)
        case .refused(let reason):
            notice = "Set aside an unusable save (\(reason)). A new run begins."
        case .none:
            break
        }
    }

    mutating func handle(_ event: DisplayEvent, now: Int) {
        switch event.kind {
        case .Shown:
            hidden = false
            redraw = true
        case .Hidden:
            hidden = true
        case .Press, .Repeat:
            press(Int32(event.code), modifiers: Int32(event.modifiers), now: now)
        default:
            break
        }
    }

    private mutating func press(_ code: Int32, modifiers: Int32, now: Int) {
        if code == KEY_C && modifiers & Int32(FB_MOD_CONTROL) != 0 {
            quit = true
            exitStatus = interruptStatus
            return
        }
        switch overlay {
        case .over:
            if now >= guardUntil && (code == KEY_ESC || code == KEY_ENTER || code == KEY_KPENTER) {
                quit = true
            }
        case .help:
            overlay = .none
        case .pack(let selected):
            pressInPack(code, selected, now: now)
        case .none:
            pressWhilePlaying(code, now: now)
        }
    }

    private mutating func pressWhilePlaying(_ code: Int32, now: Int) {
        if let way = direction(of: code) {
            perform(.move(way), now: now)
            return
        }
        switch code {
        case KEY_SPACE, KEY_KP5:
            perform(.wait, now: now)
        case KEY_ENTER, KEY_KPENTER:
            perform(.descend, now: now)
        case KEY_I:
            overlay = .pack(0)
        case KEY_H:
            overlay = .help
        case KEY_ESC:
            quit = true
        default:
            break
        }
    }

    private mutating func pressInPack(_ code: Int32, _ selected: Int, now: Int) {
        let count = game.pack.count
        switch code {
        case KEY_ESC, KEY_I:
            overlay = .none
        case KEY_UP, KEY_KP8:
            overlay = .pack(max(0, selected - 1))
        case KEY_DOWN, KEY_KP2:
            overlay = .pack(max(0, min(count - 1, selected + 1)))
        case KEY_ENTER, KEY_KPENTER:
            if selected < count {
                let command: Command =
                    game.pack[selected].isGear ? .equip(selected) : .use(selected)
                perform(command, now: now)
            }
        case KEY_D:
            if selected < count {
                perform(.drop(selected), now: now)
            }
        default:
            break
        }
        if case .pack(let current) = overlay {
            overlay = .pack(max(0, min(game.pack.count - 1, current)))
        }
    }

    private mutating func perform(_ command: Command, now: Int) {
        notice = nil
        _ = game.step(command)
        if game.outcome != .playing {
            finishRun(now: now)
        }
    }

    private mutating func finishRun(now: Int) {
        unlink(savePath)
        var hall = loadHall(path: hallPath)
        let rank = hall.insert(
            Run(score: game.score, depth: game.depth, turns: game.turn, outcome: game.outcome))
        if let error = saveHall(hall, path: hallPath) {
            problems.append("\(hallPath): \(error)")
        }
        overlay = .over(hall, rank)
        guardUntil = now + overGuardMilliseconds
    }

    mutating func leave() {
        guard game.outcome == .playing else {
            return
        }
        if let error = saveGame(game, path: savePath) {
            problems.append("\(savePath): \(error)")
            exitStatus = 1
        }
    }
}

func screenFailure(_ display: inout Display, _ what: String) -> Int32 {
    let reason = String(cString: strerror(display.Error()))
    display.Close()
    warn("\(what): \(reason)")
    return 1
}

func runGame(seed: UInt64?) -> Int32 {
    var display = Display()
    if display.Open() != .Ok {
        return screenFailure(&display, "cannot take the screen of this console")
    }
    guard let screen = display.Screen(), Int(display.Width()) >= layoutWidth,
        Int(display.Height()) >= layoutHeight
    else {
        display.Close()
        warn("the screen is too small, \(layoutWidth)x\(layoutHeight) is needed")
        return 1
    }
    guard var renderer = Renderer(screen) else {
        display.Close()
        warn("the tile art does not load")
        return 1
    }
    signal(SIGINT) { _ in interruptRequested = true }
    var player = Player(seed: seed, savePath: savePath(), hallPath: hallPath())
    var event = DisplayEvent()
    while !player.quit && !interruptRequested {
        var status = display.WaitEvent(-1, &event)
        while status == .Ok {
            player.handle(event, now: milliseconds())
            status = player.quit ? .Timeout : display.WaitEvent(0, &event)
        }
        if status != .Timeout && status != .Interrupted {
            return screenFailure(&display, "cannot read the keyboard")
        }
        if player.redraw {
            player.redraw = false
            renderer.invalidateAll()
        }
        if !player.hidden && !player.quit && !interruptRequested {
            for region in renderer.render(player.game, player.overlay, notice: player.notice) {
                display.Invalidate(
                    Int32(region.x), Int32(region.y), Int32(region.width), Int32(region.height))
            }
            if display.Present() != .Ok {
                return screenFailure(&display, "cannot present")
            }
        }
    }
    display.Close()
    player.leave()
    for problem in player.problems {
        warn(problem)
    }
    return interruptRequested ? interruptStatus : player.exitStatus
}
