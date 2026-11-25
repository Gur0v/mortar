#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <utility>

inline std::pair<uint32_t, uint32_t> floating_hinted_size(uint32_t width, uint32_t height,
                                                          const uint32_t *h, size_t count) {
    constexpr uint32_t limit = 65535;
    width = std::clamp(width, 1u, limit);
    height = std::clamp(height, 1u, limit);
    if (!h || count < 15) return {width, height};
    const auto positive = [](uint32_t v) { return v > 0 && v <= INT32_MAX; };
    const uint32_t flags = h[0];
    const auto bound = [&](size_t index, uint32_t flag, uint32_t fallback) {
        return (flags & flag) && positive(h[index]) ? std::min(h[index], limit) : fallback;
    };
    uint32_t minw = bound(5, 16, 1), minh = bound(6, 16, 1);
    uint32_t maxw = std::max(minw, bound(7, 32, limit));
    uint32_t maxh = std::max(minh, bound(8, 32, limit));
    uint32_t basew = count >= 17 && (flags & 256) && h[15] <= INT32_MAX ? h[15] : ((flags & 16) && positive(h[5]) ? h[5] : 0);
    uint32_t baseh = count >= 17 && (flags & 256) && h[16] <= INT32_MAX ? h[16] : ((flags & 16) && positive(h[6]) ? h[6] : 0);
    uint32_t incw = (flags & 64) && positive(h[9]) ? h[9] : 1;
    uint32_t inch = (flags & 64) && positive(h[10]) ? h[10] : 1;
    auto nearest = [](uint32_t target, uint32_t low, uint32_t high, uint32_t base, uint32_t inc) {
        if (base > high) return uint32_t{0};
        uint64_t first = base;
        if (first < low) first += (uint64_t{low} - first + inc - 1) / inc * inc;
        if (first > high) return uint32_t{0};
        uint64_t last = first + (high - first) / inc * inc;
        uint64_t value = std::clamp<uint64_t>(target, first, last);
        uint64_t below = first + (value - first) / inc * inc;
        uint64_t above = std::min(last, below + inc);
        return static_cast<uint32_t>(value - below <= above - value ? below : above);
    };
    uint32_t fallbackw = nearest(width, minw, maxw, basew, incw);
    uint32_t fallbackh = nearest(height, minh, maxh, baseh, inch);
    if (!fallbackw) fallbackw = std::clamp(width, minw, maxw);
    if (!fallbackh) fallbackh = std::clamp(height, minh, maxh);
    if (!(flags & 128) || !positive(h[11]) || !positive(h[12]) || !positive(h[13]) || !positive(h[14]) ||
        uint64_t{h[11]} * h[14] > uint64_t{h[13]} * h[12]) return {fallbackw, fallbackh};
    uint32_t bestw = 0, besth = 0;
    uint64_t best = UINT64_MAX;
    for (uint32_t w = minw; w <= maxw; ++w) {
        if (w < basew || (w - basew) % incw) continue;
        uint64_t low = (uint64_t{w} * h[14] + h[13] - 1) / h[13];
        uint64_t high = uint64_t{w} * h[12] / h[11];
        low = std::max<uint64_t>(low, minh);
        high = std::min<uint64_t>(high, maxh);
        if (low > high) continue;
        uint32_t candidate = nearest(height, static_cast<uint32_t>(low), static_cast<uint32_t>(high), baseh, inch);
        if (!candidate) continue;
        int64_t dw = int64_t{w} - width, dh = int64_t{candidate} - height;
        uint64_t distance = static_cast<uint64_t>(dw * dw + dh * dh);
        if (distance < best) { best = distance; bestw = w; besth = candidate; }
    }
    return bestw ? std::pair{bestw, besth} : std::pair{fallbackw, fallbackh};
}
