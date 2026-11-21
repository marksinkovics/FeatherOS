import FeatherLibC
import FeatherSupport

let hallLimit = 10
let hallDigitLimit = 9

struct Run: Equatable {
    var score: Int
    var depth: Int
    var turns: Int
    var outcome: Outcome
}

// One run per line, "score depth turns outcome", best first.
struct Hall: Equatable {
    var runs: [Run] = []

    static func parse(_ bytes: [UInt8]) -> Hall? {
        var hall = Hall()
        var index = 0
        while index < bytes.count {
            guard hall.runs.count < hallLimit, let run = parseLine(bytes, &index) else {
                return nil
            }
            hall.runs.append(run)
        }
        var sorted: [Run] = []
        for run in hall.runs {
            sorted.insert(run, at: rank(of: run.score, in: sorted))
        }
        hall.runs = sorted
        return hall
    }

    private static func parseNumber(_ bytes: [UInt8], _ index: inout Int, then separator: UInt8) -> Int? {
        var value = 0
        var digits = 0
        while index < bytes.count, bytes[index] >= UInt8(ascii: "0"),
            bytes[index] <= UInt8(ascii: "9")
        {
            value = value * 10 + Int(bytes[index] - UInt8(ascii: "0"))
            digits += 1
            index += 1
            if digits > hallDigitLimit {
                return nil
            }
        }
        guard digits > 0, index < bytes.count, bytes[index] == separator else {
            return nil
        }
        index += 1
        return value
    }

    private static func parseLine(_ bytes: [UInt8], _ index: inout Int) -> Run? {
        let space = UInt8(ascii: " ")
        guard let score = parseNumber(bytes, &index, then: space),
            let depth = parseNumber(bytes, &index, then: space), depth >= 1 && depth <= finalDepth,
            let turns = parseNumber(bytes, &index, then: space)
        else {
            return nil
        }
        for outcome in [Outcome.dead, Outcome.victory] {
            let word = Array(outcomeWord(outcome).utf8) + [UInt8(ascii: "\n")]
            if index + word.count <= bytes.count && Array(bytes[index..<index + word.count]) == word {
                index += word.count
                return Run(score: score, depth: depth, turns: turns, outcome: outcome)
            }
        }
        return nil
    }

    private static func rank(of score: Int, in runs: [Run]) -> Int {
        var position = 0
        while position < runs.count && runs[position].score >= score {
            position += 1
        }
        return position
    }

    func serialized() -> [UInt8] {
        var bytes: [UInt8] = []
        for run in runs {
            bytes += Array("\(run.score) \(run.depth) \(run.turns) \(outcomeWord(run.outcome))\n".utf8)
        }
        return bytes
    }

    func qualifies(_ score: Int) -> Bool {
        runs.count < hallLimit || score > runs[runs.count - 1].score
    }

    // The new run goes below older ones with the same score; nil when it falls off the table.
    @discardableResult
    mutating func insert(_ run: Run) -> Int? {
        let position = Hall.rank(of: run.score, in: runs)
        guard position < hallLimit else {
            return nil
        }
        runs.insert(run, at: position)
        if runs.count > hallLimit {
            runs.removeLast()
        }
        return position
    }
}

func outcomeWord(_ outcome: Outcome) -> String {
    switch outcome {
    case .playing: return "limit"
    case .dead: return "dead"
    case .victory: return "victory"
    }
}

func homeFile(_ name: String) -> String {
    guard let home = getenv("HOME"), home.pointee != 0 else {
        return "/tmp/\(name)"
    }
    let directory = String(cString: home)
    return directory == "/" ? "/\(name)" : "\(directory)/\(name)"
}

func savePath() -> String {
    homeFile(".wickdeep-save")
}

func hallPath() -> String {
    homeFile(".wickdeep-runs")
}

func readFile(path: String, limit: Int) -> [UInt8]? {
    guard let stream = fopen(path, "r") else {
        return nil
    }
    var contents = [UInt8](repeating: 0, count: limit + 1)
    let count = contents.withUnsafeMutableBytes { fread($0.baseAddress, 1, limit + 1, stream) }
    fclose(stream)
    return Array(contents[..<count])
}

func writeFileAtomically(_ bytes: [UInt8], path: String) -> SystemError? {
    let temporary = path + ".tmp"
    guard let stream = fopen(temporary, "w") else {
        return SystemError.current
    }
    let written = bytes.withUnsafeBufferPointer { fwrite($0.baseAddress, 1, $0.count, stream) }
    let failed = written != bytes.count
    let closed = fclose(stream)
    if failed || closed != 0 || rename(temporary, path) != 0 {
        let error = SystemError.current
        unlink(temporary)
        return error
    }
    return nil
}

func loadHall(path: String) -> Hall {
    guard let bytes = readFile(path: path, limit: hallLimit * 64) else {
        return Hall()
    }
    return Hall.parse(bytes) ?? Hall()
}

func saveHall(_ hall: Hall, path: String) -> SystemError? {
    writeFileAtomically(hall.serialized(), path: path)
}

func saveGame(_ game: Game, path: String) -> SystemError? {
    writeFileAtomically(game.encode(), path: path)
}

enum LoadedSave {
    case none
    case game(Game)
    case refused(String)
}

// A refused file is moved aside so the next save does not overwrite the evidence.
func loadSavedGame(path: String) -> LoadedSave {
    guard let bytes = readFile(path: path, limit: saveSizeLimit) else {
        return .none
    }
    switch Game.decode(bytes) {
    case .game(let game):
        return .game(game)
    case .refused(let reason):
        _ = rename(path, path + ".bad")
        return .refused(reason)
    }
}
