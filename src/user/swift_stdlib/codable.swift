// Codable for Embedded Swift, compiled into the Swift module in place of the unavailable stubs in
// upstream's EmbeddedStubs.swift. Upstream's containers wrap formats behind protocols with generic
// requirements, which Embedded Swift cannot call through an existential. Here a format subclasses
// EncoderBase or DecoderBase and overrides methods that take only concrete types, and the generic
// parts are containers that are structs, specialized where they are used. The compiler still
// synthesizes conformances: it finds Encoder and Decoder as protocols, here bound to those classes.

/// A type that can encode itself to an external representation.
public protocol Encodable {
    func encode(to encoder: any Encoder) throws

    // Writes the value into one slot of a container. Primitive types write themselves without a
    // nested encoder; everything else gets one.
    func _encode(into encoder: EncoderBase, at slot: CodingSlot) throws

    // Embedded Swift cannot cast to CodingKeyRepresentable, so Dictionary asks its keys these.
    static var _encodesAsCodingKey: Bool { get }
    var _dictionaryCodingKey: (any CodingKey)? { get }
}

/// A type that can decode itself from an external representation.
public protocol Decodable {
    init(from decoder: any Decoder) throws

    static func _decode(from decoder: DecoderBase, at slot: CodingSlot) throws -> Self

    static var _decodesFromCodingKey: Bool { get }
    static func _init(dictionaryCodingKey key: _DictionaryCodingKey) -> Self?
}

/// A type that can convert itself into and out of an external representation.
public typealias Codable = Encodable & Decodable

extension Encodable {
    public func _encode(into encoder: EncoderBase, at slot: CodingSlot) throws {
        try encode(to: encoder.nestedEncoder(at: slot))
    }

    public static var _encodesAsCodingKey: Bool { false }

    public var _dictionaryCodingKey: (any CodingKey)? { nil }
}

extension Decodable {
    public static func _decode(from decoder: DecoderBase, at slot: CodingSlot) throws -> Self {
        try Self(from: decoder.nestedDecoder(at: slot))
    }

    public static var _decodesFromCodingKey: Bool { false }

    public static func _init(dictionaryCodingKey key: _DictionaryCodingKey) -> Self? { nil }
}

/// A type that can be used as a key for encoding and decoding.
public protocol CodingKey: Sendable, CustomStringConvertible, CustomDebugStringConvertible {
    var stringValue: String { get }
    init?(stringValue: String)
    var intValue: Int? { get }
    init?(intValue: Int)
}

extension CodingKey {
    // Upstream starts with the key's type name, which Embedded Swift cannot print.
    public var description: String {
        _codingKeyDescription(stringValue, intValue)
    }

    public var debugDescription: String { description }
}

// Each key type has its own description, so the work is done once here.
private func _codingKeyDescription(_ stringValue: String, _ intValue: Int?) -> String {
    let intValue = intValue?.description ?? "nil"
    return "CodingKey(stringValue: \"\(stringValue)\", intValue: \(intValue))"
}

// Embedded Swift cannot call a protocol extension's method through an existential.
private func _errorPresentationDescription(
    of key: any CodingKey, isFirstInCodingPath isFirst: Bool = true
) -> String {
    if let intValue = key.intValue {
        return "[\(intValue)]"
    }
    return (isFirst ? "" : ".")
        + key.stringValue._escapedForCodingKeyErrorPresentationDescription
}

extension [any CodingKey] {
    fileprivate func _errorPresentationDescription() -> String {
        var description = ""
        for (index, key) in enumerated() {
            description += Swift._errorPresentationDescription(
                of: key, isFirstInCodingPath: index == 0)
        }
        return description
    }
}

extension String {
    // A key with a period, backtick or backslash goes in backticks, with backticks and
    // backslashes escaped, so it cannot be mistaken for the path's delimiters.
    fileprivate var _escapedForCodingKeyErrorPresentationDescription: String {
        guard utf8.contains(where: { $0 == 0x2e || $0 == 0x60 || $0 == 0x5c }) else {
            return self
        }
        var escaped = "`"
        for scalar in unicodeScalars {
            if scalar == "`" || scalar == "\\" {
                escaped.unicodeScalars.append("\\")
            }
            escaped.unicodeScalars.append(scalar)
        }
        return escaped + "`"
    }
}

/// Where a value goes in, or comes from, its encoder's or decoder's container: the whole value
/// of a single-value container, a key of a keyed container, or an element of an unkeyed one (the
/// next one to encode, or the current one to decode).
public enum CodingSlot {
    case value
    case key(any CodingKey)
    case element
}

/// A user-defined key for providing context during encoding and decoding.
public struct CodingUserInfoKey: RawRepresentable, Equatable, Hashable, Sendable {
    public typealias RawValue = String

    public let rawValue: String

    public init?(rawValue: String) {
        self.rawValue = rawValue
    }

    public static func == (lhs: CodingUserInfoKey, rhs: CodingUserInfoKey) -> Bool {
        lhs.rawValue == rhs.rawValue
    }

    public func hash(into hasher: inout Hasher) {
        hasher.combine(rawValue)
    }
}

// MARK: Encoder and Decoder

/// A type that can encode values into a native format for external representation. Every
/// encoder is a subclass of EncoderBase.
public protocol Encoder: EncoderBase {}

/// A type that can decode values from a native format into in-memory representations. Every
/// decoder is a subclass of DecoderBase.
public protocol Decoder: DecoderBase {}

/// The class a format's encoder derives from. An instance encodes one value: the format is told
/// which container the value asked for, then gets each primitive with its slot in that
/// container, and gives out a nested encoder for any other value.
open class EncoderBase: Encoder {
    public init() {}

    open var codingPath: [any CodingKey] { [] }

    open var userInfo: [CodingUserInfoKey: Any] { [:] }

    // Called when the value asks for a keyed or an unkeyed container, before anything is encoded
    // into it. A value may ask for the same kind of container more than once.
    open func beginKeyedContainer() {}
    open func beginUnkeyedContainer() {}

    // The number of elements encoded into the unkeyed container so far.
    open var elementCount: Int { 0 }

    open func encodeNil(at slot: CodingSlot) throws {
        _abstract()
    }

    open func encode(_ value: Bool, at slot: CodingSlot) throws {
        _abstract()
    }

