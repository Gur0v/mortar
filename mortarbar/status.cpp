#include "status.hpp"

#include <jv.h>

#include <csignal>
#include <limits>
#include <utility>

namespace mortarbar {
namespace {
constexpr std::size_t limit = 1024 * 1024;
constexpr int max_depth = 64;

struct Value {
    jv value;
    explicit Value(jv v) : value(v) {}
    ~Value() { jv_free(value); }
    Value(const Value&) = delete;
    Value& operator=(const Value&) = delete;
};

Value field(jv object, const char* name) { return Value(jv_object_get(jv_copy(object), jv_string(name))); }

bool string_field(jv object, const char* name, std::optional<std::string>& result) {
    auto v = field(object, name);
    if (jv_get_kind(v.value) == JV_KIND_INVALID || jv_get_kind(v.value) == JV_KIND_NULL) return !jv_object_has(jv_copy(object), jv_string(name));
    if (jv_get_kind(v.value) != JV_KIND_STRING) return false;
    result.emplace(jv_string_value(v.value), static_cast<std::size_t>(jv_string_length_bytes(jv_copy(v.value))));
    return true;
}

bool integer_field(jv object, const char* name, std::optional<int>& result) {
    auto v = field(object, name);
    if (jv_get_kind(v.value) == JV_KIND_INVALID || jv_get_kind(v.value) == JV_KIND_NULL) return !jv_object_has(jv_copy(object), jv_string(name));
    if (jv_get_kind(v.value) != JV_KIND_NUMBER) return false;
    double n = jv_number_value(v.value);
    if (!(n >= std::numeric_limits<int>::min() && n <= std::numeric_limits<int>::max())) return false;
    int number = static_cast<int>(n);
    if (number != n) return false;
    result = number;
    return true;
}

bool bool_field(jv object, const char* name, bool& result) {
    auto v = field(object, name);
    auto kind = jv_get_kind(v.value);
    if (kind == JV_KIND_INVALID || kind == JV_KIND_NULL) return !jv_object_has(jv_copy(object), jv_string(name));
    if (kind != JV_KIND_TRUE && kind != JV_KIND_FALSE) return false;
    result = kind == JV_KIND_TRUE;
    return true;
}

bool block(jv object, StatusBlock& b) {
    if (jv_get_kind(object) != JV_KIND_OBJECT) return false;
    std::optional<std::string> text;
    if (!string_field(object, "full_text", text) || !text) return false;
    b.full_text = std::move(*text);
    if (!string_field(object, "short_text", b.short_text) || !string_field(object, "color", b.color) ||
        !string_field(object, "background", b.background) || !string_field(object, "border", b.border) ||
        !string_field(object, "name", b.name) || !string_field(object, "instance", b.instance) ||
        !string_field(object, "align", b.align) || !string_field(object, "markup", b.markup) ||
        !integer_field(object, "border_top", b.border_top) || !integer_field(object, "border_right", b.border_right) ||
        !integer_field(object, "border_bottom", b.border_bottom) || !integer_field(object, "border_left", b.border_left) ||
        !integer_field(object, "separator_block_width", b.separator_block_width) ||
        !bool_field(object, "urgent", b.urgent) || !bool_field(object, "separator", b.separator)) return false;
    auto width = field(object, "min_width");
    if (jv_get_kind(width.value) == JV_KIND_STRING) return string_field(object, "min_width", b.min_width_text);
    return integer_field(object, "min_width", b.min_width);
}

bool whitespace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
}

bool StatusParser::fail(const char* message) { error_ = message; return false; }

bool StatusParser::parse_frame(const Callback& callback) {
    Value value(jv_parse_sized(frame_.data(), static_cast<int>(frame_.size())));
    frame_.clear();
    if (jv_get_kind(value.value) != JV_KIND_ARRAY) return fail("invalid status JSON array");
    int count = jv_array_length(jv_copy(value.value));
    if (count > 4096) return fail("too many status blocks");
    std::vector<StatusBlock> blocks;
    blocks.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        Value item(jv_array_get(jv_copy(value.value), i));
        StatusBlock b;
        if (!block(item.value, b)) return fail("invalid status block");
        blocks.push_back(std::move(b));
    }
    state_ = State::next;
    callback(std::move(blocks));
    return true;
}

