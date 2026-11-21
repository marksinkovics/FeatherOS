let demoTurnLimit = 20_000

// Fights what stands next to it, heals, refills and equips from the pack, picks up what the lamp
// shows, then walks, over what it has seen, to the nearest remembered cell with an unseen
// neighbour, then to the stairs or the plinth.
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

    private func adjacentTarget(_ game: Game) -> Direction? {
        for step in 0..<8 {
            let direction = Direction(rawValue: step)!
            let nx = game.x + direction.dx
            let ny = game.y + direction.dy
            guard let index = game.monsterIndex(x: nx, y: ny),
                game.reaches(fromX: game.x, fromY: game.y, toX: nx, toY: ny)
            else {
                continue
            }
            let monster = game.monsters[index]
            if monster.kind != .hearthwarden || monster.awake {
                return direction
            }
        }
        return nil
    }

    private func betterGear(_ game: Game) -> Int? {
        for (slot, kind) in game.pack.enumerated() where kind.isGear {
            let worn = kind.isWeapon ? game.weapon : game.coat
            if kind.bonus > (worn?.bonus ?? 0) {
                return slot
            }
        }
        return nil
    }

    private func monstersInView(_ game: Game) -> Int {
        var count = 0
        for monster in game.monsters where game.lit[monster.y * mapWidth + monster.x] != 0 {
            count += 1
        }
        return count
    }

    func next(_ game: Game) -> Command {
        if let direction = adjacentTarget(game) {
            return .move(direction)
        }
        if game.hp * 100 < game.maxHitPoints * 40, let slot = game.pack.firstIndex(of: .salve) {
            return .use(slot)
        }
        if game.oil < 150, let slot = game.pack.firstIndex(of: .oilFlask) {
            return .use(slot)
        }
        if monstersInView(game) >= 3, let slot = game.pack.firstIndex(of: .flareStone) {
            return .use(slot)
        }
        if let slot = betterGear(game) {
            return .equip(slot)
        }
        if game.pack.count < packLimit,
            let direction = firstStep(
                game,
                toward: { game.lit[$0] != 0 && game.itemIndex(x: $0 % mapWidth, y: $0 / mapWidth) != nil })
        {
            return .move(direction)
        }
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
    var outcome = "limit"
    switch game.outcome {
    case .playing: break
    case .dead: outcome = "dead"
    case .victory: outcome = "victory"
    }
    print(
        "wickdeep: demo turns \(game.turn) depth \(game.depth) level \(game.level) "
            + "score \(game.score) outcome \(outcome) hash \(game.stateHash())")
}