    open func encode(_ value: String, at slot: CodingSlot) throws {
        _abstract()
    }

    open func encode(_ value: Double, at slot: CodingSlot) throws {
        _abstract()
    }

    open func encode(_ value: Float, at slot: CodingSlot) throws {
        try encode(Double(value), at: slot)
    }

    open func encode(_ value: Int64, at slot: CodingSlot) throws {
        _abstract()
    }

    open func encode(_ value: UInt64, at slot: CodingSlot) throws {
        _abstract()
    }

    open func encode(_ value: Int128, at slot: CodingSlot) throws {
        guard let narrow = Int64(exactly: value) else {
            throw EncodingError.invalidValue(value, _context(at: slot, "does not fit in 64 bits"))
        }
        try encode(narrow, at: slot)
    }

    open func encode(_ value: UInt128, at slot: CodingSlot) throws {
        guard let narrow = UInt64(exactly: value) else {
            throw EncodingError.invalidValue(value, _context(at: slot, "does not fit in 64 bits"))
        }
        try encode(narrow, at: slot)
    }

    // An encoder for a value that is not a primitive, or for superEncoder().
    open func nestedEncoder(at slot: CodingSlot) -> EncoderBase {
        _abstract()
    }

    public final func container<Key: CodingKey>(keyedBy type: Key.Type)
        -> KeyedEncodingContainer<Key>
    {
        beginKeyedContainer()
        return KeyedEncodingContainer(encoder: self)
    }

    public final func unkeyedContainer() -> UnkeyedEncodingContainer {
        beginUnkeyedContainer()
        return UnkeyedEncodingContainer(encoder: self)
    }

    public final func singleValueContainer() -> SingleValueEncodingContainer {
        SingleValueEncodingContainer(encoder: self)
    }

    internal final func _encode<T: Encodable>(_ value: T, at slot: CodingSlot) throws {
        try value._encode(into: self, at: slot)
    }

    private final func _context(at slot: CodingSlot, _ problem: String) -> EncodingError.Context {
        EncodingError.Context(
            codingPath: codingPath + slot._keys(elementIndex: elementCount),
            debugDescription: "Integer \(problem).")
    }

    private final func _abstract(_ function: StaticString = #function) -> Never {
        fatalError("\(function) is not implemented by this encoder")
    }
}

/// The class a format's decoder derives from. An instance decodes one value: the format is told
/// which container the value asked for, then is asked for each primitive at its slot in that
/// container, and gives out a nested decoder for any other value.
open class DecoderBase: Decoder {
    public init() {}

    open var codingPath: [any CodingKey] { [] }

    open var userInfo: [CodingUserInfoKey: Any] { [:] }

    // Called when the value asks for a keyed or an unkeyed container. They throw if the value is
    // not one, typically DecodingError.typeMismatch.
    open func beginKeyedContainer() throws {}
    open func beginUnkeyedContainer() throws {}

    // The keyed container's keys, in the format's order.
    open var allKeys: [String] { [] }

    open func contains(_ key: any CodingKey) -> Bool {
        _abstract()
    }

    // The number of elements in the unkeyed container, if known, and the current one's index.
    // .element is always the current element; DecoderBase moves on once it was decoded, so an
    // element that failed to decode can be tried again as something else.
    open var elementCount: Int? { nil }
    open var currentIndex: Int { 0 }

    open var isAtEnd: Bool { currentIndex >= (elementCount ?? 0) }

    open func moveToNextElement() {
        _abstract()
    }

    open func decodeNil(at slot: CodingSlot) throws -> Bool {
        _abstract()
    }

    open func decode(_ type: Bool.Type, at slot: CodingSlot) throws -> Bool {
        _abstract()
    }

    open func decode(_ type: String.Type, at slot: CodingSlot) throws -> String {
        _abstract()
    }

    open func decode(_ type: Double.Type, at slot: CodingSlot) throws -> Double {
        _abstract()
    }

    open func decode(_ type: Float.Type, at slot: CodingSlot) throws -> Float {
        Float(try decode(Double.self, at: slot))
    }

    open func decode(_ type: Int64.Type, at slot: CodingSlot) throws -> Int64 {
        _abstract()
    }

    open func decode(_ type: UInt64.Type, at slot: CodingSlot) throws -> UInt64 {
        _abstract()
    }

    open func decode(_ type: Int128.Type, at slot: CodingSlot) throws -> Int128 {
        Int128(try decode(Int64.self, at: slot))
    }

    open func decode(_ type: UInt128.Type, at slot: CodingSlot) throws -> UInt128 {
        UInt128(try decode(UInt64.self, at: slot))
    }

    // A decoder for a value that is not a primitive, or for superDecoder().
    open func nestedDecoder(at slot: CodingSlot) throws -> DecoderBase {
        _abstract()
    }

    public final func container<Key: CodingKey>(keyedBy type: Key.Type) throws
        -> KeyedDecodingContainer<Key>
    {
        try beginKeyedContainer()
        return KeyedDecodingContainer(decoder: self)
    }

    public final func unkeyedContainer() throws -> UnkeyedDecodingContainer {
        try beginUnkeyedContainer()
        return UnkeyedDecodingContainer(decoder: self)
    }

    public final func singleValueContainer() throws -> SingleValueDecodingContainer {
        SingleValueDecodingContainer(decoder: self)
    }

    internal final func _decode<T: Decodable>(_ type: T.Type, at slot: CodingSlot) throws -> T {
        let value = try T._decode(from: self, at: slot)
        if case .element = slot {
            moveToNextElement()
        }
        return value
    }

    internal final func _nextElementDecoder() throws -> DecoderBase {
        let decoder = try nestedDecoder(at: .element)
        moveToNextElement()
        return decoder
    }

    private final func _abstract(_ function: StaticString = #function) -> Never {
        fatalError("\(function) is not implemented by this decoder")
    }
}

extension CodingSlot {
    fileprivate func _keys(elementIndex: Int) -> [any CodingKey] {
        switch self {
        case .value: []
        case .key(let key): [key]
        case .element: [_GenericIndexKey(intValue: elementIndex)!]
        }
    }
}

