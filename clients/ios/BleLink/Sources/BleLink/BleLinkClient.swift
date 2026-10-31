import CoreBluetooth
import Foundation
@_exported import BleLinkCore

/// GATT UUIDs of the BleLink service (PROTOCOL.md section 1).
public enum BleLinkUUID {
    public static let service = CBUUID(string: "7B1E1000-5A6C-4F0E-9D3B-2C1A8E4F6B01")
    /// client -> server writes
    public static let rx = CBUUID(string: "7B1E1001-5A6C-4F0E-9D3B-2C1A8E4F6B01")
    /// server -> client notifications
    public static let tx = CBUUID(string: "7B1E1002-5A6C-4F0E-9D3B-2C1A8E4F6B01")
    /// server limits (read)
    public static let info = CBUUID(string: "7B1E1003-5A6C-4F0E-9D3B-2C1A8E4F6B01")
}

/// Receives every `BleLinkEvent` on the client's `delegateQueue` (main queue by default).
public protocol BleLinkClientDelegate: AnyObject {
    func bleLinkClient(_ client: BleLinkClient, didEmit event: BleLinkEvent)
}

/// BleLink v1 central on CoreBluetooth: scan, connect, discover, subscribe TX, HELLO, then messages both ways.
///
/// Threading (main-actor safety):
/// - All CoreBluetooth callbacks and all protocol state run on one private serial queue (`bleQueue`), never on the
///   main thread, so a busy UI cannot stall the radio. Public methods are thread-safe: they hop onto that queue.
/// - Events are delivered on `delegateQueue` (default `.main`), both to `delegate` and to the `events` stream.
///   With the default, delegate callbacks run on the main thread and may touch UI directly; the class itself is
///   not `@MainActor`, so it can be used from any actor.
/// - `events` is a single-consumer `AsyncStream`; iterate it from one task.
public final class BleLinkClient: NSObject, @unchecked Sendable {
    public weak var delegate: BleLinkClientDelegate?
    /// Every event, in order. Single consumer.
    public let events: AsyncStream<BleLinkEvent>

    public let config: BleLinkConfig
    public let delegateQueue: DispatchQueue
    private let bleQueue = DispatchQueue(label: "blelink.corebluetooth", qos: .userInitiated)
    private let session: BleLinkSession
    private var central: CBCentralManager!
    private let eventContinuation: AsyncStream<BleLinkEvent>.Continuation

    // All below: bleQueue only.
    private var peripheral: CBPeripheral?
    private var rxChar: CBCharacteristic?
    private var txChar: CBCharacteristic?
    private var waitingForWriteResponse = false
    private var pumping = false
    private var timer: DispatchSourceTimer?
    private var connectWaiter: CheckedContinuation<HelloBody, Error>?
    private var pendingConnect: (() -> Void)?
    private var wantedIdentifier: UUID?
    private var sendWaiters: [UInt16: (Error?) -> Void] = [:]
    private var connectTimeout: DispatchWorkItem?

    /// - Parameters:
    ///   - restoreIdentifier: enables CoreBluetooth state restoration (`CBCentralManagerOptionRestoreIdentifierKey`).
    ///     After restoration the client re-subscribes and re-sends HELLO, which resets the server session.
    public init(config: BleLinkConfig = BleLinkConfig(), delegateQueue: DispatchQueue = .main, restoreIdentifier: String? = nil) {
        self.config = config
        self.delegateQueue = delegateQueue
        self.session = BleLinkSession(config: config)
        var cont: AsyncStream<BleLinkEvent>.Continuation!
        self.events = AsyncStream(bufferingPolicy: .unbounded) { cont = $0 }
        self.eventContinuation = cont
        super.init()
        var options: [String: Any] = [:]
        if let r = restoreIdentifier { options[CBCentralManagerOptionRestoreIdentifierKey] = r }
        session.onEvent = { [weak self] e in self?.handleSessionEvent(e) }
        session.onOutputAvailable = { [weak self] in self?.pump() }
        central = CBCentralManager(delegate: self, queue: bleQueue, options: options)
    }

    // MARK: - Public API

