#include "layout.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace mortar {

Layout::Layout(bool leaf, std::uint32_t window, Split direction, std::vector<Layout> children)
    : leaf_(leaf), window_(window), direction_(direction), children_(std::move(children)) {}

Layout Layout::leaf(std::uint32_t window) {
    return Layout(true, window, Split::horizontal, {});
}

Layout Layout::split(Split direction, std::vector<Layout> children) {
    if (direction != Split::horizontal && direction != Split::vertical) {
        throw std::invalid_argument("invalid split direction");
    }
    Layout result(false, 0, direction, {});
    for (auto &child : children) {
        if (!result.insert(std::move(child))) throw std::invalid_argument("duplicate window");
    }
    return result;
}

void Layout::identities(std::vector<std::uint32_t> &result) const {
    if (leaf_) result.push_back(window_);
    for (const auto &child : children_) child.identities(result);
}

bool Layout::contains(std::uint32_t window) const {
    if (leaf_) return window_ == window;
    return std::any_of(children_.begin(), children_.end(),
                       [window](const Layout &child) { return child.contains(window); });
}

bool Layout::insert(Layout child) {
    if (leaf_) return false;
    std::vector<std::uint32_t> existing;
    std::vector<std::uint32_t> incoming;
    identities(existing);
    child.identities(incoming);
    for (auto window : incoming) {
        if (std::find(existing.begin(), existing.end(), window) != existing.end()) return false;
    }
    children_.push_back(std::move(child));
    return true;
}

bool Layout::remove(std::uint32_t window) {
    for (auto it = children_.begin(); it != children_.end(); ++it) {
        if (it->leaf_ && it->window_ == window) {
            children_.erase(it);
            return true;
        }
        if (it->remove(window)) {
            if (!it->leaf_ && it->children_.empty()) children_.erase(it);
            return true;
        }
    }
    return false;
}

std::vector<Placement> Layout::geometry(Rect bounds) const {
    if (bounds.width < 0 || bounds.height < 0 ||
        bounds.x > std::numeric_limits<int>::max() - bounds.width ||
        bounds.y > std::numeric_limits<int>::max() - bounds.height) {
        throw std::invalid_argument("invalid layout bounds");
    }
    std::vector<Placement> result;
    arrange(bounds, result);
    return result;
}

void Layout::arrange(Rect bounds, std::vector<Placement> &result) const {
    if (leaf_) {
        result.push_back({window_, bounds});
        return;
    }
    const auto count = children_.size();
    for (std::size_t i = 0; i < count; ++i) {
        Rect child = bounds;
        const auto extent = direction_ == Split::horizontal ? bounds.width : bounds.height;
        const auto left = static_cast<int>(static_cast<std::uint64_t>(extent) * i / count);
        const auto right = static_cast<int>(static_cast<std::uint64_t>(extent) * (i + 1) / count);
        if (direction_ == Split::horizontal) {
            child.x += left;
            child.width = right - left;
        } else {
            child.y += left;
            child.height = right - left;
        }
        children_[i].arrange(child, result);
    }
}

}