internal struct _SuperKey: CodingKey {
    var stringValue: String { "super" }
    var intValue: Int? { nil }
    init() {}
    init?(stringValue: String) {}
    init?(intValue: Int) { return nil }
}

// MARK: Encoding containers

/// A container that encodes values under keys.
public struct KeyedEncodingContainer<K: CodingKey> {
    public typealias Key = K

    internal let encoder: EncoderBase

    public var codingPath: [any CodingKey] { encoder.codingPath }

    public mutating func encodeNil(forKey key: Key) throws {
        try encoder.encodeNil(at: .key(key))
    }

    public mutating func encode<T: Encodable>(_ value: T, forKey key: Key) throws {
        try encoder._encode(value, at: .key(key))
    }

    public mutating func encodeConditional<T: AnyObject & Encodable>(_ object: T, forKey key: Key)
        throws
    {
        try encoder._encode(object, at: .key(key))
    }

    public mutating func encodeIfPresent<T: Encodable>(_ value: T?, forKey key: Key) throws {
        guard let value else { return }
        try encoder._encode(value, at: .key(key))
    }

    public mutating func nestedContainer<NestedKey: CodingKey>(
        keyedBy keyType: NestedKey.Type, forKey key: Key
    ) -> KeyedEncodingContainer<NestedKey> {
        encoder.nestedEncoder(at: .key(key)).container(keyedBy: NestedKey.self)
    }

    public mutating func nestedUnkeyedContainer(forKey key: Key) -> UnkeyedEncodingContainer {
        encoder.nestedEncoder(at: .key(key)).unkeyedContainer()
    }

    public mutating func superEncoder() -> any Encoder {
        encoder.nestedEncoder(at: .key(_SuperKey()))
    }

    public mutating func superEncoder(forKey key: Key) -> any Encoder {
        encoder.nestedEncoder(at: .key(key))
    }
}

/// A container that encodes values one after another.
public struct UnkeyedEncodingContainer {
    internal let encoder: EncoderBase

    public var codingPath: [any CodingKey] { encoder.codingPath }

    public var count: Int { encoder.elementCount }

    public mutating func encodeNil() throws {
        try encoder.encodeNil(at: .element)
    }

    public mutating func encode<T: Encodable>(_ value: T) throws {
        try encoder._encode(value, at: .element)
    }

    public mutating func encodeConditional<T: AnyObject & Encodable>(_ object: T) throws {
        try encoder._encode(object, at: .element)
    }

    public mutating func encode<S: Sequence>(contentsOf sequence: S) throws
    where S.Element: Encodable {
        for element in sequence {
            try encoder._encode(element, at: .element)
        }
    }

    public mutating func nestedContainer<NestedKey: CodingKey>(keyedBy keyType: NestedKey.Type)
        -> KeyedEncodingContainer<NestedKey>
    {
        encoder.nestedEncoder(at: .element).container(keyedBy: NestedKey.self)
    }

    public mutating func nestedUnkeyedContainer() -> UnkeyedEncodingContainer {
        encoder.nestedEncoder(at: .element).unkeyedContainer()
    }

    public mutating func superEncoder() -> any Encoder {
        encoder.nestedEncoder(at: .element)
    }
}

/// A container that encodes a single value.
public struct SingleValueEncodingContainer {
    internal let encoder: EncoderBase

    public var codingPath: [any CodingKey] { encoder.codingPath }

    public mutating func encodeNil() throws {
        try encoder.encodeNil(at: .value)
    }

    public mutating func encode<T: Encodable>(_ value: T) throws {
        try encoder._encode(value, at: .value)
    }
}

// MARK: Decoding containers

/// A container that decodes values by key.
public struct KeyedDecodingContainer<K: CodingKey> {
    public typealias Key = K

    internal let decoder: DecoderBase

    public var codingPath: [any CodingKey] { decoder.codingPath }

    public var allKeys: [Key] {
        decoder.allKeys.compactMap { Key(stringValue: $0) }
    }

    public func contains(_ key: Key) -> Bool {
        decoder.contains(key)
    }

    public func decodeNil(forKey key: Key) throws -> Bool {
        try decoder.decodeNil(at: .key(key))
    }

    public func decode<T: Decodable>(_ type: T.Type, forKey key: Key) throws -> T {
        try decoder._decode(T.self, at: .key(key))
    }

    public func decodeIfPresent<T: Decodable>(_ type: T.Type, forKey key: Key) throws -> T? {
        guard contains(key), !(try decodeNil(forKey: key)) else { return nil }
        return try decoder._decode(T.self, at: .key(key))
    }

    public func nestedContainer<NestedKey: CodingKey>(
        keyedBy type: NestedKey.Type, forKey key: Key
    ) throws -> KeyedDecodingContainer<NestedKey> {
        try decoder.nestedDecoder(at: .key(key)).container(keyedBy: NestedKey.self)
    }

    public func nestedUnkeyedContainer(forKey key: Key) throws -> UnkeyedDecodingContainer {
        try decoder.nestedDecoder(at: .key(key)).unkeyedContainer()
    }

    public func superDecoder() throws -> any Decoder {
        try decoder.nestedDecoder(at: .key(_SuperKey()))
    }

    public func superDecoder(forKey key: Key) throws -> any Decoder {
        try decoder.nestedDecoder(at: .key(key))
    }
}

/// A container that decodes values one after another.
public struct UnkeyedDecodingContainer {
    internal let decoder: DecoderBase

    public var codingPath: [any CodingKey] { decoder.codingPath }

    public var count: Int? { decoder.elementCount }

    public var isAtEnd: Bool { decoder.isAtEnd }

    public var currentIndex: Int { decoder.currentIndex }

    public mutating func decodeNil() throws -> Bool {
        guard try decoder.decodeNil(at: .element) else {
            return false
        }
        decoder.moveToNextElement()
        return true
    }

    public mutating func decode<T: Decodable>(_ type: T.Type) throws -> T {
        try decoder._decode(T.self, at: .element)
    }

    public mutating func decodeIfPresent<T: Decodable>(_ type: T.Type) throws -> T? {
        if try isAtEnd || decodeNil() {
            return nil
        }
        return try decoder._decode(T.self, at: .element)
    }

