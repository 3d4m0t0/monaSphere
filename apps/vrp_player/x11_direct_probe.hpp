#pragma once

#include "drm_lease_probe.hpp"

#include <vector>

/** RandR non-desktop outputs, the list Monado's X11 direct mode reads. */
enum class X11DirectProbeKind {
  NoDisplay, /**< DISPLAY is unset or the X server cannot be reached. */
  NoRandR,   /**< X is up, but RandR did not answer. */
  Ok         /**< RandR answered. outputs may still be empty. */
};

struct X11DirectProbeResult {
  X11DirectProbeKind kind = X11DirectProbeKind::NoDisplay;
  /** name is the RandR output. description is the primary mode and EDID text. */
  std::vector<DrmLeaseConnectorInfo> outputs;
};

X11DirectProbeResult probeX11DirectOutputs();
