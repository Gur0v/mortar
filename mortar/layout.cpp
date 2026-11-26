#include "layout.hpp"

#include <algorithm>
#include <limits>
#include <unordered_set>
#include <stdexcept>
#include <utility>

namespace mortar {

Layout::Layout(bool leaf, std::uint32_t window, Split direction, std::vector<Layout> children)
    : leaf_(leaf), window_(window), direction_(direction), children_(std::move(children)) {}

LayoutSnapshot Layout::snapshot() const {
    LayoutSnapshot result{leaf_, window_, direction_, {}, focus_order_, weights_, resized_};
    for (const auto &child : children_) result.children.push_back(child.snapshot());
    return result;
}

Layout Layout::restore(const LayoutSnapshot &snapshot) {
    std::unordered_set<std::uint32_t> windows;
    auto restore_node = [&](auto &&self, const LayoutSnapshot &node, std::size_t depth) -> Layout {
        if (depth > 1024 || (node.direction != Split::horizontal && node.direction != Split::vertical))
            throw std::invalid_argument("invalid layout snapshot");
        if (node.leaf) {
            if (!node.children.empty() || !node.focus_order.empty() || !node.weights.empty() ||
                node.resized || node.direction != Split::horizontal || !windows.insert(node.window).second)
                throw std::invalid_argument("invalid layout snapshot");
            return Layout::leaf(node.window);
        }
        if (node.window != 0 || node.focus_order.size() != node.children.size() ||
            node.weights.size() != node.children.size())
            throw std::invalid_argument("invalid layout snapshot");
        std::vector<bool> seen(node.children.size());
        for (auto index : node.focus_order) {
            if (index >= seen.size() || seen[index]) throw std::invalid_argument("invalid layout snapshot");
            seen[index] = true;
        }
        double total = 0;
        for (double weight : node.weights) {
            if (!(weight > 0 && weight <= std::numeric_limits<double>::max())) throw std::invalid_argument("invalid layout snapshot");
            total += weight;
        }
        if (total > std::numeric_limits<double>::max()) throw std::invalid_argument("invalid layout snapshot");
        Layout result(false, 0, node.direction, {});
        for (const auto &child : node.children) result.children_.push_back(self(self, child, depth + 1));
        result.focus_order_ = node.focus_order;
        result.weights_ = node.weights;
        result.resized_ = node.resized;
        return result;
    };
    return restore_node(restore_node, snapshot, 0);
}

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
    selected_path_.reset();
    child.selected_path_.reset();
    focus_order_.push_back(children_.size());
    children_.push_back(std::move(child));
    double total = 0;
    for (double weight : weights_) total += weight;
    weights_.push_back(weights_.empty() ? 1.0 : total / weights_.size());
    return true;
}

bool Layout::insert_after(std::uint32_t focused, Layout child) {
    if (leaf_) return false;
    if (selected_path_) {
        std::vector<std::uint32_t> incoming;
        child.identities(incoming);
        for (auto window : incoming) if (contains(window)) return false;
        Layout *node = this;
        for (auto index : *selected_path_) {
            if (index >= node->children_.size()) return false;
            node = &node->children_[index];
        }
        const auto index = node->focus_order_.empty() ? 0 : node->focus_order_.front() + 1;
        if (!node->insert(std::move(child))) return false;
        std::rotate(node->children_.begin() + index, node->children_.end() - 1, node->children_.end());
        std::rotate(node->weights_.begin() + index, node->weights_.end() - 1, node->weights_.end());
        for (auto &entry : node->focus_order_) {
            if (entry == node->children_.size() - 1) entry = index;
            else if (entry >= index) ++entry;
        }
        selected_path_.reset();
        return true;
    }
    std::vector<std::uint32_t> incoming;
    child.identities(incoming);
    for (auto window : incoming)
        if (contains(window)) return false;
    auto insert_at_focus = [&](auto &&self, Layout &node) -> bool {
        for (auto it = node.children_.begin(); it != node.children_.end(); ++it) {
            if (it->leaf_ && it->window_ == focused) {
                auto index = static_cast<std::size_t>(it - node.children_.begin()) + 1;
                for (auto &entry : node.focus_order_) if (entry >= index) ++entry;
                node.focus_order_.push_back(index);
                node.children_.insert(it + 1, std::move(child));
                if (node.resized_) {
                    double total = 0;
                    for (double weight : node.weights_) total += weight;
                    node.weights_.insert(node.weights_.begin() + index, total / node.weights_.size());
                } else {
                    node.weights_.insert(node.weights_.begin() + index, 1.0);
                }
                return true;
            }
            if (!it->leaf_ && self(self, *it)) return true;
        }
        return false;
    };
    if (!focused) {
        if (!incoming.empty() && !children_.empty()) return false;
        focus_order_.push_back(children_.size());
        children_.push_back(std::move(child));
        weights_.assign(children_.size(), 1.0);
        return true;
    }
    return insert_at_focus(insert_at_focus, *this);
}

