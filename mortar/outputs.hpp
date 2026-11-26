#pragma once

#include "layout.hpp"

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
    static Outputs restore(const std::vector<Output> &outputs,
                           const std::vector<std::optional<std::string>> &owners,
                           const std::optional<std::string> &focused, std::uint32_t workspace);
    bool update(const std::vector<OutputGeometry> &outputs);
    bool focus_output(const std::string &name);
    std::optional<std::uint32_t> adjacent_workspace(bool forward) const;
    bool switch_workspace(std::uint32_t workspace);
    bool ensure(std::uint32_t workspace);

    const std::vector<Output> &outputs() const { return outputs_; }
    const std::vector<std::optional<std::string>> &owners() const { return owners_; }
    std::optional<std::string> focused_output() const { return focused_; }
    std::uint32_t focused_workspace() const { return focused_workspace_; }

private:
    std::vector<Output> outputs_;
    std::vector<std::optional<std::string>> owners_ = std::vector<std::optional<std::string>>(10);
    std::optional<std::string> focused_;
    std::uint32_t focused_workspace_ = 0;
};

}