    public mutating func nestedContainer<NestedKey: CodingKey>(keyedBy type: NestedKey.Type)
        throws -> KeyedDecodingContainer<NestedKey>
    {
        let container = try decoder.nestedDecoder(at: .element).container(keyedBy: NestedKey.self)
        decoder.moveToNextElement()
        return container
    }

    public mutating func nestedUnkeyedContainer() throws -> UnkeyedDecodingContainer {
        let container = try decoder.nestedDecoder(at: .element).unkeyedContainer()
        decoder.moveToNextElement()
        return container
    }

    public mutating func superDecoder() throws -> any Decoder {
        try decoder._nextElementDecoder()
    }
}

/// A container that decodes a single value.
public struct SingleValueDecodingContainer {
    internal let decoder: DecoderBase

    public var codingPath: [any CodingKey] { decoder.codingPath }

    public func decodeNil() -> Bool {
        (try? decoder.decodeNil(at: .value)) ?? false
    }

    public func decode<T: Decodable>(_ type: T.Type) throws -> T {
        try decoder._decode(T.self, at: .value)
    }
}

// MARK: Errors

/// An error that occurs during the encoding of a value.
public enum EncodingError: Error {
    public struct Context: Sendable {
        public let codingPath: [any CodingKey]
        public let debugDescription: String
        public let underlyingError: (any Error)?

        public init(
            codingPath: [any CodingKey], debugDescription: String,
            underlyingError: (any Error)? = nil
        ) {
            self.codingPath = codingPath
            self.debugDescription = debugDescription
            self.underlyingError = underlyingError
        }
    }

    case invalidValue(Any, Context)
}

/// An error that occurs during the decoding of a value.
public enum DecodingError: Error {
    public struct Context: Sendable {
        public let codingPath: [any CodingKey]
        public let debugDescription: String
        public let underlyingError: (any Error)?

        public init(
            codingPath: [any CodingKey], debugDescription: String,
            underlyingError: (any Error)? = nil
        ) {
            self.codingPath = codingPath
            self.debugDescription = debugDescription
            self.underlyingError = underlyingError
        }
    }

    case typeMismatch(Any.Type, Context)
    case valueNotFound(Any.Type, Context)
    case keyNotFound(any CodingKey, Context)
    case dataCorrupted(Context)
}

// Upstream's descriptions also name the value or type involved, and the underlying error, which
// Embedded Swift cannot print. Both errors are also CustomStringConvertible, since Embedded Swift
// interpolates only such values.
extension EncodingError: CustomStringConvertible, CustomDebugStringConvertible {
    public var description: String { debugDescription }

    public var debugDescription: String {
        switch self {
        case .invalidValue(_, let context):
            context._describe("EncodingError.invalidValue")
        }
    }
}

extension DecodingError: CustomStringConvertible, CustomDebugStringConvertible {
    public var description: String { debugDescription }

    public var debugDescription: String {
        switch self {
        case .typeMismatch(_, let context):
            context._describe("DecodingError.typeMismatch")
        case .valueNotFound(_, let context):
            context._describe("DecodingError.valueNotFound")
        case .keyNotFound(let key, let context):
            context._describe(
                "DecodingError.keyNotFound: Key '\(_errorPresentationDescription(of: key))' "
                    + "not found in keyed decoding container")
        case .dataCorrupted(let context):
            context._describe("DecodingError.dataCorrupted: Data was corrupted")
        }
    }
}

extension EncodingError.Context {
    fileprivate func _describe(_ message: String) -> String {
        _describeCodingError(message, codingPath, debugDescription)
    }
}

extension DecodingError.Context {
    fileprivate func _describe(_ message: String) -> String {
        _describeCodingError(message, codingPath, debugDescription)
    }
}

private func _describeCodingError(
    _ message: String, _ codingPath: [any CodingKey], _ debugDescription: String
) -> String {
    var output = message
    if !codingPath.isEmpty {
        output += ". Path: \(codingPath._errorPresentationDescription())"
    }
    if !debugDescription.isEmpty {
        output += ". Debug description: \(debugDescription)"
    }
    return output
}

internal struct _GenericIndexKey: CodingKey, Sendable {
    internal var stringValue: String
    internal var intValue: Int?

    internal init?(stringValue: String) {
        return nil
    }

    internal init?(intValue: Int) {
        self.stringValue = "Index \(intValue)"
        self.intValue = intValue
    }
}

extension DecodingError {
    public static func dataCorruptedError<Key: CodingKey>(
        forKey key: Key, in container: KeyedDecodingContainer<Key>, debugDescription: String
    ) -> DecodingError {
        .dataCorrupted(
            Context(codingPath: container.codingPath + [key], debugDescription: debugDescription))
    }

    public static func dataCorruptedError(
        in container: UnkeyedDecodingContainer, debugDescription: String
    ) -> DecodingError {
        .dataCorrupted(
            Context(
                codingPath: container.codingPath
                    + [_GenericIndexKey(intValue: container.currentIndex)!],
                debugDescription: debugDescription))
    }

    public static func dataCorruptedError(
        in container: SingleValueDecodingContainer, debugDescription: String
    ) -> DecodingError {
        .dataCorrupted(
            Context(codingPath: container.codingPath, debugDescription: debugDescription))
    }
}

// MARK: Primitive conformances

extension Bool: Codable {
    public init(from decoder: any Decoder) throws {
        self = try decoder.decode(Bool.self, at: .value)
    }

    public func encode(to encoder: any Encoder) throws {
        try encoder.encode(self, at: .value)
    }

    public func _encode(into encoder: EncoderBase, at slot: CodingSlot) throws {
        try encoder.encode(self, at: slot)
    }

    public static func _decode(from decoder: DecoderBase, at slot: CodingSlot) throws -> Bool {
        try decoder.decode(Bool.self, at: slot)
    }
}

extension String: Codable {
    public init(from decoder: any Decoder) throws {
        self = try decoder.decode(String.self, at: .value)
    }

    public func encode(to encoder: any Encoder) throws {
        try encoder.encode(self, at: .value)
    }

    public func _encode(into encoder: EncoderBase, at slot: CodingSlot) throws {
        try encoder.encode(self, at: slot)
    }

    public static func _decode(from decoder: DecoderBase, at slot: CodingSlot) throws -> String {
        try decoder.decode(String.self, at: slot)
    }
}

