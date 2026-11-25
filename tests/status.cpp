#include "../mortarbar/status.hpp"

#include <cstdlib>
#include <string>
#include <vector>

using namespace mortarbar;
#include <cstdio>
#define check(b) do { if (!(b)) { std::fprintf(stderr, "failure line %d\n", __LINE__); std::abort(); } } while (false)

int main() {
    StatusParser parser;
    std::vector<std::vector<StatusBlock>> updates;
    auto save = [&](std::vector<StatusBlock> blocks) { updates.push_back(std::move(blocks)); };
    std::string input = "{\"version\":1,\"click_events\":true,\"stop_signal\":0}\n[ [ {\"full_text\":\"a\\\"[]\",\"short_text\":\"x\",\"color\":\"#aabbcc\",\"urgent\":true,\"name\":\"n\",\"instance\":\"i\",\"min_width\":\"wide\"} ], [] ]";
    for (char c : input) { if (!parser.feed(std::string_view(&c, 1), save)) { std::fprintf(stderr, "error %s char %c\n", parser.error().c_str(), c); std::abort(); } }
    check(parser.finish(save) && parser.click_events() && parser.stop_signal() == 0);
    check(updates.size() == 2 && updates[0].size() == 1 && updates[1].empty());
    auto& b = updates[0][0];
    check(b.full_text == "a\"[]" && b.short_text == "x" && b.color == "#aabbcc" && b.urgent);
    check(b.name == "n" && b.instance == "i" && b.min_width_text == "wide");
    StatusParser text;
    std::vector<std::vector<StatusBlock>> lines;
    auto save_line = [&](std::vector<StatusBlock> blocks) { lines.push_back(std::move(blocks)); };
    check(text.feed("hel", save_line) && lines.empty());
    check(text.feed("lo\nworld\n\n", save_line) && text.finish(save_line));
    check(lines.size() == 3 && lines[0].size() == 1 && lines[0][0].full_text == "hello");
    check(lines[1][0].full_text == "world" && lines[2][0].full_text.empty() && !text.click_events());
    StatusParser partial;
    check(partial.feed("last", save_line) && partial.finish(save_line));
    check(lines.back()[0].full_text == "last");
    StatusParser partial_next;
    check(partial_next.feed("first\nlast", save_line) && partial_next.finish(save_line));
    check(lines.back()[0].full_text == "last");
    StatusParser json_text;
    check(json_text.feed("{\"version\":2}\n", save_line) && json_text.finish(save_line));
    check(lines.back()[0].full_text == "{\"version\":2}");
    StatusParser text_large;
    check(text_large.feed("text\n", save_line));
    check(!text_large.feed(std::string(1024 * 1024 + 1, 'x'), save_line));
    StatusParser bad;
    check(!bad.feed("{\"version\":1}\n[[{\"full_text\":1}]]", save));
    check(!bad.error().empty() && !bad.feed("", save));
    StatusParser truncated;
    check(truncated.feed("{\"version\":1}\n[[{", save) && !truncated.finish(save));
    StatusParser large;
    check(!large.feed(std::string(1024 * 1024 + 1, 'x'), save));
    StatusParser comma;
    check(!comma.feed("{\"version\":1}\n[[],]", save));
    StatusParser empty;
    check(empty.feed("{\"version\":1}\n[]", save) && empty.finish(save));
    ClickEvent click;
    click.name = "a\"b"; click.button = 1; click.modifiers = {"Shift"};
    auto json = click_event_json(click, true);
    check(json.starts_with("[\n{") && json.find("a\\\"b") != std::string::npos && json.find("\"button\":1") != std::string::npos);
    check(json.find("\"scale\":1") != std::string::npos);
    click.button = 4;
    auto scroll = click_event_json(click, false);
    check(scroll.starts_with(",\n{") && scroll.find("\"button\":4") != std::string::npos);
}
