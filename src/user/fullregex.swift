// Full Swift's regular expressions: upstream's _StringProcessing builds a match's output type
// while the program runs, so Embedded Swift cannot use them.
import RegexBuilder

func show(_ text: Substring?) -> String {
    text.map { String($0) } ?? "-"
}

let log = """
    2026-11-09 error disk full
    2026-11-10 info  started
    2026-12-01 error network down
    """

let date = /(?<year>\d{4})-(?<month>\d{2})-(?<day>\d{2})/
if let match = log.firstMatch(of: date) {
    print("fullregex: literal \(match.output.0) year \(match.year) month \(match.month) day \(match.day)")
}
let errors = log.matches(of: /(\d{4}-\d{2}-\d{2}) error (.+)/).map { "\($0.1)=\($0.2)" }
print("fullregex: matches \(errors)")
print("fullregex: whole \("2026-11-09".wholeMatch(of: date) != nil) \("x2026-11-09".wholeMatch(of: date) != nil)")
print("fullregex: optional \(show("b".wholeMatch(of: /(a)?b/)?.1)) \(show("ab".wholeMatch(of: /(a)?b/)?.1))")
print("fullregex: replacing \("a1b22c333".replacing(/\d+/, with: "#"))")
print("fullregex: replacing \("2026-11-09".replacing(date) { "\($0.day)/\($0.month)/\($0.year)" })")
print("fullregex: split \("one, two;three ,four".split(separator: /\s*[,;]\s*/))")
print("fullregex: ranges \("abcabcab".ranges(of: /ab/).count) contains \("hello world".contains(/o w/))")
print("fullregex: trimming \("   indented".trimmingPrefix(/\s+/))")
print("fullregex: backreference \(show("hello hello world".firstMatch(of: /(\w+) \1/)?.1))")
print("fullregex: lookahead \("price: 100 dollars 200".matches(of: /\d+(?= dollars)/).map(\.output))")
print("fullregex: reluctant \(show("<a><b>".firstMatch(of: /<.+?>/)?.output)) greedy \(show("<a><b>".firstMatch(of: /<.+>/)?.output))")
print("fullregex: ignoring case \("Hello".wholeMatch(of: /hello/.ignoresCase()) != nil)")
let lines = log.matches(of: /^\d+-12-\d+.*$/.anchorsMatchLineEndings()).map(\.output)
print("fullregex: line anchors \(lines)")

// Grapheme clusters by default, Unicode scalars on request; script lookups use _CUnicode's tables.
let accented = "cafe\u{301}"
print("fullregex: graphemes \(accented.wholeMatch(of: /\w{4}/) != nil) scalars \(accented.wholeMatch(of: /\w{4}./.matchingSemantics(.unicodeScalar)) != nil)")
print("fullregex: script \("alpha \u{3B1}\u{3B2}\u{3B3} omega".matches(of: /\p{Script=Greek}+/).map { $0.output.count })")

if let step = "move 3 north quickly".prefixMatch(of: /move (\d+) (north|south)/) {
    print("fullregex: prefix \(step.output.0) steps \(step.1)")
}
print("fullregex: starts \("north".starts(with: /no/)) \("south".starts(with: /no/))")

// Patterns made while the program runs, with captures typed as AnyRegexOutput.
do {
    let pattern = try Regex(#"(?<key>\w+)=(?<value>\w*)"#)
    let pairs = "a=1 b= c=33".matches(of: pattern).map { match in
        "\(match["key"]?.substring ?? "?"):\(match[2].substring ?? "?")"
    }
    print("fullregex: runtime \(pairs) captures \(try pattern.firstMatch(in: "x=y")?.output.count ?? 0)")
    let typed = try Regex(#"(\d+)\.(\d+)"#, as: (Substring, Substring, Substring).self)
    if let version = "swift 6.4".firstMatch(of: typed) {
        print("fullregex: typed major \(version.1) minor \(version.2)")
    }
} catch {
    print("fullregex: runtime failed \(error)")
}
do {
    _ = try Regex("(unclosed")
    print("fullregex: invalid accepted")
} catch {
    print("fullregex: invalid rejected")
}

// RegexBuilder, with a transformed capture and a reference to a capture.
let count = Reference(Int.self)
let builder = Regex {
    Anchor.startOfLine
    Capture {
        ChoiceOf {
            "apples"
            "pears"
        }
    }
    ":"
    ZeroOrMore(.whitespace)
    TryCapture(as: count) {
        OneOrMore(.digit)
    } transform: {
        Int($0)
    }
    Optionally {
        " "
        Capture { OneOrMore(.word) }
    }
}
let basket = "apples: 12 green\npears:3\nplums: 4"
let fruit = basket.matches(of: builder.anchorsMatchLineEndings()).map { match in
    "\(match.1) \(match[count]) \(show(match.3))"
}
print("fullregex: builder \(fruit)")
let total = basket.matches(of: builder.anchorsMatchLineEndings()).reduce(0) { $0 + $1[count] }
print("fullregex: builder total \(total)")
