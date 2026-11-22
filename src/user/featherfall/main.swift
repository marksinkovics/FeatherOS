import FeatherLibC
import FeatherSupport

let help = """
    usage: featherfall [--seed N] | --selftest | --demo [--seed N]
    A falling-block puzzle for a virtual console. Keys: Left and Right move, Down drops
    softly, Space drops at once, Up or X turns clockwise, Z anticlockwise, C or Shift holds
    a piece, P pauses, Esc quits. A top-10 table is kept in $HOME/.featherfall (/tmp
    without HOME); at game over type three initials, then Enter plays again.
    --seed N fixes the order of pieces. --selftest checks the rules; --demo plays a fixed
    script without a display and prints the score, lines, level and a hash of the well.
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

func parseSeedOption(_ operands: ArraySlice<String>) -> UInt64? {
    guard operands.count == 2, operands.first == "--seed" else {
        return nil
    }
    guard let parsed = parseSeed(operands[operands.startIndex + 1]) else {
        fail("invalid seed \(operands[operands.startIndex + 1]); see featherfall -h")
    }
    return parsed
}

func randomSeed() -> UInt64 {
    UInt64(milliseconds()) &* 0x9E37_79B9_7F4A_7C15 ^ UInt64(getpid())
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
    exitWith(runGame(seed: randomSeed()))
} else if operands.count == 1 && operands[0] == "--selftest" {
    if !runSelfTest() {
        exit(.failure)
    }
} else if operands.first == "--demo" {
    var seed: UInt64 = 1
    if operands.count == 3 && operands[1] == "--seed" {
        seed = parseSeedOption(operands[1...])!
    } else if operands.count != 1 {
        fail("unexpected arguments; see featherfall -h")
    }
    runDemo(seed: seed)
} else if let seed = parseSeedOption(operands[0...]) {
    exitWith(runGame(seed: seed))
} else {
    fail("expected --seed N, --selftest or --demo; see featherfall -h")
}
