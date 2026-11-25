#include "../mortar/randr.hpp"

#include <iostream>
#include <stdexcept>

int main() {
    auto *connection = xcb_connect(nullptr, nullptr);
    try {
        if (!connection || xcb_connection_has_error(connection)) throw std::runtime_error("X connection failed");
        auto *screen = xcb_setup_roots_iterator(xcb_get_setup(connection)).data;
        if (!screen) throw std::runtime_error("X screen unavailable");
        auto outputs = mortar::discover_outputs(connection, screen->root);
        if (outputs.size() != 1 || outputs[0].name.empty() ||
            outputs[0].bounds != mortar::Rect{0, 0, 800, 600})
            throw std::runtime_error("unexpected output geometry");
        mortar::Outputs model;
        if (!model.update(outputs)) throw std::runtime_error("output model rejected discovery");
        bool failed = false;
        try { mortar::discover_outputs(connection, XCB_NONE); }
        catch (const std::runtime_error &) { failed = true; }
        if (!failed) throw std::runtime_error("invalid root accepted");
        xcb_disconnect(connection);
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        if (connection) xcb_disconnect(connection);
        return 1;
    }
}
