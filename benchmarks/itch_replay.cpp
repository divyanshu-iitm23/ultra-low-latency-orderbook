// itch_replay.cpp (sketch) — mmap a file and run it through the parser
#include "itch_parser.hpp"
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cstdio>
#include<initializer_list>

int main(int argc, char** argv) {
    int fd = open(argv[1], O_RDONLY);
    struct stat st; fstat(fd, &st);
    auto* data = (const uint8_t*)mmap(nullptr, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);

    size_t counts[128] = {0};
    size_t n = itch::parseBuffer(data, st.st_size, [&](const itch::Message& m){
        counts[(unsigned char)m.type & 127]++;
    });

    printf("modeled messages: %zu\n", n);
    for (char t : {'A','F','E','C','X','D','U'})
        printf("  %c : %zu\n", t, counts[(int)t]);

    munmap((void*)data, st.st_size);
    close(fd);
    return 0;
}