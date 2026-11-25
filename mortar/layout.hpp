#pragma once

#include <cstdint>
#include <vector>

namespace mortar {

struct Rect {
    int x;
    int y;
    int width;
    int height;
    bool operator==(const Rect &) const = default;
};

enum class Split { horizontal, vertical };

struct Placement {
    std::uint32_t window;
    Rect bounds;
    bool operator==(const Placement &) const = default;
};

class Layout {
public:
    static Layout leaf(std::uint32_t window);
    static Layout split(Split direction, std::vector<Layout> children = {});
    bool insert(Layout child);
    bool remove(std::uint32_t window);
    bool contains(std::uint32_t window) const;
    std::vector<Placement> geometry(Rect bounds) const;

private:
    Layout(bool leaf, std::uint32_t window, Split direction, std::vector<Layout> children);
    void arrange(Rect bounds, std::vector<Placement> &result) const;
    void identities(std::vector<std::uint32_t> &result) const;

    bool leaf_;
    std::uint32_t window_;
    Split direction_;
    std::vector<Layout> children_;
};

}
