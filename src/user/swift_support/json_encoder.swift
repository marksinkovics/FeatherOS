// Foundation's JSONEncoder for FeatherOS's Codable: the value is encoded into a tree of nodes,
// then printed by the json program's printers. It returns bytes, since there is no Data.
public struct JSONEncoder {
    public struct OutputFormatting: OptionSet, Sendable {
        public let rawValue: UInt

        public init(rawValue: UInt) {
            self.rawValue = rawValue
        }

        public static let prettyPrinted = OutputFormatting(rawValue: 1 << 0)
        public static let sortedKeys = OutputFormatting(rawValue: 1 << 1)
    }

    public var outputFormatting: OutputFormatting = []
    public var userInfo: [CodingUserInfoKey: Any] = [:]

    public init() {}

    public func encode<T: Encodable>(_ value: T) throws -> [UInt8] {
        let tree = try encodeTree(value)
        var output: [UInt8] = []
        if outputFormatting.contains(.prettyPrinted) {
            PrettyPrinter().write(tree, into: &output)
        } else {
            CompactPrinter().write(tree, into: &output)
        }
        return output
    }

    public func encodeTree<T: Encodable>(_ value: T) throws -> JSONValue {
        let root = JSONEncodingNode()
        try value.encode(to: JSONValueEncoder(node: root, codingPath: [], userInfo: userInfo))
        guard let tree = root.resolve(sortingKeys: outputFormatting.contains(.sortedKeys)) else {
            throw EncodingError.invalidValue(
                value,
                EncodingError.Context(
                    codingPath: [], debugDescription: "Top-level value did not encode any values."))
        }
        return tree
    }
}

// A value under construction. Primitives are stored in place; only nested encoders get nodes of
// their own, since they are written to after they are handed out.
final class JSONEncodingNode {
    enum Child {
        case value(JSONValue)
        case node(JSONEncodingNode)
    }

    enum Kind {
        case empty
        case single(JSONValue)
        case object
        case array
    }

    private static let searchLimit = 8
    private(set) var kind = Kind.empty
    private var keys: [[UInt8]] = []
    private(set) var children: [Child] = []
    private var positions: [[UInt8]: Int]?

    func beginObject() {
        switch kind {
        case .empty, .single: kind = .object
        case .object: break
        case .array: preconditionFailure("an unkeyed container was already encoded here")
        }
    }

    func beginArray() {
        switch kind {
        case .empty, .single: kind = .array
        case .array: break
        case .object: preconditionFailure("a keyed container was already encoded here")
        }
    }

    func setSingle(_ value: JSONValue) {
        kind = .single(value)
    }

    func append(_ child: Child) {
        beginArray()
        children.append(child)
    }

    // As Foundation's encoder, a key encoded again replaces its value but keeps its place.
    func set(_ child: Child, forKey key: [UInt8]) {
        beginObject()
        if let index = position(of: key) {
            children[index] = child
            return
        }
        positions?[key] = keys.count
        keys.append(key)
        children.append(child)
        if positions == nil, keys.count > Self.searchLimit {
            var positions: [[UInt8]: Int] = [:]
            for (index, key) in keys.enumerated() {
                positions[key] = index
            }
            self.positions = positions
        }
    }

    // A nested container asked for again under the same key gets the same node.
    func node(forKey key: [UInt8]) -> JSONEncodingNode {
        if let index = position(of: key), case .node(let node) = children[index] {
            return node
        }
        let node = JSONEncodingNode()
        set(.node(node), forKey: key)
        return node
    }

    func resolve(sortingKeys: Bool) -> JSONValue? {
        switch kind {
        case .empty:
            return nil
        case .single(let value):
            return value
        case .array:
            return .array(children.map { Self.resolve($0, sortingKeys: sortingKeys) })
        case .object:
            var members: [JSONMember] = []
            members.reserveCapacity(keys.count)
            for (key, child) in zip(keys, children) {
                let value = Self.resolve(child, sortingKeys: sortingKeys)
                members.append(JSONMember(key: key, value: value))
            }
            if sortingKeys {
                members.sort { $0.key.lexicographicallyPrecedes($1.key) }
            }
            return .object(members)
        }
    }

