import FeatherSupport

// The workloads cppbench.cpp runs too, written the same way, so their check values match.

let lifeSize = 64
let lifeGenerations = 20
let sieveLimit = 1_000_000
let sortCount = 20_000
let jsonObjects = 500

// SplitMix64, so both programs draw the same numbers from the same seed.
struct SplitMix {
    private var state: UInt64

    init(seed: UInt64) {
        state = seed
    }

    mutating func next() -> UInt64 {
        state &+= 0x9e37_79b9_7f4a_7c15
        var value = state
        value = (value ^ (value >> 30)) &* 0xbf58_476d_1ce4_e5b9
        value = (value ^ (value >> 27)) &* 0x94d0_49bb_1331_11eb
        return value ^ (value >> 31)
    }
}

// life's own Grid, from a quarter of the cells at random.
func runLife() -> Int {
    var generator = SplitMix(seed: 1)
    var grid = Grid(GridSize(columns: lifeSize, rows: lifeSize)!)
    for row in 0..<grid.rows {
        for column in 0..<grid.columns {
            grid[column, row] = generator.next() % 4 == 0
        }
    }
    for _ in 0..<lifeGenerations {
        grid.advance()
    }
    return grid.population
}

func runSieve() -> Int {
    var composite = [Bool](repeating: false, count: sieveLimit)
    var primes = 0
    var number = 2
    while number < sieveLimit {
        if !composite[number] {
            primes += 1
            var multiple = number * number
            while multiple < sieveLimit {
                composite[multiple] = true
                multiple += number
            }
        }
        number += 1
    }
    return primes
}

func makeSortValues() -> [UInt32] {
    var generator = SplitMix(seed: 2)
    return (0..<sortCount).map { _ in UInt32(truncatingIfNeeded: generator.next()) }
}

// Depends on every value's place, so a wrong order shows.
func sortCheck(_ values: [UInt32]) -> Int {
    var sum: UInt32 = 0
    for (index, value) in values.enumerated() {
        sum = sum &+ value &* UInt32(truncatingIfNeeded: index + 1)
    }
    return Int(sum)
}

// Leaves ranges of at most 16 values for one insertion sort over the whole array.
func quicksort(_ values: inout [UInt32], _ low: Int, _ high: Int) {
    var low = low
    var high = high
    while high - low > 16 {
        let middle = low + (high - low) / 2
        if values[middle] < values[low] {
            values.swapAt(middle, low)
        }
        if values[high - 1] < values[low] {
            values.swapAt(high - 1, low)
        }
        if values[high - 1] < values[middle] {
            values.swapAt(high - 1, middle)
        }
        // Hoare's partition, which the median of three keeps inside the range.
        let pivot = values[middle]
        var left = low - 1
        var right = high
        while true {
            repeat { left += 1 } while values[left] < pivot
            repeat { right -= 1 } while values[right] > pivot
            if left >= right {
                break
            }
            values.swapAt(left, right)
        }
        if right + 1 - low < high - right - 1 {
            quicksort(&values, low, right + 1)
            low = right + 1
        } else {
            quicksort(&values, right + 1, high)
            high = right + 1
        }
    }
}

func insertionSort(_ values: inout [UInt32]) {
    for index in 1..<values.count {
        let value = values[index]
        var position = index
        while position > 0, values[position - 1] > value {
            values[position] = values[position - 1]
            position -= 1
        }
        values[position] = value
    }
}

func runQuicksort(_ original: [UInt32]) -> Int {
    var values = original
    quicksort(&values, 0, values.count)
    insertionSort(&values)
    return sortCheck(values)
}

func runLibrarySort(_ original: [UInt32]) -> Int {
    var values = original
    values.sort()
    return sortCheck(values)
}

// Objects of 8 members, so json's parser searches them in order rather than through a
// dictionary, as the C++ port does.
func makeDocument() -> [UInt8] {
    var generator = SplitMix(seed: 3)
    var text: [UInt8] = Array("[".utf8)
    for index in 0..<jsonObjects {
        let price = generator.next() % 100_000
        text += (index == 0 ? "\n  " : ",\n  ").utf8
        text += "{\"id\": \(index), \"name\": \"item \(index)\", ".utf8
        text += "\"price\": \(price / 100).\(price / 10 % 10)\(price % 10), ".utf8
        text += "\"active\": \(price % 2 == 0), ".utf8
        text += "\"tags\": [\"alpha\", \"beta\\n\", \"caf\\u00e9\"], ".utf8
        text += "\"note\": \"line \\\"\(index)\\\" \u{FC} ends\", ".utf8
        text += "\"scale\": -1.5e3, \"parent\": null}".utf8
    }
    text += "\n]\n".utf8
    return text
}

struct Tally {
    var values = 0
    var bytes = 0

    mutating func add(_ value: JSONValue) {
        values += 1
        switch value {
        case .null, .bool:
            break
        case .number(let text), .string(let text):
            bytes += text.count
        case .array(let elements):
            for element in elements {
                add(element)
            }
        case .object(let members):
            for member in members {
                bytes += member.key.count
                add(member.value)
            }
        }
    }
}

// json's own parser, walked to count the values and their bytes.
func runJSON(_ document: [UInt8]) -> Int {
    do throws(JSONParseError) {
        var tally = Tally()
        for value in try JSONParser.parseAll(document) {
            tally.add(value)
        }
        return document.count + tally.values + tally.bytes
    } catch {
        fail("json: \(error)")
    }
}
