#include "outputs.hpp"

#include <algorithm>
#include <limits>

namespace mortar {

bool Outputs::update(const std::vector<OutputGeometry> &geometries) {
    std::vector<OutputGeometry> active;
    for (const auto &geometry : geometries) {
        const auto &r = geometry.bounds;
        if (geometry.name.empty() || r.width <= 0 || r.height <= 0 ||
            r.x > std::numeric_limits<int>::max() - r.width ||
            r.y > std::numeric_limits<int>::max() - r.height) return false;
        if (std::count_if(geometries.begin(), geometries.end(), [&](const auto &other) {
                return other.name == geometry.name;
            }) != 1) return false;
        auto clone = std::find_if(active.begin(), active.end(), [&](const auto &other) {
            return other.bounds == r;
        });
        if (clone == active.end()) {
            active.push_back(geometry);
        } else if (std::any_of(outputs_.begin(), outputs_.end(), [&](const auto &output) {
                       return output.name == geometry.name;
                   }) && std::none_of(outputs_.begin(), outputs_.end(), [&](const auto &output) {
                       return output.name == clone->name;
                   })) {
            *clone = geometry;
        }
    }
    if (active.size() > owners_.size()) return false;

    Outputs next = *this;
    next.outputs_.clear();
    for (const auto &geometry : active) {
        auto old = std::find_if(outputs_.begin(), outputs_.end(), [&](const auto &output) {
            return output.name == geometry.name;
        });
        if (old != outputs_.end()) next.outputs_.push_back({geometry.name, geometry.bounds, old->workspace});
    }
    std::array<bool, 10> migrated{};
    for (std::size_t i = 0; i < next.owners_.size(); ++i) {
        auto &owner = next.owners_[i];
        if (owner && std::none_of(active.begin(), active.end(), [&](const auto &output) {
                return output.name == *owner;
            })) {
            owner.reset();
            migrated[i] = true;
        }
    }
    for (const auto &geometry : active) {
        if (std::any_of(next.outputs_.begin(), next.outputs_.end(), [&](const auto &output) {
                return output.name == geometry.name;
            })) continue;
        std::uint32_t workspace = 0;
        if (next.outputs_.empty() && !next.owners_[next.focused_workspace_]) {
            workspace = next.focused_workspace_;
        } else {
            while (workspace < next.owners_.size() && next.owners_[workspace]) ++workspace;
            if (workspace == next.owners_.size()) {
                workspace = 0;
                while (workspace < next.owners_.size() &&
                       (workspace == next.focused_workspace_ ||
                        std::any_of(next.outputs_.begin(), next.outputs_.end(), [&](const auto &output) {
                            return output.workspace == workspace;
                        }))) ++workspace;
                if (workspace == next.owners_.size()) return false;
            }
        }
        next.owners_[workspace] = geometry.name;
        next.outputs_.push_back({geometry.name, geometry.bounds, workspace});
    }
    if (next.outputs_.empty()) {
        next.focused_.reset();
    } else {
        if (!next.focused_ || std::none_of(next.outputs_.begin(), next.outputs_.end(), [&](const auto &output) {
                return output.name == *next.focused_;
            })) {
            auto visible = std::find_if(next.outputs_.begin(), next.outputs_.end(), [&](const auto &output) {
                return output.workspace == next.focused_workspace_;
            });
            if (visible != next.outputs_.end()) {
                next.focused_ = visible->name;
            } else {
                auto &first = next.outputs_.front();
                next.focused_ = first.name;
                next.owners_[next.focused_workspace_] = first.name;
                first.workspace = next.focused_workspace_;
            }
        }
        for (std::size_t i = 0; i < migrated.size(); ++i) {
            if (migrated[i] && !next.owners_[i]) next.owners_[i] = next.focused_;
        }
        auto focused = std::find_if(next.outputs_.begin(), next.outputs_.end(), [&](const auto &output) {
            return output.name == *next.focused_;
        });
        next.focused_workspace_ = focused->workspace;
    }
    *this = std::move(next);
    return true;
}

std::optional<std::uint32_t> Outputs::adjacent_workspace(bool forward) const {
    if (!focused_ || outputs_.size() < 2) return std::nullopt;
    auto current = std::find_if(outputs_.begin(), outputs_.end(), [&](const auto &output) {
        return output.name == *focused_;
    });
    if (current == outputs_.end()) return std::nullopt;
    auto index = static_cast<std::size_t>(current - outputs_.begin());
    return outputs_[(index + (forward ? 1 : outputs_.size() - 1)) % outputs_.size()].workspace;
}

bool Outputs::focus_output(const std::string &name) {
    auto output = std::find_if(outputs_.begin(), outputs_.end(), [&](const auto &item) {
        return item.name == name;
    });
    if (output == outputs_.end()) return false;
    focused_ = name;
    focused_workspace_ = output->workspace;
    return true;
}

bool Outputs::switch_workspace(std::uint32_t workspace) {
    if (workspace >= owners_.size() || !focused_) return false;
    auto visible = std::find_if(outputs_.begin(), outputs_.end(), [&](const auto &output) {
        return output.workspace == workspace;
    });
    if (visible != outputs_.end()) return focus_output(visible->name);
    auto output = std::find_if(outputs_.begin(), outputs_.end(), [&](const auto &item) {
        return item.name == *focused_;
    });
    owners_[workspace] = output->name;
    output->workspace = workspace;
    focused_workspace_ = workspace;
    return true;
}

}
