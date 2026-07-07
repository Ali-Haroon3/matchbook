// Live multicast replay over the MoldUDP64 codec: the wire-transport
// counterpart of the file replay tool.
//
//   mcast send <file.itch> [group] [port] [pps] [ifaddr]
//   mcast recv [group] [port] [SYMBOL] [ifaddr]
//
// ifaddr picks the interface carrying the feed (production multicast is
// always NIC-specific); 127.0.0.1 runs the whole loop on loopback.
//
// send  reads a length-prefixed ITCH file, frames messages into MoldUDP64
//       packets (MTU-sized batches), and multicasts them at `pps` packets
//       per second (0 = unpaced; on loopback that will drop packets, which
//       is a feature: it exercises the receiver's gap accounting).
//       Finishes with a burst of end-of-session packets.
//
// recv  joins the group, decodes packets, skips duplicate blocks and
//       counts gaps (SequenceTracker), parses ITCH, and reconstructs the
//       book through the same SPSC feed/matching split as `replay`.
//       Exits on end-of-session or after 5s of silence once data has
//       arrived; a lost end-of-session must not hang the receiver.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "matchbook/itch_book_builder.hpp"
#include "matchbook/itch_parser.hpp"
#include "matchbook/matching_engine.hpp"
#include "matchbook/mold_udp64.hpp"
#include "matchbook/spsc_ring.hpp"

using namespace matchbook;

namespace {

constexpr const char* kDefaultGroup = "239.192.0.1";
constexpr int         kDefaultPort  = 26400;
constexpr const char* kSession      = "MATCHBOOK";
// Batch until the next block would push the UDP payload past ~1400 bytes
// (safe under a 1500 MTU with IP + UDP headers).
constexpr size_t kMtuBudget = 1400;

struct Stats {
    uint64_t trades = 0;
    void on_accept(OrderId, Side, Price, Qty) {}
    void on_trade(const Trade&) { ++trades; }
    void on_cancel(OrderId) {}
    void on_reject(OrderId) {}
};

SpscRing<itch::Message, 1 << 16> g_ring;
std::atomic<bool> g_feed_done{false};

int usage(const char* argv0) {
    std::fprintf(stderr,
                 "usage: %s send <file.itch> [group] [port] [pps] [ifaddr]\n"
                 "       %s recv [group] [port] [SYMBOL] [ifaddr]\n",
                 argv0, argv0);
    return 1;
}

// Parses an interface address ("" -> INADDR_ANY). Returns false on garbage.
bool parse_ifaddr(const char* s, in_addr& out) {
    if (!s || !*s) { out.s_addr = htonl(INADDR_ANY); return true; }
    return inet_pton(AF_INET, s, &out) == 1;
}

// atoi accepts garbage silently; require a full numeric parse in [lo, hi].
bool parse_int(const char* s, long lo, long hi, int& out) {
    char* end;
    long v = std::strtol(s, &end, 10);
    if (*s == '\0' || *end != '\0' || v < lo || v > hi) return false;
    out = static_cast<int>(v);
    return true;
}

int run_send(const std::string& path, const char* group, int port, int pps,
             const char* ifaddr) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::fprintf(stderr, "cannot open %s\n", path.c_str());
        return 1;
    }

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { std::perror("socket"); return 1; }
    unsigned char ttl = 1, loop = 1;
    setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
    setsockopt(fd, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop));
    in_addr ifa;
    if (!parse_ifaddr(ifaddr, ifa)) {
        std::fprintf(stderr, "bad ifaddr %s\n", ifaddr);
        close(fd);
        return 1;
    }
    if (ifa.s_addr != htonl(INADDR_ANY))
        setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF, &ifa, sizeof(ifa));

    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(static_cast<uint16_t>(port));
    if (inet_pton(AF_INET, group, &dst.sin_addr) != 1) {
        std::fprintf(stderr, "bad group %s\n", group);
        close(fd);
        return 1;
    }

    // Split the delay across tv_sec/tv_nsec: nanosleep EINVALs on
    // tv_nsec >= 1e9, which pps=1 would otherwise hit.
    const long delay_ns = pps > 0 ? 1000000000L / pps : 0;
    const timespec gap{delay_ns / 1000000000L, delay_ns % 1000000000L};
    uint64_t seq = 1, msgs_sent = 0, pkts_sent = 0;
    std::vector<std::vector<uint8_t>> batch;
    std::vector<uint8_t> pkt;
    size_t batch_bytes = 0;

    auto flush = [&] {
        if (batch.empty()) return true;
        pkt.clear();
        mold::encode(kSession, seq, batch, pkt);
        if (sendto(fd, pkt.data(), pkt.size(), 0,
                   reinterpret_cast<sockaddr*>(&dst), sizeof(dst)) < 0) {
            std::perror("sendto");
            return false;
        }
        seq += batch.size();
        msgs_sent += batch.size();
        ++pkts_sent;
        batch.clear();
        batch_bytes = 0;
        if (gap.tv_sec || gap.tv_nsec) nanosleep(&gap, nullptr);
        return true;
    };

    uint8_t hdr[2];
    while (in.read(reinterpret_cast<char*>(hdr), 2)) {
        size_t len = itch::be16(hdr);
        std::vector<uint8_t> body(len);
        if (!in.read(reinterpret_cast<char*>(body.data()),
                     static_cast<std::streamsize>(len))) break;
        if (batch_bytes + len + 2 > kMtuBudget - mold::kHeaderSize)
            if (!flush()) { close(fd); return 1; }
        batch_bytes += len + 2;
        batch.push_back(std::move(body));
    }
    if (!flush()) { close(fd); return 1; }

    // End-of-session, a few times: any one arriving is enough.
    pkt.clear();
    mold::encode_end(kSession, seq, pkt);
    for (int i = 0; i < 3; ++i) {
        sendto(fd, pkt.data(), pkt.size(), 0,
               reinterpret_cast<sockaddr*>(&dst), sizeof(dst));
        timespec ts{0, 10000000};  // 10ms apart
        nanosleep(&ts, nullptr);
    }
    close(fd);

    std::printf("sent %llu messages in %llu packets to %s:%d\n",
                (unsigned long long)msgs_sent, (unsigned long long)pkts_sent,
                group, port);
    return 0;
}

