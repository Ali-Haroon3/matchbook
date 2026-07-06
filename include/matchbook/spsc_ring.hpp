#pragma once
// Bounded single-producer/single-consumer lock-free ring buffer.
//
// This is the seam between the feed-handler thread (producer: parses ITCH
// off the wire/file) and the matching thread (consumer: applies messages
// to the book). Head and tail live on separate cache lines to avoid false
// sharing, and each side caches the other's index so the common case is a
// single relaxed load plus one release/acquire pair per batch, not per op.
#include <atomic>
#include <cstddef>
#include <new>

namespace matchbook {

template <typename T, size_t CapacityPow2>
class SpscRing {
    static_assert((CapacityPow2 & (CapacityPow2 - 1)) == 0,
                  "capacity must be a power of two");
    static constexpr size_t kMask = CapacityPow2 - 1;
#ifdef __cpp_lib_hardware_interference_size
    static constexpr size_t kCacheLine = std::hardware_destructive_interference_size;
#else
    static constexpr size_t kCacheLine = 64;
#endif

public:
    bool try_push(const T& v) noexcept {
        const size_t head = head_.load(std::memory_order_relaxed);
        if (head - cached_tail_ == CapacityPow2) {
            cached_tail_ = tail_.load(std::memory_order_acquire);
            if (head - cached_tail_ == CapacityPow2) return false;  // full
        }
        buf_[head & kMask] = v;
        head_.store(head + 1, std::memory_order_release);
        return true;
    }

    bool try_pop(T& out) noexcept {
        const size_t tail = tail_.load(std::memory_order_relaxed);
        if (tail == cached_head_) {
            cached_head_ = head_.load(std::memory_order_acquire);
            if (tail == cached_head_) return false;  // empty
        }
        out = buf_[tail & kMask];
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    size_t size_approx() const noexcept {
        return head_.load(std::memory_order_acquire) -
               tail_.load(std::memory_order_acquire);
    }

private:
    alignas(kCacheLine) std::atomic<size_t> head_{0};  // producer writes
    alignas(kCacheLine) size_t cached_tail_{0};        // producer-local
    alignas(kCacheLine) std::atomic<size_t> tail_{0};  // consumer writes
    alignas(kCacheLine) size_t cached_head_{0};        // consumer-local
    alignas(kCacheLine) T buf_[CapacityPow2];
};

}  // namespace matchbook
