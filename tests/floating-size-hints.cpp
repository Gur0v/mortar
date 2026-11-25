#include "../mortar/size-hints.hpp"

#include <cstdlib>

void check(bool value) {
    if (!value) std::abort();
}

int main() {
    uint32_t h[18]{};
    check((floating_hinted_size(100, 80, nullptr, 0) == std::pair{100u, 80u}));
    h[0] = 16 | 32 | 64 | 256;
    h[5] = 30; h[6] = 20; h[7] = 95; h[8] = 75;
    h[9] = 10; h[10] = 8; h[15] = 5; h[16] = 3;
    check((floating_hinted_size(100, 80, h, 18) == std::pair{95u, 75u}));
    check((floating_hinted_size(10, 10, h, 18) == std::pair{35u, 27u}));
    h[0] = 128;
    h[11] = 1; h[12] = 1; h[13] = 1; h[14] = 1;
    check((floating_hinted_size(100, 80, h, 18) == std::pair{90u, 90u}));
    h[11] = UINT32_MAX;
    check((floating_hinted_size(100, 80, h, 18) == std::pair{100u, 80u}));
    check((floating_hinted_size(0, UINT32_MAX, nullptr, 0) == std::pair{1u, 65535u}));
    h[0] = 16 | 32;
    h[5] = 100; h[6] = 100; h[7] = 20; h[8] = 20;
    check((floating_hinted_size(50, 50, h, 18) == std::pair{100u, 100u}));
}
