import SwiftUI

// Minimal BleLink sample: connect, send text, send an N-KB blob with progress, show received messages.
// Needs iOS 14 / macOS 11 (SwiftUI App). Add NSBluetoothAlwaysUsageDescription to the app's Info.plist.
@main
struct BleLinkSampleApp: App {
    var body: some Scene {
        WindowGroup { ContentView() }
    }
}
