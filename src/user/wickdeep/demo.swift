let demoTurnLimit = 20_000

// Walks, over what the lamp has shown, to the nearest remembered cell with an unseen neighbour,
// then to the stairs or the plinth.
struct DemoBot {
    private func isFrontier(_ game: Game, _ index: Int) -> Bool {
        let x = index % mapWidth
        let y = index / mapWidth
        for step in 0..<8 {
            let direction = Direction(rawValue: step)!
            let nx = x + direction.dx
            let ny = y + direction.dy
            if nx >= 0 && ny >= 0 && nx < mapWidth && ny < mapHeight
                && game.remembered[ny * mapWidth + nx] == 0
            {
                return true
            }
        }
        return false
    }

    private func firstStep(_ game: Game, toward goal: (Int) -> Bool) -> Direction? {
        var parent = [Int](repeating: -2, count: mapWidth * mapHeight)
        var queue = [game.y * mapWidth + game.x]
        parent[queue[0]] = -1
        var head = 0
        while head < queue.count {
            let index = queue[head]
            head += 1
            if index != queue[0] && goal(index) {
                return pathStart(&parent, from: index, origin: queue[0])
            }
            let x = index % mapWidth
            let y = index / mapWidth
            for step in 0..<8 {
                let direction = Direction(rawValue: step)!
                guard
                    canStep(
                        game.cells, width: mapWidth, height: mapHeight, x: x, y: y,
                        dx: direction.dx, dy: direction.dy)
                else {
                    continue
                }
                let next = (y + direction.dy) * mapWidth + x + direction.dx
                if parent[next] == -2 && game.remembered[next] != 0 {
                    parent[next] = index
                    queue.append(next)
                }
            }
        }
        return nil
    }

    private func pathStart(_ parent: inout [Int], from goal: Int, origin: Int) -> Direction? {
        var index = goal
        while parent[index] != origin {
            index = parent[index]
        }
        let dx = index % mapWidth - origin % mapWidth
        let dy = index / mapWidth - origin / mapWidth
        for step in 0..<8 {
            let direction = Direction(rawValue: step)!
            if direction.dx == dx && direction.dy == dy {
                return direction
            }
        }
        return nil
    }

    func next(_ game: Game) -> Command {
        if let direction = firstStep(game, toward: { isFrontier(game, $0) }) {
            return .move(direction)
        }
        if game.cell(game.x, game.y) == .stairs {
            return .descend
        }
        let exits = [Terrain.stairs.rawValue, Terrain.plinth.rawValue]
        if let direction = firstStep(game, toward: { exits.contains(game.cells[$0]) }) {
            return .move(direction)
        }
        return .wait
    }
}

func runDemo(seed: UInt64) {
    var game = Game(seed: seed)
    let bot = DemoBot()
    while game.outcome == .playing && game.turn < demoTurnLimit {
        _ = game.step(bot.next(game))
    }
    let outcome = game.outcome == .bottom ? "bottom" : "limit"
    print(
        "wickdeep: demo turns \(game.turn) depth \(game.depth) level \(game.level) "
            + "score \(game.score) outcome \(outcome) hash \(game.stateHash())")
}
