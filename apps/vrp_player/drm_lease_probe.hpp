#pragma once

#include <string>
#include <vector>

/** Result of reading the compositor's wp_drm_lease_device_v1 connector list. */
enum class DrmLeaseProbeKind {
  NoDisplay,   /**< No Wayland display. Direct mode will not use this list. */
  NoProtocol,  /**< Display is up, but the compositor has no drm-lease global. */
  Ok           /**< Lease device is bound. connectors may still be empty. */
};

struct DrmLeaseConnectorInfo {
  std::string name;
  std::string description;
};

struct DrmLeaseProbeResult {
  DrmLeaseProbeKind kind = DrmLeaseProbeKind::NoDisplay;
  std::vector<DrmLeaseConnectorInfo> connectors;
};

/**
 * Connect to the current Wayland display, read leasable connectors, and
 * disconnect without submitting a lease.
 */
DrmLeaseProbeResult probeDrmLeaseConnectors();
