#pragma once

#include <cstdint>
#include <cctype>
#include <string>

namespace vrp {

enum class ProjectionMode : uint8_t { Flat = 0, Deg180 = 1, Deg360 = 2 };
enum class StereoLayout : uint8_t { Mono = 0, Sbs = 1, OverUnder = 2 };

/** AMD FidelityFX Super Resolution 1 quality preset (spatial upscale). */
enum class FsrMode : uint8_t {
  Off = 0,
  UltraQuality = 1,  // ~1.3x
  Quality = 2,       // ~1.5x
  Performance = 3,   // ~2.0x
};

inline const char* fsr_mode_name(FsrMode m) {
  switch (m) {
    case FsrMode::Off: return "off";
    case FsrMode::UltraQuality: return "fsr uq";
    case FsrMode::Quality: return "fsr q";
    case FsrMode::Performance: return "fsr perf";
  }
  return "?";
}

inline float fsr_mode_scale(FsrMode m) {
  switch (m) {
    case FsrMode::UltraQuality: return 1.3f;
    case FsrMode::Quality: return 1.5f;
    case FsrMode::Performance: return 2.0f;
    case FsrMode::Off:
    default: return 1.0f;
  }
}

/** Which OpenXR interaction profile(s) to suggest. Applied at HMD connect. */
enum class ControllerProfile : uint8_t {
  Auto = 0,            // suggest all known XR profiles
  Simple = 1,          // Khronos simple controller
  OculusTouch = 2,     // Sense / Touch 系（PSVR2 で多い）
  MicrosoftMotion = 3,
  ViveController = 4,
  ValveIndex = 5,
  Gamepad = 6,         // USB/BT ゲームパッド（SDL2）
};

/** Which hand owns stick + confirm for the HMD menu. */
enum class ControllerHand : uint8_t { Right = 0, Left = 1 };

struct PlayerOptions {
  std::string video_path;
  ProjectionMode projection = ProjectionMode::Flat;
  StereoLayout stereo = StereoLayout::Mono;
  float flat_fov_deg = 70.f;
  float screen_distance = 4.f;
  bool mute = false;
  bool clear_only = false;  // debug: skip video, solid color
  ControllerProfile controller_profile = ControllerProfile::Gamepad;
  ControllerHand controller_hand = ControllerHand::Right;
};

inline const char* projection_name(ProjectionMode m) {
  switch (m) {
    case ProjectionMode::Flat: return "flat";
    case ProjectionMode::Deg180: return "180";
    case ProjectionMode::Deg360: return "360";
  }
  return "?";
}

inline const char* stereo_name(StereoLayout s) {
  switch (s) {
    case StereoLayout::Mono: return "mono";
    case StereoLayout::Sbs: return "sbs";
    case StereoLayout::OverUnder: return "ou";
  }
  return "?";
}

inline const char* controller_profile_name(ControllerProfile p) {
  switch (p) {
    case ControllerProfile::Auto: return "auto";
    case ControllerProfile::Simple: return "simple";
    case ControllerProfile::OculusTouch: return "oculus_touch";
    case ControllerProfile::MicrosoftMotion: return "ms_motion";
    case ControllerProfile::ViveController: return "vive";
    case ControllerProfile::ValveIndex: return "index";
    case ControllerProfile::Gamepad: return "gamepad";
  }
  return "?";
}

inline const char* controller_hand_name(ControllerHand h) {
  return h == ControllerHand::Left ? "left" : "right";
}

/** True if `needle` appears as a token (bounded by non-alnum) in lowercase `hay`. */
inline bool name_has_token(const std::string& hay_lower, const std::string& needle) {
  if (needle.empty() || hay_lower.size() < needle.size()) return false;
  for (size_t pos = 0; (pos = hay_lower.find(needle, pos)) != std::string::npos; ++pos) {
    const bool before_ok = (pos == 0) || !std::isalnum(static_cast<unsigned char>(hay_lower[pos - 1]));
    const size_t end = pos + needle.size();
    const bool after_ok =
        (end >= hay_lower.size()) || !std::isalnum(static_cast<unsigned char>(hay_lower[end]));
    if (before_ok && after_ok) return true;
  }
  return false;
}

/** Infer projection / stereo from a media file name (or path basename). */
inline void detect_format_from_filename(const std::string& path_or_name, ProjectionMode& projection,
                                        StereoLayout& stereo) {
  std::string base = path_or_name;
  const auto slash = base.find_last_of("/\\");
  if (slash != std::string::npos) base = base.substr(slash + 1);
  for (char& c : base) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

  if (base.find("360") != std::string::npos) projection = ProjectionMode::Deg360;
  else if (base.find("180") != std::string::npos) projection = ProjectionMode::Deg180;
  else projection = ProjectionMode::Flat;

  if (name_has_token(base, "sbs")) stereo = StereoLayout::Sbs;
  else if (name_has_token(base, "tb") || name_has_token(base, "ou")) stereo = StereoLayout::OverUnder;
  else stereo = StereoLayout::Mono;
}

}  // namespace vrp
