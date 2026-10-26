#pragma once

#include "capture/eis_pointer.hpp"

#include <string>

namespace vrp {

/**
 * KWin Wayland seat pointer via org.kde.KWin.EIS.RemoteDesktop + EisPointer.
 * Prefer ScreenCast-only video on Plasma; do not combine portal RemoteDesktop
 * (that path auto-shares the whole desktop without a MONITOR picker).
 */
class KdeEisPointer {
 public:
  KdeEisPointer() = default;
  ~KdeEisPointer();

  KdeEisPointer(const KdeEisPointer&) = delete;
  KdeEisPointer& operator=(const KdeEisPointer&) = delete;

  /**
   * Connect to KWin EIS and hand the fd to EisPointer.
   * mapping_id_substr: match ei_region mapping_id (e.g. Virtual-monaSphere).
   */
  bool start(const std::string& mapping_id_substr = {});
  void stop();

  bool ready() const { return core_.ready(); }
  std::string last_error() const;

  void warp(double x, double y) { core_.warp(x, y); }
  void click_left() { core_.click_left(); }
  void scroll_discrete(int steps) { core_.scroll_discrete(steps); }

  EisPointer& core() { return core_; }
  const EisPointer& core() const { return core_; }

 private:
  EisPointer core_;
  int cookie_ = -1;
  std::string last_error_;
};

}  // namespace vrp
