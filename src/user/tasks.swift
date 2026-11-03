import _Concurrency

// Swift concurrency: the global executor runs jobs on a thread for each CPU, so tasks that are
// not on the same actor run at once and their interleaving changes from run to run.

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
    let entries = await log.entries
    let pings = entries.filter { $0.hasPrefix("ping") }
    let pongs = entries.filter { $0.hasPrefix("pong") }
    let inOrder = pings == (1...rounds).map { "ping \($0)" }
        && pongs == (1...rounds).map { "pong \($0)" }
    return "\(pings.count) pings and \(pongs.count) pongs, \(inOrder ? "each" : "not") in order"
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

// Every value waits in the stream's buffer before the first is read.
func buffered(count: Int) async -> Int {
    let (stream, continuation) = AsyncStream.makeStream(of: Int.self)
    for number in 1...count {
        continuation.yield(number)
    }
    continuation.finish()
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

func milliseconds(_ duration: Duration) -> Int64 {
    let (seconds, attoseconds) = duration.components
    return seconds * 1000 + attoseconds / 1_000_000_000_000_000
}

func sleeps() async throws -> String {
    let clock = ContinuousClock()
    let start = clock.now
    try await Task.sleep(nanoseconds: 20_000_000)
    try await Task.sleep(for: .milliseconds(30))
    try await Task.sleep(until: .now + .milliseconds(50), clock: .suspending)
    let elapsed = start.duration(to: clock.now)
    return elapsed >= .milliseconds(100) ? "at least 100 ms" : "only \(milliseconds(elapsed)) ms"
}

func cancelledSleep() async -> String {
    let clock = ContinuousClock()
    let start = clock.now
    let sleeper = Task { () async -> String in
        do {
            try await Task.sleep(for: .seconds(60))
            return "woke"
        } catch is CancellationError {
            return "CancellationError"
        } catch {
            return "another error"
        }
    }
    try? await Task.sleep(for: .milliseconds(10))
    sleeper.cancel()
    let outcome = await sleeper.value
    return "\(start.duration(to: clock.now) < .seconds(1) ? "early" : "late") with \(outcome)"
}

func checkedCancellation() async -> String {
    let task = Task { () async -> String in
        while !Task.isCancelled {
            await Task.yield()
        }
        do {
            try Task.checkCancellation()
            return "nothing"
        } catch is CancellationError {
            return "CancellationError"
        } catch {
            return "another error"
        }
    }
    task.cancel()
    return await task.value
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
        print("tasks: a buffered stream of 1 to 100 sums to \(await buffered(count: 100))")
        print("tasks: cancelled after \(await spinUntilCancelled()) turns")
        print("tasks: the error came through \(await thrown())")
        let names = await Request.$name.withValue("outer") {
            let inner = await Task { Request.name }.value
            let detached = await Task.detached { Request.name }.value
            return "\(Request.name), task \(inner), detached \(detached)"
        }
        print("tasks: task-local \(names)")
        do {
            print("tasks: slept 20, 30 and 50 ms for \(try await sleeps())")
        } catch {
            print("tasks: a sleep failed")
        }
        print("tasks: a sleep of 60 s ended \(await cancelledSleep())")
        print("tasks: checkCancellation threw \(await checkedCancellation())")
    }
}
