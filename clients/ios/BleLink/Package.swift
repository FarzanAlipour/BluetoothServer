// swift-tools-version:5.9
// BleLink v1 client (GATT central) for iOS 13+ and macOS. See README.md and ../../../docs/PROTOCOL.md.
import PackageDescription

let package = Package(
    name: "BleLink",
    platforms: [
        .iOS(.v13),
        // macOS 11 so the SwiftUI sample (App protocol) type-checks with `swift build` on a Mac.
        .macOS(.v11),
    ],
    products: [
        // Codec + session state machine. No Bluetooth, no threads, no timers: unit-testable anywhere.
        .library(name: "BleLinkCore", targets: ["BleLinkCore"]),
        // CoreBluetooth transport adapter (re-exports BleLinkCore).
        .library(name: "BleLink", targets: ["BleLink"]),
    ],
    targets: [
        .target(name: "BleLinkCore"),
        .target(name: "BleLink", dependencies: ["BleLinkCore"]),
        // Minimal SwiftUI sample. Built on macOS by `swift build` as a compile check; to run it on a
        // phone, add the two files to an iOS app target (see Sources/BleLinkSample/README.md).
        .executableTarget(name: "BleLinkSample", dependencies: ["BleLink"],
                          exclude: ["README.md"]),
        .testTarget(name: "BleLinkCoreTests", dependencies: ["BleLinkCore"]),
    ]
)
