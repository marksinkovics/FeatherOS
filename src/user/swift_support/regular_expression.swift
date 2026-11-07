import FeatherLibC

public struct RegularExpressionError: Error, CustomStringConvertible {
    public let code: Int32
    public let description: String
}

// A POSIX regular expression from the C library's regex.h. It matches bytes, so a range may
// split a character that takes more than one byte in UTF-8.
public final class RegularExpression {
    public struct Options: OptionSet {
        public let rawValue: Int32

        public init(rawValue: Int32) {
            self.rawValue = rawValue
        }

        public static let extended = Options(rawValue: REG_EXTENDED)
        public static let ignoreCase = Options(rawValue: REG_ICASE)
        // "." and non-matching lists skip newlines, and "^" and "$" match around them.
        public static let newlineSensitive = Options(rawValue: REG_NEWLINE)
    }

    public struct Match {
        public let text: String
        public let range: Range<String.Index>
        // One per subexpression, nil for each that took no part.
        public let groups: [Range<String.Index>?]

        public var matched: Substring {
            text[range]
        }

        // 0 is the whole match, as in regmatch_t.
        public subscript(group: Int) -> Substring? {
            if group == 0 {
                return matched
            }
            return groups[group - 1].map { text[$0] }
        }
    }

    private var regex = regex_t()

    public var groupCount: Int {
        regex.re_nsub
    }

    public init(_ pattern: String, options: Options = .extended) throws(RegularExpressionError) {
        let code = regcomp(&regex, pattern, options.rawValue)
        guard code == 0 else {
            var message = [CChar](repeating: 0, count: 64)
            regerror(code, &regex, &message, message.count)
            throw RegularExpressionError(
                code: code,
                description: String(decoding: message.prefix { $0 != 0 }.map { UInt8($0) },
                                    as: UTF8.self))
        }
    }

    deinit {
        regfree(&regex)
    }

    public func matches(_ text: String) -> Bool {
        regexec(&regex, text, 0, nil, 0) == 0
    }

    public func firstMatch(in text: String) -> Match? {
        text.withCString { search(text, $0, from: 0) }
    }

    // Each match after the end of the one before; an empty match moves on by a byte.
    public func allMatches(in text: String) -> [Match] {
        text.withCString { start in
            var found: [Match] = []
            var offset = 0
            let length = text.utf8.count
            while offset <= length, let match = search(text, start, from: offset) {
                found.append(match)
                let from = text.utf8.distance(from: text.startIndex, to: match.range.lowerBound)
                let to = text.utf8.distance(from: text.startIndex, to: match.range.upperBound)
                offset = to > from ? to : to + 1
            }
            return found
        }
    }

    private func search(_ text: String, _ start: UnsafePointer<CChar>, from offset: Int) -> Match? {
        var spans = [regmatch_t](repeating: regmatch_t(), count: groupCount + 1)
        let flags = offset > 0 ? REG_NOTBOL : 0
        guard regexec(&regex, start + offset, spans.count, &spans, flags) == 0 else {
            return nil
        }
        let utf8 = text.utf8
        func range(_ span: regmatch_t) -> Range<String.Index>? {
            guard span.rm_so >= 0 else {
                return nil
            }
            let lower = utf8.index(utf8.startIndex, offsetBy: offset + span.rm_so)
            let upper = utf8.index(lower, offsetBy: span.rm_eo - span.rm_so)
            return lower..<upper
        }
        return Match(text: text, range: range(spans[0])!, groups: spans.dropFirst().map(range))
    }
}
