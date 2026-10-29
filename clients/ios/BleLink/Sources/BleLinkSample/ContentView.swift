import BleLink
import SwiftUI

/// UI model. BleLinkClient delivers events on the main queue (its default `delegateQueue`), so the
/// delegate callback can update @Published state directly.
final class SampleModel: ObservableObject, BleLinkClientDelegate {
    @Published var status = "idle"
    @Published var text = "hello"
    @Published var blobKB = 64
    @Published var progress: Double = 0
    @Published var log: [String] = []

    private let client = BleLinkClient()
    private var startTimes: [UInt16: Date] = [:]
    private var sizes: [UInt16: Int] = [:]

    init() { client.delegate = self }

    func connect() {
        status = "connecting..."
        Task { @MainActor in
            do {
                let peer = try await client.connect()
                status = "ready (server maxMessage \(peer.maxMessage), window \(peer.window))"
            } catch {
                status = "connect failed: \(error)"
            }
        }
    }

    func disconnect() { client.disconnect() }

    func sendText() { send(Data(text.utf8)) }

    func sendBlob() {
        let n = blobKB * 1024
        send(Data((0..<n).map { UInt8(truncatingIfNeeded: $0 &* 7 &+ 3) }))
    }

    private func send(_ data: Data) {
        progress = 0
        client.send(data) { [weak self] result in
            guard let self = self else { return }
            switch result {
            case let .success(id):
                self.startTimes[id] = Date()
                self.sizes[id] = data.count
                self.append("-> #\(id) queued, \(data.count) B")
            case let .failure(e):
                self.append("send error: \(e)")
            }
        }
    }

    func bleLinkClient(_ client: BleLinkClient, didEmit event: BleLinkEvent) {
        switch event {
        case let .peerReady(peer, packetSize):
            append("ready: packetSize \(packetSize), server window \(peer.window)")
        case let .received(id, data):
            let s = String(data: data, encoding: .utf8).map { "\"\($0.prefix(60))\"" } ?? "\(data.count) B binary"
            append("<- #\(id): \(s)")
        case let .sent(id):
            let dt = startTimes.removeValue(forKey: id).map { Date().timeIntervalSince($0) } ?? 0
            let size = sizes.removeValue(forKey: id) ?? 0
            let rate = dt > 0 ? String(format: ", %.1f KB/s", Double(size) / 1024 / dt) : ""
            append("-> #\(id) ACK ok (\(String(format: "%.2f", dt)) s\(rate))")
            progress = 1
        case let .failed(id, reason):
            append("-> #\(id) failed: \(reason)")
        case let .progress(p):
            if p.direction == .outgoing && !p.confirmedByPeer { progress = p.fraction }
        case .helloTimeout:
            append("no HELLO from server")
        case let .disconnected(error):
            status = "disconnected"
            append("disconnected \(error.map { "\($0.localizedDescription)" } ?? "")")
        }
    }

    private func append(_ s: String) {
        log.append(s)
        if log.count > 200 { log.removeFirst(log.count - 200) }
    }
}

struct ContentView: View {
    @StateObject private var model = SampleModel()

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text("BleLink sample").font(.headline)
            Text(model.status).font(.caption)
            HStack {
                Button("Connect") { model.connect() }
                Button("Disconnect") { model.disconnect() }
            }
            HStack {
                TextField("text", text: $model.text)
                Button("Send text") { model.sendText() }
            }
            HStack {
                Stepper("Blob \(model.blobKB) KB", value: $model.blobKB, in: 1...4096, step: 16)
                Button("Send blob") { model.sendBlob() }
            }
            ProgressView(value: model.progress)
            List(Array(model.log.enumerated()), id: \.offset) { _, line in
                Text(line).font(.system(.caption, design: .monospaced))
            }
        }
        .padding()
    }
}
