// A slope as an exact fraction; run is always positive.
struct Slope {
    var rise: Int
    var run: Int

    func below(_ other: Slope) -> Bool {
        rise * other.run < other.rise * run
    }
}

// Recursive shadowcasting over one octant at a time: depth counts away from the viewer along one
// axis and offset along the other, and a cell spans the slopes of its corners.
struct Caster {
    let cells: [UInt8]
    let width: Int
    let height: Int
    let originX: Int
    let originY: Int
    let radius: Int
    var lit: [UInt8]
    var xx = 1
    var xy = 0
    var yx = 0
    var yy = 1

    init(_ cells: [UInt8], width: Int, height: Int, x: Int, y: Int, radius: Int) {
        self.cells = cells
        self.width = width
        self.height = height
        originX = x
        originY = y
        self.radius = radius
        lit = [UInt8](repeating: 0, count: width * height)
    }

    func opaque(_ x: Int, _ y: Int) -> Bool {
        x < 0 || y < 0 || x >= width || y >= height || cells[y * width + x] == Terrain.wall.rawValue
    }

    mutating func scan(depth: Int, start: Slope, end: Slope) {
        guard !start.below(end) else {
            return
        }
        var start = start
        var depth = depth
        while depth <= radius {
            var blocked = false
            var next = start
            var offset = depth
            while offset >= 0 {
                let upper = Slope(rise: 2 * offset + 1, run: 2 * depth - 1)
                let lower = Slope(rise: 2 * offset - 1, run: 2 * depth + 1)
                if start.below(lower) {
                    offset -= 1
                    continue
                }
                if upper.below(end) {
                    break
                }
                let x = originX + offset * xx + depth * xy
                let y = originY + offset * yx + depth * yy
                let wall = opaque(x, y)
                if x >= 0 && y >= 0 && x < width && y < height
                    && offset * offset + depth * depth <= radius * radius + radius
                {
                    lit[y * width + x] = 1
                }
                if blocked {
                    if wall {
                        next = lower
                    } else {
                        blocked = false
                        start = next
                    }
                } else if wall && depth < radius {
                    blocked = true
                    scan(depth: depth + 1, start: start, end: upper)
                    next = lower
                }
                offset -= 1
            }
            if blocked {
                break
            }
            depth += 1
        }
    }
}

func fieldOfView(
    _ cells: [UInt8], width: Int, height: Int, x: Int, y: Int, radius: Int
) -> [UInt8] {
    var caster = Caster(cells, width: width, height: height, x: x, y: y, radius: radius)
    caster.lit[y * width + x] = 1
    let octants = [
        (1, 0, 0, 1), (-1, 0, 0, 1), (1, 0, 0, -1), (-1, 0, 0, -1),
        (0, 1, 1, 0), (0, 1, -1, 0), (0, -1, 1, 0), (0, -1, -1, 0),
    ]
    for octant in octants {
        caster.xx = octant.0
        caster.xy = octant.1
        caster.yx = octant.2
        caster.yy = octant.3
        caster.scan(depth: 1, start: Slope(rise: 1, run: 1), end: Slope(rise: 0, run: 1))
    }
    return caster.lit
}

func lampRadius(oil: Int) -> Int {
    if oil >= 500 {
        return 6
    }
    if oil >= 200 {
        return 5
    }
    if oil >= 50 {
        return 3
    }
    return oil > 0 ? 2 : 1
}
