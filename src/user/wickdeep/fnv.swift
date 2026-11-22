struct Fnv1a {
    private(set) var value: UInt64 = 0xCBF2_9CE4_8422_2325

    mutating func add(_ byte: UInt8) {
        value = (value ^ UInt64(byte)) &* 0x0000_0100_0000_01B3
    }

    mutating func add(_ number: UInt64, bytes: Int) {
        for index in 0..<bytes {
            add(UInt8(truncatingIfNeeded: number >> UInt64(8 * index)))
        }
    }

    mutating func add(_ number: Int, bytes: Int) {
        add(UInt64(truncatingIfNeeded: number), bytes: bytes)
    }

    mutating func add(_ array: [UInt8]) {
        for byte in array {
            add(byte)
        }
    }
}
