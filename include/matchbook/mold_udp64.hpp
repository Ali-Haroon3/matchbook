#pragma once
// MoldUDP64 framing (Nasdaq MoldUDP64 1.00): the UDP transport ITCH is
// multicast over. Packet layout:
//
//   10 bytes  session (alpha, space-padded)
//    8 bytes  sequence number of the first message block (big-endian)
//    2 bytes  message count (big-endian)
//   then `count` blocks of: 2-byte big-endian length + payload
//
// count == 0 with no blocks is a heartbeat; count == 0xFFFF signals end
// of session. Downstream packets can be lost, duplicated, or reordered,
// so a receiver tracks the expected sequence number (SequenceTracker)
// and skips already-seen message blocks.
#include <cstdint>
#include <cstring>
#include <vector>

#include "itch_parser.hpp"  // be16/be64

namespace matchbook::mold {

inline constexpr size_t   kHeaderSize   = 20;
inline constexpr uint16_t kEndOfSession = 0xFFFF;

struct Header {
    char     session[11];  // NUL-terminated copy of the 10-byte field
    uint64_t seq;
    uint16_t count;
};

// Decode one packet, invoking cb(msg_seq, payload, len) per message block.
// Returns the number of blocks delivered, 0 for heartbeat/end-of-session
// (check hdr.count for which), or -1 for a malformed packet (truncated
// header, or a block running past the end).
template <typename Callback>
inline int64_t decode(const uint8_t* pkt, size_t len, Header& hdr,
                      Callback&& cb) {
    if (len < kHeaderSize) return -1;
    std::memcpy(hdr.session, pkt, 10);
    hdr.session[10] = '\0';
    hdr.seq   = itch::be64(pkt + 10);
    hdr.count = itch::be16(pkt + 18);
    if (hdr.count == kEndOfSession) return 0;
    size_t off = kHeaderSize;
    for (uint16_t i = 0; i < hdr.count; ++i) {
        if (off + 2 > len) return -1;
        size_t mlen = itch::be16(pkt + off);
        off += 2;
        if (off + mlen > len) return -1;
        cb(hdr.seq + i, pkt + off, mlen);
        off += mlen;
    }
    return hdr.count;
}

inline void put16(std::vector<uint8_t>& out, uint16_t v) {
    out.push_back(static_cast<uint8_t>(v >> 8));
    out.push_back(static_cast<uint8_t>(v));
}
inline void put64(std::vector<uint8_t>& out, uint64_t v) {
    for (int s = 56; s >= 0; s -= 8)
        out.push_back(static_cast<uint8_t>(v >> s));
}

// Append a packet framing `msgs` to `out`. An empty `msgs` encodes a
// heartbeat. `session` is space-padded/truncated to 10 bytes.
inline void encode(const char* session, uint64_t seq,
                   const std::vector<std::vector<uint8_t>>& msgs,
                   std::vector<uint8_t>& out) {
    char pad[10];
    std::memset(pad, ' ', sizeof(pad));
    std::memcpy(pad, session, std::strlen(session) < 10 ? std::strlen(session) : 10);
    out.insert(out.end(), pad, pad + 10);
    put64(out, seq);
    put16(out, static_cast<uint16_t>(msgs.size()));
    for (const auto& m : msgs) {
        put16(out, static_cast<uint16_t>(m.size()));
        out.insert(out.end(), m.begin(), m.end());
    }
}

// End-of-session packet: count = 0xFFFF, no blocks.
inline void encode_end(const char* session, uint64_t seq,
                       std::vector<uint8_t>& out) {
    size_t start = out.size();
    encode(session, seq, {}, out);
    out[start + 18] = 0xFF;
    out[start + 19] = 0xFF;
}

// Sequence/gap accounting across packets. MoldUDP64 sequences start at 1
// and each message block consumes one. A real receiver would request
// retransmission on a gap; here we count the missed messages and resync
// forward, which is the right behavior for lossy replay too.
class SequenceTracker {
public:
    // Call per data packet. Returns how many leading blocks were already
    // seen (skip that many; == count means the whole packet is a
    // duplicate). If the packet starts ahead of the expected sequence,
    // the jumped-over messages are added to gap_messages().
    uint16_t on_packet(uint64_t seq, uint16_t count) noexcept {
        if (seq > expected_) {
            gap_msgs_ += seq - expected_;
            expected_ = seq;
        }
        uint64_t skip = expected_ - seq;
        if (skip >= count) return count;  // stale resend, nothing new
        expected_ = seq + count;
        return static_cast<uint16_t>(skip);
    }

    uint64_t expected() const noexcept { return expected_; }
    uint64_t gap_messages() const noexcept { return gap_msgs_; }

private:
    uint64_t expected_ = 1;
    uint64_t gap_msgs_ = 0;
};

}  // namespace matchbook::mold
