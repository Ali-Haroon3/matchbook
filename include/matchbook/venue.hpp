#pragma once
// Multi-symbol venue: one MatchingEngine per symbol, each with its own
// price band and its own Handler instance, registered explicitly. This
// is an exchange segment, not a whole-tape reconstructor: the flat
// per-symbol level arrays that make single-book operations O(1) cost
// (band width) x 2 sides x sizeof(Level) per symbol, so bands are a
// deliberate, visible choice at registration. Dozens to hundreds of
// books are cheap with sane bands; eight thousand full-range books are
// not, and pretending otherwise would just move the failure to runtime.
//
// Handlers are per-symbol (default-constructed) so event streams stay
// attributable: handler(sym) is the sink that engine at(sym) fires
// into. Symbol names follow ITCH convention: at most 8 characters,
// space-padded internally, so lookups from parsed feed messages need no
// normalization.
#include <array>
#include <cstring>
#include <deque>
#include <string>
#include <unordered_map>

#include "matching_engine.hpp"
#include "types.hpp"

namespace matchbook {

using SymbolId = uint32_t;
inline constexpr SymbolId kInvalidSymbol = 0xFFFFFFFFu;

template <typename Handler = NullHandler>
class Venue {
public:
    using Engine = MatchingEngine<Handler>;

    // Register a book for `name` covering [min_price, max_price]. If the
    // symbol already exists, returns its id and ignores the band (books
    // are not re-shaped mid-session).
    SymbolId add_symbol(const char* name, Price min_price, Price max_price,
                        size_t expected_orders = 1 << 16) {
        std::string key = pad(name);
        auto it = by_name_.find(key);
        if (it != by_name_.end()) return it->second;
        SymbolId id = static_cast<SymbolId>(engines_.size());
        handlers_.emplace_back();
        engines_.emplace_back(min_price, max_price, handlers_.back(),
                              expected_orders);
        std::array<char, 9> n{};
        std::memcpy(n.data(), key.data(), 8);
        names_.push_back(n);
        by_name_.emplace(std::move(key), id);
        return id;
    }

    SymbolId find(const char* name) const {
        auto it = by_name_.find(pad(name));
        return it == by_name_.end() ? kInvalidSymbol : it->second;
    }

    size_t size() const noexcept { return engines_.size(); }

    Engine& at(SymbolId s) { return engines_[s]; }
    const Engine& at(SymbolId s) const { return engines_[s]; }
    Handler& handler(SymbolId s) { return handlers_[s]; }
    const Handler& handler(SymbolId s) const { return handlers_[s]; }
    const char* name(SymbolId s) const { return names_[s].data(); }

private:
    static std::string pad(const char* name) {
        std::string key(8, ' ');
        size_t n = std::strlen(name);
        std::memcpy(key.data(), name, n < 8 ? n : 8);
        return key;
    }

    std::unordered_map<std::string, SymbolId> by_name_;
    std::deque<Handler> handlers_;   // stable addresses: engines hold refs
    std::deque<Engine> engines_;     // deque: engines are not movable
    std::deque<std::array<char, 9>> names_;
};

}  // namespace matchbook
