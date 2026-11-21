import FeatherGraphics
import FeatherLibC
import FeatherSupport

typealias Display = FeatherOS.Graphics.Display
typealias DisplayEvent = FeatherOS.Graphics.Event

let stepMilliseconds = 10
let presentMilliseconds = 20
let stallLimitMilliseconds = 250
let overGuardMilliseconds = 750
let interruptStatus: Int32 = 130

var interruptRequested = false

func milliseconds() -> Int {
    var now = timespec()
    clock_gettime(CLOCK_MONOTONIC, &now)
    return Int(now.tv_sec) * 1000 + Int(now.tv_nsec) / 1_000_000
}

func initialsLetter(_ code: Int32) -> UInt8? {
    let rows: [(Int32, [UInt8])] = [
        (KEY_Q, Array("QWERTYUIOP".utf8)), (KEY_A, Array("ASDFGHJKL".utf8)),
        (KEY_Z, Array("ZXCVBNM".utf8)),
    ]
    for (first, letters) in rows where code >= first && code < first + Int32(letters.count) {
        return letters[Int(code - first)]
    }
    return nil
}

enum Mode {
    case playing
    case entering
    case table
}

struct Player {
    var game: Game
    var scores: HighScores
    let scorePath: String
    var nextSeed: UInt64
    var mode = Mode.playing
    var initials: [UInt8] = []
    var rank: Int?
    var held = Inputs()
    var hidden = false
    var redraw = false
    var quit = false
    var exitStatus: Int32 = 0
    var guardUntil = 0
    var saveError: SystemError?

    init(seed: UInt64, scorePath: String) {
        game = Game(seed: seed)
        nextSeed = seed &+ 1
        self.scorePath = scorePath
        scores = loadHighScores(path: scorePath)
    }

    var overlay: Overlay {
        switch mode {
        case .entering:
            return .entry(initials)
        case .table:
            return .table(scores, rank)
        case .playing:
            return game.phase == .paused ? .paused : .none
        }
    }

    mutating func handle(_ event: DisplayEvent, now: Int) {
        switch event.kind {
        case .Shown:
            hidden = false
            redraw = true
        case .Hidden:
            hidden = true
            held = Inputs()
            if mode == .playing && game.phase == .playing {
                game.step(Inputs(pause: true), milliseconds: 0)
            }
        case .Release:
            release(Int32(event.code))
        case .Press:
            press(Int32(event.code), modifiers: Int32(event.modifiers), now: now)
        default:
            break
        }
        checkGameOver(now: now)
    }

    mutating func clearHeldKeys() {
        held = Inputs()
    }

    private mutating func release(_ code: Int32) {
        switch code {
        case KEY_LEFT:
            held.left = false
        case KEY_RIGHT:
            held.right = false
        case KEY_DOWN:
            held.softDrop = false
        default:
            return
        }
        advance(held)
    }

    private mutating func advance(_ inputs: Inputs) {
        if mode == .playing {
            game.step(inputs, milliseconds: 0)
        }
    }

    private mutating func press(_ code: Int32, modifiers: Int32, now: Int) {
        if code == KEY_C && modifiers & Int32(FB_MOD_CONTROL) != 0 {
            quit = true
            exitStatus = interruptStatus
        } else if code == KEY_ESC && mode != .entering {
            quit = true
        } else if mode == .playing {
            pressWhilePlaying(code)
        } else if now >= guardUntil {
            if mode == .entering {
                pressWhileEntering(code)
            } else if code == KEY_ENTER || code == KEY_KPENTER || code == KEY_SPACE {
                startGame()
            }
        }
    }

    private mutating func pressWhilePlaying(_ code: Int32) {
        var inputs = held
        switch code {
        case KEY_LEFT:
            held.left = true
            inputs = held
        case KEY_RIGHT:
            held.right = true
            inputs = held
        case KEY_DOWN:
            held.softDrop = true
            inputs = held
        case KEY_UP, KEY_X:
            inputs.rotateClockwise = true
        case KEY_Z:
            inputs.rotateCounterclockwise = true
        case KEY_SPACE:
            inputs.hardDrop = true
        case KEY_C, KEY_LEFTSHIFT, KEY_RIGHTSHIFT:
            inputs.hold = true
        case KEY_P:
            inputs.pause = true
        default:
            return
        }
        advance(inputs)
    }

