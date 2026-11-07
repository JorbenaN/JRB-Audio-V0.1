#pragma once

// Hands immutable objects (render plans) from a control thread to the audio
// thread and back for destruction, without locks or allocation on the audio
// side (docs/03-audio-engine.md §4.3).
//
//   control thread:  publish(next)      — newest wins; an unconsumed older one is freed here
//   audio thread:    acquire()          — at block start; returns the current object
//   control thread:  collectGarbage()   — frees objects the audio thread has let go of
//
// The audio thread never runs a destructor: retired objects travel back
// through an SPSC queue. If that queue is full, the audio thread simply keeps
// the current object for another block.

#include "jrb/rt/SpscQueue.hpp"

#include <atomic>
#include <cstddef>
#include <memory>

namespace jrb::rt {

template <class T>
class PlanExchange {
public:
    explicit PlanExchange(std::size_t retireCapacity = 256) : retired_(retireCapacity) {}

    PlanExchange(const PlanExchange&) = delete;
    PlanExchange& operator=(const PlanExchange&) = delete;

    /// Must be called after the audio thread has stopped using this exchange.
    ~PlanExchange() {
        delete pending_.exchange(nullptr);
        delete current_;
        collectGarbage();
    }

    /// Control thread.
    void publish(std::unique_ptr<T> next) {
        T* superseded = pending_.exchange(next.release(), std::memory_order_acq_rel);
        delete superseded;  // never seen by the audio thread
    }

    /// Audio thread, once per block. Returns the object to use (may be null).
    T* acquire() noexcept JRB_NONBLOCKING {
        if (pending_.load(std::memory_order_relaxed) != nullptr && retired_.hasSpace()) {
            if (T* next = pending_.exchange(nullptr, std::memory_order_acq_rel)) {
                if (current_)
                    (void)retired_.push(current_);  // space was checked; single producer
                current_ = next;
                swaps_.fetch_add(1, std::memory_order_relaxed);
            }
        }
        return current_;
    }

    /// Control thread. Returns the number of objects freed.
    std::size_t collectGarbage() {
        std::size_t freed = 0;
        T* old = nullptr;
        while (retired_.pop(old)) {
            delete old;
            ++freed;
        }
        return freed;
    }

    [[nodiscard]] std::uint64_t swapCount() const noexcept { return swaps_.load(std::memory_order_relaxed); }

private:
    std::atomic<T*> pending_{nullptr};
    T* current_ = nullptr;  // audio thread only
    SpscQueue<T*> retired_;
    std::atomic<std::uint64_t> swaps_{0};
};

} // namespace jrb::rt
