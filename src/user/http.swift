import FeatherLibC
import FeatherSupport
import HttpMessage

let help = """
    usage: http url <url> [reference...]
           http request [file]
           http response [file]
    Parses HTTP with the C++ code of fetch and httpd. url prints a URL's host, port and path,
    then each reference resolved against it. request prints each request head in its input,
    and response a response's status, then its body without the framing.
    """

typealias Http = FeatherOS.User.Http

// C++ character arrays reach Swift as tuples.
func string<Characters>(_ characters: Characters) -> String {
    withUnsafeBytes(of: characters) {
        String(cString: $0.assumingMemoryBound(to: CChar.self).baseAddress!)
    }
}

func string(_ characters: [CChar]) -> String {
    characters.withUnsafeBufferPointer { String(cString: $0.baseAddress!) }
}

func describe(_ status: Http.UrlStatus) -> String {
    switch status {
    case .UnsupportedScheme: "only http URLs are supported"
    case .TooLong: "too long"
    default: "malformed"
    }
}

func formatted(_ url: Http.Url) -> String {
    var host = string(url.host)
    if host.utf8.contains(UInt8(ascii: ":")) {
        host = "[\(host)]"
    }
    let port = string(url.port)
    let shownPort = port.utf8.elementsEqual("80".utf8) ? "" : ":\(port)"
    return "http://\(host)\(shownPort)\(string(url.path))"
}

func showUrl(_ text: String, references: ArraySlice<String>) -> ExitCode {
    var base = Http.Url()
    let status = Http.ParseUrl(text, &base)
    guard status == .Ok else {
        fail("\(text): \(describe(status))")
    }
    print("host \(string(base.host))")
    print("port \(string(base.port))")
    print("path \(string(base.path))")
    var result = ExitCode.success
    for reference in references {
        var url = Http.Url()
        let status = Http.ResolveUrl(base, reference, &url)
        if status == .Ok {
            print("\(reference) resolves to \(formatted(url))")
        } else {
            warn("\(reference): \(describe(status))")
            result = .failure
        }
    }
    return result
}

// Passes the input to body a chunk at a time while it returns true.
func readChunks(
    from path: String?, _ body: (UnsafeMutableBufferPointer<CChar>) -> Bool
) throws(SystemError) {
    var descriptor: Int32 = 0
    if let path {
        descriptor = open(path, O_RDONLY)
        guard descriptor >= 0 else {
            throw SystemError.current
        }
    }
    defer {
        if path != nil {
            close(descriptor)
        }
    }
    var buffer = [CChar](repeating: 0, count: Int(BUFSIZ))
    while true {
        let count = buffer.withUnsafeMutableBytes { read(descriptor, $0.baseAddress, $0.count) }
        if count < 0 {
            throw SystemError.current
        }
        let more = buffer.withUnsafeMutableBufferPointer {
            count > 0 && body(UnsafeMutableBufferPointer(rebasing: $0[..<count]))
        }
        if !more {
            return
        }
    }
}

// Prints a complete request head, and returns whether another may follow it.
func report(_ parser: Http.RequestParser) -> Bool {
    let error = parser.ErrorStatus()
    guard error == 0 else {
        print("refused with \(error) \(String(cString: Http.StatusReason(error)))")
        return false
    }
    var path = [CChar](repeating: 0, count: Http.MaximumPathLength + 1)
    var decoded = "a malformed path"
    if Http.RequestPath(parser.Target(), &path, path.count) {
        decoded = "\(string(path)) (\(String(cString: Http.ContentType(path))))"
    }
    let method = String(cString: parser.MethodName())
    let target = String(cString: parser.Target())
    let connection = parser.KeepAlive() ? "keep-alive" : "close"
    print("\(method) \(target) names \(decoded), HTTP/1.\(parser.MinorVersion()), \(connection)")
    if parser.HasBody() {
        print("a body follows")
        return false
    }
    return true
}

func showRequests(from path: String?) throws(SystemError) -> ExitCode {
    var parser = Http.RequestParser()
    var started = false
    try readChunks(from: path) { chunk in
        var offset = 0
        while offset < chunk.count {
            offset += parser.Feed(chunk.baseAddress! + offset, chunk.count - offset)
            started = true
            guard parser.Done() else {
                break
            }
            guard report(parser) else {
                return false
            }
            parser = Http.RequestParser()
            started = false
        }
        return true
    }
    if started && !parser.Done() {
        warn("the input ends inside a request head")
        return .failure
    }
    return .success
}

func showResponse(from path: String?) throws(SystemError) -> ExitCode {
    var parser = Http.ResponseParser()
    var shown = false
    try readChunks(from: path) { chunk in
        let length = parser.Feed(chunk.baseAddress, chunk.count)
        if parser.HeadersComplete() && !shown {
            shown = true
            print("\(parser.Status()) \(String(cString: parser.Reason()))")
            let location = String(cString: parser.Location())
            if !location.isEmpty {
                print("location \(location)")
            }
        }
        fwrite(chunk.baseAddress, 1, length, stdout)
        return !parser.Done()
    }
    if !parser.Done() && !parser.Finish() || parser.Error() != .None {
        warn(String(cString: Http.ResponseErrorText(parser.Error())))
        return .failure
    }
    return .success
}

if Arguments.helpRequested {
    print(help)
    exit(.success)
}
let operands = Arguments.operands
let usage = "usage: http url <url> [reference...] | request [file] | response [file]"
guard let command = operands.first else {
    fail(usage)
}
if command.utf8.elementsEqual("url".utf8) {
    guard operands.count >= 2 else {
        fail(usage)
    }
    exit(showUrl(operands[1], references: operands[2...]))
}
guard operands.count <= 2 else {
    fail(usage)
}
let path = operands.count == 2 ? operands[1] : nil
do throws(SystemError) {
    if command.utf8.elementsEqual("request".utf8) {
        exit(try showRequests(from: path))
    }
    if command.utf8.elementsEqual("response".utf8) {
        exit(try showResponse(from: path))
    }
} catch {
    fail("\(path ?? "standard input"): \(error)")
}
fail(usage)
