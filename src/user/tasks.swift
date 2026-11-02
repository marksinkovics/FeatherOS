import _Concurrency

// Swift concurrency on one thread: the toolchain's cooperative executor runs every job in turn,
// in the order they were queued, so the interleaving below is the same on every run.

actor Counter {
    private(set) var value = 0

    func increment() {
        value += 1
    }
}

actor Log {
    private(set) var entries: [String] = []

    func append(_ entry: String) {
        entries.append(entry)
    }
}

enum Failure: Error {
    case expected
}

enum Request {
    @TaskLocal static var name = "none"
}

func square(_ number: Int) async -> Int {
    await Task.yield()
    return number * number
}

func sumOfSquares(upTo limit: Int) async -> Int {
    await withTaskGroup(of: Int.self) { group in
        for number in 1...limit {
            group.addTask { await square(number) }
        }
        var sum = 0
        for await value in group {
            sum += value
        }
        return sum
    }
}

func takeTurns(rounds: Int) async -> String {
    let log = Log()
    let ping = Task {
        for round in 1...rounds {
            await log.append("ping \(round)")
            await Task.yield()
        }
    }
    let pong = Task {
        for round in 1...rounds {
            await log.append("pong \(round)")
            await Task.yield()
        }
    }
    await ping.value
    await pong.value
    var text = ""
    for entry in await log.entries {
        text += text.isEmpty ? entry : ", " + entry
    }
    return text
}

func countConcurrently(times: Int) async -> Int {
    let counter = Counter()
    await withTaskGroup(of: Void.self) { group in
        for _ in 0..<times {
            group.addTask { await counter.increment() }
        }
    }
    return await counter.value
}

func resumedLater() async -> Int {
    await withCheckedContinuation { continuation in
        Task {
            await Task.yield()
            continuation.resume(returning: 7)
        }
    }
}

func streamed(count: Int) async -> Int {
    let (stream, continuation) = AsyncStream.makeStream(of: Int.self)
    Task {
        for number in 1...count {
            continuation.yield(number)
            await Task.yield()
        }
        continuation.finish()
    }
    var sum = 0
    for await number in stream {
        sum += number
    }
    return sum
}

func spinUntilCancelled() async -> Int {
    let spinner = Task {
        var turns = 0
        while !Task.isCancelled {
            turns += 1
            await Task.yield()
        }
        return turns
    }
    for _ in 1...3 {
        await Task.yield()
    }
    spinner.cancel()
    return await spinner.value
}

func thrown() async -> Bool {
    let failing = Task { () async throws -> Int in
        throw Failure.expected
    }
    do {
        _ = try await failing.value
        return false
    } catch Failure.expected {
        return true
    } catch {
        return false
    }
}

@main
struct Tasks {
    static func main() async {
        let answer = await Task { 6 * 7 }.value
        print("tasks: a task returned \(answer)")
        print("tasks: turns \(await takeTurns(rounds: 3))")
        print("tasks: 100 increments counted \(await countConcurrently(times: 100))")
        print("tasks: squares of 1 to 10 sum to \(await sumOfSquares(upTo: 10))")
        async let three = square(3)
        async let four = square(4)
        print("tasks: async let gave \(await three + four)")
        print("tasks: a continuation resumed with \(await resumedLater())")
        print("tasks: a stream of 1 to 5 sums to \(await streamed(count: 5))")
        print("tasks: cancelled after \(await spinUntilCancelled()) turns")
        print("tasks: the error came through \(await thrown())")
        let names = await Request.$name.withValue("outer") {
            let inner = await Task { Request.name }.value
            let detached = await Task.detached { Request.name }.value
            return "\(Request.name), task \(inner), detached \(detached)"
        }
        print("tasks: task-local \(names)")
    }
}
