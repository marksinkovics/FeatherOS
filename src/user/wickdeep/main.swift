import FeatherLibC
import FeatherSupport

let help = """
    usage: wickdeep [--seed N] | --selftest | --check-save PATH | --demo [--seed N] [--save-at TURN PATH]
    A turn-based dungeon crawler for a virtual console: eight floors lit by a lamp that runs
    down, and the Heartwick at the bottom. Keys: arrows or keypad 8 4 6 2 move or attack,
    keypad 7 9 1 3 move diagonally, Space waits, Enter takes the stairs, I opens the pack (Up
    and Down choose, Enter uses or equips, D drops, Esc closes), H shows help, Esc or Ctrl+C
    saves the run to $HOME/.wickdeep-save (/tmp without HOME) and quits; the next start
    resumes it, and a save that does not check out is set aside as .wickdeep-save.bad. Runs
    that end are ranked in $HOME/.wickdeep-runs. --seed N fixes a new run's seed.
    --selftest checks the rules and the save format; --check-save reads a save and says whether
    it is sound; --demo plays a fixed bot from the first floor to its death or the Heartwick
    without a display and prints the turns, depth, level, score, outcome and a hash of the
    encoded save, which --save-at writes to PATH when the bot reaches that turn.
    """

func parseSeed(_ text: String) -> UInt64? {
    guard !text.utf8.isEmpty && text.utf8.count <= 19 else {
        return nil
    }
    var value: UInt64 = 0
    for byte in text.utf8 {
        guard byte >= UInt8(ascii: "0") && byte <= UInt8(ascii: "9") else {
            return nil
        }
        value = value * 10 + UInt64(byte - UInt8(ascii: "0"))
    }
    return value
}

func checkSave(path: String) {
    guard let bytes = readFile(path: path, limit: saveSizeLimit) else {
        fail("cannot read \(path)")
    }
    switch Game.decode(bytes) {
    case .game(let game):
        print("wickdeep: save ok depth \(game.depth) turn \(game.turn)")
    case .refused(let reason):
        print("wickdeep: save rejected \(reason)")
        exit(.failure)
    }
}

func demoArguments(_ operands: [String]) {
    var seed: UInt64 = 1
    var save: DemoSave?
    var index = 1
    while index < operands.count {
        if operands[index] == "--seed" && index + 1 < operands.count {
            guard let parsed = parseSeed(operands[index + 1]) else {
                fail("invalid seed \(operands[index + 1]); see wickdeep -h")
            }
            seed = parsed
            index += 2
        } else if operands[index] == "--save-at" && index + 2 < operands.count {
            guard let turn = parseSeed(operands[index + 1]), turn <= UInt64(demoTurnLimit) else {
                fail("invalid turn \(operands[index + 1]); see wickdeep -h")
            }
            save = DemoSave(turn: Int(turn), path: operands[index + 2])
            index += 3
        } else {
            fail("unexpected arguments; see wickdeep -h")
        }
    }
    runDemo(seed: seed, save: save)
}

func exitWith(_ status: Int32) {
    if status != 0 {
        FeatherLibC.exit(status)
    }
}

let operands = Arguments.operands
if Arguments.helpRequested {
    print(help)
} else if operands.isEmpty {
    exitWith(runGame(seed: nil))
} else if operands.count == 2 && operands[0] == "--seed" {
    guard let seed = parseSeed(operands[1]) else {
        fail("invalid seed \(operands[1]); see wickdeep -h")
    }
    exitWith(runGame(seed: seed))
} else if operands.count == 1 && operands[0] == "--selftest" {
    if !runSelfTest() {
        exit(.failure)
    }
} else if operands.count == 2 && operands[0] == "--check-save" {
    checkSave(path: operands[1])
} else if operands.first == "--demo" {
    demoArguments(operands)
} else {
    fail("expected --seed N, --selftest, --check-save or --demo; see wickdeep -h")
}