struct RecvStats {
    uint64_t packets = 0, messages = 0, dup_blocks = 0, malformed = 0;
    uint64_t gaps = 0;
    bool failed = false;  // socket setup never came up
};

void recv_thread(const char* group, int port, const std::string& symbol,
                 const char* ifaddr, RecvStats* out) {
    // On any setup failure: flag it, wake the matching thread, bail.
    auto fail = [&](int fd) {
        if (fd >= 0) close(fd);
        out->failed = true;
        g_feed_done.store(true, std::memory_order_release);
    };

    ip_mreq mreq{};
    if (inet_pton(AF_INET, group, &mreq.imr_multiaddr) != 1) {
        std::fprintf(stderr, "bad group %s\n", group);
        return fail(-1);
    }
    if (!parse_ifaddr(ifaddr, mreq.imr_interface)) {
        std::fprintf(stderr, "bad ifaddr %s\n", ifaddr);
        return fail(-1);
    }

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { std::perror("socket"); return fail(-1); }
    int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
#ifdef SO_REUSEPORT
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &on, sizeof(on));
#endif

    // Bind to the group address, not INADDR_ANY: a wildcard-bound socket
    // also receives unicast to the port and other groups joined on the
    // host, and foreign packets would poison the sequence tracker.
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    addr.sin_addr = mreq.imr_multiaddr;
    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::perror("bind");
        return fail(fd);
    }
    if (setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
        std::perror("IP_ADD_MEMBERSHIP");
        return fail(fd);
    }

    timeval tv{1, 0};  // 1s poll so silence and EOS checks can run
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    char padded[9];
    std::snprintf(padded, sizeof(padded), "%-8s", symbol.c_str());

    mold::SequenceTracker tracker;
    std::vector<uint8_t> buf(65536);
    int idle_secs = 0;
    bool got_data = false, eos = false;

    while (!eos) {
        ssize_t n = recv(fd, buf.data(), buf.size(), 0);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // Wait forever for the first packet; once data has flowed,
                // 5s of silence means the end-of-session got lost.
                if (got_data && ++idle_secs >= 5) break;
                continue;
            }
            std::perror("recv");
            break;
        }
        idle_secs = 0;
        got_data = true;

        // Peek seq/count before decoding: the tracker's skip count has to
        // be known before the per-block callback starts firing.
        if (n < static_cast<ssize_t>(mold::kHeaderSize)) {
            ++out->malformed;
            continue;
        }
        const uint64_t seq = itch::be64(buf.data() + 10);
        const uint16_t count = itch::be16(buf.data() + 18);
        if (count == mold::kEndOfSession || count == 0) {
            // Heartbeat/end-of-session carry the next-expected sequence;
            // feeding it (as a zero-block packet) books any tail loss that
            // no later data packet would ever reveal.
            tracker.on_packet(seq, 0);
            ++out->packets;
            if (count == mold::kEndOfSession) { eos = true; break; }
            continue;
        }
        const uint16_t skip = tracker.on_packet(seq, count);

        mold::Header hdr;
        uint16_t block = 0;
        int64_t rc = mold::decode(
            buf.data(), static_cast<size_t>(n), hdr,
            [&](uint64_t, const uint8_t* body, size_t len) {
                if (block++ < skip) return;  // already seen (stale resend)
                itch::Message m;
                if (!itch::parse(body, len, m)) return;
                if (m.type == itch::MsgType::Add &&
                    std::memcmp(m.stock, padded, 8) != 0) return;
                while (!g_ring.try_push(m)) { /* backpressure */ }
            });
        // A malformed packet may have desynced the tracker (its count was
        // trusted above); the next good packet's seq resyncs it. Loss can't
        // truncate UDP datagrams, so this only guards a buggy sender.
        if (rc < 0) { ++out->malformed; continue; }
        ++out->packets;
        out->messages += static_cast<uint64_t>(rc) - skip;
        out->dup_blocks += skip;
    }

    out->gaps = tracker.gap_messages();
    close(fd);
    g_feed_done.store(true, std::memory_order_release);
}

