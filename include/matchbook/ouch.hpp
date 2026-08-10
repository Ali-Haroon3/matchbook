#pragma once
// OUCH-style binary order entry: the client-facing counterpart to the
// ITCH market-data side. Message shapes follow Nasdaq OUCH 4.2 -- fixed
// width, big-endian, one letter of type, 14-byte space-padded
// alphanumeric order tokens chosen by the client and never reused --
// with three documented deviations for matchbook semantics: prices are
// 8-byte signed ticks (not 4-byte 1/10000 dollars), time-in-force is
// the engine's one-byte enum (not seconds-to-live), and Enter carries a
// display quantity (OUCH 5.0 style), an owner id, and an STP policy so
// icebergs and self-trade prevention are reachable over the wire.
//
// Inbound (client -> venue)          Outbound (venue -> client)
//   'O' Enter    token,side,qty,       'A' Accepted  token,id,side,qty,px
//                px,tif,display,       'E' Executed  token,qty,px,match
//                owner,stp             'C' Canceled  token,reason
//   'U' Replace  old,new,qty,px        'U' Replaced  old,new,id,qty,px
//   'X' Cancel   token                 'J' Rejected  token,reason
//
// The two directions are separate streams (as on the real wire), so 'U'
// meaning Replace inbound and Replaced outbound is not ambiguous:
// decode_in() and decode_out() are distinct.
//
// Gateway wires the message stream to a MatchingEngine and owns the
// session state (token -> order id, single-use token enforcement, match
// numbering). Outbound messages come back as one byte vector per
// message, ready to frame into MoldUDP64 packets (mold::encode takes
// exactly this shape).
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "matching_engine.hpp"
#include "types.hpp"