bool Layout::set_split(std::uint32_t focused, Split direction) {
    if (direction != Split::horizontal && direction != Split::vertical) return false;
    if (selected_path_) {
        Layout *node = this;
        Layout *parent = nullptr;
        for (auto index : *selected_path_) {
            if (index >= node->children_.size()) return false;
            parent = node;
            node = &node->children_[index];
        }
        if (!parent) {
            if (node->children_.size() > 1) {
                Layout inner = std::move(*node);
                inner.selected_path_.reset();
                *node = Layout::split(direction, {std::move(inner)});
                node->selected_path_ = std::vector<std::size_t>{0};
            } else node->direction_ = direction;
        } else if (parent->children_.size() == 1) parent->direction_ = direction;
        else {
            *node = Layout::split(direction, {std::move(*node)});
            selected_path_->push_back(0);
        }
        return true;
    }
    if (!focused && !leaf_ && children_.empty()) {
        direction_ = direction;
        return true;
    }
    for (auto &child : children_) {
        if (child.leaf_ && child.window_ == focused) {
            if (children_.size() == 1) direction_ = direction;
            else child = Layout::split(direction, {std::move(child)});
            return true;
        }
        if (!child.leaf_ && child.set_split(focused, direction)) return true;
    }
    return false;
}

bool Layout::toggle_split(std::uint32_t focused) {
    auto path = selected_path_ ? selected_path_ : window_path(focused);
    if (!path) {
        if (focused || leaf_ || !children_.empty()) return false;
    } else if (!path->empty()) {
        path->pop_back();
    }
    Layout *node = this;
    if (path) {
        for (auto index : *path) {
            if (index >= node->children_.size()) return false;
            node = &node->children_[index];
        }
    }
    if (node->leaf_) return false;
    node->direction_ = node->direction_ == Split::horizontal ? Split::vertical : Split::horizontal;
    return true;
}

bool Layout::focus(std::uint32_t window) {
    if (leaf_) return window_ == window;
    for (std::size_t i = 0; i < children_.size(); ++i) {
        if (!children_[i].focus(window)) continue;
        selected_path_.reset();
        auto it = std::find(focus_order_.begin(), focus_order_.end(), i);
        std::rotate(focus_order_.begin(), it, it + 1);
        return true;
    }
    return false;
}

bool Layout::select_parent(std::uint32_t window) {
    auto path = selected_path_ ? selected_path_ : window_path(window);
    if (!path || path->empty()) return false;
    path->pop_back();
    selected_path_ = std::move(path);
    return true;
}

bool Layout::select_child() {
    if (!selected_path_) return false;
    const Layout *node = this;
    for (auto index : *selected_path_) {
        if (index >= node->children_.size()) return false;
        node = &node->children_[index];
    }
    if (node->leaf_ || node->focus_order_.empty()) return false;
    selected_path_->push_back(node->focus_order_.front());
    if (node->children_[selected_path_->back()].leaf_) selected_path_.reset();
    return true;
}

std::optional<std::vector<std::size_t>> Layout::window_path(std::uint32_t window) const {
    std::vector<std::size_t> path;
    auto find = [&](auto &&self, const Layout &node) -> bool {
        if (node.leaf_) return node.window_ == window;
        for (std::size_t i = 0; i < node.children_.size(); ++i) {
            path.push_back(i);
            if (self(self, node.children_[i])) return true;
            path.pop_back();
        }
        return false;
    };
    if (!find(find, *this)) return std::nullopt;
    return path;
}

std::optional<std::vector<std::size_t>> Layout::parent_path(const std::vector<std::size_t> &path) const {
    if (path.empty()) return std::nullopt;
    const Layout *node = this;
    for (auto index : path) {
        if (index >= node->children_.size()) return std::nullopt;
        node = &node->children_[index];
    }
    auto parent = path;
    parent.pop_back();
    return parent;
}