    /// Scans for the BleLink service (or connects to a known peripheral `identifier`), connects, discovers,
    /// enables TX notifications and runs the HELLO exchange. Returns the server's limits.
    public func connect(identifier: UUID? = nil, timeout: TimeInterval = 20) async throws -> HelloBody {
        try await withCheckedThrowingContinuation { (c: CheckedContinuation<HelloBody, Error>) in
            bleQueue.async {
                if self.connectWaiter != nil {
                    c.resume(throwing: BleLinkError.connectFailed("connect already in progress"))
                    return
                }
                self.connectWaiter = c
                self.wantedIdentifier = identifier
                let work = DispatchWorkItem { [weak self] in self?.failConnect(.connectFailed("timeout"), cancelLink: true) }
                self.connectTimeout = work
                self.bleQueue.asyncAfter(deadline: .now() + timeout, execute: work)
                self.pendingConnect = { [weak self] in self?.startScanOrConnect() }
                switch self.central.state {
                case .poweredOn: self.runPendingConnect()
                case .unauthorized, .unsupported, .poweredOff:
                    // Already known to be unusable: fail now instead of waiting for the timeout.
                    self.centralManagerDidUpdateState(self.central)
                default: break  // .unknown / .resetting: centralManagerDidUpdateState will run it
                }
            }
        }
    }

    /// Queues a message; returns its msgId at once. Completion is reported by `.sent` / `.failed` events.
    public func send(_ data: Data) async throws -> UInt16 {
        try await withCheckedThrowingContinuation { (c: CheckedContinuation<UInt16, Error>) in
            bleQueue.async {
                do { c.resume(returning: try self.session.send(data)) } catch { c.resume(throwing: error) }
            }
        }
    }

    /// Callback variant of `send(_:)`; `completion` runs on `delegateQueue`.
    public func send(_ data: Data, completion: @escaping (Result<UInt16, Error>) -> Void) {
        bleQueue.async {
            let r = Result { try self.session.send(data) }
            self.delegateQueue.async { completion(r) }
        }
    }

    /// Sends and waits for the server's ACK. Throws `BleLinkSendError` with the failure reason.
    @discardableResult
    public func sendAndWait(_ data: Data) async throws -> UInt16 {
        // Queue and register the waiter in one bleQueue block, so the ACK cannot arrive in between.
        try await withCheckedThrowingContinuation { (c: CheckedContinuation<UInt16, Error>) in
            bleQueue.async {
                do {
                    let id = try self.session.send(data)
                    self.sendWaiters[id] = { error in
                        if let e = error { c.resume(throwing: e) } else { c.resume(returning: id) }
                    }
                } catch {
                    c.resume(throwing: error)
                }
            }
        }
    }

    public func cancel(_ msgId: UInt16) { bleQueue.async { self.session.cancel(msgId) } }

    /// Re-runs HELLO (resets both sessions; in-flight messages fail with `.reset`).
    public func resetSession() {
        bleQueue.async {
            self.refreshOsLimit()
            self.session.reset()
        }
    }

    public func disconnect() {
        bleQueue.async {
            self.central.stopScan()
            if let p = self.peripheral {
                self.central.cancelPeripheralConnection(p)
                // A pending (not yet established) connect gets no didDisconnect callback for sure: clean up here.
                if p.state != .connected { self.peripheral = nil }
            }
            self.failConnect(.disconnected, cancelLink: false)
        }
    }

    // MARK: - connection steps (bleQueue)

    private func runPendingConnect() {
        let f = pendingConnect
        pendingConnect = nil
        f?()
    }

    private func startScanOrConnect() {
        if let id = wantedIdentifier, let p = central.retrievePeripherals(withIdentifiers: [id]).first {
            connectTo(p)
            return
        }
        if let p = central.retrieveConnectedPeripherals(withServices: [BleLinkUUID.service]).first,
           wantedIdentifier == nil || p.identifier == wantedIdentifier {
            connectTo(p)
            return
        }
        central.scanForPeripherals(withServices: [BleLinkUUID.service], options: nil)
    }

