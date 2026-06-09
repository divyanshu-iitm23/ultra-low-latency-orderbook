// itch_book_replay.cpp — reconstruct one symbol's order book from a real ITCH file.
// Pre-faults the mmap (MAP_POPULATE + madvise) so page faults are charged to LOAD,
// not interleaved into the parse loop — making the engine visible in a flamegraph.
// Build (profiling): g++ -O3 -g -fno-omit-frame-pointer -std=c++17 -march=native \
//                      -Iinclude src/orderbook.cpp itch_book_replay.cpp -o itch_book_replay_prof
#include "book_replay.hpp"
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <chrono>
using namespace orderbook;

int main(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s <itch_file> <SYMBOL>\n", argv[0]); return 1; }
    const char* path = argv[1];
    const char* sym  = argv[2];

    int fd = open(path, O_RDONLY);
    if (fd < 0) { perror("open"); return 1; }
    struct stat st; fstat(fd, &st);

    using clk = std::chrono::steady_clock;
    auto t0 = clk::now();

    // ---- LOAD phase: fault the whole file in up front ----
    // MAP_POPULATE pre-faults (read-ahead) the mapping at mmap() time, so faults are
    // charged here rather than interleaved into the parse loop.
    void* p = mmap(nullptr, st.st_size, PROT_READ, MAP_PRIVATE | MAP_POPULATE, fd, 0);
    if (p == MAP_FAILED) { perror("mmap"); return 1; }
    const uint8_t* data = (const uint8_t*)p;

    // hints: single sequential scan -> aggressive read-ahead, pages droppable behind us
    madvise(p, st.st_size, MADV_SEQUENTIAL);
    madvise(p, st.st_size, MADV_WILLNEED);

    // belt-and-suspenders: touch one byte per page so the parse loop is fault-free
    // for any file that fits in RAM (e.g. the 500 MB slice).
    volatile uint64_t touch = 0;
    for (size_t i = 0; i < (size_t)st.st_size; i += 4096) touch += data[i];
    (void)touch;

    auto t1 = clk::now();   // <-- everything ABOVE is LOAD; everything BELOW is PROCESS

    // ---- PROCESS phase: parse + drive the book (this is what we want to profile) ----
    OrderBook book(1, 500000, 1 << 21);
    BookReplay rp(book, sym);
    size_t n = itch::parseBuffer(data, (size_t)st.st_size,
                                 [&](const itch::Message& m){ rp.onMessage(m); });

    auto t2 = clk::now();

    auto ms = [](auto a, auto b){
        return (long)std::chrono::duration_cast<std::chrono::milliseconds>(b - a).count();
    };
    long load_ms = ms(t0, t1), proc_ms = ms(t1, t2);

    printf("=== ITCH replay: %s ===\n", sym);
    printf("file size            : %.1f MB\n", st.st_size / 1e6);
    printf("load (mmap+prefault) : %ld ms\n", load_ms);
    printf("process (parse+book) : %ld ms", proc_ms);
    if (proc_ms > 0) printf("   (%.1f M msg/s)", n / (proc_ms * 1000.0));
    printf("\nmodeled messages     : %zu\n", n);
    printf("  adds=%zu deletes=%zu reduces=%zu replaces=%zu\n",
           rp.added(), rp.deleted(), rp.reduced(), rp.replaced());
    printf("final book state for %s:\n", sym);
    printf("  live orders   : %zu\n", book.getTotalOrders());
    printf("  bid levels    : %zu   ask levels: %zu\n",
           book.getActiveBids(), book.getActiveAsks());
    if (book.getActiveBids() && book.getActiveAsks()) {
        printf("  best bid      : $%.2f\n", book.getBestBid() / 100.0);
        printf("  best ask      : $%.2f\n", book.getBestAsk() / 100.0);
        printf("  spread        : $%.2f\n", book.getSpread() / 100.0);
        printf("  crossed?      : %s\n",
               (book.getBestBid() < book.getBestAsk()) ? "no (good)" : "YES (bug/locked)");
    } else {
        printf("  (one side empty — wrong symbol, or full-day file drained to empty)\n");
    }

    munmap(p, st.st_size);
    close(fd);
    return 0;
}