extension Double: Codable {
    public init(from decoder: any Decoder) throws {
        self = try decoder.decode(Double.self, at: .value)
    }

    public func encode(to encoder: any Encoder) throws {
        try encoder.encode(self, at: .value)
    }

    public func _encode(into encoder: EncoderBase, at slot: CodingSlot) throws {
        try encoder.encode(self, at: slot)
    }

    public static func _decode(from decoder: DecoderBase, at slot: CodingSlot) throws -> Double {
        try decoder.decode(Double.self, at: slot)
    }
}

extension Float: Codable {
    public init(from decoder: any Decoder) throws {
        self = try decoder.decode(Float.self, at: .value)
    }

    public func encode(to encoder: any Encoder) throws {
        try encoder.encode(self, at: .value)
    }

    public func _encode(into encoder: EncoderBase, at slot: CodingSlot) throws {
        try encoder.encode(self, at: slot)
    }

    public static func _decode(from decoder: DecoderBase, at slot: CodingSlot) throws -> Float {
        try decoder.decode(Float.self, at: slot)
    }
}

#if !((os(macOS) || targetEnvironment(macCatalyst)) && arch(x86_64))
    extension Float16: Codable {
        public init(from decoder: any Decoder) throws {
            let floatValue = try Float(from: decoder)
            self = Float16(floatValue)
            if isInfinite && floatValue.isFinite {
                throw DecodingError.dataCorrupted(
                    DecodingError.Context(
                        codingPath: decoder.codingPath,
                        debugDescription: "Parsed number \(floatValue) does not fit in Float16."))
            }
        }

        public func encode(to encoder: any Encoder) throws {
            try Float(self).encode(to: encoder)
        }
    }
#endif

extension Int128: Codable {
    public init(from decoder: any Decoder) throws {
        self = try decoder.decode(Int128.self, at: .value)
    }

    public func encode(to encoder: any Encoder) throws {
        try encoder.encode(self, at: .value)
    }

    public func _encode(into encoder: EncoderBase, at slot: CodingSlot) throws {
        try encoder.encode(self, at: slot)
    }

    public static func _decode(from decoder: DecoderBase, at slot: CodingSlot) throws -> Int128 {
        try decoder.decode(Int128.self, at: slot)
    }
}

extension UInt128: Codable {
    public init(from decoder: any Decoder) throws {
        self = try decoder.decode(UInt128.self, at: .value)
    }

    public func encode(to encoder: any Encoder) throws {
        try encoder.encode(self, at: .value)
    }

    public func _encode(into encoder: EncoderBase, at slot: CodingSlot) throws {
        try encoder.encode(self, at: slot)
    }

    public static func _decode(from decoder: DecoderBase, at slot: CodingSlot) throws -> UInt128 {
        try decoder.decode(UInt128.self, at: slot)
    }
}

// The other integers travel as Int64 or UInt64.
extension FixedWidthInteger where Self: Encodable {
    internal func _encodeInteger(into encoder: EncoderBase, at slot: CodingSlot) throws {
        if Self.isSigned {
            try encoder.encode(Int64(self), at: slot)
        } else {
            try encoder.encode(UInt64(self), at: slot)
        }
    }
}

extension FixedWidthInteger where Self: Decodable {
    internal static func _decodeInteger(from decoder: DecoderBase, at slot: CodingSlot) throws
        -> Self
    {
        if Self.isSigned {
            let wide = try decoder.decode(Int64.self, at: slot)
            guard let value = Self(exactly: wide) else {
                throw _integerDoesNotFit(wide.description, "Int", Self.bitWidth, decoder, slot)
            }
            return value
        }
        let wide = try decoder.decode(UInt64.self, at: slot)
        guard let value = Self(exactly: wide) else {
            throw _integerDoesNotFit(wide.description, "UInt", Self.bitWidth, decoder, slot)
        }
        return value
    }
}

// Only Int8 to Int32 and their unsigned forms can overflow, so the name is exact.
private func _integerDoesNotFit(
    _ text: String, _ prefix: String, _ bitWidth: Int, _ decoder: DecoderBase, _ slot: CodingSlot
) -> DecodingError {
    .dataCorrupted(
        DecodingError.Context(
            codingPath: decoder.codingPath + slot._keys(elementIndex: decoder.currentIndex),
            debugDescription: "Parsed number <\(text)> does not fit in \(prefix)\(bitWidth)."))
}

extension Int: Codable {
    public init(from decoder: any Decoder) throws {
        self = try Self._decode(from: decoder, at: .value)
    }

    public func encode(to encoder: any Encoder) throws {
        try _encode(into: encoder, at: .value)
    }

    public func _encode(into encoder: EncoderBase, at slot: CodingSlot) throws {
        try _encodeInteger(into: encoder, at: slot)
    }

    public static func _decode(from decoder: DecoderBase, at slot: CodingSlot) throws -> Self {
        try _decodeInteger(from: decoder, at: slot)
    }
}

extension Int8: Codable {
    public init(from decoder: any Decoder) throws {
        self = try Self._decode(from: decoder, at: .value)
    }

    public func encode(to encoder: any Encoder) throws {
        try _encode(into: encoder, at: .value)
    }

    public func _encode(into encoder: EncoderBase, at slot: CodingSlot) throws {
        try _encodeInteger(into: encoder, at: slot)
    }

    public static func _decode(from decoder: DecoderBase, at slot: CodingSlot) throws -> Self {
        try _decodeInteger(from: decoder, at: slot)
    }
}

extension Int16: Codable {
    public init(from decoder: any Decoder) throws {
        self = try Self._decode(from: decoder, at: .value)
    }

    public func encode(to encoder: any Encoder) throws {
        try _encode(into: encoder, at: .value)
    }

    public func _encode(into encoder: EncoderBase, at slot: CodingSlot) throws {
        try _encodeInteger(into: encoder, at: slot)
    }

    public static func _decode(from decoder: DecoderBase, at slot: CodingSlot) throws -> Self {
        try _decodeInteger(from: decoder, at: slot)
    }
}

extension Int32: Codable {
    public init(from decoder: any Decoder) throws {
        self = try Self._decode(from: decoder, at: .value)
    }

    public func encode(to encoder: any Encoder) throws {
        try _encode(into: encoder, at: .value)
    }

