import FeatherLibC
import FeatherSupport

let help = """
    usage: swiftbench [-n runs | -t seconds] [workload...]
    Runs the workloads cppbench runs, written the same way in Swift, and prints
    each one's check value, which matches cppbench's, its runs and the time each took.
      life       64x64 cells for 20 generations, on life's own grid
      sieve      the primes below 1000000
      quicksort  20000 numbers, by the quicksort cppbench has too
      sort       the same numbers, by the standard library's sort
      json       a document of 500 objects, by json's own parser
    -t runs each workload for that many seconds (3 by default), -n that many times.
    """

enum Workload: CaseIterable {
    case life
    case sieve
    case quicksort
    case sort
    case json

    var name: String {
        switch self {
        case .life: "life"
        case .sieve: "sieve"
        case .quicksort: "quicksort"
        case .sort: "sort"
        case .json: "json"
        }
    }
}

enum Limit {
    case runs(Int)
    case milliseconds(Int)

    func reached(runs: Int, elapsed: Int) -> Bool {
        switch self {
        case .runs(let count): runs >= count
        case .milliseconds(let duration): elapsed >= duration
        }
    }
}

func parseOptions() -> (limit: Limit, chosen: [Workload]) {
    var limit = Limit.milliseconds(3000)
    var chosen: [Workload] = []
    var operands = Arguments.operands[...]
    while let operand = operands.popFirst() {
        let isRuns = operand.utf8.elementsEqual("-n".utf8)
        if isRuns || operand.utf8.elementsEqual("-t".utf8) {
            guard let value = operands.popFirst(), let count = parseCount(Array(value.utf8)[...]),
                count > 0
            else {
                fail("\(operand) needs a positive count; see swiftbench -h")
            }
            limit = isRuns ? .runs(count) : .milliseconds(count * 1000)
        } else if let workload = Workload.allCases.first(where: {
            $0.name.utf8.elementsEqual(operand.utf8)
        }) {
            chosen.append(workload)
        } else {
            fail("unknown workload \(operand); see swiftbench -h")
        }
    }
    return (limit, chosen.isEmpty ? Workload.allCases : chosen)
}

// The monotonic clock advances in 10 ms ticks.
func milliseconds() -> Int {
    var now = timespec()
    clock_gettime(CLOCK_MONOTONIC, &now)
    return Int(now.tv_sec) * 1000 + Int(now.tv_nsec) / 1_000_000
}

func measure(_ workload: Workload, _ limit: Limit, _ body: () -> Int) {
    let start = milliseconds()
    var runs = 0
    var check = 0
    var elapsed = 0
    repeat {
        check = body()
        runs += 1
        elapsed = milliseconds() - start
    } while !limit.reached(runs: runs, elapsed: elapsed)
    print(
        "\(workload.name): check \(check), \(runs) run\(runs == 1 ? "" : "s") in \(elapsed) ms, "
            + "\(elapsed * 1000 / runs) us each")
}

if Arguments.helpRequested {
    print(help)
    exit(.success)
}
let (limit, chosen) = parseOptions()
let sortValues = makeSortValues()
let document = makeDocument()
for workload in chosen {
    switch workload {
    case .life:
        measure(workload, limit, runLife)
    case .sieve:
        measure(workload, limit, runSieve)
    case .quicksort:
        measure(workload, limit) { runQuicksort(sortValues) }
    case .sort:
        measure(workload, limit) { runLibrarySort(sortValues) }
    case .json:
        measure(workload, limit) { runJSON(document) }
    }
}
