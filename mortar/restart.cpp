#include "restart.hpp"

#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <unordered_set>

namespace mortar {
namespace {

[[noreturn]] void invalid() { throw std::invalid_argument("invalid restart snapshot"); }

struct Codec {
    std::vector<std::uint8_t> output;
    std::span<const std::uint8_t> input;
    std::size_t position = 0;
    std::size_t nodes = 0;
    std::unordered_set<std::uint32_t> windows;

    void put(std::uint64_t value, unsigned count) {
        if (output.size() > restart_max_bytes - count) invalid();
        for (unsigned i = 0; i < count; ++i) output.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
    }

    std::uint64_t get(unsigned count) {
        if (count > input.size() - position) invalid();
        std::uint64_t value = 0;
        for (unsigned i = 0; i < count; ++i) value |= std::uint64_t(input[position++]) << (8 * i);
        return value;
    }

    void validate(const LayoutSnapshot &node, std::size_t depth) {
        if (depth > restart_max_depth || ++nodes > restart_max_nodes ||
            (node.direction != Split::horizontal && node.direction != Split::vertical)) invalid();
        if (node.leaf) {
            if (!node.children.empty() || !node.focus_order.empty() || !node.weights.empty() || node.resized ||
                node.direction != Split::horizontal || !windows.insert(node.window).second) invalid();
            return;
        }
        const auto count = node.children.size();
        if (node.window != 0 || count > restart_max_nodes - nodes ||
            node.focus_order.size() != count || node.weights.size() != count) invalid();
        std::vector<bool> seen(count);
        for (auto index : node.focus_order) {
            if (index >= count || seen[index]) invalid();
            seen[index] = true;
        }
        double total = 0;
        for (double weight : node.weights) {
            if (!(weight > 0 && weight <= std::numeric_limits<double>::max())) invalid();
            total += weight;
        }
        if (total > std::numeric_limits<double>::max()) invalid();
    }

    void encode(const LayoutSnapshot &node, std::size_t depth) {
        validate(node, depth);
        put(node.leaf, 1);
        put(node.direction == Split::vertical, 1);
        put(node.resized, 1);
        put(node.window, 4);
        put(node.children.size(), 4);
        for (auto index : node.focus_order) put(index, 4);
        for (double weight : node.weights) put(std::bit_cast<std::uint64_t>(weight), 8);
        for (const auto &child : node.children) encode(child, depth + 1);
    }

    LayoutSnapshot decode(std::size_t depth) {
        if (depth > restart_max_depth || nodes >= restart_max_nodes) invalid();
        ++nodes;
        auto leaf = get(1);
        auto direction = get(1);
        auto resized = get(1);
        if (leaf > 1 || direction > 1 || resized > 1) invalid();
        auto window = static_cast<std::uint32_t>(get(4));
        auto count = static_cast<std::size_t>(get(4));
        if (count > restart_max_nodes - nodes || count > (input.size() - position) / 12) invalid();
        LayoutSnapshot node{leaf != 0, window, direction ? Split::vertical : Split::horizontal, {}, {}, {}, resized != 0};
        node.focus_order.reserve(count);
        node.weights.reserve(count);
        for (std::size_t i = 0; i < count; ++i) node.focus_order.push_back(static_cast<std::size_t>(get(4)));
        for (std::size_t i = 0; i < count; ++i) node.weights.push_back(std::bit_cast<double>(get(8)));
        node.children.reserve(count);
        for (std::size_t i = 0; i < count; ++i) node.children.push_back(decode(depth + 1));
        return node;
    }
};

static_assert(sizeof(double) == sizeof(std::uint64_t) && std::numeric_limits<double>::is_iec559);

}

std::vector<std::uint8_t> encode_layout_snapshot(const LayoutSnapshot &snapshot) {
    Codec codec;
    codec.put(0x3154524d, 4);
    codec.put(1, 4);
    codec.encode(snapshot, 0);
    return codec.output;
}

LayoutSnapshot decode_layout_snapshot(std::span<const std::uint8_t> bytes) {
    if (bytes.size() > restart_max_bytes) invalid();
    Codec codec;
    codec.input = bytes;
    if (codec.get(4) != 0x3154524d || codec.get(4) != 1) invalid();
    auto result = codec.decode(0);
    if (codec.position != bytes.size()) invalid();
    codec.nodes = 0;
    codec.validate(result, 0);
    auto validate_children = [&](auto &&self, const LayoutSnapshot &node, std::size_t depth) -> void {
        for (const auto &child : node.children) {
            codec.validate(child, depth + 1);
            self(self, child, depth + 1);
        }
    };
    validate_children(validate_children, result, 0);
    return result;
}

}
