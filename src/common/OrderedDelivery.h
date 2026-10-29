#pragma once
// Delivers items to a callback one at a time and in reservation order, even when they are produced
// on several threads and finish out of order.
//
// Used by the Windows backend: WriteRequested events arrive in order, but each request body is
// obtained with an async GetRequestAsync() that may resume on any thread pool thread, so writes
// could otherwise reach the write callback reordered or concurrently.
//
//   const uint64_t t = d.Reserve();            // in event order (cheap, thread-safe)
//   ... async work ...
//   d.Complete(t, std::move(item), deliver);   // or d.Complete(t, std::nullopt, deliver) to skip
//
// Every reserved ticket MUST be completed (skip on error), or later items wait forever.
// `deliver` runs with an internal delivery mutex held (never two at once, never out of order) and
// with the state mutex released, so it may call Reserve()/Complete() for other tickets. It must
// not wait for another ticket's Complete().
// Platform-neutral, header-only, thread-safe.
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <utility>

namespace BleServer {

template <class T>
class OrderedDelivery {
public:
    uint64_t Reserve() {
        std::lock_guard<std::mutex> l(m_);
        return reserved_++;
    }

    template <class Deliver>
    void Complete(uint64_t ticket, std::optional<T> item, Deliver&& deliver) {
        {
            std::lock_guard<std::mutex> l(m_);
            if (ticket < next_ || ticket >= reserved_) return;  // stale (after Reset) or never reserved
            done_.emplace(ticket, std::move(item));
        }
        std::lock_guard<std::mutex> d(deliverMutex_);
        for (;;) {
            std::optional<T> ready;
            {
                std::lock_guard<std::mutex> l(m_);
                auto it = done_.find(next_);
                if (it == done_.end()) return;
                ready = std::move(it->second);
                done_.erase(it);
                ++next_;
            }
            if (ready) deliver(*ready);
        }
    }

    // Drops everything pending and skips every ticket reserved so far (shutdown).
    void Reset() {
        std::lock_guard<std::mutex> l(m_);
        done_.clear();
        next_ = reserved_;
    }

    size_t Pending() const {
        std::lock_guard<std::mutex> l(m_);
        return static_cast<size_t>(reserved_ - next_);
    }

private:
    mutable std::mutex m_;
    std::mutex deliverMutex_;
    uint64_t reserved_ = 0;
    uint64_t next_ = 0;
    std::map<uint64_t, std::optional<T>> done_;
};

} // namespace BleServer
