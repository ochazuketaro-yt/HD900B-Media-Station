#include "../src/common.h"
using namespace ms;
int main(int argc, char **argv) {
    if (argc > 1) { // hash a file: sha256 + xxh64
        FILE *f = fopen(argv[1], "rb"); static unsigned char b[1 << 20]; size_t n; Sha256 s; Xxh64 x;
        while ((n = fread(b, 1, sizeof b, f)) > 0) { s.update(b, n); x.update(b, n); }
        printf("%s %s\n", s.final_hex().c_str(), x.hex().c_str()); return 0;
    }
    Xxh64 x; printf("xxh64(\"\")=%s (expect ef46db3751d8e999)\n", x.hex().c_str());
    return 0;
}
