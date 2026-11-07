#pragma once

// Wait-free single-producer / single-consumer ring buffer. Capacity is fixed at
// construction (rounded up to a power of two); push and pop never allocate.

#include "jrb/rt/Realtime.hpp"

#include <atomic>
#include <cstddef>
#include <memory>
#include <type_traits>

namespace jrb::rt {

template <class T>
class SpscQueue {
    static_assert(std::is_trivially_copyable_v<T>, "SpscQueue holds plain data only");

public:
    explicit SpscQueue(std::size_t capacity) {
        std::size_t size = 2;
        while (size < capacity + 1)
            size <<= 1;
        mask_ = size - 1;
        slots_ = std::make_unique<T[]>(size);
    }

    SpscQueue(const SpscQueue&) = delete;
    SpscQueue& operator=(const SpscQueue&) = delete;

    /// Producer only. Returns false if full.
    bool push(const T& value) noexcept JRB_NONBLOCKING {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        const std::size_t next = (tail + 1) & mask_;
        if (next == head_.load(std::memory_order_acquire))
            return false;
        slots_[tail] = value;
        tail_.store(next, std::memory_order_release);
        return true;
    }

    /// Producer only. True if at least `count` more items fit.
    [[nodiscard]] bool hasSpace(std::size_t count = 1) const noexcept JRB_NONBLOCKING {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        const std::size_t head = head_.load(std::memory_order_acquire);
        return ((head - tail - 1) & mask_) >= count;
    }

    /// Consumer only. Returns false if empty.
    bool pop(T& out) noexcept JRB_NONBLOCKING {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        if (head == tail_.load(std::memory_order_acquire))
            return false;
        out = slots_[head];
        head_.store((head + 1) & mask_, std::memory_order_release);
        return true;
    }

    [[nodiscard]] std::size_t capacity() const noexcept { return mask_; }

private:
    std::unique_ptr<T[]> slots_;
    std::size_t mask_ = 0;
    alignas(128) std::atomic<std::size_t> head_{0};  // consumer
    alignas(128) std::atomic<std::size_t> tail_{0};  // producer
};

} // namespace jrb::rt
