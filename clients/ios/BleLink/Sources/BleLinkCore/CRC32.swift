import Foundation

/// CRC-32/IEEE 802.3 (zlib, PNG, Ethernet): reflected, poly 0xEDB88320, init 0xFFFFFFFF, xorout 0xFFFFFFFF.
/// PROTOCOL.md section 2.4. Running use: `var c = CRC32(); c.update(a); c.update(b); c.value`.
public struct CRC32: Equatable {
    private var state: UInt32 = 0xFFFF_FFFF

    public init() {}

    public mutating func update<D: DataProtocol>(_ data: D) {
        var s = state
        for region in data.regions {
            for byte in region {
                s = CRC32.table[Int((s ^ UInt32(byte)) & 0xFF)] ^ (s >> 8)
            }
        }
        state = s
    }

    public var value: UInt32 { state ^ 0xFFFF_FFFF }

    public static func checksum<D: DataProtocol>(_ data: D) -> UInt32 {
        var c = CRC32()
        c.update(data)
        return c.value
    }

    private static let table: [UInt32] = (0..<256).map { i -> UInt32 in
        var c = UInt32(i)
        for _ in 0..<8 { c = (c & 1) != 0 ? (0xEDB8_8320 ^ (c >> 1)) : (c >> 1) }
        return c
    }
}
