# BleLink SwiftUI sample

Two files: `BleLinkSampleApp.swift`, `ContentView.swift`. They connect to the first peripheral that advertises the
BleLink service, send text or an N-KB blob with a progress bar, and log received messages and ACKs.

`swift build` compiles them on macOS as a type check. A bare SwiftPM executable has no Info.plist, so macOS
kills it on its first Bluetooth access (TCC). To run it:

1. Xcode -> New Project -> iOS App (SwiftUI, iOS 14+). Delete the generated `App` and `ContentView` files.
2. File -> Add Package Dependencies -> Add Local... -> `clients/ios/BleLink`; add the `BleLink` library to the target.
3. Drag the two `.swift` files above into the target.
4. Info tab: add `NSBluetoothAlwaysUsageDescription` (Privacy - Bluetooth Always Usage Description).
5. Run on a real iPhone (the Simulator has no Bluetooth). Start the server first: `ble_test_server` speaking BleLink v1
   (see ../../NOTES.md for what has to exist on the server side).

The same steps with a macOS App target let you run it on the Mac (also enable "Bluetooth" under
Signing & Capabilities -> App Sandbox).