    // A nested encoder that was never written to stands for an empty object, as in Foundation.
    private static func resolve(_ child: Child, sortingKeys: Bool) -> JSONValue {
        switch child {
        case .value(let value): value
        case .node(let node): node.resolve(sortingKeys: sortingKeys) ?? .object([])
        }
    }

    private func position(of key: [UInt8]) -> Int? {
        if let positions {
            return positions[key]
        }
        return keys.firstIndex(of: key)
    }
}

struct JSONIndexKey: CodingKey {
    let index: Int

    init(_ index: Int) {
        self.index = index
    }

    init?(stringValue: String) {
        return nil
    }

    init?(intValue: Int) {
        self.index = intValue
    }

    var stringValue: String { "Index \(index)" }
    var intValue: Int? { index }
}

final class JSONValueEncoder: EncoderBase {
    private let node: JSONEncodingNode
    private let path: [any CodingKey]
    private let info: [CodingUserInfoKey: Any]

    init(node: JSONEncodingNode, codingPath: [any CodingKey], userInfo: [CodingUserInfoKey: Any]) {
        self.node = node
        self.path = codingPath
        self.info = userInfo
    }

    override var codingPath: [any CodingKey] { path }

    override var userInfo: [CodingUserInfoKey: Any] { info }

    override func beginKeyedContainer() {
        node.beginObject()
    }

    override func beginUnkeyedContainer() {
        node.beginArray()
    }

    override var elementCount: Int {
        if case .array = node.kind {
            return node.children.count
        }
        return 0
    }

    override func encodeNil(at slot: CodingSlot) throws {
        store(.null, at: slot)
    }

    override func encode(_ value: Bool, at slot: CodingSlot) throws {
        store(.bool(value), at: slot)
    }

    override func encode(_ value: String, at slot: CodingSlot) throws {
        store(.string(Array(value.utf8)), at: slot)
    }

    override func encode(_ value: Double, at slot: CodingSlot) throws {
        guard value.isFinite else {
            throw nonFinite(value, at: slot)
        }
        store(.number(Self.numberText(value.description)), at: slot)
    }

    override func encode(_ value: Float, at slot: CodingSlot) throws {
        guard value.isFinite else {
            throw nonFinite(value, at: slot)
        }
        store(.number(Self.numberText(value.description)), at: slot)
    }

    override func encode(_ value: Int64, at slot: CodingSlot) throws {
        store(.number(Array(value.description.utf8)), at: slot)
    }

    override func encode(_ value: UInt64, at slot: CodingSlot) throws {
        store(.number(Array(value.description.utf8)), at: slot)
    }

    override func nestedEncoder(at slot: CodingSlot) -> EncoderBase {
        switch slot {
        case .value:
            return self
        case .key(let key):
            return JSONValueEncoder(
                node: node.node(forKey: Array(key.stringValue.utf8)), codingPath: path + [key],
                userInfo: info)
        case .element:
            let child = JSONEncodingNode()
            let index = elementCount
            node.append(.node(child))
            return JSONValueEncoder(
                node: child, codingPath: path + [JSONIndexKey(index)], userInfo: info)
        }
    }

    private func store(_ value: JSONValue, at slot: CodingSlot) {
        switch slot {
        case .value: node.setSingle(value)
        case .key(let key): node.set(.value(value), forKey: Array(key.stringValue.utf8))
        case .element: node.append(.value(value))
        }
    }

    private func nonFinite(_ value: Any, at slot: CodingSlot) -> EncodingError {
        var path = self.path
        switch slot {
        case .value: break
        case .key(let key): path.append(key)
        case .element: path.append(JSONIndexKey(elementCount))
        }
        return EncodingError.invalidValue(
            value,
            EncodingError.Context(
                codingPath: path,
                debugDescription: "Unable to encode a non-finite number in JSON."))
    }

    // Foundation's encoder writes whole numbers without Swift's ".0".
    private static func numberText(_ description: String) -> [UInt8] {
        var text = Array(description.utf8)
        if text.count > 2, text[text.count - 2] == UInt8(ascii: "."),
            text[text.count - 1] == UInt8(ascii: "0")
        {
            text.removeLast(2)
        }
        return text
    }
}
