#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace vrp::dlna {

struct Server {
  std::string usn;
  std::string friendly_name;
  std::string base_url;     // e.g. http://192.168.1.10:8200/
  std::string control_url;  // absolute ContentDirectory controlURL
};

struct Item {
  std::string id;
  std::string parent_id;
  std::string title;
  bool is_container = false;
  std::string url;   // best cleartext http(s) resource (empty if none)
  std::string mime;
  /** DTCP-IP / .dtp / CPRM style — not playable without a licensed stack. */
  bool dtcp_protected = false;
  bool playable = false;
};

/**
 * UPnP AV MediaServer control-point helpers (TV / recorder DLNA).
 * Blocking; call off the render hot path or with short timeouts.
 */
std::vector<Server> discover(int timeout_ms = 2500);

/** BrowseDirectChildren. object_id "0" is the root. */
std::vector<Item> browse(const Server& server, const std::string& object_id, int timeout_ms = 8000,
                         int starting_index = 0, int requested_count = 200);

}  // namespace vrp::dlna
