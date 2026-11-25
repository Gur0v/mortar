#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mortarbar {
struct StatusBlock {
    std::string full_text;
    std::optional<std::string> short_text, color, background, border, name, instance, align, markup;
    std::optional<int> border_top, border_right, border_bottom, border_left, separator_block_width;
    std::optional<int> min_width;
    std::optional<std::string> min_width_text;
    bool urgent = false, separator = true;
};

struct ClickEvent {
    std::optional<std::string> name, instance;
    int button = 0, x = 0, y = 0, relative_x = 0, relative_y = 0, width = 0, height = 0;
    std::vector<std::string> modifiers;
};

class StatusParser {
public:
    using Callback = std::function<void(std::vector<StatusBlock>)>;
    bool feed(std::string_view input, const Callback& callback);
    bool finish(const Callback& callback);
    const std::string& error() const { return error_; }
    bool click_events() const { return click_events_; }
    int stop_signal() const { return stop_signal_; }
    int cont_signal() const { return cont_signal_; }
private:
    bool parse_frame(const Callback& callback);
    bool fail(const char* message);
    enum class State { header, text, outer, first, after_comma, next, frame, done } state_ = State::header;
    std::string frame_, error_;
    int depth_ = 0;
    bool quoted_ = false, escaped_ = false, click_events_ = false;
    int stop_signal_ = 0, cont_signal_ = 0;
};

std::string click_event_json(const ClickEvent& event, bool first);
}
