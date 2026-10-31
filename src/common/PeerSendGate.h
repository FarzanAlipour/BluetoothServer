#pragma once
// Per-central send accounting for IBleServer::NotifyPeer (busy/ready flow control).
//
// Platform-neutral, header-only, NOT thread-safe: the owner calls it under its own lock and fires
// ReadyToSend callbacks after releasing that lock.
//
//   - TryAcquire(peer): a send may start if the peer has fewer than `maxInFlight` sends in flight
//     (Windows: NotifyValueAsync not completed yet). Otherwise returns false and remembers that the
//     peer is waiting, so the owner owes it a ready signal.
//   - Release(peer): a send completed. Returns true when the peer was waiting (signal it now).
//   - MarkWaiting(peer) / TakeWaiting(peer) / TakeAllWaiting(): for backends where "busy" comes from
//     the OS (macOS updateValue NO, BlueZ EAGAIN) instead of an in-flight count.
//   - Forget(peer) on unsubscribe/disconnect, Clear() on shutdown: no ready signal is owed any more.
#include <cstddef>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace BleServer {

class PeerSendGate {
public:
    explicit PeerSendGate(size_t maxInFlightPerPeer = 1)
        : maxInFlight_(maxInFlightPerPeer == 0 ? 1 : maxInFlightPerPeer) {}

    bool TryAcquire(const std::string& peer) {
        size_t& n = inFlight_[peer];
        if (n >= maxInFlight_) {
            waiting_.insert(peer);
            return false;
        }
        ++n;
        return true;
    }

    // Undo a TryAcquire whose send could not even be started (no ready signal is owed for it).
    void Cancel(const std::string& peer) { Decrement(peer); }

    bool Release(const std::string& peer) {
        Decrement(peer);
        return waiting_.erase(peer) > 0;
    }

    void MarkWaiting(const std::string& peer) { waiting_.insert(peer); }
    bool TakeWaiting(const std::string& peer) { return waiting_.erase(peer) > 0; }
    std::vector<std::string> TakeAllWaiting() {
        std::vector<std::string> out(waiting_.begin(), waiting_.end());
        waiting_.clear();
        return out;
    }
    bool AnyWaiting() const { return !waiting_.empty(); }
    bool IsWaiting(const std::string& peer) const { return waiting_.count(peer) > 0; }

    void Forget(const std::string& peer) {
        inFlight_.erase(peer);
        waiting_.erase(peer);
    }
    void Clear() {
        inFlight_.clear();
        waiting_.clear();
    }

    size_t InFlight(const std::string& peer) const {
        auto it = inFlight_.find(peer);
        return it == inFlight_.end() ? 0 : it->second;
    }
    size_t MaxInFlight() const { return maxInFlight_; }

private:
    void Decrement(const std::string& peer) {
        auto it = inFlight_.find(peer);
        if (it == inFlight_.end()) return;
        if (it->second > 0) --it->second;
        if (it->second == 0) inFlight_.erase(it);
    }

    size_t maxInFlight_;
    std::map<std::string, size_t> inFlight_;
    std::set<std::string> waiting_;
};

} // namespace BleServer
