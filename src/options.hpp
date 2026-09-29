#pragma once

#include <cctype>
#include <cstdint>
#include <initializer_list>
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
  /** Full: each eye already has its display aspect. Half: the eye is squeezed into the frame. */
  bool stereo_full = true;
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

inline const char* stereo_detect_name(StereoLayout s, bool stereo_full) {
  if (s == StereoLayout::Sbs) return stereo_full ? "sbs full" : "sbs half";
  if (s == StereoLayout::OverUnder) return stereo_full ? "ou full" : "ou half";
  return "mono";
}

/**
 * Flat-screen aspect. Half SBS/OU is unsqueezed onto the full frame.
 * Full SBS/OU uses one eye's pixel aspect. 180/360 ignore this.
 */
inline float frame_display_aspect(float width, float height, StereoLayout stereo, bool stereo_full) {
  const float aspect = (width > 0.f && height > 0.f) ? width / height : (16.f / 9.f);
  if (stereo == StereoLayout::Mono || !stereo_full) return aspect;
  if (stereo == StereoLayout::Sbs) return aspect * 0.5f;
  return aspect * 2.f;
}

inline std::string filename_match_key(const std::string& path_or_name) {
  std::string base = path_or_name;
  const auto slash = base.find_last_of("/\\");
  if (slash != std::string::npos) base = base.substr(slash + 1);
  std::string out;
  bool space = false;
  for (unsigned char uc : base) {
    const char c = static_cast<char>(std::tolower(uc));
    if (!std::isalnum(static_cast<unsigned char>(c))) {
      if (!space && !out.empty()) out.push_back(' ');
      space = true;
    } else {
      out.push_back(c);
      space = false;
    }
  }
  return out;
}

inline bool name_has_any(const std::string& hay, std::initializer_list<const char*> words) {
  for (const char* word : words) {
    if (name_has_token(hay, word)) return true;
  }
  return false;
}

/**
 * Infer projection, stereo, and half/full packing from a media file name.
 * Case is ignored. Separators (space, underscore, dot, hyphen) are word breaks.
 * Fisheye / EAC names stay flat: those meshes are not equirectangular.
 * A bare sbs/ou/tb/lr tag is half, matching the usual 3D-movie files.
 */
inline void detect_format_from_filename(const std::string& path_or_name, ProjectionMode& projection,
                                        StereoLayout& stereo, bool& stereo_full) {
  const std::string key = filename_match_key(path_or_name);

  bool tag_180f = false;
  for (size_t pos = 0; (pos = key.find("180f", pos)) != std::string::npos; ++pos) {
    if (pos + 4 < key.size() && key[pos + 4] == 'p') continue;  // 180fps is a frame rate
    tag_180f = true;
    break;
  }
  const bool non_equirect = key.find("fisheye") != std::string::npos || tag_180f ||
                             key.find("mkx200") != std::string::npos || key.find("vrca220") != std::string::npos ||
                             key.find("rf52") != std::string::npos || key.find("360eac") != std::string::npos ||
                             name_has_token(key, "eac");
  std::string proj_key = key;
  for (const char* rate : {"360fps", "180fps"}) {
    const size_t n = std::char_traits<char>::length(rate);
    for (size_t pos = 0; (pos = proj_key.find(rate, pos)) != std::string::npos;) proj_key.erase(pos, n);
  }
  if (non_equirect) {
    projection = ProjectionMode::Flat;
  } else if (proj_key.find("360") != std::string::npos) {
    projection = ProjectionMode::Deg360;
  } else if (proj_key.find("180") != std::string::npos) {
    projection = ProjectionMode::Deg180;
  } else {
    projection = ProjectionMode::Flat;
  }

  if (name_has_any(key, {"fullsbs", "full sbs", "sbs full", "fsbs", "3dfsbs", "3dhf", "3dphf", "lrf",
                         "full side by side", "full sidebyside", "side by side full"})) {
    stereo = StereoLayout::Sbs;
    stereo_full = true;
  } else if (name_has_any(key, {"halfsbs", "half sbs", "sbs half", "hsbs", "3dhsbs", "3dsbs", "3dh",
                                "3dph", "lrh", "sbs3d", "half side by side", "half sidebyside",
                                "side by side half"})) {
    stereo = StereoLayout::Sbs;
    stereo_full = false;
  } else if (name_has_any(key, {"fullou", "full ou", "ou full", "fulltb", "full tb", "tb full", "ftab",
                                "ftb", "3dftab", "3dvf", "3dpvf", "tbf", "full over under",
                                "over under full", "full overunder", "full top bottom", "top bottom full",
                                "full topbottom"})) {
    stereo = StereoLayout::OverUnder;
    stereo_full = true;
  } else if (name_has_any(key, {"halfou", "half ou", "ou half", "hou", "ouh", "htab", "htb", "3dhtab",
                                "3dtab", "3dv", "3dpv", "tbh", "half tb", "tb half", "tab3d", "ou3d",
                                "tb3d", "half over under", "over under half", "half overunder",
                                "half top bottom", "top bottom half"})) {
    stereo = StereoLayout::OverUnder;
    stereo_full = false;
  } else if (name_has_any(key, {"side by side", "sidebyside", "left right", "leftright", "sbs", "lr",
                                "3d"})) {
    stereo = StereoLayout::Sbs;
    stereo_full = false;
  } else if (name_has_any(key, {"over under", "overunder", "top bottom", "topbottom", "ou", "tb"})) {
    stereo = StereoLayout::OverUnder;
    stereo_full = false;
  } else {
    stereo = StereoLayout::Mono;
    stereo_full = true;
  }
}

}  // namespace vrp
