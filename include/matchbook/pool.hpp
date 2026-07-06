#pragma once
// Chunked object pool. Orders are allocated once from large slabs and
// recycled through a free list, so the matching hot path never touches
// malloc/free. Pointers are stable for the lifetime of the pool.
#include <cstddef>
#include <memory>
#include <vector>

namespace matchbook {

template <typename T, size_t ChunkSize = 1 << 16>
class Pool {
public:
    explicit Pool(size_t initial_capacity = ChunkSize) {
        reserve(initial_capacity);
    }

    Pool(const Pool&) = delete;
    Pool& operator=(const Pool&) = delete;

    T* alloc() {
        if (free_.empty()) [[unlikely]] {
            grow();
        }
        T* p = free_.back();
        free_.pop_back();
        return p;
    }

    void free(T* p) noexcept { free_.push_back(p); }

    size_t capacity() const noexcept { return chunks_.size() * ChunkSize; }
    size_t in_use() const noexcept { return capacity() - free_.size(); }

private:
    void reserve(size_t n) {
        while (capacity() < n) grow();
    }

    void grow() {
        auto chunk = std::make_unique<T[]>(ChunkSize);
        T* base = chunk.get();
        free_.reserve(free_.size() + ChunkSize);
        // Push in reverse so early allocations come from the front of the
        // slab -- consecutive allocs are cache-adjacent.
        for (size_t i = ChunkSize; i > 0; --i) {
            free_.push_back(base + (i - 1));
        }
        chunks_.push_back(std::move(chunk));
    }

    std::vector<std::unique_ptr<T[]>> chunks_;
    std::vector<T*> free_;
};

}  // namespace matchbook
