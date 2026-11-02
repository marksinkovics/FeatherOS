import FeatherLibC
import FeatherSupport

let help = """
    usage: life [-p glider|blinker|random] [-n generations] [-s columnsxrows]
    Plays Conway's Game of Life on a grid whose edges wrap. Without -n it animates on the
    terminal until q; with -n it prints the grid after that many generations, # for a live
    cell and . for a dead one.
      -p  start from a shape in the middle, or a quarter of the cells at random (the default)
      -n  print the grid after this many generations instead
      -s  the grid's size, from 3x3 to 1000x1000; the terminal's above its last row by default
    """

struct Options {
    var pattern = Pattern.random
    var generations: Int?
    var size: GridSize?
}

func parseOptions() -> Options {
    var options = Options()
    var operands = Arguments.operands[...]
    while let option = operands.popFirst() {
        let letter = option.utf8.count == 2 && option.utf8.first == UInt8(ascii: "-")
            ? option.utf8.last! : 0
        guard [UInt8(ascii: "p"), UInt8(ascii: "n"), UInt8(ascii: "s")].contains(letter) else {
            fail("unknown option \(option); see life -h")
        }
        guard let value = operands.popFirst() else {
            fail("\(option) needs a value; see life -h")
        }
        let bytes = Array(value.utf8)
        switch letter {
        case UInt8(ascii: "p"):
            guard let pattern = Pattern(named: bytes) else {
                fail("unknown pattern \(value); see life -h")
            }
            options.pattern = pattern
        case UInt8(ascii: "n"):
            guard let generations = parseCount(bytes[...]) else {
                fail("invalid generation count \(value)")
            }
            options.generations = generations
        default:
            guard let size = GridSize(parsing: bytes) else {
                fail("invalid size \(value); see life -h")
            }
            options.size = size
        }
    }
    return options
}

// The rows above the last, which shows the generation.
func terminalSize() -> GridSize? {
    var window = winsize()
    guard tcgetwinsize(STDOUT_FILENO, &window) == 0 else {
        return nil
    }
    return GridSize(columns: Int(window.ws_col), rows: Int(window.ws_row) - 1)
}

func printGrid(_ grid: Grid) {
    var output: [UInt8] = []
    for row in 0..<grid.rows {
        grid.appendRow(row, live: UInt8(ascii: "#"), dead: UInt8(ascii: "."), to: &output)
        output.append(UInt8(ascii: "\n"))
    }
    Output.standard.write(bytes: output)
}

// Rewrites only the rows that changed since the shown grid.
func draw(_ grid: Grid, over shown: Grid?, generation: Int) {
    var output: [UInt8] = []
    for row in 0..<grid.rows where !(shown?.rowEquals(row, in: grid) ?? false) {
        output += "\u{1B}[\(row + 1);1H".utf8
        grid.appendRow(row, live: UInt8(ascii: "#"), dead: UInt8(ascii: " "), to: &output)
    }
    // The console wraps as soon as a row fills, which on the last row would scroll.
    let status = "life: generation \(generation), \(grid.population) alive; q quits"
    output += "\u{1B}[\(grid.rows + 1);1H".utf8
    output += status.utf8.prefix(grid.columns - 1)
    output += "\u{1B}[K".utf8
    Output.standard.write(bytes: output)
    Output.standard.flush()
}

func animate(_ grid: Grid) {
    var grid = grid
    var original = termios()
    guard isatty(STDIN_FILENO) != 0, tcgetattr(STDIN_FILENO, &original) == 0 else {
        fail("standard input is not a terminal; use -n to print the grid")
    }
    guard let screen = terminalSize(), grid.columns <= screen.columns, grid.rows <= screen.rows
    else {
        fail("the grid does not fit the terminal; use -n to print it")
    }
    // A read waits at most a tenth of a second, which paces the generations. Ctrl+C comes as a
    // key, since raw mode turns signals off.
    var raw = original
    cfmakeraw(&raw)
    withUnsafeMutableBytes(of: &raw.c_cc) {
        $0[Int(VMIN)] = 0
        $0[Int(VTIME)] = 1
    }
    guard tcsetattr(STDIN_FILENO, TCSANOW, &raw) == 0 else {
        fail("cannot switch the terminal to raw input: \(SystemError.current)")
    }
    Output.standard.write("\u{1B}[?1049h\u{1B}[?25l\u{1B}[2J")
    var shown: Grid?
    var generation = 0
    while true {
        draw(grid, over: shown, generation: generation)
        shown = grid
        var key: UInt8 = 0
        let count = read(STDIN_FILENO, &key, 1)
        if count < 0 && errno != EINTR {
            break
        }
        if count == 1 && [UInt8(ascii: "q"), UInt8(ascii: "Q"), 3].contains(key) {
            break
        }
        grid.advance()
        generation += 1
    }
    Output.standard.write("\u{1B}[?25h\u{1B}[?1049l")
    Output.standard.flush()
    tcsetattr(STDIN_FILENO, TCSANOW, &original)
}

if Arguments.helpRequested {
    print(help)
    exit(.success)
}
let options = parseOptions()
var grid = Grid(options.size ?? terminalSize() ?? GridSize(columns: 80, rows: 23)!)
var generator = SystemRandomNumberGenerator()
options.pattern.place(on: &grid, using: &generator)
if let generations = options.generations {
    for _ in 0..<generations {
        grid.advance()
    }
    printGrid(grid)
} else {
    animate(grid)
}
exit(.success)
