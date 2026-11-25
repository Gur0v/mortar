#pragma once

#include "layout.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace mortar {

struct Output {
    std::string name;
    Rect bounds;
    std::uint32_t workspace;
    bool operator==(const Output &) const = default;
};

struct OutputGeometry {
    std::string name;
    Rect bounds;
};

class Outputs {
public:
    bool update(const std::vector<OutputGeometry> &outputs);
    bool focus_output(const std::string &name);
    std::optional<std::uint32_t> adjacent_workspace(bool forward) const;
    bool switch_workspace(std::uint32_t workspace);

    const std::vector<Output> &outputs() const { return outputs_; }
    const std::array<std::optional<std::string>, 10> &owners() const { return owners_; }
    std::optional<std::string> focused_output() const { return focused_; }
    std::uint32_t focused_workspace() const { return focused_workspace_; }

private:
    std::vector<Output> outputs_;
    std::array<std::optional<std::string>, 10> owners_{};
    std::optional<std::string> focused_;
    std::uint32_t focused_workspace_ = 0;
};

}
