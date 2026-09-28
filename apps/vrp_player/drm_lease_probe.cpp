#include "drm_lease_probe.hpp"

#include "drm-lease-v1-client-protocol.h"

#include <wayland-client.h>

#include <unistd.h>

#include <cstring>
#include <string>
#include <vector>

namespace {

struct LeaseConnector {
  std::string name;
  std::string description;
  bool withdrawn = false;
};

struct LeaseDevice {
  std::vector<LeaseConnector*> connectors;
  bool done = false;
};

void connector_name(void* data, struct wp_drm_lease_connector_v1*, const char* name) {
  static_cast<LeaseConnector*>(data)->name = name ? name : "";
}

void connector_description(void* data, struct wp_drm_lease_connector_v1*, const char* description) {
  static_cast<LeaseConnector*>(data)->description = description ? description : "";
}

void connector_id(void*, struct wp_drm_lease_connector_v1*, uint32_t) {}

void connector_done(void*, struct wp_drm_lease_connector_v1*) {}

void connector_withdrawn(void* data, struct wp_drm_lease_connector_v1*) {
  static_cast<LeaseConnector*>(data)->withdrawn = true;
}

const struct wp_drm_lease_connector_v1_listener kConnectorListener = {
    .name = connector_name,
    .description = connector_description,
    .connector_id = connector_id,
    .done = connector_done,
    .withdrawn = connector_withdrawn,
};

void device_drm_fd(void*, struct wp_drm_lease_device_v1*, int32_t fd) {
  if (fd >= 0) close(fd);
}

void device_connector(void* data, struct wp_drm_lease_device_v1*,
                      struct wp_drm_lease_connector_v1* connector) {
  auto* dev = static_cast<LeaseDevice*>(data);
  auto* conn = new LeaseConnector();
  dev->connectors.push_back(conn);
  wp_drm_lease_connector_v1_add_listener(connector, &kConnectorListener, conn);
}

void device_done(void* data, struct wp_drm_lease_device_v1*) {
  static_cast<LeaseDevice*>(data)->done = true;
}

void device_released(void*, struct wp_drm_lease_device_v1*) {}

const struct wp_drm_lease_device_v1_listener kDeviceListener = {
    .drm_fd = device_drm_fd,
    .connector = device_connector,
    .done = device_done,
    .released = device_released,
};

struct RegistryState {
  std::vector<LeaseDevice*> devices;
};

void registry_global(void* data, struct wl_registry* registry, uint32_t name, const char* interface,
                     uint32_t version) {
  if (std::strcmp(interface, wp_drm_lease_device_v1_interface.name) != 0) return;
  auto* state = static_cast<RegistryState*>(data);
  auto* dev = new LeaseDevice();
  state->devices.push_back(dev);
  const uint32_t bind_version = version < 1 ? version : 1;
  auto* device = static_cast<struct wp_drm_lease_device_v1*>(
      wl_registry_bind(registry, name, &wp_drm_lease_device_v1_interface, bind_version));
  wp_drm_lease_device_v1_add_listener(device, &kDeviceListener, dev);
}

void registry_global_remove(void*, struct wl_registry*, uint32_t) {}

const struct wl_registry_listener kRegistryListener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

}  // namespace

DrmLeaseProbeResult probeDrmLeaseConnectors() {
  DrmLeaseProbeResult result;
  struct wl_display* display = wl_display_connect(nullptr);
  if (!display) return result;

  RegistryState state;
  struct wl_registry* registry = wl_display_get_registry(display);
  wl_registry_add_listener(registry, &kRegistryListener, &state);
  if (wl_display_roundtrip(display) < 0) {
    wl_registry_destroy(registry);
    wl_display_disconnect(display);
    return result;
  }
  if (state.devices.empty()) {
    result.kind = DrmLeaseProbeKind::NoProtocol;
    wl_registry_destroy(registry);
    wl_display_disconnect(display);
    return result;
  }

  result.kind = DrmLeaseProbeKind::Ok;
  for (int i = 0; i < 4; ++i) {
    if (wl_display_roundtrip(display) < 0) break;
    bool all_done = true;
    for (const LeaseDevice* dev : state.devices) {
      if (!dev->done) all_done = false;
    }
    if (all_done) break;
  }

  for (const LeaseDevice* dev : state.devices) {
    for (const LeaseConnector* conn : dev->connectors) {
      if (conn->withdrawn) continue;
      result.connectors.push_back(DrmLeaseConnectorInfo{conn->name, conn->description});
    }
  }

  wl_registry_destroy(registry);
  wl_display_disconnect(display);
  for (LeaseDevice* dev : state.devices) {
    for (LeaseConnector* conn : dev->connectors) delete conn;
    delete dev;
  }
  return result;
}