    private mutating func pressWhileEntering(_ code: Int32) {
        if code == KEY_ESC {
            mode = .table
        } else if code == KEY_BACKSPACE {
            if !initials.isEmpty {
                initials.removeLast()
            }
        } else if (code == KEY_ENTER || code == KEY_KPENTER)
            && initials.count == highScoreInitialCount
        {
            rank = scores.insert(HighScore(initials: initials, score: game.score))
            saveError = saveHighScores(scores, path: scorePath)
            mode = .table
        } else if let letter = initialsLetter(code), initials.count < highScoreInitialCount {
            initials.append(letter)
        }
    }

    mutating func checkGameOver(now: Int) {
        guard mode == .playing && game.phase == .over else {
            return
        }
        held = Inputs()
        guardUntil = now + overGuardMilliseconds
        initials = []
        rank = nil
        mode = scores.qualifies(game.score) ? .entering : .table
    }

    private mutating func startGame() {
        game = Game(seed: nextSeed)
        nextSeed = nextSeed &+ 1
        held = Inputs()
        initials = []
        rank = nil
        mode = .playing
    }

    mutating func tick(elapsed: Int) -> Int {
        var remaining = elapsed
        while remaining >= stepMilliseconds {
            game.step(held, milliseconds: stepMilliseconds)
            remaining -= stepMilliseconds
        }
        return remaining
    }
}

func screenFailure(_ display: inout Display, _ what: String) -> Int32 {
    let reason = String(cString: strerror(display.Error()))
    display.Close()
    warn("\(what): \(reason)")
    return 1
}

func runGame(seed: UInt64) -> Int32 {
    var display = Display()
    if display.Open() != .Ok {
        return screenFailure(&display, "cannot take the screen of this console")
    }
    guard let screen = display.Screen(), Int(display.Width()) >= minimumScreenWidth,
        Int(display.Height()) >= minimumScreenHeight
    else {
        display.Close()
        warn("the screen is too small, \(minimumScreenWidth)x\(minimumScreenHeight) is needed")
        return 1
    }
    signal(SIGINT) { _ in interruptRequested = true }
    var renderer = Renderer(screen)
    var player = Player(seed: seed, scorePath: highScorePath())
    var dropped: UInt32 = 0
    var event = DisplayEvent()
    var last = milliseconds()
    var lastPresent = last - presentMilliseconds
    var carry = 0
    while !player.quit && !interruptRequested {
        var status = display.WaitEvent(Int32(stepMilliseconds), &event)
        while status == .Ok {
            player.handle(event, now: milliseconds())
            status = display.WaitEvent(0, &event)
        }
        if status != .Timeout && status != .Interrupted {
            return screenFailure(&display, "cannot read the keyboard")
        }
        var total: UInt32 = 0
        if display.DroppedEvents(&total) == .Ok && total != dropped {
            dropped = total
            player.clearHeldKeys()
        }
        let now = milliseconds()
        carry = player.tick(elapsed: carry + min(now - last, stallLimitMilliseconds))
        last = now
        player.checkGameOver(now: now)
        if player.redraw {
            player.redraw = false
            renderer.invalidateAll()
        }
        if !player.hidden && now - lastPresent >= presentMilliseconds {
            lastPresent = now
            for region in renderer.render(player.game, player.overlay) {
                display.Invalidate(
                    Int32(region.x), Int32(region.y), Int32(region.width), Int32(region.height))
            }
            if display.Present() != .Ok {
                return screenFailure(&display, "cannot present")
            }
        }
    }
    display.Close()
    if let error = player.saveError {
        warn("\(player.scorePath): \(error)")
    }
    return interruptRequested ? interruptStatus : player.exitStatus
}