bool StatusParser::feed(std::string_view input, const Callback& callback) {
    if (!error_.empty()) return false;
    for (char c : input) {
        if (state_ == State::header) {
            if (c != '\n') {
                if (frame_.size() == limit) return fail("header too large");
                frame_ += c;
                continue;
            }
            Value header(jv_parse_sized(frame_.data(), static_cast<int>(frame_.size())));
            frame_.clear();
            if (jv_get_kind(header.value) != JV_KIND_OBJECT) return fail("invalid header JSON");
            std::optional<int> version, stop, cont;
            if (!integer_field(header.value, "version", version) || version != 1 ||
                !integer_field(header.value, "stop_signal", stop) || !integer_field(header.value, "cont_signal", cont) ||
                !bool_field(header.value, "click_events", click_events_) ||
                (stop && (*stop < 0 || *stop >= NSIG)) || (cont && (*cont < 0 || *cont >= NSIG))) return fail("invalid header fields");
            stop_signal_ = stop.value_or(SIGSTOP);
            cont_signal_ = cont.value_or(SIGCONT);
            state_ = State::outer;
        } else if (state_ == State::frame) {
            if (frame_.size() == limit) return fail("status frame too large");
            frame_ += c;
            if (quoted_) {
                if (escaped_) escaped_ = false;
                else if (c == '\\') escaped_ = true;
                else if (c == '"') quoted_ = false;
            } else if (c == '"') quoted_ = true;
            else if (c == '[' || c == '{') {
                if (++depth_ > max_depth) return fail("status nesting too deep");
            } else if (c == ']' || c == '}') {
                if (--depth_ == 0 && !parse_frame(callback)) return false;
            }
        } else if (!whitespace(c)) {
            if (state_ == State::outer && c == '[') state_ = State::first;
            else if (state_ == State::next && c == ',') state_ = State::after_comma;
            else if ((state_ == State::first || state_ == State::next) && c == ']') state_ = State::done;
            else if ((state_ == State::first || state_ == State::after_comma) && c == '[') { state_ = State::frame; frame_ = "["; depth_ = 1; }
            else return fail("invalid status stream framing");
        }
    }
    return true;
}

bool StatusParser::finish() {
    if (!error_.empty()) return false;
    if (state_ == State::done || state_ == State::next || state_ == State::first || state_ == State::after_comma) return true;
    return fail("incomplete status stream");
}

std::string click_event_json(const ClickEvent& e, bool first) {
    jv obj = jv_object();
    auto set_string = [&](const char* key, const std::optional<std::string>& value) {
        if (value) obj = jv_object_set(obj, jv_string(key), jv_string_sized(value->data(), static_cast<int>(value->size())));
    };
    auto set_int = [&](const char* key, int value) { obj = jv_object_set(obj, jv_string(key), jv_number(value)); };
    set_string("name", e.name); set_string("instance", e.instance);
    set_int("button", e.button); set_int("x", e.x); set_int("y", e.y);
    set_int("relative_x", e.relative_x); set_int("relative_y", e.relative_y);
    set_int("width", e.width); set_int("height", e.height); set_int("scale", 1);
    jv mods = jv_array();
    for (const auto& mod : e.modifiers) mods = jv_array_append(mods, jv_string_sized(mod.data(), static_cast<int>(mod.size())));
    obj = jv_object_set(obj, jv_string("modifiers"), mods);
    Value result(jv_dump_string(obj, 0));
    return std::string(first ? "[\n" : ",\n") + std::string(jv_string_value(result.value), static_cast<std::size_t>(jv_string_length_bytes(jv_copy(result.value)))) + "\n";
}
}
