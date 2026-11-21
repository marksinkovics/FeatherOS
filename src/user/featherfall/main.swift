import FeatherSupport

let help = """
    usage: featherfall --selftest | --demo [--seed N]
    A falling-block puzzle. --selftest checks the rules; --demo plays a fixed script
    without a display and prints the score, lines, level and a hash of the well.
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

let operands = Arguments.operands
if Arguments.helpRequested {
    print(help)
} else if operands.count == 1 && operands[0] == "--selftest" {
    if !runSelfTest() {
        exit(.failure)
    }
} else if operands.first == "--demo" {
    var seed: UInt64 = 1
    if operands.count == 3 && operands[1] == "--seed" {
        guard let parsed = parseSeed(operands[2]) else {
            fail("invalid seed \(operands[2]); see featherfall -h")
        }
        seed = parsed
    } else if operands.count != 1 {
        fail("unexpected arguments; see featherfall -h")
    }
    runDemo(seed: seed)
} else {
    fail("expected --selftest or --demo; see featherfall -h")
}
