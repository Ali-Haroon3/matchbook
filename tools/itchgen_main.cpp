// Generates a synthetic ITCH 5.0 file (standard 2-byte big-endian length
// framing) so the replay pipeline can be demoed offline. The message
// layouts match the real spec, so `replay` works identically on Nasdaq
// sample-day files.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace {

struct XorShift {
    uint64_t s;
    explicit XorShift(uint64_t seed) : s(seed) {}
    uint64_t next() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; }
    uint64_t next(uint64_t n) { return next() % n; }
};

void put16(std::vector<uint8_t>& b, uint16_t v) {
    b.push_back(v >> 8); b.push_back(v & 0xFF);
}
void put32(std::vector<uint8_t>& b, uint32_t v) {
    for (int i = 3; i >= 0; --i) b.push_back((v >> (8 * i)) & 0xFF);
}
void put48(std::vector<uint8_t>& b, uint64_t v) {
    for (int i = 5; i >= 0; --i) b.push_back((v >> (8 * i)) & 0xFF);
}
void put64(std::vector<uint8_t>& b, uint64_t v) {
    for (int i = 7; i >= 0; --i) b.push_back((v >> (8 * i)) & 0xFF);
}

void write_msg(std::ofstream& out, const std::vector<uint8_t>& body) {
    uint8_t len[2] = {static_cast<uint8_t>(body.size() >> 8),
                      static_cast<uint8_t>(body.size() & 0xFF)};
    out.write(reinterpret_cast<const char*>(len), 2);
    out.write(reinterpret_cast<const char*>(body.data()),
              static_cast<std::streamsize>(body.size()));
}

}  // namespace

int main(int argc, char** argv) {
    const char* path = (argc > 1) ? argv[1] : "sample.itch";
    size_t n_msgs = (argc > 2) ? static_cast<size_t>(std::atoll(argv[2]))
                               : 2'000'000;
    const char stock[8] = {'M','B','T','E','S','T',' ',' '};

    std::ofstream out(path, std::ios::binary);
    if (!out) { std::fprintf(stderr, "cannot open %s\n", path); return 1; }

    XorShift rng(0xDEADBEEF);
    uint64_t next_ref = 1;
    uint64_t ts = 0;
    double mid = 100.0000;  // dollars; ITCH price = dollars * 10000

    struct Live { uint64_t ref; uint32_t shares; };
    std::vector<Live> live;
    live.reserve(1 << 16);

    std::vector<uint8_t> b;
    for (size_t i = 0; i < n_msgs; ++i) {
        b.clear();
        ts += 1000 + rng.next(5000);
        mid += (static_cast<double>(rng.next(200)) - 99.5) * 0.00003;
        uint64_t roll = rng.next(100);

        if (roll < 55 || live.empty()) {
            // 'A' Add Order, 36 bytes
            uint64_t ref = next_ref++;
            char side = (rng.next(2) == 0) ? 'B' : 'S';
            double off = (1 + rng.next(30)) * 0.0001;
            double px = (side == 'B') ? mid - off : mid + off;
            uint32_t shares = static_cast<uint32_t>((1 + rng.next(10)) * 100);
            b.push_back('A');
            put16(b, 1);            // stock locate
            put16(b, 0);            // tracking
            put48(b, ts);
            put64(b, ref);
            b.push_back(static_cast<uint8_t>(side));
            put32(b, shares);
            b.insert(b.end(), stock, stock + 8);
            put32(b, static_cast<uint32_t>(px * 10000.0));
            live.push_back({ref, shares});
        } else if (roll < 75) {
            // 'D' Order Delete, 19 bytes
            size_t k = rng.next(live.size());
            b.push_back('D');
            put16(b, 1); put16(b, 0); put48(b, ts);
            put64(b, live[k].ref);
            live[k] = live.back(); live.pop_back();
        } else if (roll < 88) {
            // 'E' Order Executed, 31 bytes
            size_t k = rng.next(live.size());
            uint32_t ex = 100;
            if (ex >= live[k].shares) ex = live[k].shares;
            b.push_back('E');
            put16(b, 1); put16(b, 0); put48(b, ts);
            put64(b, live[k].ref);
            put32(b, ex);
            put64(b, i);  // match number
            live[k].shares -= ex;
            if (live[k].shares == 0) {
                live[k] = live.back(); live.pop_back();
            }
        } else if (roll < 95) {
            // 'X' Order Cancel (partial), 23 bytes
            size_t k = rng.next(live.size());
            uint32_t cx = 100;
            if (cx >= live[k].shares) cx = live[k].shares;
            b.push_back('X');
            put16(b, 1); put16(b, 0); put48(b, ts);
            put64(b, live[k].ref);
            put32(b, cx);
            live[k].shares -= cx;
            if (live[k].shares == 0) {
                live[k] = live.back(); live.pop_back();
            }
        } else {
            // 'U' Order Replace, 35 bytes
            size_t k = rng.next(live.size());
            uint64_t new_ref = next_ref++;
            double off = (1 + rng.next(30)) * 0.0001;
            double px = mid + ((rng.next(2) == 0) ? -off : off);
            uint32_t shares = static_cast<uint32_t>((1 + rng.next(10)) * 100);
            b.push_back('U');
            put16(b, 1); put16(b, 0); put48(b, ts);
            put64(b, live[k].ref);
            put64(b, new_ref);
            put32(b, shares);
            put32(b, static_cast<uint32_t>(px * 10000.0));
            live[k] = {new_ref, shares};
        }
        write_msg(out, b);
    }
    std::printf("wrote %zu messages to %s\n", n_msgs, path);
    return 0;
}