    public func _encode(into encoder: EncoderBase, at slot: CodingSlot) throws {
        try _encodeInteger(into: encoder, at: slot)
    }

    public static func _decode(from decoder: DecoderBase, at slot: CodingSlot) throws -> Self {
        try _decodeInteger(from: decoder, at: slot)
    }
}

extension Int64: Codable {
    public init(from decoder: any Decoder) throws {
        self = try Self._decode(from: decoder, at: .value)
    }

    public func encode(to encoder: any Encoder) throws {
        try _encode(into: encoder, at: .value)
    }

    public func _encode(into encoder: EncoderBase, at slot: CodingSlot) throws {
        try _encodeInteger(into: encoder, at: slot)
    }

    public static func _decode(from decoder: DecoderBase, at slot: CodingSlot) throws -> Self {
        try _decodeInteger(from: decoder, at: slot)
    }
}

extension UInt: Codable {
    public init(from decoder: any Decoder) throws {
        self = try Self._decode(from: decoder, at: .value)
    }

    public func encode(to encoder: any Encoder) throws {
        try _encode(into: encoder, at: .value)
    }

    public func _encode(into encoder: EncoderBase, at slot: CodingSlot) throws {
        try _encodeInteger(into: encoder, at: slot)
    }

    public static func _decode(from decoder: DecoderBase, at slot: CodingSlot) throws -> Self {
        try _decodeInteger(from: decoder, at: slot)
    }
}

extension UInt8: Codable {
    public init(from decoder: any Decoder) throws {
        self = try Self._decode(from: decoder, at: .value)
    }

    public func encode(to encoder: any Encoder) throws {
        try _encode(into: encoder, at: .value)
    }

    public func _encode(into encoder: EncoderBase, at slot: CodingSlot) throws {
        try _encodeInteger(into: encoder, at: slot)
    }

    public static func _decode(from decoder: DecoderBase, at slot: CodingSlot) throws -> Self {
        try _decodeInteger(from: decoder, at: slot)
    }
}

extension UInt16: Codable {
    public init(from decoder: any Decoder) throws {
        self = try Self._decode(from: decoder, at: .value)
    }

    public func encode(to encoder: any Encoder) throws {
        try _encode(into: encoder, at: .value)
    }

    public func _encode(into encoder: EncoderBase, at slot: CodingSlot) throws {
        try _encodeInteger(into: encoder, at: slot)
    }

    public static func _decode(from decoder: DecoderBase, at slot: CodingSlot) throws -> Self {
        try _decodeInteger(from: decoder, at: slot)
    }
}

extension UInt32: Codable {
    public init(from decoder: any Decoder) throws {
        self = try Self._decode(from: decoder, at: .value)
    }

    public func encode(to encoder: any Encoder) throws {
        try _encode(into: encoder, at: .value)
    }

    public func _encode(into encoder: EncoderBase, at slot: CodingSlot) throws {
        try _encodeInteger(into: encoder, at: slot)
    }

    public static func _decode(from decoder: DecoderBase, at slot: CodingSlot) throws -> Self {
        try _decodeInteger(from: decoder, at: slot)
    }
}

extension UInt64: Codable {
    public init(from decoder: any Decoder) throws {
        self = try Self._decode(from: decoder, at: .value)
    }

    public func encode(to encoder: any Encoder) throws {
        try _encode(into: encoder, at: .value)
    }

    public func _encode(into encoder: EncoderBase, at slot: CodingSlot) throws {
        try _encodeInteger(into: encoder, at: slot)
    }

    public static func _decode(from decoder: DecoderBase, at slot: CodingSlot) throws -> Self {
        try _decodeInteger(from: decoder, at: slot)
    }
}

// MARK: Raw-value enums

// As upstream, one pair of extensions per primitive raw type, so other RawRepresentable types
// still get synthesized conformances.
extension RawRepresentable where RawValue: Encodable {
    internal func _encodeRawValue(to encoder: any Encoder) throws {
        var container = encoder.singleValueContainer()
        try container.encode(rawValue)
    }
}

extension RawRepresentable where RawValue: Decodable & CustomStringConvertible {
    internal init(_decodingRawValueFrom decoder: any Decoder, typeName: StaticString) throws {
        let decoded = try decoder.singleValueContainer().decode(RawValue.self)
        guard let value = Self(rawValue: decoded) else {
            throw DecodingError.dataCorrupted(
                DecodingError.Context(
                    codingPath: decoder.codingPath,
                    debugDescription:
                        "Cannot initialize from invalid \(typeName) value \(decoded)"))
        }
        self = value
    }
}

extension RawRepresentable<Bool> where Self: Encodable {
    public func encode(to encoder: any Encoder) throws {
        try _encodeRawValue(to: encoder)
    }
}

extension RawRepresentable<Bool> where Self: Decodable {
    public init(from decoder: any Decoder) throws {
        try self.init(_decodingRawValueFrom: decoder, typeName: "Bool")
    }
}

extension RawRepresentable<String> where Self: Encodable {
    public func encode(to encoder: any Encoder) throws {
        try _encodeRawValue(to: encoder)
    }
}

extension RawRepresentable<String> where Self: Decodable {
    public init(from decoder: any Decoder) throws {
        try self.init(_decodingRawValueFrom: decoder, typeName: "String")
    }
}

extension RawRepresentable<Double> where Self: Encodable {
    public func encode(to encoder: any Encoder) throws {
        try _encodeRawValue(to: encoder)
    }
}

extension RawRepresentable<Double> where Self: Decodable {
    public init(from decoder: any Decoder) throws {
        try self.init(_decodingRawValueFrom: decoder, typeName: "Double")
    }
}

extension RawRepresentable<Float> where Self: Encodable {
    public func encode(to encoder: any Encoder) throws {
        try _encodeRawValue(to: encoder)
    }
}

extension RawRepresentable<Float> where Self: Decodable {
    public init(from decoder: any Decoder) throws {
        try self.init(_decodingRawValueFrom: decoder, typeName: "Float")
    }
}

extension RawRepresentable<Int> where Self: Encodable {
    public func encode(to encoder: any Encoder) throws {
        try _encodeRawValue(to: encoder)
    }
}

