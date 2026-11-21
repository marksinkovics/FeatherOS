enum PieceKind: Int {
    case bar = 0
    case block
    case spur
    case rise
    case fall
    case hook
    case crook

    static let count = 7

    var boxSize: Int {
        switch self {
        case .bar: return 4
        case .block: return 2
        default: return 3
        }
    }
}

struct Cell: Equatable {
    var x: Int
    var y: Int
}

// Cells of a piece in its bounding box at rotation 0 (y grows downwards). Each quarter turn
// clockwise maps (x, y) to (size - 1 - y, x) inside the same box.
func baseCells(_ kind: PieceKind) -> [Cell] {
    switch kind {
    case .bar: return [Cell(x: 0, y: 1), Cell(x: 1, y: 1), Cell(x: 2, y: 1), Cell(x: 3, y: 1)]
    case .block: return [Cell(x: 0, y: 0), Cell(x: 1, y: 0), Cell(x: 0, y: 1), Cell(x: 1, y: 1)]
    case .spur: return [Cell(x: 1, y: 0), Cell(x: 0, y: 1), Cell(x: 1, y: 1), Cell(x: 2, y: 1)]
    case .rise: return [Cell(x: 1, y: 0), Cell(x: 2, y: 0), Cell(x: 0, y: 1), Cell(x: 1, y: 1)]
    case .fall: return [Cell(x: 0, y: 0), Cell(x: 1, y: 0), Cell(x: 1, y: 1), Cell(x: 2, y: 1)]
    case .hook: return [Cell(x: 0, y: 0), Cell(x: 0, y: 1), Cell(x: 1, y: 1), Cell(x: 2, y: 1)]
    case .crook: return [Cell(x: 2, y: 0), Cell(x: 0, y: 1), Cell(x: 1, y: 1), Cell(x: 2, y: 1)]
    }
}

func pieceCells(_ kind: PieceKind, rotation: Int) -> [Cell] {
    let size = kind.boxSize
    var result = baseCells(kind)
    for _ in 0..<(rotation & 3) {
        for index in result.indices {
            result[index] = Cell(x: size - 1 - result[index].y, y: result[index].x)
        }
    }
    return result
}
