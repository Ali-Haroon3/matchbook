#pragma once
// Occupancy bitmap over price levels. One bit per level; finding the next
// best price after a level empties is a masked scan over 64-bit words with
// ctz/clz -- effectively O(1) for realistic books instead of a linear walk
// over empty levels or an O(log n) tree step.
#include <cstdint>
#include <vector>

namespace matchbook {

class LevelBitmap {
public:
    explicit LevelBitmap(size_t n_levels)
        : words_((n_levels + 63) / 64, 0), n_(n_levels) {}

    void set(size_t i) noexcept { words_[i >> 6] |= (1ull << (i & 63)); }
    void clear(size_t i) noexcept { words_[i >> 6] &= ~(1ull << (i & 63)); }

    bool test(size_t i) const noexcept {
        return (words_[i >> 6] >> (i & 63)) & 1ull;
    }

    // Highest set index <= i, or -1 if none. Used to find the next best bid.
    int64_t find_le(int64_t i) const noexcept {
        if (i < 0) return -1;
        if (static_cast<size_t>(i) >= n_) i = static_cast<int64_t>(n_) - 1;
        size_t w = static_cast<size_t>(i) >> 6;
        unsigned bit = static_cast<unsigned>(i) & 63;
        uint64_t mask = (bit == 63) ? ~0ull : ((1ull << (bit + 1)) - 1);
        uint64_t v = words_[w] & mask;
        while (true) {
            if (v) {
                return static_cast<int64_t>(w << 6) + 63 - __builtin_clzll(v);
            }
            if (w == 0) return -1;
            v = words_[--w];
        }
    }

    // Lowest set index >= i, or -1 if none. Used to find the next best ask.
    int64_t find_ge(int64_t i) const noexcept {
        if (i < 0) i = 0;
        if (static_cast<size_t>(i) >= n_) return -1;
        size_t w = static_cast<size_t>(i) >> 6;
        uint64_t v = words_[w] & (~0ull << (static_cast<unsigned>(i) & 63));
        while (true) {
            if (v) {
                return static_cast<int64_t>(w << 6) + __builtin_ctzll(v);
            }
            if (++w == words_.size()) return -1;
            v = words_[w];
        }
    }

private:
    std::vector<uint64_t> words_;
    size_t n_;
};

}  // namespace matchbook