namespace matchbook::ouch {

inline constexpr size_t kTokenLen = 14;

// Canceled reasons.
inline constexpr char kUserRequested = 'U';  // explicit Cancel message
inline constexpr char kKilledOnEntry = 'K';  // IOC/FOK/PostOnly/STP kill
inline constexpr char kSelfTrade     = 'S';  // STP removed a resting order

// Rejected reasons.
inline constexpr char kBadToken   = 'T';  // unknown, dead, or reused token
inline constexpr char kValidation = 'V';  // band/qty/engine refused

// Wire sizes (including the type byte).
inline constexpr size_t kEnterSize    = 1 + kTokenLen + 1 + 4 + 8 + 1 + 4 + 2 + 1;
inline constexpr size_t kReplaceSize  = 1 + 2 * kTokenLen + 4 + 8;
inline constexpr size_t kCancelSize   = 1 + kTokenLen;
inline constexpr size_t kAcceptedSize = 1 + kTokenLen + 8 + 1 + 4 + 8;
inline constexpr size_t kExecutedSize = 1 + kTokenLen + 4 + 8 + 8;
inline constexpr size_t kCanceledSize = 1 + kTokenLen + 1;
inline constexpr size_t kReplacedSize = 1 + 2 * kTokenLen + 8 + 4 + 8;
inline constexpr size_t kRejectedSize = 1 + kTokenLen + 1;

// --- primitives ----------------------------------------------------------

inline void put16(std::vector<uint8_t>& out, uint16_t v) {
    out.push_back(static_cast<uint8_t>(v >> 8));
    out.push_back(static_cast<uint8_t>(v));
}
inline void put32(std::vector<uint8_t>& out, uint32_t v) {
    for (int s = 24; s >= 0; s -= 8)
        out.push_back(static_cast<uint8_t>(v >> s));
}
inline void put64(std::vector<uint8_t>& out, uint64_t v) {
    for (int s = 56; s >= 0; s -= 8)
        out.push_back(static_cast<uint8_t>(v >> s));
}
inline uint16_t get16(const uint8_t* p) noexcept {
    return static_cast<uint16_t>((p[0] << 8) | p[1]);
}
inline uint32_t get32(const uint8_t* p) noexcept {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
           (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}
inline uint64_t get64(const uint8_t* p) noexcept {
    return (uint64_t(get32(p)) << 32) | get32(p + 4);
}

// Tokens on the wire are exactly kTokenLen bytes, space-padded.
inline void put_token(std::vector<uint8_t>& out, const char* token) {
    size_t n = std::strlen(token);
    if (n > kTokenLen) n = kTokenLen;
    out.insert(out.end(), token, token + n);
    out.insert(out.end(), kTokenLen - n, ' ');
}
inline void get_token(const uint8_t* p, char* dst) noexcept {
    std::memcpy(dst, p, kTokenLen);
    dst[kTokenLen] = '\0';
}

// --- inbound encode / decode ----------------------------------------------

inline void encode_enter(std::vector<uint8_t>& out, const char* token,
                         Side side, uint32_t qty, Price price,
                         TimeInForce tif = TimeInForce::GTC,
                         uint32_t display = 0, OwnerId owner = 0,
                         StpPolicy stp = StpPolicy::CancelTaker) {
    out.push_back('O');
    put_token(out, token);
    out.push_back(side == Side::Buy ? 'B' : 'S');
    put32(out, qty);
    put64(out, static_cast<uint64_t>(price));
    out.push_back(static_cast<uint8_t>(tif));
    put32(out, display);
    put16(out, owner);
    out.push_back(static_cast<uint8_t>(stp));
}

inline void encode_replace(std::vector<uint8_t>& out, const char* old_token,
                           const char* new_token, uint32_t qty, Price price) {
    out.push_back('U');
    put_token(out, old_token);
    put_token(out, new_token);
    put32(out, qty);
    put64(out, static_cast<uint64_t>(price));
}

inline void encode_cancel(std::vector<uint8_t>& out, const char* token) {
    out.push_back('X');
    put_token(out, token);
}

enum class InType : uint8_t { Enter, Replace, Cancel };

struct InMsg {
    InType      type;
    char        token[kTokenLen + 1];      // Enter/Cancel; Replace: old
    char        new_token[kTokenLen + 1];  // Replace only
    Side        side;
    uint32_t    qty;
    Price       price;
    TimeInForce tif;
    uint32_t    display;
    OwnerId     owner;
    StpPolicy   stp;
};

// Returns false for a truncated or unknown message (caller drops it).
inline bool decode_in(const uint8_t* p, size_t len, InMsg& out) noexcept {
    if (len == 0) return false;
    switch (p[0]) {
        case 'O': {
            if (len < kEnterSize) return false;
            out.type = InType::Enter;
            get_token(p + 1, out.token);
            out.side    = (p[15] == 'B') ? Side::Buy : Side::Sell;
            out.qty     = get32(p + 16);
            out.price   = static_cast<Price>(get64(p + 20));
            out.tif     = static_cast<TimeInForce>(p[28]);
            out.display = get32(p + 29);
            out.owner   = get16(p + 33);
            out.stp     = static_cast<StpPolicy>(p[35]);
            return out.tif <= TimeInForce::PostOnly &&
                   out.stp <= StpPolicy::CancelBoth;
        }
        case 'U': {
            if (len < kReplaceSize) return false;
            out.type = InType::Replace;
            get_token(p + 1, out.token);
            get_token(p + 1 + kTokenLen, out.new_token);
            out.qty   = get32(p + 1 + 2 * kTokenLen);
            out.price = static_cast<Price>(get64(p + 5 + 2 * kTokenLen));
            return true;
        }
        case 'X': {
            if (len < kCancelSize) return false;
            out.type = InType::Cancel;
            get_token(p + 1, out.token);
            return true;
        }
        default:
            return false;
    }
}

// --- outbound encode / decode ----------------------------------------------

inline void encode_accepted(std::vector<uint8_t>& out, const char* token,
                            OrderId id, Side side, uint32_t qty, Price price) {
    out.push_back('A');
    put_token(out, token);
    put64(out, id);
    out.push_back(side == Side::Buy ? 'B' : 'S');
    put32(out, qty);
    put64(out, static_cast<uint64_t>(price));
}

inline void encode_executed(std::vector<uint8_t>& out, const char* token,
                            uint32_t qty, Price price, uint64_t match) {
    out.push_back('E');
    put_token(out, token);
    put32(out, qty);
    put64(out, static_cast<uint64_t>(price));
    put64(out, match);
}

inline void encode_canceled(std::vector<uint8_t>& out, const char* token,
                            char reason) {
    out.push_back('C');
    put_token(out, token);
    out.push_back(static_cast<uint8_t>(reason));
}

inline void encode_replaced(std::vector<uint8_t>& out, const char* old_token,
                            const char* new_token, OrderId id, uint32_t qty,
                            Price price) {
    out.push_back('U');
    put_token(out, old_token);
    put_token(out, new_token);
    put64(out, id);
    put32(out, qty);
    put64(out, static_cast<uint64_t>(price));
}

inline void encode_rejected(std::vector<uint8_t>& out, const char* token,
                            char reason) {
    out.push_back('J');
    put_token(out, token);
    out.push_back(static_cast<uint8_t>(reason));
}

enum class OutType : uint8_t { Accepted, Executed, Canceled, Replaced, Rejected };

struct OutMsg {
    OutType  type;
    char     token[kTokenLen + 1];      // Replaced: old token
    char     new_token[kTokenLen + 1];  // Replaced only
    OrderId  id;
    Side     side;
    uint32_t qty;
    Price    price;
    uint64_t match;
    char     reason;
};

inline bool decode_out(const uint8_t* p, size_t len, OutMsg& out) noexcept {
    if (len == 0) return false;
    switch (p[0]) {
        case 'A': {
            if (len < kAcceptedSize) return false;
            out.type = OutType::Accepted;
            get_token(p + 1, out.token);
            out.id    = get64(p + 15);
            out.side  = (p[23] == 'B') ? Side::Buy : Side::Sell;
            out.qty   = get32(p + 24);
            out.price = static_cast<Price>(get64(p + 28));
            return true;
        }
        case 'E': {
            if (len < kExecutedSize) return false;
            out.type = OutType::Executed;
            get_token(p + 1, out.token);
            out.qty   = get32(p + 15);
            out.price = static_cast<Price>(get64(p + 19));
            out.match = get64(p + 27);
            return true;
        }
        case 'C': {
            if (len < kCanceledSize) return false;
            out.type = OutType::Canceled;
            get_token(p + 1, out.token);
            out.reason = static_cast<char>(p[15]);
            return true;
        }
        case 'U': {
            if (len < kReplacedSize) return false;
            out.type = OutType::Replaced;
            get_token(p + 1, out.token);
            get_token(p + 1 + kTokenLen, out.new_token);
            out.id    = get64(p + 1 + 2 * kTokenLen);
            out.qty   = get32(p + 9 + 2 * kTokenLen);
            out.price = static_cast<Price>(get64(p + 13 + 2 * kTokenLen));
            return true;
        }
        case 'J': {
            if (len < kRejectedSize) return false;
            out.type = OutType::Rejected;
            get_token(p + 1, out.token);
            out.reason = static_cast<char>(p[15]);
            return true;
        }
        default:
            return false;
    }
}

// --- session gateway --------------------------------------------------------

// One OUCH session driving one engine. Inbound messages go through
// on_message(); every response and unsolicited event (fills, STP
// cancels) is appended to the outbound queue as its own byte vector.
//
// Semantics, matching the real protocol where matchbook has the
// concepts: tokens are single-use for the life of the session (reuse is
// rejected, including reuse of a replaced-away or dead token); a
// replace carries a fresh token and the old one dies with it; a
// canceled/killed order frees nothing -- its token stays burned. Both
// sides of a fill get an Executed with a shared match number (this
// gateway is the venue, so the one session sees both). The engine is
// the default (direct-dispatch) variant: the handler only appends bytes,
// so it never needs to re-enter.
class Gateway {
public:
    struct Sink;   // engine event handler, defined below

    Gateway(Price min_price, Price max_price, size_t expected_orders = 1 << 20)
        : engine_(min_price, max_price, sink_, expected_orders) {
        sink_.gw = this;
        token_of_.push_back(std::string());  // id 0 is invalid
    }

    // Feed one inbound message. Returns false only for a malformed
    // buffer (nothing emitted); every well-formed message produces at
    // least one outbound response.
    bool on_message(const uint8_t* p, size_t len) {
        InMsg m;
        if (!decode_in(p, len, m)) return false;
        switch (m.type) {
            case InType::Enter:   handle_enter(m); break;
            case InType::Replace: handle_replace(m); break;
            case InType::Cancel:  handle_cancel(m); break;
        }
        return true;
    }

    // Outbound messages since the last call, oldest first. The shape
    // plugs straight into mold::encode for wire framing.
    std::vector<std::vector<uint8_t>> take_out() {
        std::vector<std::vector<uint8_t>> v;
        v.swap(out_);
        return v;
    }

    const MatchingEngine<Sink>& engine() const { return engine_; }

    // Engine event handler; public only because the engine type names it.
    struct Sink {
        Gateway* gw = nullptr;
        void on_accept(OrderId id, Side s, Price p, Qty q) {
            gw->ev_accept(id, s, p, q);
        }
        void on_trade(const Trade& t) { gw->ev_trade(t); }
        void on_cancel(OrderId id) { gw->ev_cancel(id); }
        void on_reject(OrderId) { gw->ev_reject(); }
    };

private:
    enum class Mode : uint8_t { Idle, Enter, Replace, Cancel };

    void emit(std::vector<uint8_t>&& m) { out_.push_back(std::move(m)); }

    const char* token_str(OrderId id) const {
        return (id < token_of_.size()) ? token_of_[id].c_str() : "";
    }

    void handle_enter(const InMsg& m) {
        // Tokens are single-use: reject reuse before touching the engine.
        if (by_token_.count(m.token)) {
            std::vector<uint8_t> r;
            encode_rejected(r, m.token, kBadToken);
            emit(std::move(r));
            return;
        }
        mode_ = Mode::Enter;
        pending_token_ = m.token;
        entered_id_ = kInvalidOrderId;
        OrderId id;
        if (m.display != 0 && m.display < m.qty) {
            id = engine_.submit_iceberg(m.side, m.price, m.qty, m.display,
                                        m.tif, m.owner, m.stp);
        } else {
            id = engine_.submit_limit(m.side, m.price, m.qty, m.tif, m.owner,
                                      m.stp);
        }
        mode_ = Mode::Idle;
        if (id != kInvalidOrderId) by_token_[m.token] = id;
    }

    void handle_cancel(const InMsg& m) {
        auto it = by_token_.find(m.token);
        if (it == by_token_.end() || it->second == kInvalidOrderId) {
            std::vector<uint8_t> r;
            encode_rejected(r, m.token, kBadToken);
            emit(std::move(r));
            return;
        }
        mode_ = Mode::Cancel;
        bool ok = engine_.cancel(it->second);
        mode_ = Mode::Idle;
        if (!ok) {  // already filled away: token burned, order long gone
            std::vector<uint8_t> r;
            encode_rejected(r, m.token, kBadToken);
            emit(std::move(r));
        }
    }

    void handle_replace(const InMsg& m) {
        std::vector<uint8_t> r;
        auto it = by_token_.find(m.token);
        if (it == by_token_.end() || it->second == kInvalidOrderId ||
            by_token_.count(m.new_token)) {
            encode_rejected(r, m.new_token, kBadToken);
            emit(std::move(r));
            return;
        }
        OrderId id = it->second;
        // Bind the new token up front so any re-entry executions print
        // under it; the Replaced ack is inserted *before* them below,
        // preserving wire order (ack, then fills).
        std::string old_token = token_of_[id];
        token_of_[id] = m.new_token;
        size_t pos = out_.size();
        mode_ = Mode::Replace;
        replace_id_ = id;
        bool ok = engine_.modify(id, m.price, m.qty);
        mode_ = Mode::Idle;
        if (!ok) {
            token_of_[id] = old_token;   // nothing happened; restore
            encode_rejected(r, m.new_token, kValidation);
            emit(std::move(r));
            return;
        }
        it->second = kInvalidOrderId;    // old token dies with the replace
        by_token_[m.new_token] = id;
        encode_replaced(r, old_token.c_str(), m.new_token, id,
                        m.qty, m.price);
        out_.insert(out_.begin() + static_cast<ptrdiff_t>(pos), std::move(r));
    }

    // --- engine events -> outbound messages -----------------------------

    void ev_accept(OrderId id, Side s, Price p, Qty q) {
        if (mode_ == Mode::Replace && id == replace_id_) return;  // folded
        if (mode_ != Mode::Enter) return;
        entered_id_ = id;
        if (token_of_.size() <= id) token_of_.resize(id + 1);
        token_of_[id] = pending_token_;
        std::vector<uint8_t> r;
        encode_accepted(r, pending_token_.c_str(), id, s,
                        static_cast<uint32_t>(q), p);
        emit(std::move(r));
    }

    void ev_trade(const Trade& t) {
        uint64_t match = ++match_seq_;
        std::vector<uint8_t> r1, r2;
        encode_executed(r1, token_str(t.taker), static_cast<uint32_t>(t.qty),
                        t.price, match);
        encode_executed(r2, token_str(t.maker), static_cast<uint32_t>(t.qty),
                        t.price, match);
        emit(std::move(r1));
        emit(std::move(r2));
    }

    void ev_cancel(OrderId id) {
        if (mode_ == Mode::Replace && id == replace_id_) return;  // folded
        char reason = kSelfTrade;  // someone else's order died: STP
        if (mode_ == Mode::Cancel) reason = kUserRequested;
        else if (mode_ == Mode::Enter && id == entered_id_)
            reason = kKilledOnEntry;
        std::vector<uint8_t> r;
        encode_canceled(r, token_str(id), reason);
        emit(std::move(r));
    }

    void ev_reject() {
        if (mode_ != Mode::Enter) return;
        std::vector<uint8_t> r;
        encode_rejected(r, pending_token_.c_str(), kValidation);
        emit(std::move(r));
    }

    Sink sink_;
    MatchingEngine<Sink> engine_;
    std::unordered_map<std::string, OrderId> by_token_;  // burned = kInvalid
    std::vector<std::string> token_of_;                  // id -> live token
    std::vector<std::vector<uint8_t>> out_;
    uint64_t match_seq_ = 0;
    Mode mode_ = Mode::Idle;
    std::string pending_token_;
    OrderId entered_id_ = kInvalidOrderId;
    OrderId replace_id_ = kInvalidOrderId;
};

}  // namespace matchbook::ouch