int run_recv(const char* group, int port, const std::string& symbol,
             const char* ifaddr) {
    RecvStats rs;
    std::printf("listening on %s:%d for %s (exits on end-of-session or 5s silence)\n",
                group, port, symbol.c_str());
    std::thread feeder(recv_thread, group, port, symbol, ifaddr, &rs);

    Stats h;
    // Band: $0.0001 .. $2000.0000 in 1/10000-dollar ticks.
    MatchingEngine<Stats> e(1, 20'000'000, h, 1 << 21);
    itch::BookBuilder book;

    uint64_t applied = 0;
    itch::Message m;
    while (true) {
        if (!g_ring.try_pop(m)) {
            if (g_feed_done.load(std::memory_order_acquire) &&
                g_ring.size_approx() == 0) break;
            continue;
        }
        book.apply(e, m);
        ++applied;
    }
    feeder.join();
    if (rs.failed) return 1;  // setup error already on stderr

    std::printf("packets:     %llu (%llu malformed dropped)\n",
                (unsigned long long)rs.packets, (unsigned long long)rs.malformed);
    std::printf("messages:    %llu received, %llu lost in gaps, %llu dup blocks skipped\n",
                (unsigned long long)rs.messages, (unsigned long long)rs.gaps,
                (unsigned long long)rs.dup_blocks);
    std::printf("for %s: %llu messages applied\n", symbol.c_str(),
                (unsigned long long)applied);
    std::printf("trades:      %llu\n", (unsigned long long)h.trades);
    if (e.has_bid() && e.has_ask()) {
        std::printf("final book:  bid %.4f x ask %.4f, %zu open orders\n",
                    e.best_bid() / 10000.0, e.best_ask() / 10000.0,
                    e.open_orders());
    } else {
        std::printf("final book:  one or both sides empty, %zu open orders\n",
                    e.open_orders());
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) return usage(argv[0]);
    std::string mode = argv[1];

    if (mode == "send") {
        if (argc < 3) return usage(argv[0]);
        const char* group = (argc > 3) ? argv[3] : kDefaultGroup;
        int port = kDefaultPort, pps = 2000;
        if ((argc > 4 && !parse_int(argv[4], 1, 65535, port)) ||
            (argc > 5 && !parse_int(argv[5], 0, 1000000, pps)))
            return usage(argv[0]);
        const char* ifaddr = (argc > 6) ? argv[6] : "";
        return run_send(argv[2], group, port, pps, ifaddr);
    }
    if (mode == "recv") {
        const char* group = (argc > 2) ? argv[2] : kDefaultGroup;
        int port = kDefaultPort;
        if (argc > 3 && !parse_int(argv[3], 1, 65535, port))
            return usage(argv[0]);
        std::string symbol = (argc > 4) ? argv[4] : "MBTEST";
        const char* ifaddr = (argc > 5) ? argv[5] : "";
        return run_recv(group, port, symbol, ifaddr);
    }
    return usage(argv[0]);
}
