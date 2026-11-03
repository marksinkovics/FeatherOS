import FeatherLibC
import FeatherSupport
import _Concurrency

let help = """
    usage: taskprimes [tasks 1-64] [limit]
    Counts the primes below limit (default 1000000, at most 1000000000) in a task group of
    tasks (default 16), which the global executor runs on a thread for each CPU, and shows
    the threads and CPUs they ran on. Compare it with primes.
    """

struct Share {
    var primes = 0
    var cpus: UInt64 = 0
    var thread: pthread_t = 0
}

func isOddPrime(_ number: Int) -> Bool {
    var divisor = 3
    while divisor * divisor <= number {
        if number % divisor == 0 {
            return false
        }
        divisor += 2
    }
    return true
}

func currentCpu() -> UInt64 {
    let cpu = sched_getcpu()
    return cpu >= 0 && cpu < 64 ? 1 << UInt64(cpu) : 0
}

// Task index takes every count-th odd number from 3, so the shares take about as long.
func countShare(index: Int, count: Int, limit: Int) -> Share {
    var share = Share(cpus: currentCpu(), thread: pthread_self())
    var number = 3 + 2 * index
    while number < limit {
        if isOddPrime(number) {
            share.primes += 1
        }
        number += 2 * count
    }
    share.cpus |= currentCpu()
    return share
}

func cpuList(_ cpus: UInt64) -> String {
    var text = ""
    for cpu in 0..<64 where cpus & (1 << UInt64(cpu)) != 0 {
        text += text.isEmpty ? "\(cpu)" : ",\(cpu)"
    }
    return text
}

func milliseconds() -> Int {
    var now = timespec()
    clock_gettime(CLOCK_MONOTONIC, &now)
    return Int(now.tv_sec) * 1000 + Int(now.tv_nsec) / 1_000_000
}

func parseOptions() -> (tasks: Int, limit: Int) {
    let operands = Arguments.operands
    guard operands.count <= 2,
        let tasks = operands.count > 0 ? Int(operands[0]) : 16, (1...64).contains(tasks),
        let limit = operands.count > 1 ? Int(operands[1]) : 1_000_000,
        (0...1_000_000_000).contains(limit)
    else {
        fail("usage: taskprimes [tasks 1-64] [limit up to 1000000000]")
    }
    return (tasks, limit)
}

@main
struct TaskPrimes {
    static func main() async {
        if Arguments.helpRequested {
            print(help)
            exit(.success)
        }
        let (tasks, limit) = parseOptions()
        let start = milliseconds()
        let shares = await withTaskGroup(of: Share.self) { group in
            for index in 0..<tasks {
                group.addTask { countShare(index: index, count: tasks, limit: limit) }
            }
            var shares: [Share] = []
            for await share in group {
                shares.append(share)
            }
            return shares
        }
        let elapsed = milliseconds() - start
        // 2 is the only even prime.
        var total = limit > 2 ? 1 : 0
        var cpus: UInt64 = 0
        var threads: Set<pthread_t> = []
        for share in shares {
            total += share.primes
            cpus |= share.cpus
            threads.insert(share.thread)
        }
        print("taskprimes: \(total) primes below \(limit) with \(tasks) tasks in \(elapsed) ms")
        print("taskprimes: the tasks ran on \(threads.count) threads and CPU \(cpuList(cpus))")
    }
}
