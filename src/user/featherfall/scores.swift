import FeatherLibC
import FeatherSupport

let highScoreLimit = 10
let highScoreInitialCount = 3
let highScoreDigitLimit = 9
let highScoreMaximum = 999_999_999

struct HighScore: Equatable {
    var initials: [UInt8]
    var score: Int
}

// One entry per line, "AAA 1234": three capital letters, a space and at most nine digits.
struct HighScores: Equatable {
    var entries: [HighScore] = []

    static func parse(_ bytes: [UInt8]) -> HighScores? {
        var scores = HighScores()
        var index = 0
        while index < bytes.count {
            guard scores.entries.count < highScoreLimit,
                let entry = parseLine(bytes, &index)
            else {
                return nil
            }
            scores.entries.append(entry)
        }
        scores.entries.sort { $0.score > $1.score }
        return scores
    }

    private static func parseLine(_ bytes: [UInt8], _ index: inout Int) -> HighScore? {
        var initials: [UInt8] = []
        while initials.count < highScoreInitialCount {
            guard index < bytes.count, bytes[index] >= UInt8(ascii: "A"),
                bytes[index] <= UInt8(ascii: "Z")
            else {
                return nil
            }
            initials.append(bytes[index])
            index += 1
        }
        guard index < bytes.count, bytes[index] == UInt8(ascii: " ") else {
            return nil
        }
        index += 1
        var score = 0
        var digits = 0
        while index < bytes.count, bytes[index] >= UInt8(ascii: "0"),
            bytes[index] <= UInt8(ascii: "9")
        {
            score = score * 10 + Int(bytes[index] - UInt8(ascii: "0"))
            digits += 1
            index += 1
            if digits > highScoreDigitLimit {
                return nil
            }
        }
        guard digits > 0, index < bytes.count, bytes[index] == UInt8(ascii: "\n") else {
            return nil
        }
        index += 1
        return HighScore(initials: initials, score: score)
    }

    func serialized() -> [UInt8] {
        var bytes: [UInt8] = []
        for entry in entries {
            bytes += entry.initials
            bytes.append(UInt8(ascii: " "))
            bytes += Array(String(entry.score).utf8)
            bytes.append(UInt8(ascii: "\n"))
        }
        return bytes
    }

    func qualifies(_ score: Int) -> Bool {
        guard score > 0 else {
            return false
        }
        return entries.count < highScoreLimit || score > entries[entries.count - 1].score
    }

    // The new entry goes below older ones with the same score; nil when it falls off the table.
    @discardableResult
    mutating func insert(_ entry: HighScore) -> Int? {
        var rank = 0
        while rank < entries.count && entries[rank].score >= entry.score {
            rank += 1
        }
        guard rank < highScoreLimit else {
            return nil
        }
        entries.insert(entry, at: rank)
        if entries.count > highScoreLimit {
            entries.removeLast()
        }
        return rank
    }
}

func highScorePath() -> String {
    guard let home = getenv("HOME"), home.pointee != 0 else {
        return "/tmp/.featherfall"
    }
    let directory = String(cString: home)
    return directory == "/" ? "/.featherfall" : "\(directory)/.featherfall"
}

func loadHighScores(path: String) -> HighScores {
    guard let file = try? InputFile(path: path), let bytes = try? file.readAll() else {
        return HighScores()
    }
    return HighScores.parse(bytes) ?? HighScores()
}

func saveHighScores(_ scores: HighScores, path: String) -> SystemError? {
    let temporary = path + ".tmp"
    guard let stream = fopen(temporary, "w") else {
        return SystemError.current
    }
    let bytes = scores.serialized()
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
