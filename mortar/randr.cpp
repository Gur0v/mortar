#include "randr.hpp"

#include <xcb/randr.h>

#include <algorithm>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>

namespace mortar {
namespace {

template <typename T> using Reply = std::unique_ptr<T, decltype(&std::free)>;

std::vector<OutputGeometry> root_output(xcb_connection_t *connection, xcb_window_t root) {
    Reply<xcb_get_geometry_reply_t> geometry(
        xcb_get_geometry_reply(connection, xcb_get_geometry(connection, root), nullptr), &std::free);
    if (!geometry || !geometry->width || !geometry->height)
        throw std::runtime_error("cannot query root window geometry");
    return {{"root", {0, 0, geometry->width, geometry->height}}};
}

}

std::vector<OutputGeometry> discover_outputs(xcb_connection_t *connection, xcb_window_t root) {
    if (!connection || xcb_connection_has_error(connection))
        throw std::runtime_error("X connection unavailable");
    const auto *extension = xcb_get_extension_data(connection, &xcb_randr_id);
    if (!extension) throw std::runtime_error("cannot query RandR extension");
    if (!extension->present) return root_output(connection, root);
    Reply<xcb_randr_query_version_reply_t> version(
        xcb_randr_query_version_reply(connection, xcb_randr_query_version(connection, 1, 5), nullptr), &std::free);
    if (!version) throw std::runtime_error("cannot query RandR version");
    if (version->major_version < 1 ||
        (version->major_version == 1 && version->minor_version < 2))
        return root_output(connection, root);

    if (version->major_version > 1 || version->minor_version >= 5) {
        Reply<xcb_randr_get_monitors_reply_t> monitors(
            xcb_randr_get_monitors_reply(connection, xcb_randr_get_monitors(connection, root, 1), nullptr), &std::free);
        if (!monitors) throw std::runtime_error("cannot query RandR monitors");
        {
            std::vector<OutputGeometry> outputs;
            const auto *data = reinterpret_cast<const std::uint8_t *>(monitors.get()) + 32;
            std::uint64_t remaining = std::uint64_t(monitors->length) * 4;
            std::uint64_t output_count = 0;
            for (std::uint32_t i = 0; i < monitors->nMonitors; ++i) {
                if (remaining < sizeof(xcb_randr_monitor_info_t)) throw std::runtime_error("invalid RandR monitors reply");
                xcb_randr_monitor_info_t monitor;
                std::memcpy(&monitor, data, sizeof(monitor));
                auto size = sizeof(monitor) + std::uint64_t(monitor.nOutput) * sizeof(xcb_randr_output_t);
                if (size > remaining) throw std::runtime_error("invalid RandR monitors reply");
                data += size;
                remaining -= size;
                output_count += monitor.nOutput;
                if (!monitor.width || !monitor.height || monitor.name == XCB_NONE)
                    throw std::runtime_error("invalid RandR monitor");
                Reply<xcb_get_atom_name_reply_t> name(
                    xcb_get_atom_name_reply(connection, xcb_get_atom_name(connection, monitor.name), nullptr), &std::free);
                if (!name || !name->name_len || std::uint64_t(name->name_len) > std::uint64_t(name->length) * 4)
                    throw std::runtime_error("invalid RandR monitor name");
                std::string monitor_name(xcb_get_atom_name_name(name.get()), name->name_len);
                if (monitor_name.find('\0') != std::string::npos ||
                    std::any_of(outputs.begin(), outputs.end(), [&](const auto &output) { return output.name == monitor_name; }))
                    throw std::runtime_error("invalid RandR monitor name");
                outputs.push_back({std::move(monitor_name), {monitor.x, monitor.y, monitor.width, monitor.height}});
            }
            if (output_count != monitors->nOutputs || remaining != 0)
                throw std::runtime_error("invalid RandR monitors reply");
            if (!outputs.empty()) return outputs;
            return root_output(connection, root);
        }
    }

    Reply<xcb_randr_get_screen_resources_reply_t> resources(
        xcb_randr_get_screen_resources_reply(connection, xcb_randr_get_screen_resources(connection, root), nullptr), &std::free);
    if (!resources) throw std::runtime_error("cannot query RandR resources");
    const auto resources_size = std::uint64_t(resources->num_crtcs) * 4 +
        std::uint64_t(resources->num_outputs) * 4 + std::uint64_t(resources->num_modes) * 32 + resources->names_len;
    if (resources_size > std::uint64_t(resources->length) * 4)
        throw std::runtime_error("invalid RandR resources reply");
    std::vector<OutputGeometry> outputs;
    auto *ids = xcb_randr_get_screen_resources_outputs(resources.get());
    auto count = xcb_randr_get_screen_resources_outputs_length(resources.get());
    for (int i = 0; i < count; ++i) {
        Reply<xcb_randr_get_output_info_reply_t> info(
            xcb_randr_get_output_info_reply(connection,
                xcb_randr_get_output_info(connection, ids[i], resources->config_timestamp), nullptr), &std::free);
        if (!info || info->status != XCB_RANDR_SET_CONFIG_SUCCESS)
            throw std::runtime_error("cannot query RandR output");
        const auto info_size = std::uint64_t(info->num_crtcs) * 4 + std::uint64_t(info->num_modes) * 4 +
            std::uint64_t(info->num_clones) * 4 + info->name_len;
        if (info_size > std::uint64_t(info->length) * 4)
            throw std::runtime_error("invalid RandR output reply");
        if (info->connection != XCB_RANDR_CONNECTION_CONNECTED || info->crtc == XCB_NONE) continue;
        Reply<xcb_randr_get_crtc_info_reply_t> crtc(
            xcb_randr_get_crtc_info_reply(connection,
                xcb_randr_get_crtc_info(connection, info->crtc, resources->config_timestamp), nullptr), &std::free);
        if (!crtc || crtc->status != XCB_RANDR_SET_CONFIG_SUCCESS)
            throw std::runtime_error("cannot query RandR CRTC");
        auto length = xcb_randr_get_output_info_name_length(info.get());
        if (length == 0 || !crtc->width || !crtc->height)
            throw std::runtime_error("invalid active RandR output");
        std::string name(reinterpret_cast<const char *>(xcb_randr_get_output_info_name(info.get())), static_cast<std::size_t>(length));
        if (name.find('\0') != std::string::npos ||
            std::any_of(outputs.begin(), outputs.end(), [&](const auto &output) { return output.name == name; }))
            throw std::runtime_error("invalid RandR output name");
        outputs.push_back({std::move(name), {crtc->x, crtc->y, crtc->width, crtc->height}});
    }
    return outputs.empty() ? root_output(connection, root) : outputs;
}

}