std::uint32_t Layout::neighbor(std::uint32_t focused, Direction direction, bool allow_wrap) const {
    if (direction != Direction::left && direction != Direction::right &&
        direction != Direction::up && direction != Direction::down) return 0;
    const bool previous = direction == Direction::left || direction == Direction::up;
    const Split axis = direction == Direction::left || direction == Direction::right
                           ? Split::horizontal : Split::vertical;
    const Layout *wrap = nullptr;
    const Layout *next = nullptr;
    auto find = [&](auto &&self, const Layout &node) -> bool {
        if (node.leaf_) return node.window_ == focused;
        for (std::size_t i = 0; i < node.children_.size(); ++i) {
            if (!self(self, node.children_[i])) continue;
            if (!next && node.direction_ == axis && node.children_.size() > 1) {
                if ((previous && i > 0) || (!previous && i + 1 < node.children_.size()))
                    next = &node.children_[previous ? i - 1 : i + 1];
                else if (!wrap)
                    wrap = &node.children_[previous ? node.children_.size() - 1 : 0];
            }
            return true;
        }
        return false;
    };
    if (selected_path_) {
        const Layout *node = this;
        for (auto i : *selected_path_) {
            if (i >= node->children_.size()) return 0;
            if (node->direction_ == axis && node->children_.size() > 1) {
                if ((previous && i > 0) || (!previous && i + 1 < node->children_.size()))
                    next = &node->children_[previous ? i - 1 : i + 1];
                else
                    wrap = &node->children_[previous ? node->children_.size() - 1 : 0];
            }
            node = &node->children_[i];
        }
    } else if (!find(find, *this)) return 0;
    const Layout *target = next ? next : (allow_wrap ? wrap : nullptr);
    while (target && !target->leaf_) {
        if (target->children_.empty()) return 0;
        target = &target->children_[target->focus_order_.front()];
    }
    return target ? target->window_ : 0;
}

std::uint32_t Layout::sibling(std::uint32_t focused, bool previous) const {
    auto path = selected_path_ ? selected_path_ : window_path(focused);
    if (!path || path->empty()) return 0;
    const Layout *parent = this;
    for (std::size_t i = 0; i + 1 < path->size(); ++i) parent = &parent->children_[(*path)[i]];
    auto index = path->back();
    if (previous ? index == 0 : index + 1 == parent->children_.size()) return 0;
    const Layout *target = &parent->children_[previous ? index - 1 : index + 1];
    while (!target->leaf_) {
        if (target->children_.empty()) return 0;
        target = &target->children_[target->focus_order_.front()];
    }
    return target->window_;
}

bool Layout::remove(std::uint32_t window) {
    for (std::size_t i = 0; i < children_.size(); ++i) {
        auto &child = children_[i];
        bool found = child.leaf_ ? child.window_ == window : child.remove(window);
        if (!found) continue;
        selected_path_.reset();
        if (child.leaf_ || child.children_.empty()) {
            children_.erase(children_.begin() + i);
            weights_.erase(weights_.begin() + i);
            std::erase(focus_order_, i);
            for (auto &entry : focus_order_) if (entry > i) --entry;
        }
        return true;
    }
    return false;
}