    private func connectTo(_ p: CBPeripheral) {
        central.stopScan()
        peripheral = p
        p.delegate = self
        if p.state == .connected {
            p.discoverServices([BleLinkUUID.service])
        } else {
            central.connect(p, options: nil)
        }
    }

    private func refreshOsLimit() {
        guard let p = peripheral else { return }
        session.setOsLimit(p.maximumWriteValueLength(for: .withoutResponse))
    }

    private func linkUp() {
        refreshOsLimit()
        startTimer()
        session.start()
    }

    private func failConnect(_ error: BleLinkError, cancelLink: Bool) {
        connectTimeout?.cancel()
        connectTimeout = nil
        pendingConnect = nil
        if cancelLink {
            central.stopScan()
            if let p = peripheral { central.cancelPeripheralConnection(p) }
        }
        if let c = connectWaiter {
            connectWaiter = nil
            c.resume(throwing: error)
        }
    }

    private func linkDown(error: Error?) {
        timer?.cancel()
        timer = nil
        waitingForWriteResponse = false
        rxChar = nil
        txChar = nil
        session.linkDown()
        failConnect(.disconnected, cancelLink: false)
        deliver(.disconnected(error: error))
    }

    private func startTimer() {
        timer?.cancel()
        let t = DispatchSource.makeTimerSource(queue: bleQueue)
        t.schedule(deadline: .now() + 0.25, repeating: 0.25)
        t.setEventHandler { [weak self] in
            guard let self = self else { return }
            self.session.tick()
            self.pump()
        }
        t.resume()
        timer = t
    }

    /// Writes as many packets as CoreBluetooth accepts right now (PROTOCOL.md 6.2, 14.2).
    private func pump() {
        guard !pumping, let p = peripheral, let rx = rxChar, p.state == .connected else { return }
        pumping = true
        defer { pumping = false }
        refreshOsLimit()
        while !waitingForWriteResponse && session.hasOutput {
            // Writes issued while this is false are silently dropped by iOS (the seq-gap case of section 8).
            guard p.canSendWriteWithoutResponse else {
                session.setFlowBlocked(true)
                return  // resumes in peripheralIsReady(toSendWriteWithoutResponse:)
            }
            guard let pkt = session.nextPacket() else { break }
            if pkt.isHello && rx.properties.contains(.write) {
                waitingForWriteResponse = true
                p.writeValue(pkt.data, for: rx, type: .withResponse)
            } else {
                p.writeValue(pkt.data, for: rx, type: .withoutResponse)
            }
        }
    }

    // MARK: - events

    private func handleSessionEvent(_ e: BleLinkEvent) {
        switch e {
        case let .peerReady(peer, _):
            connectTimeout?.cancel()
            connectTimeout = nil
            if let c = connectWaiter {
                connectWaiter = nil
                c.resume(returning: peer)
            }
        case .helloTimeout:
            failConnect(.helloTimeout, cancelLink: true)
        case let .sent(id):
            sendWaiters.removeValue(forKey: id)?(nil)
        case let .failed(id, reason):
            sendWaiters.removeValue(forKey: id)?(BleLinkSendError(msgId: id, reason: reason))
        default:
            break
        }
        deliver(e)
    }

    private func deliver(_ e: BleLinkEvent) {
        delegateQueue.async {
            self.delegate?.bleLinkClient(self, didEmit: e)
            self.eventContinuation.yield(e)
        }
    }
}

/// Thrown by `sendAndWait`.
public struct BleLinkSendError: Error, Equatable {
    public let msgId: UInt16
    public let reason: BleLinkFailure
}

// MARK: - CBCentralManagerDelegate

extension BleLinkClient: CBCentralManagerDelegate {
    public func centralManagerDidUpdateState(_ central: CBCentralManager) {
        switch central.state {
        case .poweredOn:
            runPendingConnect()
        case .unauthorized:
            failConnect(.bluetoothUnavailable("unauthorized (check NSBluetoothAlwaysUsageDescription / user permission)"), cancelLink: false)
        case .unsupported:
            failConnect(.bluetoothUnavailable("unsupported"), cancelLink: false)
        case .poweredOff:
            if peripheral != nil { linkDown(error: nil) }
            failConnect(.bluetoothUnavailable("powered off"), cancelLink: false)
        default:
            break
        }
    }

