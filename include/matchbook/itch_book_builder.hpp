#pragma once
// Applies a normalized ITCH message stream to a MatchingEngine, tracking
// the ITCH order-reference -> engine order id mapping. Shared by the file
// replay and live multicast tools; tolerant of gaps in the stream
// (lifecycle messages for unknown refs are dropped), which is exactly what
// a lossy UDP feed needs.
#include <unordered_map>

#include "itch_parser.hpp"
#include "types.hpp"

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
            default:
                break;
        }
    }

private:
    std::unordered_map<uint64_t, OrderId> refmap_;
};

}  // namespace matchbook::itch