extension RawRepresentable<Int> where Self: Decodable {
    public init(from decoder: any Decoder) throws {
        try self.init(_decodingRawValueFrom: decoder, typeName: "Int")
    }
}

extension RawRepresentable<Int8> where Self: Encodable {
    public func encode(to encoder: any Encoder) throws {
        try _encodeRawValue(to: encoder)
    }
}

extension RawRepresentable<Int8> where Self: Decodable {
    public init(from decoder: any Decoder) throws {
        try self.init(_decodingRawValueFrom: decoder, typeName: "Int8")
    }
}

extension RawRepresentable<Int16> where Self: Encodable {
    public func encode(to encoder: any Encoder) throws {
        try _encodeRawValue(to: encoder)
    }
}

extension RawRepresentable<Int16> where Self: Decodable {
    public init(from decoder: any Decoder) throws {
        try self.init(_decodingRawValueFrom: decoder, typeName: "Int16")
    }
}

extension RawRepresentable<Int32> where Self: Encodable {
    public func encode(to encoder: any Encoder) throws {
        try _encodeRawValue(to: encoder)
    }
}

extension RawRepresentable<Int32> where Self: Decodable {
    public init(from decoder: any Decoder) throws {
        try self.init(_decodingRawValueFrom: decoder, typeName: "Int32")
    }
}

extension RawRepresentable<Int64> where Self: Encodable {
    public func encode(to encoder: any Encoder) throws {
        try _encodeRawValue(to: encoder)
    }
}

extension RawRepresentable<Int64> where Self: Decodable {
    public init(from decoder: any Decoder) throws {
        try self.init(_decodingRawValueFrom: decoder, typeName: "Int64")
    }
}

extension RawRepresentable<Int128> where Self: Encodable {
    public func encode(to encoder: any Encoder) throws {
        try _encodeRawValue(to: encoder)
    }
}

extension RawRepresentable<Int128> where Self: Decodable {
    public init(from decoder: any Decoder) throws {
        try self.init(_decodingRawValueFrom: decoder, typeName: "Int128")
    }
}

extension RawRepresentable<UInt> where Self: Encodable {
    public func encode(to encoder: any Encoder) throws {
        try _encodeRawValue(to: encoder)
    }
}

extension RawRepresentable<UInt> where Self: Decodable {
    public init(from decoder: any Decoder) throws {
        try self.init(_decodingRawValueFrom: decoder, typeName: "UInt")
    }
}

extension RawRepresentable<UInt8> where Self: Encodable {
    public func encode(to encoder: any Encoder) throws {
        try _encodeRawValue(to: encoder)
    }
}

extension RawRepresentable<UInt8> where Self: Decodable {
    public init(from decoder: any Decoder) throws {
        try self.init(_decodingRawValueFrom: decoder, typeName: "UInt8")
    }
}

extension RawRepresentable<UInt16> where Self: Encodable {
    public func encode(to encoder: any Encoder) throws {
        try _encodeRawValue(to: encoder)
    }
}

extension RawRepresentable<UInt16> where Self: Decodable {
    public init(from decoder: any Decoder) throws {
        try self.init(_decodingRawValueFrom: decoder, typeName: "UInt16")
    }
}

extension RawRepresentable<UInt32> where Self: Encodable {
    public func encode(to encoder: any Encoder) throws {
        try _encodeRawValue(to: encoder)
    }
}

extension RawRepresentable<UInt32> where Self: Decodable {
    public init(from decoder: any Decoder) throws {
        try self.init(_decodingRawValueFrom: decoder, typeName: "UInt32")
    }
}

extension RawRepresentable<UInt64> where Self: Encodable {
    public func encode(to encoder: any Encoder) throws {
        try _encodeRawValue(to: encoder)
    }
}

extension RawRepresentable<UInt64> where Self: Decodable {
    public init(from decoder: any Decoder) throws {
        try self.init(_decodingRawValueFrom: decoder, typeName: "UInt64")
    }
}

extension RawRepresentable<UInt128> where Self: Encodable {
    public func encode(to encoder: any Encoder) throws {
        try _encodeRawValue(to: encoder)
    }
}

extension RawRepresentable<UInt128> where Self: Decodable {
    public init(from decoder: any Decoder) throws {
        try self.init(_decodingRawValueFrom: decoder, typeName: "UInt128")
    }
}

// MARK: Optionals and collections

extension Optional: Encodable where Wrapped: Encodable {
    public func encode(to encoder: any Encoder) throws {
        var container = encoder.singleValueContainer()
        switch self {
        case .none: try container.encodeNil()
        case .some(let wrapped): try container.encode(wrapped)
        }
    }

    public func _encode(into encoder: EncoderBase, at slot: CodingSlot) throws {
        switch self {
        case .none: try encoder.encodeNil(at: slot)
        case .some(let wrapped): try wrapped._encode(into: encoder, at: slot)
        }
    }
}

extension Optional: Decodable where Wrapped: Decodable {
    public init(from decoder: any Decoder) throws {
        let container = try decoder.singleValueContainer()
        if container.decodeNil() {
            self = .none
        } else {
            self = .some(try container.decode(Wrapped.self))
        }
    }

    public static func _decode(from decoder: DecoderBase, at slot: CodingSlot) throws -> Self {
        if try decoder.decodeNil(at: slot) {
            return .none
        }
        return .some(try Wrapped._decode(from: decoder, at: slot))
    }
}

extension Array: Encodable where Element: Encodable {
    public func encode(to encoder: any Encoder) throws {
        var container = encoder.unkeyedContainer()
        for element in self {
            try container.encode(element)
        }
    }
}

extension Array: Decodable where Element: Decodable {
    public init(from decoder: any Decoder) throws {
        self.init()
        var container = try decoder.unkeyedContainer()
        if let count = container.count {
            reserveCapacity(count)
        }
        while !container.isAtEnd {
            append(try container.decode(Element.self))
        }
    }
}

extension ContiguousArray: Encodable where Element: Encodable {
    public func encode(to encoder: any Encoder) throws {
        var container = encoder.unkeyedContainer()
        for element in self {
            try container.encode(element)
        }
    }
}

extension ContiguousArray: Decodable where Element: Decodable {
    public init(from decoder: any Decoder) throws {
        self.init()
        var container = try decoder.unkeyedContainer()
        while !container.isAtEnd {
            append(try container.decode(Element.self))
        }
    }
}