    public func centralManager(_ central: CBCentralManager, didDiscover peripheral: CBPeripheral,
                               advertisementData: [String: Any], rssi RSSI: NSNumber) {
        guard connectWaiter != nil, self.peripheral == nil else { return }
        if let id = wantedIdentifier, peripheral.identifier != id { return }
        connectTo(peripheral)
    }

    public func centralManager(_ central: CBCentralManager, didConnect peripheral: CBPeripheral) {
        peripheral.discoverServices([BleLinkUUID.service])
    }

    public func centralManager(_ central: CBCentralManager, didFailToConnect peripheral: CBPeripheral, error: Error?) {
        self.peripheral = nil
        failConnect(.connectFailed(error?.localizedDescription ?? "unknown"), cancelLink: false)
    }

    public func centralManager(_ central: CBCentralManager, didDisconnectPeripheral peripheral: CBPeripheral, error: Error?) {
        guard peripheral == self.peripheral else { return }
        self.peripheral = nil
        linkDown(error: error)
    }

    /// State restoration: adopt the restored peripheral and redo discovery + subscribe + HELLO.
    public func centralManager(_ central: CBCentralManager, willRestoreState dict: [String: Any]) {
        guard let ps = dict[CBCentralManagerRestoredStatePeripheralsKey] as? [CBPeripheral], let p = ps.first else { return }
        peripheral = p
        p.delegate = self
        if p.state == .connected { p.discoverServices([BleLinkUUID.service]) }
    }
}

// MARK: - CBPeripheralDelegate

extension BleLinkClient: CBPeripheralDelegate {
    public func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: Error?) {
        guard let s = peripheral.services?.first(where: { $0.uuid == BleLinkUUID.service }) else {
            failConnect(.serviceNotFound, cancelLink: true)
            return
        }
        peripheral.discoverCharacteristics([BleLinkUUID.rx, BleLinkUUID.tx], for: s)
    }

    public func peripheral(_ peripheral: CBPeripheral, didDiscoverCharacteristicsFor service: CBService, error: Error?) {
        rxChar = service.characteristics?.first { $0.uuid == BleLinkUUID.rx }
        txChar = service.characteristics?.first { $0.uuid == BleLinkUUID.tx }
        guard let tx = txChar, rxChar != nil else {
            failConnect(.serviceNotFound, cancelLink: true)
            return
        }
        peripheral.setNotifyValue(true, for: tx)
    }

    public func peripheral(_ peripheral: CBPeripheral, didUpdateNotificationStateFor characteristic: CBCharacteristic, error: Error?) {
        guard characteristic.uuid == BleLinkUUID.tx else { return }
        if let e = error {
            failConnect(.connectFailed("enable notifications: \(e.localizedDescription)"), cancelLink: true)
            return
        }
        // The client MUST enable TX notifications before HELLO (section 1).
        if characteristic.isNotifying { linkUp() }
    }

    public func peripheral(_ peripheral: CBPeripheral, didUpdateValueFor characteristic: CBCharacteristic, error: Error?) {
        guard characteristic.uuid == BleLinkUUID.tx, error == nil, let v = characteristic.value else { return }
        session.receive(v)
        pump()
    }

    public func peripheral(_ peripheral: CBPeripheral, didWriteValueFor characteristic: CBCharacteristic, error: Error?) {
        // Only HELLO is written with response. On error the HELLO timer re-sends it.
        waitingForWriteResponse = false
        pump()
    }

    public func peripheralIsReady(toSendWriteWithoutResponse peripheral: CBPeripheral) {
        session.setFlowBlocked(false)
        pump()
    }

    public func peripheral(_ peripheral: CBPeripheral, didModifyServices invalidatedServices: [CBService]) {
        // The server's GATT database changed (for example the Unity app restarted): rediscover and redo HELLO.
        guard invalidatedServices.contains(where: { $0.uuid == BleLinkUUID.service }) else { return }
        rxChar = nil
        txChar = nil
        session.linkDown()
        peripheral.discoverServices([BleLinkUUID.service])
    }
}
