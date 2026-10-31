#pragma once
// Bounded FIFO of notifications the OS transmit queue could not take yet (flow control).
//
// Platform-neutral (no OS includes), header-only, NOT thread-safe: the owning backend calls every
// method while holding its own mutex. The `send` functions passed in must not run user callbacks;
// the results tell the owner what to report after it has released its lock.
//
// Contract (used by the macOS backend, modelled by tests/fake/FakeBleServer):
//   - Submit(): if nothing is queued, try to send right away; if the transport is busy, queue it.
//     If something is already queued the value goes behind it (never overtakes: order is kept).
//   - Drain(): call when the OS signals it can take more (macOS:
//     peripheralManagerIsReadyToUpdateSubscribers:). Sends queued values in order until the
//     transport is busy again.
//   - At most `capacity` values are kept; on overflow the OLDEST is dropped and counted.
//   - RemoveCharacteristic() / Clear(): drop queued values (last unsubscribe, characteristic
//     removed, shutdown).
#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <utility>
#include <vector>

namespace BleServer {

class NotificationQueue {
public:
    static constexpr size_t kDefaultCapacity = 256;

    struct Item {
        std::string service;
        std::string characteristic;
        std::vector<uint8_t> value;
    };

    // What the transport did with one value.
    enum class SendOutcome {
        Sent,     // accepted by the OS
        Busy,     // OS queue full: keep the value, try again on the next "ready" signal
        Discard   // can never be sent (e.g. characteristic gone): drop it
    };

    struct SubmitResult {
        bool sentNow = false;     // transmitted immediately
        bool queued = false;      // kept for a later Drain()
        bool backlogStarted = false;  // queue went from empty to non-empty (log once per backlog)
        size_t dropped = 0;       // oldest values dropped because of the capacity bound
        size_t depth = 0;         // queued values after the call
    };

    struct DrainResult {
        size_t sent = 0;
        size_t discarded = 0;
        size_t remaining = 0;
        bool backlogCleared = false;  // queue was non-empty and is now empty
    };

    explicit NotificationQueue(size_t capacity = kDefaultCapacity)
        : capacity_(capacity == 0 ? 1 : capacity) {}

    template <typename SendFn>
    SubmitResult Submit(Item item, SendFn&& send) {
        SubmitResult r;
        if (items_.empty()) {
            const SendOutcome outcome = send(item);
            if (outcome == SendOutcome::Sent) {
                r.sentNow = true;
                return r;
            }
            if (outcome == SendOutcome::Discard) {
                return r;
            }
            r.backlogStarted = true;
        }
        items_.push_back(std::move(item));
        r.queued = true;
        while (items_.size() > capacity_) {
            items_.pop_front();
            ++r.dropped;
        }
        totalDropped_ += r.dropped;
        r.depth = items_.size();
        if (r.depth > highWater_) highWater_ = r.depth;
        return r;
    }

    template <typename SendFn>
    DrainResult Drain(SendFn&& send) {
        DrainResult r;
        const bool hadBacklog = !items_.empty();
        while (!items_.empty()) {
            const SendOutcome outcome = send(items_.front());
            if (outcome == SendOutcome::Busy) break;
            if (outcome == SendOutcome::Sent) ++r.sent; else ++r.discarded;
            items_.pop_front();
        }
        r.remaining = items_.size();
        r.backlogCleared = hadBacklog && items_.empty();
        return r;
    }

    // Drops every queued value for one characteristic; returns how many were dropped.
    size_t RemoveCharacteristic(const std::string& service, const std::string& characteristic) {
        size_t removed = 0;
        for (auto it = items_.begin(); it != items_.end();) {
            if (it->service == service && it->characteristic == characteristic) {
                it = items_.erase(it);
                ++removed;
            } else {
                ++it;
            }
        }
        return removed;
    }

    // Drops every queued value of a service; returns how many were dropped.
    size_t RemoveService(const std::string& service) {
        size_t removed = 0;
        for (auto it = items_.begin(); it != items_.end();) {
            if (it->service == service) {
                it = items_.erase(it);
                ++removed;
            } else {
                ++it;
            }
        }
        return removed;
    }

    size_t Clear() {
        const size_t n = items_.size();
        items_.clear();
        return n;
    }

    size_t Size() const { return items_.size(); }
    bool Empty() const { return items_.empty(); }
    size_t Capacity() const { return capacity_; }
    size_t HighWater() const { return highWater_; }
    size_t TotalDropped() const { return totalDropped_; }

private:
    size_t capacity_;
    std::deque<Item> items_;
    size_t highWater_ = 0;
    size_t totalDropped_ = 0;
};

} // namespace BleServer