bool Layout::move(std::uint32_t window, Direction direction) {
    if (direction != Direction::left && direction != Direction::right &&
        direction != Direction::up && direction != Direction::down) return false;
    const bool previous = direction == Direction::left || direction == Direction::up;
    const Split axis = direction == Direction::left || direction == Direction::right
                           ? Split::horizontal : Split::vertical;
    if (selected_path_) {
        auto &path = *selected_path_;
        for (std::size_t depth = path.size(); depth > 0; --depth) {
            Layout *parent = this;
            for (std::size_t n = 0; n + 1 < depth; ++n) parent = &parent->children_[path[n]];
            const auto i = path[depth - 1];
            if (parent->direction_ != axis || (previous ? i == 0 : i + 1 == parent->children_.size())) continue;
            const auto other = previous ? i - 1 : i + 1;
            if (depth == path.size()) {
                std::swap(parent->children_[i], parent->children_[other]);
                std::swap(parent->weights_[i], parent->weights_[other]);
                for (auto &entry : parent->focus_order_) {
                    if (entry == i) entry = other;
                    else if (entry == other) entry = i;
                }
                path.back() = other;
                return true;
            }
            Layout *selected_parent = this;
            for (std::size_t n = 0; n + 1 < path.size(); ++n) selected_parent = &selected_parent->children_[path[n]];
            auto selected_index = path.back();
            auto selected = std::move(selected_parent->children_[selected_index]);
            selected_parent->children_.erase(selected_parent->children_.begin() + selected_index);
            selected_parent->weights_.erase(selected_parent->weights_.begin() + selected_index);
            std::erase(selected_parent->focus_order_, selected_index);
            for (auto &entry : selected_parent->focus_order_) if (entry > selected_index) --entry;
            bool branch_removed = false;
            for (std::size_t level = path.size() - 1; level >= depth; --level) {
                Layout *ancestor = this;
                for (std::size_t n = 0; n + 1 < level; ++n) ancestor = &ancestor->children_[path[n]];
                auto child_index = path[level - 1];
                if (!ancestor->children_[child_index].children_.empty()) break;
                if (level == depth) {
                    branch_removed = true;
                    break;
                }
                ancestor->children_.erase(ancestor->children_.begin() + child_index);
                ancestor->weights_.erase(ancestor->weights_.begin() + child_index);
                std::erase(ancestor->focus_order_, child_index);
                for (auto &entry : ancestor->focus_order_) if (entry > child_index) --entry;
            }
            auto target = other;
            if (branch_removed) {
                parent->children_.erase(parent->children_.begin() + i);
                parent->weights_.erase(parent->weights_.begin() + i);
                std::erase(parent->focus_order_, i);
                for (auto &entry : parent->focus_order_) if (entry > i) --entry;
                if (i < target) --target;
            }
            auto index = target + (previous ? 0 : 1);
            auto weight = parent->weights_[target] / 2;
            parent->weights_[target] = weight;
            parent->children_.insert(parent->children_.begin() + index, std::move(selected));
            parent->weights_.insert(parent->weights_.begin() + index, weight);
            for (auto &entry : parent->focus_order_) if (entry >= index) ++entry;
            parent->focus_order_.insert(parent->focus_order_.begin(), index);
            path.resize(depth);
            path.back() = index;
            return true;
        }
        return false;
    }
    auto move_in = [&](auto &&self, Layout &node) -> bool {
        for (std::size_t i = 0; i < node.children_.size(); ++i) {
            auto &child = node.children_[i];
            if (!child.contains(window)) continue;
            if (self(self, child)) return true;
            if (node.direction_ != axis) return false;
            const bool edge = previous ? i == 0 : i + 1 == node.children_.size();
            if (edge && child.leaf_) return false;
            const auto other = edge ? i : (previous ? i - 1 : i + 1);
            if (edge) {
                std::vector<std::uint32_t> windows;
                child.identities(windows);
                if (windows.size() <= 1) return false;
            }
            if (child.leaf_) {
                std::swap(node.children_[i], node.children_[other]);
                std::swap(node.weights_[i], node.weights_[other]);
                for (auto &entry : node.focus_order_) {
                    if (entry == i) entry = other;
                    else if (entry == other) entry = i;
                }
                return true;
            }
            if (!child.remove(window)) return false;
            const bool empty = child.children_.empty();
            if (empty) {
                node.children_.erase(node.children_.begin() + i);
                node.weights_.erase(node.weights_.begin() + i);
                std::erase(node.focus_order_, i);
                for (auto &entry : node.focus_order_) if (entry > i) --entry;
            }
            const auto target = other - (empty && i < other ? 1 : 0);
            const auto index = target + (previous ? 0 : 1);
            const double weight = node.weights_[target] / 2;
            node.weights_[target] = weight;
            node.weights_.insert(node.weights_.begin() + index, weight);
            node.children_.insert(node.children_.begin() + index, Layout::leaf(window));
            for (auto &entry : node.focus_order_) if (entry >= index) ++entry;
            node.focus_order_.insert(node.focus_order_.begin(), index);
            return true;
        }
        return false;
    };
    return move_in(move_in, *this);
}

