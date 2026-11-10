// In Embedded Swift a format subclasses the standard library's EncoderBase or DecoderBase, from
// src/user/swift_stdlib/codable.swift. Full Swift's library has upstream's Encoder and Decoder
// protocols instead, so here the same two classes are FeatherSupport's own and conform to them,
// and the formats compile unchanged in both modes.
#if !$Embedded

    enum CodingSlot {
        case value
        case key(any CodingKey)
        case element
    }

    extension CodingSlot {
        fileprivate func keys(elementIndex: Int) -> [any CodingKey] {
            switch self {
            case .value: []
            case .key(let key): [key]
            case .element: [IndexKey(intValue: elementIndex)]
            }
        }
    }

    private struct IndexKey: CodingKey {
        let intValue: Int?

        init(intValue: Int) {
            self.intValue = intValue
        }

        init?(stringValue: String) {
            return nil
        }

        var stringValue: String { "Index \(intValue!)" }
    }

    private struct SuperKey: CodingKey {
        var stringValue: String { "super" }
        var intValue: Int? { nil }
        init() {}
        init?(stringValue: String) {}
        init?(intValue: Int) { return nil }
    }

    class EncoderBase: Encoder {
        init() {}

        var codingPath: [any CodingKey] { [] }

        var userInfo: [CodingUserInfoKey: Any] { [:] }

        func beginKeyedContainer() {}
        func beginUnkeyedContainer() {}

        var elementCount: Int { 0 }

        func encodeNil(at slot: CodingSlot) throws {
            abstract()
        }

        func encode(_ value: Bool, at slot: CodingSlot) throws {
            abstract()
        }

        func encode(_ value: String, at slot: CodingSlot) throws {
            abstract()
        }

        func encode(_ value: Double, at slot: CodingSlot) throws {
            abstract()
        }

        func encode(_ value: Float, at slot: CodingSlot) throws {
            try encode(Double(value), at: slot)
        }

        func encode(_ value: Int64, at slot: CodingSlot) throws {
            abstract()
        }

        func encode(_ value: UInt64, at slot: CodingSlot) throws {
            abstract()
        }

        func encode(_ value: Int128, at slot: CodingSlot) throws {
            guard let narrow = Int64(exactly: value) else {
                throw EncodingError.invalidValue(value, context(at: slot))
            }
            try encode(narrow, at: slot)
        }

        func encode(_ value: UInt128, at slot: CodingSlot) throws {
            guard let narrow = UInt64(exactly: value) else {
                throw EncodingError.invalidValue(value, context(at: slot))
            }
            try encode(narrow, at: slot)
        }

        func nestedEncoder(at slot: CodingSlot) -> EncoderBase {
            abstract()
        }

        final func container<Key: CodingKey>(keyedBy type: Key.Type)
            -> KeyedEncodingContainer<Key>
        {
            beginKeyedContainer()
            return KeyedEncodingContainer(KeyedSlotEncoder<Key>(encoder: self))
        }

        final func unkeyedContainer() -> any UnkeyedEncodingContainer {
            beginUnkeyedContainer()
            return UnkeyedSlotEncoder(encoder: self)
        }

        final func singleValueContainer() -> any SingleValueEncodingContainer {
            SingleSlotEncoder(encoder: self)
        }

        // Primitives go to the format without a nested encoder, as in Embedded Swift.
        final func encodeValue<T: Encodable>(_ value: T, at slot: CodingSlot) throws {
            if let primitive = value as? any SlotPrimitive {
                try primitive.encodePrimitive(into: self, at: slot)
            } else {
                try value.encode(to: nestedEncoder(at: slot))
            }
        }

        private final func context(at slot: CodingSlot) -> EncodingError.Context {
            EncodingError.Context(
                codingPath: codingPath + slot.keys(elementIndex: elementCount),
                debugDescription: "Integer does not fit in 64 bits.")
        }

        private final func abstract(_ function: StaticString = #function) -> Never {
            fatalError("\(function) is not implemented by this encoder")
        }
    }

    class DecoderBase: Decoder {
        init() {}

        var codingPath: [any CodingKey] { [] }

        var userInfo: [CodingUserInfoKey: Any] { [:] }

        func beginKeyedContainer() throws {}
        func beginUnkeyedContainer() throws {}

        var allKeys: [String] { [] }

        func contains(_ key: any CodingKey) -> Bool {
            abstract()
        }

        // .element is always the current element; it is moved past once decoded, so an element
        // that failed to decode can be tried again as something else.
        var elementCount: Int? { nil }
        var currentIndex: Int { 0 }

        var isAtEnd: Bool { currentIndex >= (elementCount ?? 0) }

        func moveToNextElement() {
            abstract()
        }

        func decodeNil(at slot: CodingSlot) throws -> Bool {
            abstract()
        }

        func decode(_ type: Bool.Type, at slot: CodingSlot) throws -> Bool {
            abstract()
        }

        func decode(_ type: String.Type, at slot: CodingSlot) throws -> String {
            abstract()
        }

        func decode(_ type: Double.Type, at slot: CodingSlot) throws -> Double {
            abstract()
        }

        func decode(_ type: Float.Type, at slot: CodingSlot) throws -> Float {
            Float(try decode(Double.self, at: slot))
        }

        func decode(_ type: Int64.Type, at slot: CodingSlot) throws -> Int64 {
            abstract()
        }

        func decode(_ type: UInt64.Type, at slot: CodingSlot) throws -> UInt64 {
            abstract()
        }

        func decode(_ type: Int128.Type, at slot: CodingSlot) throws -> Int128 {
            Int128(try decode(Int64.self, at: slot))
        }

        func decode(_ type: UInt128.Type, at slot: CodingSlot) throws -> UInt128 {
            UInt128(try decode(UInt64.self, at: slot))
        }

        func nestedDecoder(at slot: CodingSlot) throws -> DecoderBase {
            abstract()
        }

        final func container<Key: CodingKey>(keyedBy type: Key.Type) throws
            -> KeyedDecodingContainer<Key>
        {
            try beginKeyedContainer()
            return KeyedDecodingContainer(KeyedSlotDecoder<Key>(decoder: self))
        }

        final func unkeyedContainer() throws -> any UnkeyedDecodingContainer {
            try beginUnkeyedContainer()
            return UnkeyedSlotDecoder(decoder: self)
        }

        final func singleValueContainer() throws -> any SingleValueDecodingContainer {
            SingleSlotDecoder(decoder: self)
        }

        final func decodeValue<T: Decodable>(_ type: T.Type, at slot: CodingSlot) throws -> T {
            let value: T
            if let primitive = T.self as? any SlotPrimitive.Type {
                value = try primitive.decodePrimitive(from: self, at: slot) as! T
            } else {
                value = try T(from: nestedDecoder(at: slot))
            }
            if case .element = slot {
                moveToNextElement()
            }
            return value
        }

        final func nextElementDecoder() throws -> DecoderBase {
            let decoder = try nestedDecoder(at: .element)
            moveToNextElement()
            return decoder
        }

        // Only Int8 to Int32 and their unsigned forms can overflow, so the name is exact.
        final func integerDoesNotFit(_ text: String, _ name: String, at slot: CodingSlot)
            -> DecodingError
        {
            .dataCorrupted(
                DecodingError.Context(
                    codingPath: codingPath + slot.keys(elementIndex: currentIndex),
                    debugDescription: "Parsed number <\(text)> does not fit in \(name)."))
        }

        private final func abstract(_ function: StaticString = #function) -> Never {
            fatalError("\(function) is not implemented by this decoder")
        }
    }

    // MARK: Primitives

    protocol SlotPrimitive {
        func encodePrimitive(into encoder: EncoderBase, at slot: CodingSlot) throws
        static func decodePrimitive(from decoder: DecoderBase, at slot: CodingSlot) throws -> Self
    }

    extension Bool: SlotPrimitive {
        func encodePrimitive(into encoder: EncoderBase, at slot: CodingSlot) throws {
            try encoder.encode(self, at: slot)
        }

        static func decodePrimitive(from decoder: DecoderBase, at slot: CodingSlot) throws -> Self {
            try decoder.decode(Self.self, at: slot)
        }
    }

    extension String: SlotPrimitive {
        func encodePrimitive(into encoder: EncoderBase, at slot: CodingSlot) throws {
            try encoder.encode(self, at: slot)
        }

        static func decodePrimitive(from decoder: DecoderBase, at slot: CodingSlot) throws -> Self {
            try decoder.decode(Self.self, at: slot)
        }
    }

    extension Double: SlotPrimitive {
        func encodePrimitive(into encoder: EncoderBase, at slot: CodingSlot) throws {
            try encoder.encode(self, at: slot)
        }

        static func decodePrimitive(from decoder: DecoderBase, at slot: CodingSlot) throws -> Self {
            try decoder.decode(Self.self, at: slot)
        }
    }

    extension Float: SlotPrimitive {
        func encodePrimitive(into encoder: EncoderBase, at slot: CodingSlot) throws {
            try encoder.encode(self, at: slot)
        }

        static func decodePrimitive(from decoder: DecoderBase, at slot: CodingSlot) throws -> Self {
            try decoder.decode(Self.self, at: slot)
        }
    }

    extension Int128: SlotPrimitive {
        func encodePrimitive(into encoder: EncoderBase, at slot: CodingSlot) throws {
            try encoder.encode(self, at: slot)
        }

        static func decodePrimitive(from decoder: DecoderBase, at slot: CodingSlot) throws -> Self {
            try decoder.decode(Self.self, at: slot)
        }
    }

    extension UInt128: SlotPrimitive {
        func encodePrimitive(into encoder: EncoderBase, at slot: CodingSlot) throws {
            try encoder.encode(self, at: slot)
        }

        static func decodePrimitive(from decoder: DecoderBase, at slot: CodingSlot) throws -> Self {
            try decoder.decode(Self.self, at: slot)
        }
    }

    // The other integers travel as Int64 or UInt64.
    protocol SlotInteger: FixedWidthInteger, SlotPrimitive {}

    extension SlotInteger {
        func encodePrimitive(into encoder: EncoderBase, at slot: CodingSlot) throws {
            if Self.isSigned {
                try encoder.encode(Int64(self), at: slot)
            } else {
                try encoder.encode(UInt64(self), at: slot)
            }
        }

        static func decodePrimitive(from decoder: DecoderBase, at slot: CodingSlot) throws -> Self {
            do {
                if Self.isSigned {
                    let wide = try decoder.decode(Int64.self, at: slot)
                    guard let value = Self(exactly: wide) else {
                        throw decoder.integerDoesNotFit(
                            wide.description, "Int\(bitWidth)", at: slot)
                    }
                    return value
                }
                let wide = try decoder.decode(UInt64.self, at: slot)
                guard let value = Self(exactly: wide) else {
                    throw decoder.integerDoesNotFit(wide.description, "UInt\(bitWidth)", at: slot)
                }
                return value
            } catch DecodingError.typeMismatch(_, let context) {
                // The format was asked for the wide type, but the error names the one decoded.
                throw DecodingError.typeMismatch(Self.self, context)
            } catch DecodingError.valueNotFound(_, let context) {
                throw DecodingError.valueNotFound(Self.self, context)
            }
        }
    }

    extension Int: SlotInteger {}
    extension Int8: SlotInteger {}
    extension Int16: SlotInteger {}
    extension Int32: SlotInteger {}
    extension Int64: SlotInteger {}
    extension UInt: SlotInteger {}
    extension UInt8: SlotInteger {}
    extension UInt16: SlotInteger {}
    extension UInt32: SlotInteger {}
    extension UInt64: SlotInteger {}

    // MARK: Encoding containers

    private struct KeyedSlotEncoder<Key: CodingKey>: KeyedEncodingContainerProtocol {
        let encoder: EncoderBase

        var codingPath: [any CodingKey] { encoder.codingPath }

        mutating func encodeNil(forKey key: Key) throws {
            try encoder.encodeNil(at: .key(key))
        }

        mutating func encode(_ value: Bool, forKey key: Key) throws {
            try encoder.encodeValue(value, at: .key(key))
        }

        mutating func encode(_ value: String, forKey key: Key) throws {
            try encoder.encodeValue(value, at: .key(key))
        }

        mutating func encode(_ value: Double, forKey key: Key) throws {
            try encoder.encodeValue(value, at: .key(key))
        }

        mutating func encode(_ value: Float, forKey key: Key) throws {
            try encoder.encodeValue(value, at: .key(key))
        }

        mutating func encode(_ value: Int, forKey key: Key) throws {
            try encoder.encodeValue(value, at: .key(key))
        }

        mutating func encode(_ value: Int8, forKey key: Key) throws {
            try encoder.encodeValue(value, at: .key(key))
        }

        mutating func encode(_ value: Int16, forKey key: Key) throws {
            try encoder.encodeValue(value, at: .key(key))
        }

        mutating func encode(_ value: Int32, forKey key: Key) throws {
            try encoder.encodeValue(value, at: .key(key))
        }

        mutating func encode(_ value: Int64, forKey key: Key) throws {
            try encoder.encodeValue(value, at: .key(key))
        }

        mutating func encode(_ value: Int128, forKey key: Key) throws {
            try encoder.encodeValue(value, at: .key(key))
        }

        mutating func encode(_ value: UInt, forKey key: Key) throws {
            try encoder.encodeValue(value, at: .key(key))
        }

        mutating func encode(_ value: UInt8, forKey key: Key) throws {
            try encoder.encodeValue(value, at: .key(key))
        }

        mutating func encode(_ value: UInt16, forKey key: Key) throws {
            try encoder.encodeValue(value, at: .key(key))
        }

        mutating func encode(_ value: UInt32, forKey key: Key) throws {
            try encoder.encodeValue(value, at: .key(key))
        }

        mutating func encode(_ value: UInt64, forKey key: Key) throws {
            try encoder.encodeValue(value, at: .key(key))
        }

        mutating func encode(_ value: UInt128, forKey key: Key) throws {
            try encoder.encodeValue(value, at: .key(key))
        }

        mutating func encode<T: Encodable>(_ value: T, forKey key: Key) throws {
            try encoder.encodeValue(value, at: .key(key))
        }

        mutating func nestedContainer<NestedKey: CodingKey>(
            keyedBy keyType: NestedKey.Type, forKey key: Key
        ) -> KeyedEncodingContainer<NestedKey> {
            encoder.nestedEncoder(at: .key(key)).container(keyedBy: NestedKey.self)
        }

        mutating func nestedUnkeyedContainer(forKey key: Key) -> any UnkeyedEncodingContainer {
            encoder.nestedEncoder(at: .key(key)).unkeyedContainer()
        }

        mutating func superEncoder() -> any Encoder {
            encoder.nestedEncoder(at: .key(SuperKey()))
        }

        mutating func superEncoder(forKey key: Key) -> any Encoder {
            encoder.nestedEncoder(at: .key(key))
        }
    }

    private struct UnkeyedSlotEncoder: UnkeyedEncodingContainer {
        let encoder: EncoderBase

        var codingPath: [any CodingKey] { encoder.codingPath }

        var count: Int { encoder.elementCount }

        mutating func encodeNil() throws {
            try encoder.encodeNil(at: .element)
        }

        mutating func encode(_ value: Bool) throws {
            try encoder.encodeValue(value, at: .element)
        }

        mutating func encode(_ value: String) throws {
            try encoder.encodeValue(value, at: .element)
        }

        mutating func encode(_ value: Double) throws {
            try encoder.encodeValue(value, at: .element)
        }

        mutating func encode(_ value: Float) throws {
            try encoder.encodeValue(value, at: .element)
        }

        mutating func encode(_ value: Int) throws {
            try encoder.encodeValue(value, at: .element)
        }

        mutating func encode(_ value: Int8) throws {
            try encoder.encodeValue(value, at: .element)
        }

        mutating func encode(_ value: Int16) throws {
            try encoder.encodeValue(value, at: .element)
        }

        mutating func encode(_ value: Int32) throws {
            try encoder.encodeValue(value, at: .element)
        }

        mutating func encode(_ value: Int64) throws {
            try encoder.encodeValue(value, at: .element)
        }

        mutating func encode(_ value: Int128) throws {
            try encoder.encodeValue(value, at: .element)
        }

        mutating func encode(_ value: UInt) throws {
            try encoder.encodeValue(value, at: .element)
        }

        mutating func encode(_ value: UInt8) throws {
            try encoder.encodeValue(value, at: .element)
        }

        mutating func encode(_ value: UInt16) throws {
            try encoder.encodeValue(value, at: .element)
        }

        mutating func encode(_ value: UInt32) throws {
            try encoder.encodeValue(value, at: .element)
        }

        mutating func encode(_ value: UInt64) throws {
            try encoder.encodeValue(value, at: .element)
        }

        mutating func encode(_ value: UInt128) throws {
            try encoder.encodeValue(value, at: .element)
        }

        mutating func encode<T: Encodable>(_ value: T) throws {
            try encoder.encodeValue(value, at: .element)
        }

        mutating func nestedContainer<NestedKey: CodingKey>(keyedBy keyType: NestedKey.Type)
            -> KeyedEncodingContainer<NestedKey>
        {
            encoder.nestedEncoder(at: .element).container(keyedBy: NestedKey.self)
        }

        mutating func nestedUnkeyedContainer() -> any UnkeyedEncodingContainer {
            encoder.nestedEncoder(at: .element).unkeyedContainer()
        }

        mutating func superEncoder() -> any Encoder {
            encoder.nestedEncoder(at: .element)
        }
    }

    private struct SingleSlotEncoder: SingleValueEncodingContainer {
        let encoder: EncoderBase

        var codingPath: [any CodingKey] { encoder.codingPath }

        mutating func encodeNil() throws {
            try encoder.encodeNil(at: .value)
        }

        mutating func encode(_ value: Bool) throws {
            try encoder.encodeValue(value, at: .value)
        }

        mutating func encode(_ value: String) throws {
            try encoder.encodeValue(value, at: .value)
        }

        mutating func encode(_ value: Double) throws {
            try encoder.encodeValue(value, at: .value)
        }

        mutating func encode(_ value: Float) throws {
            try encoder.encodeValue(value, at: .value)
        }

        mutating func encode(_ value: Int) throws {
            try encoder.encodeValue(value, at: .value)
        }

        mutating func encode(_ value: Int8) throws {
            try encoder.encodeValue(value, at: .value)
        }

        mutating func encode(_ value: Int16) throws {
            try encoder.encodeValue(value, at: .value)
        }

        mutating func encode(_ value: Int32) throws {
            try encoder.encodeValue(value, at: .value)
        }

        mutating func encode(_ value: Int64) throws {
            try encoder.encodeValue(value, at: .value)
        }

        mutating func encode(_ value: Int128) throws {
            try encoder.encodeValue(value, at: .value)
        }

        mutating func encode(_ value: UInt) throws {
            try encoder.encodeValue(value, at: .value)
        }

        mutating func encode(_ value: UInt8) throws {
            try encoder.encodeValue(value, at: .value)
        }

        mutating func encode(_ value: UInt16) throws {
            try encoder.encodeValue(value, at: .value)
        }

        mutating func encode(_ value: UInt32) throws {
            try encoder.encodeValue(value, at: .value)
        }

        mutating func encode(_ value: UInt64) throws {
            try encoder.encodeValue(value, at: .value)
        }

        mutating func encode(_ value: UInt128) throws {
            try encoder.encodeValue(value, at: .value)
        }

        mutating func encode<T: Encodable>(_ value: T) throws {
            try encoder.encodeValue(value, at: .value)
        }
    }

    // MARK: Decoding containers

    private struct KeyedSlotDecoder<Key: CodingKey>: KeyedDecodingContainerProtocol {
        let decoder: DecoderBase

        var codingPath: [any CodingKey] { decoder.codingPath }

        var allKeys: [Key] {
            decoder.allKeys.compactMap { Key(stringValue: $0) }
        }

        func contains(_ key: Key) -> Bool {
            decoder.contains(key)
        }

        func decodeNil(forKey key: Key) throws -> Bool {
            try decoder.decodeNil(at: .key(key))
        }

        func decode(_ type: Bool.Type, forKey key: Key) throws -> Bool {
            try decoder.decodeValue(type, at: .key(key))
        }

        func decode(_ type: String.Type, forKey key: Key) throws -> String {
            try decoder.decodeValue(type, at: .key(key))
        }

        func decode(_ type: Double.Type, forKey key: Key) throws -> Double {
            try decoder.decodeValue(type, at: .key(key))
        }

        func decode(_ type: Float.Type, forKey key: Key) throws -> Float {
            try decoder.decodeValue(type, at: .key(key))
        }

        func decode(_ type: Int.Type, forKey key: Key) throws -> Int {
            try decoder.decodeValue(type, at: .key(key))
        }

        func decode(_ type: Int8.Type, forKey key: Key) throws -> Int8 {
            try decoder.decodeValue(type, at: .key(key))
        }

        func decode(_ type: Int16.Type, forKey key: Key) throws -> Int16 {
            try decoder.decodeValue(type, at: .key(key))
        }

        func decode(_ type: Int32.Type, forKey key: Key) throws -> Int32 {
            try decoder.decodeValue(type, at: .key(key))
        }

        func decode(_ type: Int64.Type, forKey key: Key) throws -> Int64 {
            try decoder.decodeValue(type, at: .key(key))
        }

        func decode(_ type: Int128.Type, forKey key: Key) throws -> Int128 {
            try decoder.decodeValue(type, at: .key(key))
        }

        func decode(_ type: UInt.Type, forKey key: Key) throws -> UInt {
            try decoder.decodeValue(type, at: .key(key))
        }

        func decode(_ type: UInt8.Type, forKey key: Key) throws -> UInt8 {
            try decoder.decodeValue(type, at: .key(key))
        }

        func decode(_ type: UInt16.Type, forKey key: Key) throws -> UInt16 {
            try decoder.decodeValue(type, at: .key(key))
        }

        func decode(_ type: UInt32.Type, forKey key: Key) throws -> UInt32 {
            try decoder.decodeValue(type, at: .key(key))
        }

        func decode(_ type: UInt64.Type, forKey key: Key) throws -> UInt64 {
            try decoder.decodeValue(type, at: .key(key))
        }

        func decode(_ type: UInt128.Type, forKey key: Key) throws -> UInt128 {
            try decoder.decodeValue(type, at: .key(key))
        }

        func decode<T: Decodable>(_ type: T.Type, forKey key: Key) throws -> T {
            try decoder.decodeValue(type, at: .key(key))
        }

        func nestedContainer<NestedKey: CodingKey>(
            keyedBy type: NestedKey.Type, forKey key: Key
        ) throws -> KeyedDecodingContainer<NestedKey> {
            try decoder.nestedDecoder(at: .key(key)).container(keyedBy: NestedKey.self)
        }

        func nestedUnkeyedContainer(forKey key: Key) throws -> any UnkeyedDecodingContainer {
            try decoder.nestedDecoder(at: .key(key)).unkeyedContainer()
        }

        func superDecoder() throws -> any Decoder {
            try decoder.nestedDecoder(at: .key(SuperKey()))
        }

        func superDecoder(forKey key: Key) throws -> any Decoder {
            try decoder.nestedDecoder(at: .key(key))
        }
    }

    private struct UnkeyedSlotDecoder: UnkeyedDecodingContainer {
        let decoder: DecoderBase

        var codingPath: [any CodingKey] { decoder.codingPath }

        var count: Int? { decoder.elementCount }

        var isAtEnd: Bool { decoder.isAtEnd }

        var currentIndex: Int { decoder.currentIndex }

        mutating func decodeNil() throws -> Bool {
            guard try decoder.decodeNil(at: .element) else {
                return false
            }
            decoder.moveToNextElement()
            return true
        }

        mutating func decode(_ type: Bool.Type) throws -> Bool {
            try decoder.decodeValue(type, at: .element)
        }

        mutating func decode(_ type: String.Type) throws -> String {
            try decoder.decodeValue(type, at: .element)
        }

        mutating func decode(_ type: Double.Type) throws -> Double {
            try decoder.decodeValue(type, at: .element)
        }

        mutating func decode(_ type: Float.Type) throws -> Float {
            try decoder.decodeValue(type, at: .element)
        }

        mutating func decode(_ type: Int.Type) throws -> Int {
            try decoder.decodeValue(type, at: .element)
        }

        mutating func decode(_ type: Int8.Type) throws -> Int8 {
            try decoder.decodeValue(type, at: .element)
        }

        mutating func decode(_ type: Int16.Type) throws -> Int16 {
            try decoder.decodeValue(type, at: .element)
        }

        mutating func decode(_ type: Int32.Type) throws -> Int32 {
            try decoder.decodeValue(type, at: .element)
        }

        mutating func decode(_ type: Int64.Type) throws -> Int64 {
            try decoder.decodeValue(type, at: .element)
        }

        mutating func decode(_ type: Int128.Type) throws -> Int128 {
            try decoder.decodeValue(type, at: .element)
        }

        mutating func decode(_ type: UInt.Type) throws -> UInt {
            try decoder.decodeValue(type, at: .element)
        }

        mutating func decode(_ type: UInt8.Type) throws -> UInt8 {
            try decoder.decodeValue(type, at: .element)
        }

        mutating func decode(_ type: UInt16.Type) throws -> UInt16 {
            try decoder.decodeValue(type, at: .element)
        }

        mutating func decode(_ type: UInt32.Type) throws -> UInt32 {
            try decoder.decodeValue(type, at: .element)
        }

        mutating func decode(_ type: UInt64.Type) throws -> UInt64 {
            try decoder.decodeValue(type, at: .element)
        }

        mutating func decode(_ type: UInt128.Type) throws -> UInt128 {
            try decoder.decodeValue(type, at: .element)
        }

        mutating func decode<T: Decodable>(_ type: T.Type) throws -> T {
            try decoder.decodeValue(type, at: .element)
        }

        mutating func nestedContainer<NestedKey: CodingKey>(keyedBy type: NestedKey.Type)
            throws -> KeyedDecodingContainer<NestedKey>
        {
            let container = try decoder.nestedDecoder(at: .element).container(
                keyedBy: NestedKey.self)
            decoder.moveToNextElement()
            return container
        }

        mutating func nestedUnkeyedContainer() throws -> any UnkeyedDecodingContainer {
            let container = try decoder.nestedDecoder(at: .element).unkeyedContainer()
            decoder.moveToNextElement()
            return container
        }

        mutating func superDecoder() throws -> any Decoder {
            try decoder.nextElementDecoder()
        }
    }

    private struct SingleSlotDecoder: SingleValueDecodingContainer {
        let decoder: DecoderBase

        var codingPath: [any CodingKey] { decoder.codingPath }

        func decodeNil() -> Bool {
            (try? decoder.decodeNil(at: .value)) ?? false
        }

        func decode(_ type: Bool.Type) throws -> Bool {
            try decoder.decodeValue(type, at: .value)
        }

        func decode(_ type: String.Type) throws -> String {
            try decoder.decodeValue(type, at: .value)
        }

        func decode(_ type: Double.Type) throws -> Double {
            try decoder.decodeValue(type, at: .value)
        }

        func decode(_ type: Float.Type) throws -> Float {
            try decoder.decodeValue(type, at: .value)
        }

        func decode(_ type: Int.Type) throws -> Int {
            try decoder.decodeValue(type, at: .value)
        }

        func decode(_ type: Int8.Type) throws -> Int8 {
            try decoder.decodeValue(type, at: .value)
        }

        func decode(_ type: Int16.Type) throws -> Int16 {
            try decoder.decodeValue(type, at: .value)
        }

        func decode(_ type: Int32.Type) throws -> Int32 {
            try decoder.decodeValue(type, at: .value)
        }

        func decode(_ type: Int64.Type) throws -> Int64 {
            try decoder.decodeValue(type, at: .value)
        }

        func decode(_ type: Int128.Type) throws -> Int128 {
            try decoder.decodeValue(type, at: .value)
        }

        func decode(_ type: UInt.Type) throws -> UInt {
            try decoder.decodeValue(type, at: .value)
        }

        func decode(_ type: UInt8.Type) throws -> UInt8 {
            try decoder.decodeValue(type, at: .value)
        }

        func decode(_ type: UInt16.Type) throws -> UInt16 {
            try decoder.decodeValue(type, at: .value)
        }

        func decode(_ type: UInt32.Type) throws -> UInt32 {
            try decoder.decodeValue(type, at: .value)
        }

        func decode(_ type: UInt64.Type) throws -> UInt64 {
            try decoder.decodeValue(type, at: .value)
        }

        func decode(_ type: UInt128.Type) throws -> UInt128 {
            try decoder.decodeValue(type, at: .value)
        }

        func decode<T: Decodable>(_ type: T.Type) throws -> T {
            try decoder.decodeValue(type, at: .value)
        }
    }

#endif
