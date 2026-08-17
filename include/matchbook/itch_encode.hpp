#pragma once
// ITCH 5.0 message encoders: the outbound counterpart to itch_parser.hpp.
// Layouts match the real spec byte for byte (same offsets the parser
// reads), so a stream built from these functions replays through the
// existing parser/BookBuilder pipeline and through `replay` unchanged.
//
// Two conventions, documented once here: prices are written as the raw
// 32-bit ITCH price field (the engine's integer ticks cast to uint32_t --
// callers keep their band inside 32 bits, as the parser assumes in the
// other direction), and the 6-byte timestamp is whatever monotonic
// counter the caller supplies (the engine is deliberately clockless; a
// message sequence number keeps replays deterministic).
#include <cstdint>
#include <vector>

namespace matchbook::itch {

inline void enc16(std::vector<uint8_t>& b, uint16_t v) {
    b.push_back(static_cast<uint8_t>(v >> 8));
    b.push_back(static_cast<uint8_t>(v));
}
inline void enc32(std::vector<uint8_t>& b, uint32_t v) {
    for (int s = 24; s >= 0; s -= 8)
        b.push_back(static_cast<uint8_t>(v >> s));
}
inline void enc48(std::vector<uint8_t>& b, uint64_t v) {
    for (int s = 40; s >= 0; s -= 8)
        b.push_back(static_cast<uint8_t>(v >> s));
}
inline void enc64(std::vector<uint8_t>& b, uint64_t v) {
    for (int s = 56; s >= 0; s -= 8)
        b.push_back(static_cast<uint8_t>(v >> s));
}

// Shared 11-byte prefix: type, stock locate, tracking number, timestamp.
inline void enc_head(std::vector<uint8_t>& b, char type, uint64_t ts) {
    b.push_back(static_cast<uint8_t>(type));
    enc16(b, 1);
    enc16(b, 0);
    enc48(b, ts);
}

// 'A' Add Order (no MPID), 36 bytes.
inline void encode_add(std::vector<uint8_t>& b, uint64_t ts, uint64_t ref,
                       char side, uint32_t shares, const char stock[8],
                       uint32_t price) {
    enc_head(b, 'A', ts);
    enc64(b, ref);
    b.push_back(static_cast<uint8_t>(side));
    enc32(b, shares);
    b.insert(b.end(), stock, stock + 8);
    enc32(b, price);
}

// 'E' Order Executed, 31 bytes.
inline void encode_execute(std::vector<uint8_t>& b, uint64_t ts, uint64_t ref,
                           uint32_t shares, uint64_t match) {
    enc_head(b, 'E', ts);
    enc64(b, ref);
    enc32(b, shares);
    enc64(b, match);
}

// 'C' Order Executed With Price, 36 bytes. Auction/cross executions where
// the print price can differ from the order's limit price.
inline void encode_execute_px(std::vector<uint8_t>& b, uint64_t ts,
                              uint64_t ref, uint32_t shares, uint64_t match,
                              uint32_t price, bool printable = true) {
    enc_head(b, 'C', ts);
    enc64(b, ref);
    enc32(b, shares);
    enc64(b, match);
    b.push_back(printable ? 'Y' : 'N');
    enc32(b, price);
}

// 'X' Order Cancel (partial), 23 bytes.
inline void encode_cancel(std::vector<uint8_t>& b, uint64_t ts, uint64_t ref,
                          uint32_t shares) {
    enc_head(b, 'X', ts);
    enc64(b, ref);
    enc32(b, shares);
}

// 'D' Order Delete, 19 bytes.
inline void encode_delete(std::vector<uint8_t>& b, uint64_t ts, uint64_t ref) {
    enc_head(b, 'D', ts);
    enc64(b, ref);
}

// 'P' Trade (non-cross), 44 bytes: an execution against non-displayed
// liquidity. Reference number 0 marks it as against hidden interest.
inline void encode_trade(std::vector<uint8_t>& b, uint64_t ts, char side,
                         uint32_t shares, const char stock[8], uint32_t price,
                         uint64_t match) {
    enc_head(b, 'P', ts);
    enc64(b, 0);
    b.push_back(static_cast<uint8_t>(side));
    enc32(b, shares);
    b.insert(b.end(), stock, stock + 8);
    enc32(b, price);
    enc64(b, match);
}

// 'Q' Cross Trade, 40 bytes: the aggregate auction print. Non-displayed
// participation shows up here (the Q volume can exceed what the
// per-order 'C' executions account for).
inline void encode_cross(std::vector<uint8_t>& b, uint64_t ts,
                         uint64_t shares, const char stock[8], uint32_t price,
                         uint64_t match, char cross_type = 'O') {
    enc_head(b, 'Q', ts);
    enc64(b, shares);
    b.insert(b.end(), stock, stock + 8);
    enc32(b, price);
    enc64(b, match);
    b.push_back(static_cast<uint8_t>(cross_type));
}

// 'H' Stock Trading Action, 25 bytes. state 'H' = halted, 'T' = trading.
inline void encode_action(std::vector<uint8_t>& b, uint64_t ts,
                          const char stock[8], char state) {
    enc_head(b, 'H', ts);
    b.insert(b.end(), stock, stock + 8);
    b.push_back(static_cast<uint8_t>(state));
    b.push_back(' ');                       // reserved
    const char reason[4] = {' ', ' ', ' ', ' '};
    b.insert(b.end(), reason, reason + 4);
}

// Standard ITCH file framing: 2-byte big-endian length + message body.
inline void frame(std::vector<uint8_t>& out, const std::vector<uint8_t>& body) {
    enc16(out, static_cast<uint16_t>(body.size()));
    out.insert(out.end(), body.begin(), body.end());
}

}  // namespace matchbook::itch
