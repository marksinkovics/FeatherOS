enum Pattern {
    case glider
    case blinker
    case random

    init?(named name: [UInt8]) {
        switch name {
        case Array("glider".utf8):
            self = .glider
        case Array("blinker".utf8):
            self = .blinker
        case Array("random".utf8):
            self = .random
        default:
            return nil
        }
    }

    // Live cells as (column, row) from the pattern's top left corner.
    private var shape: [(Int, Int)] {
        switch self {
        case .glider:
            [(1, 0), (2, 1), (0, 2), (1, 2), (2, 2)]
        case .blinker:
            [(0, 1), (1, 1), (2, 1)]
        case .random:
            []
        }
    }

    // A shape in the middle of the grid, or a quarter of the cells at random.
    func place<Generator: RandomNumberGenerator>(
        on grid: inout Grid, using generator: inout Generator
    ) {
        if self == .random {
            for row in 0..<grid.rows {
                for column in 0..<grid.columns {
                    grid[column, row] = UInt8.random(in: 0..<4, using: &generator) == 0
                }
            }
            return
        }
        let left = (grid.columns - 3) / 2
        let top = (grid.rows - 3) / 2
        for (column, row) in shape {
            grid[left + column, top + row] = true
        }
    }
}
