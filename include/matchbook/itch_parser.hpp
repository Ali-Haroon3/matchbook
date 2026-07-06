#pragma once
// Minimal Nasdaq TotalView-ITCH 5.0 parser for the standard file format
// (each message prefixed by a 2-byte big-endian length). Handles the order
// lifecycle messages needed to reconstruct a book -- A, F, E, C, X, D, U --
// and skips everything else by length.
//
// Works on real Nasdaq sample-day files (emma.nasdaq.com hosts them) as
// well as the synthetic files produced by tools/itchgen.
#include <cstdint>
#include <cstring>

#include "types.hpp"

namespace matchbook::itch {

// Normalized message handed from the feed thread to the matching thread.
enum class MsgType : uint8_t { Add, Execute, Cancel, Delete, Replace, Other };

struct Message {
    MsgType  type;
    uint64_t ref;        // ITCH order reference number
    uint64_t new_ref;    // Replace only
    Side     side;       // Add only
    Qty      qty;
    Price    price;      // in ITCH units: price * 10000 (i.e. 1/100 cent)
    char     stock[9];   // Add only, space-padded, NUL-terminated here
};

inline uint16_t be16(const uint8_t* p) noexcept {
    return static_cast<uint16_t>((p[0] << 8) | p[1]);
}
inline uint32_t be32(const uint8_t* p) noexcept {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
           (uint32_t(p[2]) << 8)  |  uint32_t(p[3]);
}
inline uint64_t be64(const uint8_t* p) noexcept {
    return (uint64_t(be32(p)) << 32) | be32(p + 4);
}

// Parse one ITCH message body (without the 2-byte length prefix).
// Returns false for message types we don't track (caller just skips them).
inline bool parse(const uint8_t* body, size_t len, Message& out) noexcept {
    if (len == 0) return false;
    switch (body[0]) {
        case 'A':    // Add Order (no MPID), 36 bytes
        case 'F': {  // Add Order with MPID, 40 bytes
            if (len < 36) return false;
            out.type = MsgType::Add;
            out.ref  = be64(body + 11);
            out.side = (body[19] == 'B') ? Side::Buy : Side::Sell;
            out.qty  = be32(body + 20);
            std::memcpy(out.stock, body + 24, 8);
            out.stock[8] = '\0';
            out.price = static_cast<Price>(be32(body + 32));
            return true;
        }
        case 'E': {  // Order Executed, 31 bytes
            if (len < 31) return false;
            out.type = MsgType::Execute;
            out.ref  = be64(body + 11);
            out.qty  = be32(body + 19);
            return true;
        }
        case 'C': {  // Order Executed With Price, 36 bytes
            if (len < 36) return false;
            out.type  = MsgType::Execute;
            out.ref   = be64(body + 11);
            out.qty   = be32(body + 19);
            out.price = static_cast<Price>(be32(body + 32));
            return true;
        }
        case 'X': {  // Order Cancel (partial), 23 bytes
            if (len < 23) return false;
            out.type = MsgType::Cancel;
            out.ref  = be64(body + 11);
            out.qty  = be32(body + 19);
            return true;
        }
        case 'D': {  // Order Delete, 19 bytes
            if (len < 19) return false;
            out.type = MsgType::Delete;
            out.ref  = be64(body + 11);
            return true;
        }
        case 'U': {  // Order Replace, 35 bytes
            if (len < 35) return false;
            out.type    = MsgType::Replace;
            out.ref     = be64(body + 11);
            out.new_ref = be64(body + 19);
            out.qty     = be32(body + 27);
            out.price   = static_cast<Price>(be32(body + 31));
            return true;
        }
        default:
            out.type = MsgType::Other;
            return false;
    }
}

}  // namespace matchbook::itch