bool Layout::resize(std::uint32_t window, Direction direction, int pixels, Rect bounds, bool clamp) {
    if (pixels == 0 || (direction != Direction::left && direction != Direction::right &&
                        direction != Direction::up && direction != Direction::down)) return false;
    if (bounds.width < 0 || bounds.height < 0 ||
        bounds.x > std::numeric_limits<int>::max() - bounds.width ||
        bounds.y > std::numeric_limits<int>::max() - bounds.height) {
        throw std::invalid_argument("invalid layout bounds");
    }
    const bool previous = direction == Direction::left || direction == Direction::up;
    const Split axis = direction == Direction::left || direction == Direction::right
                           ? Split::horizontal : Split::vertical;
    bool found = false;
    std::vector<std::size_t> path;
    auto resize_in = [&](auto &&self, Layout &node, Rect area) -> bool {
        if (node.leaf_) return false;
        double total = 0;
        for (double weight : node.weights_) total += weight;
        double accumulated = 0;
        const int extent = node.direction_ == Split::horizontal ? area.width : area.height;
        for (std::size_t i = 0; i < node.children_.size(); ++i) {
            const double before = accumulated;
            const int start = static_cast<int>(extent * (accumulated / total));
            accumulated += node.weights_[i];
            const int end = i + 1 == node.children_.size() ? extent :
                            static_cast<int>(extent * (accumulated / total));
            Rect child_area = area;
            if (node.direction_ == Split::horizontal) {
                child_area.x += start;
                child_area.width = end - start;
            } else {
                child_area.y += start;
                child_area.height = end - start;
            }
            if (selected_path_) {
                if (path.size() >= selected_path_->size() || (*selected_path_)[path.size()] != i) continue;
            } else if (!node.children_[i].contains(window)) continue;
            path.push_back(i);
            if ((!selected_path_ || path.size() < selected_path_->size()) && self(self, node.children_[i], child_area)) return true;
            path.pop_back();
            if (found || node.direction_ != axis || extent == 0 ||
                (clamp && extent > std::numeric_limits<std::uint16_t>::max()) ||
                (previous ? i == 0 : i + 1 == node.children_.size())) return false;
            found = true;
            const auto other = previous ? i - 1 : i + 1;
            if (clamp) {
                const int boundary = previous ? start : end;
                const auto requested = static_cast<std::int64_t>(boundary) +
                    (previous ? -static_cast<std::int64_t>(pixels) : pixels);
                const int wanted = static_cast<int>(std::clamp(requested, std::int64_t{0}, static_cast<std::int64_t>(extent)));
                const auto original_i = node.weights_[i];
                const auto original_other = node.weights_[other];
                const auto original_geometry = node.geometry(area);
                auto valid = [&](int target) {
                    const double boundary_weight = previous ? before : accumulated;
                    const double shift = ((static_cast<double>(target) + 0.5) / extent - boundary_weight / total) * total;
                    const double delta = previous ? -shift : shift;
                    node.weights_[i] = original_i + delta;
                    node.weights_[other] = original_other - delta;
                    if (!(node.weights_[i] > 0 && node.weights_[other] > 0 &&
                          node.weights_[i] <= std::numeric_limits<double>::max() &&
                          node.weights_[other] <= std::numeric_limits<double>::max())) return false;
                    double sum = 0;
                    double pos = 0;
                    for (std::size_t j = 0; j < node.weights_.size(); ++j) {
                        if (j == (previous ? i : i + 1)) pos = sum;
                        sum += node.weights_[j];
                    }
                    if (!(sum <= std::numeric_limits<double>::max())) return false;
                    const auto placements = node.geometry(area);
                    for (std::size_t j = 0; j < placements.size(); ++j) {
                        const int size = axis == Split::horizontal ? placements[j].bounds.width : placements[j].bounds.height;
                        const int original = axis == Split::horizontal ? original_geometry[j].bounds.width : original_geometry[j].bounds.height;
                        if (size < std::min(1, original)) return false;
                    }
                    return static_cast<int>(extent * (pos / sum)) == target;
                };
                const int step = wanted > boundary ? -1 : 1;
                int target = wanted;
                const auto limit = std::max<std::size_t>(1, 65536 / std::max<std::size_t>(1, original_geometry.size()));
                std::size_t attempts = 0;
                while (target != boundary && attempts < limit && !valid(target)) {
                    target += step;
                    ++attempts;
                }
                if (attempts == limit) target = boundary;
                if (target == boundary) {
                    node.weights_[i] = original_i;
                    node.weights_[other] = original_other;
                    return false;
                }
            } else {
                const double delta = (static_cast<double>(pixels) / extent) * total;
                if (node.weights_[i] + delta <= 0 || node.weights_[other] - delta <= 0) return false;
                node.weights_[i] += delta;
                node.weights_[other] -= delta;
            }
            node.resized_ = true;
            return true;
        }
        return false;
    };
    return resize_in(resize_in, *this, bounds);
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
    double total = 0;
    for (double weight : weights_) total += weight;
    double accumulated = 0;
    for (std::size_t i = 0; i < count; ++i) {
        Rect child = bounds;
        const auto extent = direction_ == Split::horizontal ? bounds.width : bounds.height;
        const auto left = static_cast<int>(extent * (accumulated / total));
        accumulated += weights_[i];
        const auto right = i + 1 == count ? extent : static_cast<int>(extent * (accumulated / total));
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
