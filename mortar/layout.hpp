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
enum class Direction { left, right, up, down };

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
    bool insert_after(std::uint32_t focused, Layout child);
    bool set_split(std::uint32_t focused, Split direction);
    std::uint32_t neighbor(std::uint32_t focused, Direction direction) const;
    bool focus(std::uint32_t window);
    bool remove(std::uint32_t window);
    bool move(std::uint32_t window, Direction direction);
    bool resize(std::uint32_t window, Direction direction, int pixels, Rect bounds);
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
    std::vector<std::size_t> focus_order_;
    std::vector<double> weights_;
    bool resized_ = false;
};

}