extension Set: Encodable where Element: Encodable {
    public func encode(to encoder: any Encoder) throws {
        var container = encoder.unkeyedContainer()
        for element in self {
            try container.encode(element)
        }
    }
}

extension Set: Decodable where Element: Decodable {
    public init(from decoder: any Decoder) throws {
        self.init()
        var container = try decoder.unkeyedContainer()
        while !container.isAtEnd {
            insert(try container.decode(Element.self))
        }
    }
}

// MARK: Dictionaries

public struct _DictionaryCodingKey: CodingKey {
    public let stringValue: String
    public let intValue: Int?

    public init(stringValue: String) {
        self.stringValue = stringValue
        self.intValue = Int(stringValue)
    }

    public init(intValue: Int) {
        self.stringValue = intValue.description
        self.intValue = intValue
    }

    internal init(codingKey: any CodingKey) {
        self.stringValue = codingKey.stringValue
        self.intValue = codingKey.intValue
    }
}

/// A type that can be converted to and from a coding key, so that a dictionary keyed by it is
/// encoded with a keyed container.
public protocol CodingKeyRepresentable {
    var codingKey: any CodingKey { get }
    init?<T: CodingKey>(codingKey: T)
}

extension RawRepresentable where Self: CodingKeyRepresentable, RawValue == String {
    public var codingKey: any CodingKey {
        _DictionaryCodingKey(stringValue: rawValue)
    }

    public init?<T: CodingKey>(codingKey: T) {
        self.init(rawValue: codingKey.stringValue)
    }
}

extension RawRepresentable where Self: CodingKeyRepresentable, RawValue == Int {
    public var codingKey: any CodingKey {
        _DictionaryCodingKey(intValue: rawValue)
    }

    public init?<T: CodingKey>(codingKey: T) {
        guard let intValue = codingKey.intValue else { return nil }
        self.init(rawValue: intValue)
    }
}

extension Int: CodingKeyRepresentable {
    public var codingKey: any CodingKey {
        _DictionaryCodingKey(intValue: self)
    }

    public init?<T: CodingKey>(codingKey: T) {
        guard let intValue = codingKey.intValue else { return nil }
        self = intValue
    }
}

extension String: CodingKeyRepresentable {
    public var codingKey: any CodingKey {
        _DictionaryCodingKey(stringValue: self)
    }

    public init?<T: CodingKey>(codingKey: T) {
        self = codingKey.stringValue
    }
}

extension Encodable where Self: CodingKeyRepresentable {
    public static var _encodesAsCodingKey: Bool { true }

    public var _dictionaryCodingKey: (any CodingKey)? { codingKey }
}

extension Decodable where Self: CodingKeyRepresentable {
    public static var _decodesFromCodingKey: Bool { true }

    public static func _init(dictionaryCodingKey key: _DictionaryCodingKey) -> Self? {
        Self(codingKey: key)
    }
}

// Keys that are coding-key representable, as String and Int are, become a keyed container's keys;
// other keys alternate with their values in an unkeyed container.
extension Dictionary: Encodable where Key: Encodable, Value: Encodable {
    public func encode(to encoder: any Encoder) throws {
        if Key._encodesAsCodingKey {
            var container = encoder.container(keyedBy: _DictionaryCodingKey.self)
            for (key, value) in self {
                try container.encode(
                    value, forKey: _DictionaryCodingKey(codingKey: key._dictionaryCodingKey!))
            }
        } else {
            var container = encoder.unkeyedContainer()
            for (key, value) in self {
                try container.encode(key)
                try container.encode(value)
            }
        }
    }
}

extension Dictionary: Decodable where Key: Decodable, Value: Decodable {
    public init(from decoder: any Decoder) throws {
        self.init()
        if Key._decodesFromCodingKey {
            let container = try decoder.container(keyedBy: _DictionaryCodingKey.self)
            for codingKey in container.allKeys {
                guard let key = Key._init(dictionaryCodingKey: codingKey) else {
                    throw DecodingError.dataCorruptedError(
                        forKey: codingKey, in: container,
                        debugDescription: "Could not convert key to the dictionary's key type")
                }
                self[key] = try container.decode(Value.self, forKey: codingKey)
            }
            return
        }
        var container = try decoder.unkeyedContainer()
        if let count = container.count, count % 2 != 0 {
            throw DecodingError.dataCorrupted(
                DecodingError.Context(
                    codingPath: decoder.codingPath,
                    debugDescription: "Expected collection of key-value pairs; "
                        + "encountered odd-length array instead."))
        }
        while !container.isAtEnd {
            let key = try container.decode(Key.self)
            guard !container.isAtEnd else {
                throw DecodingError.dataCorrupted(
                    DecodingError.Context(
                        codingPath: decoder.codingPath,
                        debugDescription:
                            "Unkeyed container reached end before value in key-value pair."))
            }
            self[key] = try container.decode(Value.self)
        }
    }
}

// MARK: SIMD vectors

// Upstream makes Codable a requirement of SIMD itself, outside Embedded Swift.
extension SIMD where Scalar: Encodable, Self: Encodable {
    public func encode(to encoder: any Encoder) throws {
        var container = encoder.unkeyedContainer()
        for i in indices {
            try container.encode(self[i])
        }
    }
}

extension SIMD where Scalar: Decodable, Self: Decodable {
    public init(from decoder: any Decoder) throws {
        self.init()
        var container = try decoder.unkeyedContainer()
        guard container.count == scalarCount else {
            throw DecodingError.dataCorrupted(
                DecodingError.Context(
                    codingPath: decoder.codingPath,
                    debugDescription: "Expected vector with exactly \(scalarCount) elements."))
        }
        for i in indices {
            self[i] = try container.decode(Scalar.self)
        }
    }
}

extension SIMD2: Codable where Scalar: Codable {}
extension SIMD3: Codable where Scalar: Codable {}
extension SIMD4: Codable where Scalar: Codable {}
extension SIMD8: Codable where Scalar: Codable {}
extension SIMD16: Codable where Scalar: Codable {}
extension SIMD32: Codable where Scalar: Codable {}
extension SIMD64: Codable where Scalar: Codable {}
