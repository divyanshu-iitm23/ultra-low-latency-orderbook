// itch_dump.cpp - golden-model dump for the RTL scoreboard.
//
// Decodes a framed ITCH stream (2-byte big-endian length per message) with the
// Phase-1 software parser and prints one canonical line per message -- every
// message, modeled or not, so the testbench can diff the hardware parser's
// output line-for-line. Built on demand by tb/test_itch_parser.py.
//

#include "../../include/itch_parser.hpp"
#include <cstdio>
#include <cstdlib>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: itch_dump <framed.bin> [max_msgs]\n");
        return 1;
    }
    std::FILE* f = std::fopen(argv[1], "rb");
    if (!f) { std::perror(argv[1]); return 1; }
    std::fseek(f, 0, SEEK_END);
    long fsize = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> data(static_cast<size_t>(fsize), 0);
    if (fsize > 0 && std::fread(data.data(), 1, data.size(), f) != data.size()) {
        std::fprintf(stderr, "short read\n");
        return 1;
    }
    std::fclose(f);

    size_t maxn = (argc > 2) ? std::strtoull(argv[2], nullptr, 10) : size_t(-1);

    // same framing walk as itch::parseBuffer, but report EVERY message
    size_t off = 0, n = 0;
    while (off + 2 <= data.size() && n < maxn) {
        uint16_t len = itch::rdU16(data.data() + off);
        if (len == 0) break;                          // padding / end
        if (off + 2 + size_t(len) > data.size()) break;

        itch::Message m;
        bool modeled = itch::decode(data.data() + off + 2, len, m);

        std::printf("typ=%02x mod=%d ts=%llu ref=%llu nref=%llu side=%02x sh=%u px=%u stock=",
                    unsigned(uint8_t(m.type)), modeled ? 1 : 0,
                    (unsigned long long)m.timestamp,
                    (unsigned long long)m.orderRef,
                    (unsigned long long)m.newOrderRef,
                    unsigned(uint8_t(m.side)), m.shares, m.price);
        for (int i = 0; i < 8; ++i)
            std::printf("%02x", uint8_t(m.rawStock[i]));
        std::printf("\n");

        off += 2 + len;
        ++n;
    }
    return 0;
}
