#include "x11_direct_probe.hpp"

#include <xcb/randr.h>
#include <xcb/xcb.h>

#include <cstdlib>
#include <cstring>
#include <string>

namespace {

std::string edidText(const uint8_t* data, int length) {
  std::string text;
  std::string run;
  auto flush = [&]() {
    if (run.size() >= 4) {
      if (!text.empty()) text.push_back(' ');
      text += run;
    }
    run.clear();
  };
  for (int i = 0; i < length; ++i) {
    const unsigned char c = data[i];
    if (c >= 32 && c < 127) {
      run.push_back(static_cast<char>(c));
    } else {
      flush();
    }
  }
  flush();
  if (text.size() > 180) text.resize(180);
  return text;
}

bool outputIsNonDesktop(xcb_connection_t* connection, xcb_randr_output_t output, xcb_atom_t atom) {
  xcb_generic_error_t* error = nullptr;
  xcb_randr_get_output_property_cookie_t cookie = xcb_randr_get_output_property(
      connection, output, atom, XCB_ATOM_NONE, 0, 4, 0, 0);
  xcb_randr_get_output_property_reply_t* reply =
      xcb_randr_get_output_property_reply(connection, cookie, &error);
  if (error != nullptr) {
    std::free(error);
    std::free(reply);
    return false;
  }
  if (reply == nullptr) return false;
  const bool ok = reply->type == XCB_ATOM_INTEGER && reply->num_items == 1 && reply->format == 32 &&
                  xcb_randr_get_output_property_data_length(reply) >= 1 &&
                  *xcb_randr_get_output_property_data(reply) == 1;
  std::free(reply);
  return ok;
}

void collectScreen(xcb_connection_t* connection, xcb_window_t root, xcb_atom_t non_desktop,
                   xcb_atom_t edid, std::vector<DrmLeaseConnectorInfo>* outputs) {
  xcb_randr_get_screen_resources_cookie_t resources_cookie =
      xcb_randr_get_screen_resources(connection, root);
  xcb_randr_get_screen_resources_reply_t* resources =
      xcb_randr_get_screen_resources_reply(connection, resources_cookie, nullptr);
  if (resources == nullptr) return;

  xcb_randr_output_t* xoutputs = xcb_randr_get_screen_resources_outputs(resources);
  const int count = xcb_randr_get_screen_resources_outputs_length(resources);
  xcb_randr_mode_info_t* modes = xcb_randr_get_screen_resources_modes(resources);
  const int mode_count = xcb_randr_get_screen_resources_modes_length(resources);

  for (int i = 0; i < count; ++i) {
    xcb_randr_get_output_info_cookie_t info_cookie =
        xcb_randr_get_output_info(connection, xoutputs[i], XCB_CURRENT_TIME);
    xcb_randr_get_output_info_reply_t* info =
        xcb_randr_get_output_info_reply(connection, info_cookie, nullptr);
    if (info == nullptr) continue;
    if (info->num_modes == 0 || !outputIsNonDesktop(connection, xoutputs[i], non_desktop)) {
      std::free(info);
      continue;
    }

    const xcb_randr_mode_t* output_modes = xcb_randr_get_output_info_modes(info);
    const int output_mode_count = xcb_randr_get_output_info_modes_length(info);
    uint16_t width = 0;
    uint16_t height = 0;
    bool panel_4000 = false;
    for (int m = 0; m < output_mode_count; ++m) {
      for (int k = 0; k < mode_count; ++k) {
        if (modes[k].id != output_modes[m]) continue;
        if (m == 0) {
          width = modes[k].width;
          height = modes[k].height;
        }
        if (modes[k].width == 4000 && modes[k].height == 2040) panel_4000 = true;
      }
    }

    std::string description;
    if (width > 0 && height > 0) {
      description = std::to_string(width) + "x" + std::to_string(height);
    }
    if (panel_4000 && description.find("4000x2040") == std::string::npos) {
      if (!description.empty()) description.push_back(' ');
      description += "4000x2040";
    }
    if (edid != XCB_NONE) {
      xcb_randr_get_output_property_cookie_t edid_cookie = xcb_randr_get_output_property(
          connection, xoutputs[i], edid, XCB_ATOM_NONE, 0, 64, 0, 0);
      xcb_randr_get_output_property_reply_t* edid_reply =
          xcb_randr_get_output_property_reply(connection, edid_cookie, nullptr);
      if (edid_reply != nullptr) {
        const std::string text = edidText(xcb_randr_get_output_property_data(edid_reply),
                                           xcb_randr_get_output_property_data_length(edid_reply));
        if (!text.empty()) {
          if (!description.empty()) description.push_back(' ');
          description += text;
        }
        std::free(edid_reply);
      }
    }

    const uint8_t* name_bytes = xcb_randr_get_output_info_name(info);
    const int name_len = xcb_randr_get_output_info_name_length(info);
    DrmLeaseConnectorInfo connector;
    if (name_bytes != nullptr && name_len > 0) {
      connector.name.assign(reinterpret_cast<const char*>(name_bytes), static_cast<size_t>(name_len));
    }
    connector.description = std::move(description);
    outputs->push_back(std::move(connector));
    std::free(info);
  }
  std::free(resources);
}

}  // namespace

X11DirectProbeResult probeX11DirectOutputs() {
  X11DirectProbeResult result;
  const char* display_name = std::getenv("DISPLAY");
  if (display_name == nullptr || display_name[0] == '\0') return result;

  int screen = 0;
  xcb_connection_t* connection = xcb_connect(display_name, &screen);
  if (connection == nullptr || xcb_connection_has_error(connection)) {
    if (connection != nullptr) xcb_disconnect(connection);
    return result;
  }

  xcb_randr_query_version_cookie_t version_cookie =
      xcb_randr_query_version(connection, XCB_RANDR_MAJOR_VERSION, XCB_RANDR_MINOR_VERSION);
  xcb_randr_query_version_reply_t* version =
      xcb_randr_query_version_reply(connection, version_cookie, nullptr);
  if (version == nullptr) {
    result.kind = X11DirectProbeKind::NoRandR;
    xcb_disconnect(connection);
    return result;
  }
  std::free(version);

  xcb_intern_atom_cookie_t non_desktop_cookie =
      xcb_intern_atom(connection, 1, sizeof("non-desktop") - 1, "non-desktop");
  xcb_intern_atom_reply_t* non_desktop = xcb_intern_atom_reply(connection, non_desktop_cookie, nullptr);
  xcb_intern_atom_cookie_t edid_cookie = xcb_intern_atom(connection, 1, sizeof("EDID") - 1, "EDID");
  xcb_intern_atom_reply_t* edid = xcb_intern_atom_reply(connection, edid_cookie, nullptr);
  const xcb_atom_t non_desktop_atom =
      non_desktop != nullptr ? non_desktop->atom : static_cast<xcb_atom_t>(XCB_NONE);
  const xcb_atom_t edid_atom = edid != nullptr ? edid->atom : static_cast<xcb_atom_t>(XCB_NONE);
  std::free(non_desktop);
  std::free(edid);

  result.kind = X11DirectProbeKind::Ok;
  if (non_desktop_atom == XCB_NONE) {
    xcb_disconnect(connection);
    return result;
  }

  const xcb_setup_t* setup = xcb_get_setup(connection);
  xcb_screen_iterator_t iter = xcb_setup_roots_iterator(setup);
  for (; iter.rem > 0 && result.outputs.empty(); xcb_screen_next(&iter)) {
    collectScreen(connection, iter.data->root, non_desktop_atom, edid_atom, &result.outputs);
  }

  xcb_disconnect(connection);
  return result;
}
