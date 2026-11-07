struct GridSize: Equatable {
    static let smallest = 3
    static let largest = 1000

    let columns: Int
    let rows: Int

    init?(columns: Int, rows: Int) {
        let range = GridSize.smallest...GridSize.largest
        guard range.contains(columns), range.contains(rows) else {
            return nil
        }
        self.columns = columns
        self.rows = rows
    }

    // columnsxrows, as in 80x23.
    init?(parsing text: [UInt8]) {
        guard let cross = text.firstIndex(of: UInt8(ascii: "x")),
            let columns = parseCount(text[..<cross]),
            let rows = parseCount(text[(cross + 1)...])
        else {
            return nil
        }
        self.init(columns: columns, rows: rows)
    }
}

// Digits only, at most a billion.
func parseCount(_ text: ArraySlice<UInt8>) -> Int? {
    guard !text.isEmpty, text.count <= 10 else {
        return nil
    }
    var value = 0
    for byte in text {
        guard (UInt8(ascii: "0")...UInt8(ascii: "9")).contains(byte) else {
            return nil
        }
        value = value * 10 + Int(byte - UInt8(ascii: "0"))
    }
    return value <= 1_000_000_000 ? value : nil
}

// The edges wrap, so what leaves one side comes back on the other.
struct Grid {
    let columns: Int
    let rows: Int
    private var cells: [UInt8]

    init(_ size: GridSize) {
        columns = size.columns
        rows = size.rows
        cells = [UInt8](repeating: 0, count: columns * rows)
    }

    subscript(column: Int, row: Int) -> Bool {
        get { cells[index(column, row)] != 0 }
        set { cells[index(column, row)] = newValue ? 1 : 0 }
    }

    var population: Int {
        cells.reduce(0) { $0 + Int($1) }
    }

    mutating func advance() {
        var next = [UInt8](repeating: 0, count: cells.count)
        for row in 0..<rows {
            let above = (row + rows - 1) % rows * columns
            let here = row * columns
            let below = (row + 1) % rows * columns
            for column in 0..<columns {
                let left = (column + columns - 1) % columns
                let right = (column + 1) % columns
                let neighbors =
                    cells[above + left] + cells[above + column] + cells[above + right]
                    + cells[here + left] + cells[here + right]
                    + cells[below + left] + cells[below + column] + cells[below + right]
                if neighbors == 3 || (neighbors == 2 && cells[here + column] != 0) {
                    next[here + column] = 1
                }
            }
        }
        cells = next
    }

    func appendRow(_ row: Int, live: UInt8, dead: UInt8, to output: inout [UInt8]) {
        for cell in cells[row * columns..<(row + 1) * columns] {
            output.append(cell != 0 ? live : dead)
        }
    }

    func rowEquals(_ row: Int, in other: Grid) -> Bool {
        let range = row * columns..<(row + 1) * columns
        return columns == other.columns && rows == other.rows
            && cells[range].elementsEqual(other.cells[range])
    }

    private func index(_ column: Int, _ row: Int) -> Int {
        (row % rows + rows) % rows * columns + (column % columns + columns) % columns
    }
}
