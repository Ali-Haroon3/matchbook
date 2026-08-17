#pragma once
// Applies a normalized ITCH message stream to a MatchingEngine, tracking
// the ITCH order-reference -> engine order id mapping. Shared by the file
// replay and live multicast tools; tolerant of gaps in the stream
// (lifecycle messages for unknown refs are dropped), which is exactly what
// a lossy UDP feed needs.
#include <unordered_map>

#include "itch_parser.hpp"
#include "types.hpp"
#include "venue.hpp"

namespace matchbook::itch {

class BookBuilder {
public:
    BookBuilder() { refmap_.reserve(1 << 20); }

    template <typename Engine>
    void apply(Engine& e, const Message& m) {
        switch (m.type) {
            case MsgType::Add: {
                OrderId id = e.submit_limit(m.side, m.price, m.qty);
                if (id != kInvalidOrderId) refmap_[m.ref] = id;
                break;
            }
            case MsgType::Execute:
            case MsgType::Cancel: {
                auto it = refmap_.find(m.ref);
                if (it == refmap_.end()) break;
                // ITCH guarantees exec/cancel qty <= remaining, so reduce
                // keeps queue priority (reduce-to-zero removes the order
                // but returns true; the stale ref is dropped on next touch,
                // when the engine no longer knows the id).
                if (!e.reduce(it->second, m.qty)) refmap_.erase(it);
                break;
            }
            case MsgType::Delete: {
                auto it = refmap_.find(m.ref);
                if (it == refmap_.end()) break;
                e.cancel(it->second);
                refmap_.erase(it);
                break;
            }
            case MsgType::Replace: {
                auto it = refmap_.find(m.ref);
                if (it == refmap_.end()) break;
                OrderId id = it->second;
                // Only rebind the ref once modify succeeds: a rejected
                // replace (e.g. out-of-band price) must leave the order
                // reachable under its old ref, not orphan it in the engine.
                // Erase via `it` before the insert to avoid invalidation.
                if (!e.modify(id, m.price, m.qty)) break;
                refmap_.erase(it);
                refmap_[m.new_ref] = id;
                break;
            }
            case MsgType::Action: {
                // Trading action: mirror the venue's halt state so adds
                // published during a call phase rest without matching
                // (the pre-open book can legitimately be crossed).
                if (m.state == 'H') e.halt();
                else if (m.state == 'T') e.resume();
                break;
            }
            default:
                break;
        }
    }

private:
    std::unordered_map<uint64_t, OrderId> refmap_;
};

// Multi-symbol variant: routes a mixed feed across a Venue's books,
// tracking ref -> (symbol, order id). ITCH refs are day-unique across
// the whole feed, so one map serves every symbol.
//
// `auto_band` is the replay convenience: an Add naming an unknown
// symbol registers it on the spot with a band of +/- auto_band ticks
// around that first price (clamped below at 1). Zero means unknown
// symbols are ignored. Either way, an Add the target book rejects
// (out of band) never maps its ref, so later lifecycle messages for it
// drop harmlessly -- the same tolerance the single-book builder has for
// lossy feeds; dropped() counts both cases.
class VenueBookBuilder {
public:
    explicit VenueBookBuilder(Price auto_band = 0) : auto_band_(auto_band) {
        refmap_.reserve(1 << 20);
    }

    template <typename Handler>
    void apply(Venue<Handler>& v, const Message& m) {
        switch (m.type) {
            case MsgType::Add: {
                SymbolId s = v.find(m.stock);
                if (s == kInvalidSymbol) {
                    if (auto_band_ == 0) { ++dropped_; break; }
                    Price lo = (m.price > auto_band_) ? m.price - auto_band_
                                                      : 1;
                    s = v.add_symbol(m.stock, lo, m.price + auto_band_);
                }
                OrderId id = v.at(s).submit_limit(m.side, m.price, m.qty);
                if (id != kInvalidOrderId) refmap_[m.ref] = Loc{s, id};
                else ++dropped_;
                break;
            }
            case MsgType::Execute:
            case MsgType::Cancel: {
                auto it = refmap_.find(m.ref);
                if (it == refmap_.end()) break;
                if (!v.at(it->second.sym).reduce(it->second.id, m.qty))
                    refmap_.erase(it);
                break;
            }
            case MsgType::Delete: {
                auto it = refmap_.find(m.ref);
                if (it == refmap_.end()) break;
                v.at(it->second.sym).cancel(it->second.id);
                refmap_.erase(it);
                break;
            }
            case MsgType::Replace: {
                auto it = refmap_.find(m.ref);
                if (it == refmap_.end()) break;
                Loc loc = it->second;
                if (!v.at(loc.sym).modify(loc.id, m.price, m.qty)) break;
                refmap_.erase(it);
                refmap_[m.new_ref] = loc;
                break;
            }
            case MsgType::Action: {
                SymbolId s = v.find(m.stock);
                if (s == kInvalidSymbol) break;
                if (m.state == 'H') v.at(s).halt();
                else if (m.state == 'T') v.at(s).resume();
                break;
            }
            default:
                break;
        }
    }

    uint64_t dropped() const noexcept { return dropped_; }

private:
    struct Loc {
        SymbolId sym;
        OrderId id;
    };
    std::unordered_map<uint64_t, Loc> refmap_;
    Price auto_band_;
    uint64_t dropped_ = 0;
};

}  // namespace matchbook::itch
