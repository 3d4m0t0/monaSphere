#include "ui/vr_menu.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <pwd.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace vrp {
namespace {

fs::path resolve_home() {
  if (const char* h = std::getenv("HOME"); h && *h) return fs::path(h);
  if (passwd* pw = getpwuid(getuid())) {
    if (pw->pw_dir && *pw->pw_dir) return fs::path(pw->pw_dir);
  }
  return fs::current_path();
}

void upsert_conf_key(const fs::path& conf, const std::string& key, const std::string& value) {
  std::error_code ec;
  fs::create_directories(conf.parent_path(), ec);
  const std::string prefix = key + "=";
  std::vector<std::string> lines;
  bool replaced = false;
  {
    std::ifstream in(conf);
    std::string line;
    while (std::getline(in, line)) {
      if (line.empty()) continue;
      if (line.rfind(prefix, 0) == 0) {
        if (!replaced) lines.push_back(prefix + value);
        replaced = true;
      } else {
        lines.push_back(line);
      }
    }
  }
  if (!replaced) lines.push_back(prefix + value);
  std::ofstream out(conf, std::ios::trunc);
  if (!out) return;
  for (const std::string& line : lines) out << line << '\n';
}

}  // namespace

bool VrMenu::is_video_ext(const fs::path& p) {
  auto e = p.extension().string();
  for (char& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return e == ".mp4" || e == ".mkv" || e == ".mov" || e == ".webm" || e == ".avi" || e == ".m4v" ||
         e == ".ts" || e == ".m2ts" || e == ".wmv" || e == ".flv";
}

bool VrMenu::path_under(const fs::path& path, const fs::path& root) const {
  std::error_code ec;
  const fs::path a = fs::weakly_canonical(path, ec);
  const fs::path b = fs::weakly_canonical(root, ec);
  if (a.empty() || b.empty()) return false;
  auto ait = a.begin();
  auto bit = b.begin();
  for (; bit != b.end(); ++ait, ++bit) {
    if (ait == a.end() || *ait != *bit) return false;
  }
  return true;
}

void VrMenu::init(const fs::path& start_dir) {
  home_ = resolve_home();
  std::error_code ec;
  if (fs::is_directory(home_, ec)) {
    home_ = fs::weakly_canonical(home_, ec);
  }

  fs::path start = start_dir.empty() ? home_ : start_dir;
  if (fs::is_regular_file(start, ec)) start = start.parent_path();
  if (!fs::is_directory(start, ec)) start = home_;
  start = fs::weakly_canonical(start, ec);

  if (path_under(start, home_) || start == home_) {
    place_ = Place::HomeTree;
    cwd_ = start;
  } else {
    place_ = Place::Roots;
    cwd_ = home_;
  }
  removable_root_.clear();
  load_volume();
  refresh_dir();
  bump();
}

void VrMenu::set_media(const std::string& path, double duration_sec) {
  media_path_ = path;
  media_name_ = fs::path(path).filename().string();
  duration_sec_ = duration_sec;
  rate_ = 1.f;
  bump();
}

void VrMenu::set_media_info(const std::string& video_codec, const std::string& res,
                            const std::string& fps, const std::string& bitrate,
                            const std::string& audio_codec, const std::string& audio_rate,
                            const std::string& audio_channels, const std::string& hwaccel,
                            const std::string& output_note) {
  info_video_codec_ = video_codec;
  info_res_ = res;
  info_fps_ = fps;
  info_bitrate_ = bitrate;
  info_audio_codec_ = audio_codec;
  info_audio_rate_ = audio_rate;
  info_audio_channels_ = audio_channels;
  info_hwaccel_ = hwaccel;
  info_output_note_ = output_note;
  bump();
}

void VrMenu::set_output_note(const std::string& output_note) {
  if (info_output_note_ == output_note) return;
  info_output_note_ = output_note;
  bump();
}

void VrMenu::set_playback(bool playing, double position_sec) {
  playing_ = playing;
  position_sec_ = position_sec;
  bump();
}

void VrMenu::set_volume(float v) {
  volume_ = std::clamp(v, 0.f, 1.f);
  bump();
}

void VrMenu::set_rate(float r) {
  rate_ = std::clamp(r, 0.5f, 20.f);
  bump();
}

void VrMenu::set_fsr(FsrMode m) {
  fsr_ = m;
  bump();
}

void VrMenu::save_last_dir() const {
  if (place_ != Place::HomeTree && place_ != Place::RemovableTree) return;
  if (cwd_.empty()) return;
  upsert_conf_key(user_conf_path(), "last_dir", cwd_.string());
}

std::filesystem::path VrMenu::load_last_dir() const {
  const fs::path conf = user_conf_path();
  std::ifstream in(conf);
  if (!in) return {};
  std::string line;
  while (std::getline(in, line)) {
    constexpr const char* k = "last_dir=";
    if (line.rfind(k, 0) == 0) return fs::path(line.substr(9));
  }
  return {};
}

int VrMenu::hz_option_value(int index) {
  switch (index) {
    case 1: return 90;
    case 2: return 120;
    default: return 0;
  }
}

int VrMenu::hz_option_index(int hz) {
  if (hz >= 105) return 2;
  if (hz >= 75) return 1;
  return 0;
}

const char* VrMenu::hz_option_label(int index) {
  switch (index) {
    case 1: return "90Hz";
    case 2: return "120Hz";
    default: return "Auto";
  }
}

void VrMenu::set_preferred_hz(int hz) {
  preferred_hz_ = (hz <= 0) ? 0 : hz;
  bump();
}

void VrMenu::set_display_hz(float hz) {
  if (std::fabs(display_hz_ - hz) < 0.05f && !info_display_hz_.empty()) {
    display_hz_ = hz;
    return;
  }
  display_hz_ = hz;
  if (hz > 0.5f) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%.0f", hz);
    info_display_hz_ = buf;
  } else {
    info_display_hz_ = "-";
  }
  bump();
}

void VrMenu::set_format(ProjectionMode proj, StereoLayout stereo) {
  projection_ = proj;
  stereo_ = stereo;
  bump();
}

void VrMenu::set_preview(std::vector<uint8_t> rgba, int w, int h) {
  std::lock_guard lock(mu_);
  preview_rgba_ = std::move(rgba);
  preview_w_ = w;
  preview_h_ = h;
  ++gen_;
}

void VrMenu::clear_preview() {
  std::lock_guard lock(mu_);
  preview_rgba_.clear();
  preview_w_ = preview_h_ = 0;
  ++gen_;
}

void VrMenu::list_directory(const fs::path& dir, bool add_up, const fs::path& up_path,
                            bool up_is_special) {
  entries_.clear();
  if (add_up) {
    Entry up;
    up.name = "..";
    up.path = up_path;
    up.is_dir = true;
    up.is_special = up_is_special;
    entries_.push_back(std::move(up));
  }

  std::error_code ec;
  std::vector<Entry> dirs;
  std::vector<Entry> files;
  for (const auto& it : fs::directory_iterator(dir, ec)) {
    Entry e;
    e.path = it.path();
    e.name = it.path().filename().string();
    if (!e.name.empty() && e.name[0] == '.') continue;
    if (it.is_directory(ec)) {
      e.is_dir = true;
      dirs.push_back(std::move(e));
    } else if (it.is_regular_file(ec) && is_video_ext(it.path())) {
      e.is_video = true;
      files.push_back(std::move(e));
    }
  }
  auto by_name = [](const Entry& a, const Entry& b) { return a.name < b.name; };
  std::sort(dirs.begin(), dirs.end(), by_name);
  std::sort(files.begin(), files.end(), by_name);
  entries_.insert(entries_.end(), dirs.begin(), dirs.end());
  entries_.insert(entries_.end(), files.begin(), files.end());
}

void VrMenu::list_removable_mounts() {
  entries_.clear();
  Entry up;
  up.name = "..";
  up.path.clear();
  up.is_dir = true;
  up.is_special = true;
  entries_.push_back(std::move(up));

  const std::string user = [&]() -> std::string {
    if (passwd* pw = getpwuid(getuid())) {
      if (pw->pw_name) return pw->pw_name;
    }
    if (const char* u = std::getenv("USER"); u && *u) return u;
    return {};
  }();

  std::vector<fs::path> roots;
  if (!user.empty()) {
    roots.push_back(fs::path("/media") / user);
    roots.push_back(fs::path("/run/media") / user);
  }
  roots.push_back("/media");
  roots.push_back("/mnt");

  std::error_code ec;
  std::vector<Entry> mounts;
  for (const auto& root : roots) {
    if (!fs::is_directory(root, ec)) continue;
    for (const auto& it : fs::directory_iterator(root, ec)) {
      if (!it.is_directory(ec)) continue;
      // Skip the user folder itself when scanning /media
      if ((root == "/media" || root == "/run/media") && !user.empty() &&
          it.path().filename() == user) {
        continue;
      }
      Entry e;
      e.path = it.path();
      e.name = it.path().filename().string();
      e.is_dir = true;
      mounts.push_back(std::move(e));
    }
  }
  std::sort(mounts.begin(), mounts.end(),
            [](const Entry& a, const Entry& b) { return a.name < b.name; });
  // Deduplicate by path
  std::vector<Entry> unique;
  for (auto& m : mounts) {
    bool dup = false;
    for (const auto& u : unique) {
      if (u.path == m.path) {
        dup = true;
        break;
      }
    }
    if (!dup) unique.push_back(std::move(m));
  }
  entries_.insert(entries_.end(), unique.begin(), unique.end());
}

void VrMenu::refresh_dir(const std::string& select_name) {
  cursor_ = 0;
  scroll_ = 0;
  entries_.clear();

  if (place_ == Place::Roots) {
    Entry home;
    home.name = "[ホーム]";
    home.path = home_;
    home.is_dir = true;
    home.is_special = true;
    entries_.push_back(std::move(home));

    Entry rem;
    rem.name = "[リムーバブル]";
    rem.path.clear();
    rem.is_dir = true;
    rem.is_special = true;
    entries_.push_back(std::move(rem));
    select_entry_named(select_name);
    return;
  }

  if (place_ == Place::RemovableList) {
    list_removable_mounts();
    select_entry_named(select_name);
    return;
  }

  if (place_ == Place::HomeTree) {
    const bool at_home = (cwd_ == home_);
    list_directory(cwd_, true, at_home ? fs::path{} : cwd_.parent_path(), at_home);
    select_entry_named(select_name);
    return;
  }

  if (place_ == Place::RemovableTree) {
    const bool at_mount = (cwd_ == removable_root_);
    list_directory(cwd_, true, at_mount ? fs::path{} : cwd_.parent_path(), at_mount);
    select_entry_named(select_name);
  }
}

void VrMenu::select_entry_named(const std::string& name) {
  if (name.empty() || entries_.empty()) return;
  for (int i = 0; i < static_cast<int>(entries_.size()); ++i) {
    if (entries_[static_cast<size_t>(i)].name == name) {
      cursor_ = i;
      if (cursor_ < scroll_) scroll_ = cursor_;
      if (cursor_ >= scroll_ + kVisibleRows) scroll_ = cursor_ - kVisibleRows + 1;
      return;
    }
  }
}

void VrMenu::close_controls_edit() {
  controls_edit_ = ControlsEdit::None;
  picker_cursor_ = 0;
}

void VrMenu::move_cursor(int delta) {
  if (screen_ == Screen::Format) {
    format_cursor_ = (format_cursor_ + delta + 8) % 8;
    bump();
    return;
  }
  if (controls_visible_ && controls_edit_ == ControlsEdit::None && screen_ == Screen::Player) {
    transport_cursor_ = (transport_cursor_ + delta + kTransportCount) % kTransportCount;
    bump();
    return;
  }
  if (entries_.empty()) return;
  cursor_ = std::clamp(cursor_ + delta, 0, static_cast<int>(entries_.size()) - 1);
  if (cursor_ < scroll_) scroll_ = cursor_;
  if (cursor_ >= scroll_ + kVisibleRows) scroll_ = cursor_ - kVisibleRows + 1;
  bump();
}

std::string VrMenu::sibling_video(int delta) const {
  if (media_path_.empty() || delta == 0) return {};
  const fs::path dir = media_path_.parent_path();
  if (dir.empty()) return {};

  std::error_code ec;
  std::vector<fs::path> vids;
  for (const auto& it : fs::directory_iterator(dir, ec)) {
    if (!it.is_regular_file(ec)) continue;
    if (!is_video_ext(it.path())) continue;
    vids.push_back(it.path());
  }
  std::sort(vids.begin(), vids.end(), [](const fs::path& a, const fs::path& b) {
    return a.filename().string() < b.filename().string();
  });
  if (vids.empty()) return {};

  std::error_code cec;
  const fs::path cur = fs::weakly_canonical(media_path_, cec);
  int idx = -1;
  for (int i = 0; i < static_cast<int>(vids.size()); ++i) {
    const fs::path c = fs::weakly_canonical(vids[static_cast<size_t>(i)], cec);
    if (c == cur || vids[static_cast<size_t>(i)] == media_path_) {
      idx = i;
      break;
    }
  }
  if (idx < 0) {
    const std::string name = media_path_.filename().string();
    for (int i = 0; i < static_cast<int>(vids.size()); ++i) {
      if (vids[static_cast<size_t>(i)].filename().string() == name) {
        idx = i;
        break;
      }
    }
  }
  if (idx < 0) return {};
  const int ni = idx + delta;
  if (ni < 0 || ni >= static_cast<int>(vids.size())) return {};
  return vids[static_cast<size_t>(ni)].string();
}

float VrMenu::step_rate(float cur, int dir) const {
  static constexpr float kSteps[] = {0.5f, 0.75f, 1.f,  1.25f, 1.5f, 2.f,  2.5f, 3.f,
                                     4.f,  5.f,   6.f,  8.f,   10.f, 12.f, 15.f, 20.f};
  constexpr int n = static_cast<int>(sizeof(kSteps) / sizeof(kSteps[0]));
  int idx = 0;
  float best = std::fabs(kSteps[0] - cur);
  for (int i = 1; i < n; ++i) {
    const float d = std::fabs(kSteps[i] - cur);
    if (d < best) {
      best = d;
      idx = i;
    }
  }
  idx = std::clamp(idx + dir, 0, n - 1);
  return kSteps[idx];
}

float VrMenu::step_playback_rate(float cur, int dir) const {
  static constexpr float kSteps[] = {0.5f, 0.75f, 1.f, 1.25f, 1.5f, 1.75f, 2.f};
  constexpr int n = static_cast<int>(sizeof(kSteps) / sizeof(kSteps[0]));
  int idx = 0;
  float best = std::fabs(kSteps[0] - cur);
  for (int i = 1; i < n; ++i) {
    const float d = std::fabs(kSteps[i] - cur);
    if (d < best) {
      best = d;
      idx = i;
    }
  }
  idx = std::clamp(idx + dir, 0, n - 1);
  return kSteps[idx];
}

fs::path VrMenu::user_conf_path() const {
  return home_ / ".config" / "monasphere" / "monasphere.conf";
}

void VrMenu::load_volume() {
  volume_ = 0.2f;
  std::ifstream in(user_conf_path());
  if (!in) return;
  std::string line;
  while (std::getline(in, line)) {
    if (line.rfind("volume=", 0) == 0) {
      try {
        volume_ = std::clamp(std::stof(line.substr(7)), 0.f, 1.f);
      } catch (...) {
      }
    }
  }
}

void VrMenu::save_volume() const {
  const fs::path conf = user_conf_path();
  std::error_code ec;
  fs::create_directories(conf.parent_path(), ec);
  std::vector<std::string> kept;
  {
    std::ifstream in(conf);
    std::string line;
    while (std::getline(in, line)) {
      if (line.rfind("volume=", 0) == 0 || line.empty()) continue;
      kept.push_back(line);
    }
  }
  std::ofstream out(conf, std::ios::trunc);
  if (!out) return;
  for (const std::string& line : kept) out << line << '\n';
  out << "volume=" << volume_ << '\n';
}

void VrMenu::activate() {
  if (screen_ == Screen::Format) {
    if (format_cursor_ < 4) {
      projection_ = static_cast<ProjectionMode>(format_cursor_);
    } else {
      stereo_ = static_cast<StereoLayout>(format_cursor_ - 4);
    }
    bump();
    return;
  }
  if (screen_ == Screen::Player) {
    // Handled in update()
    return;
  }
  if (entries_.empty() || cursor_ < 0 || cursor_ >= static_cast<int>(entries_.size())) return;
  const Entry& e = entries_[static_cast<size_t>(cursor_)];

  if (place_ == Place::Roots) {
    if (e.name == "[ホーム]") {
      place_ = Place::HomeTree;
      cwd_ = home_;
      refresh_dir();
      save_last_dir();
      bump();
      return;
    }
    if (e.name == "[リムーバブル]") {
      place_ = Place::RemovableList;
      refresh_dir();
      bump();
      return;
    }
  }

  if (place_ == Place::RemovableList) {
    if (e.name == ".." || e.is_special) {
      place_ = Place::Roots;
      refresh_dir();
      bump();
      return;
    }
    if (e.is_dir) {
      place_ = Place::RemovableTree;
      removable_root_ = e.path;
      cwd_ = e.path;
      refresh_dir();
      save_last_dir();
      bump();
      return;
    }
  }

  if (e.name == ".." || (e.is_special && e.name == "..")) {
    go_back();
    return;
  }

  if (e.is_dir) {
    if (place_ == Place::HomeTree) {
      if (!path_under(e.path, home_) && e.path != home_) return;
      cwd_ = e.path;
      refresh_dir();
      save_last_dir();
      bump();
      return;
    }
    if (place_ == Place::RemovableTree) {
      if (!path_under(e.path, removable_root_) && e.path != removable_root_) return;
      cwd_ = e.path;
      refresh_dir();
      save_last_dir();
      bump();
      return;
    }
  }

  if (e.is_video) {
    // Open handled by update output
  }
}

void VrMenu::go_back() {
  if (screen_ == Screen::Format) {
    screen_ = Screen::Browser;
    bump();
    return;
  }
  if (screen_ == Screen::Player) {
    controls_visible_ = false;
    bump();
    return;
  }

  if (place_ == Place::Roots) {
    visible_ = false;
    bump();
    return;
  }

  if (place_ == Place::RemovableList) {
    place_ = Place::Roots;
    refresh_dir("[リムーバブル]");
    bump();
    return;
  }

  if (place_ == Place::HomeTree) {
    if (cwd_ == home_) {
      place_ = Place::Roots;
      refresh_dir("[ホーム]");
      bump();
      return;
    }
    const std::string left = cwd_.filename().string();
    fs::path parent = cwd_.parent_path();
    if (!path_under(parent, home_) && parent != home_) {
      place_ = Place::Roots;
      refresh_dir("[ホーム]");
      bump();
      return;
    }
    cwd_ = parent;
    refresh_dir(left);
    save_last_dir();
    bump();
    return;
  }

  if (place_ == Place::RemovableTree) {
    if (cwd_ == removable_root_) {
      const std::string left = removable_root_.filename().string();
      place_ = Place::RemovableList;
      removable_root_.clear();
      refresh_dir(left);
      bump();
      return;
    }
    const std::string left = cwd_.filename().string();
    fs::path parent = cwd_.parent_path();
    if (!path_under(parent, removable_root_) && parent != removable_root_) {
      const std::string mount = removable_root_.filename().string();
      place_ = Place::RemovableList;
      removable_root_.clear();
      refresh_dir(mount);
      bump();
      return;
    }
    cwd_ = parent;
    refresh_dir(left);
    save_last_dir();
    bump();
  }
}

void VrMenu::bump() { ++gen_; }

VrMenu::Output VrMenu::update(const PadInput& in, float dt) {
  Output out;
  nav_cooldown_ = std::max(0.f, nav_cooldown_ - dt);

  const bool confirm = in.confirm && !prev_confirm_;
  const bool menu_toggle = in.menu_toggle && !prev_menu_toggle_;
  prev_confirm_ = in.confirm;
  prev_menu_toggle_ = in.menu_toggle;

  // B held (level): short-release = back/toggle; long-hold on controls = rate ×1
  bool back_short = false;
  bool back_long_reset = false;
  if (in.back_held) {
    if (!back_held_) {
      back_held_ = true;
      back_long_fired_ = false;
      back_held_sec_ = 0.f;
    } else {
      back_held_sec_ += dt;
      // Rate reset stays available with the controls hidden. The file dialog uses B to go back.
      if (!back_long_fired_ && !visible_ && controls_edit_ == ControlsEdit::None &&
          back_held_sec_ >= kBackLongPressSec) {
        back_long_fired_ = true;
        back_long_reset = true;
      }
    }
  } else if (back_held_) {
    if (!back_long_fired_) back_short = true;
    back_held_ = false;
    back_long_fired_ = false;
    back_held_sec_ = 0.f;
  }
  prev_back_ = in.back_held;

  if (back_long_reset) {
    rate_ = 1.f;
    out.rate_changed = true;
    out.rate = 1.f;
    bump();
    return out;
  }

  if (menu_toggle) {
    if (visible_) {
      visible_ = false;
    } else {
      visible_ = true;
      controls_visible_ = false;
      screen_ = Screen::Browser;
      close_controls_edit();
    }
    bump();
    return out;
  }

  // B short: back in menus / close edit / toggle playback controls
  if (back_short) {
    if (controls_visible_ && controls_edit_ != ControlsEdit::None) {
      if (controls_edit_ == ControlsEdit::HzConfirm) {
        // Back to Hz list (keep pending pick).
        controls_edit_ = ControlsEdit::HzPick;
        picker_cursor_ = hz_option_index(pick_hz_);
        bump();
        return out;
      }
      if (controls_edit_ == ControlsEdit::Volume && volume_ != pick_volume_) {
        out.volume_delta = pick_volume_ - volume_;
        volume_ = pick_volume_;
        save_volume();
      }
      if (controls_edit_ == ControlsEdit::FsrPick && fsr_ != pick_fsr_) {
        // Restore mode from when the picker opened.
        fsr_ = pick_fsr_;
        out.fsr_changed = true;
        out.fsr = fsr_;
      }
      close_controls_edit();
      bump();
      return out;
    }
    if (visible_) {
      if (screen_ == Screen::Format) {
        screen_ = Screen::Browser;
        bump();
      } else {
        go_back();
      }
    } else {
      controls_visible_ = !controls_visible_;
      if (controls_visible_) {
        screen_ = Screen::Player;
        transport_cursor_ = kPlayPause;
        close_controls_edit();
      } else {
        close_controls_edit();
      }
      bump();
    }
    return out;
  }

  if (!visible_ && !controls_visible_) {
    const bool stick_up = in.nav_up || in.stick_y > 0.55f;
    const bool stick_down = in.nav_down || in.stick_y < -0.55f;
    if (confirm && (stick_up != stick_down)) {
      rate_ = step_playback_rate(rate_, stick_up ? +1 : -1);
      out.rate_changed = true;
      out.rate = rate_;
      bump();
      return out;
    }
    if (confirm) {
      out.play_toggle = true;
    }
    return out;
  }

  auto nav = [&](int dy, int dx) {
    if (nav_cooldown_ > 0) return;
    if (controls_visible_ && screen_ == Screen::Player) {
      if (controls_edit_ == ControlsEdit::Volume && dy != 0) {
        const int pct = static_cast<int>(std::lround(volume_ * 100.f));
        // 0–9%: ±1; 10% and above: ±5
        const int step = (pct < 10) ? 1 : 5;
        out.volume_delta = (dy < 0) ? (step / 100.f) : -(step / 100.f);
        volume_ = std::clamp(volume_ + out.volume_delta, 0.f, 1.f);
        save_volume();
        bump();
        nav_cooldown_ = 0.12f;
        return;
      }
      if (controls_edit_ == ControlsEdit::FsrPick && dy != 0) {
        picker_cursor_ = (picker_cursor_ + (dy < 0 ? -1 : 1) + 4) % 4;
        fsr_ = static_cast<FsrMode>(picker_cursor_);
        out.fsr_changed = true;
        out.fsr = fsr_;
        bump();
        nav_cooldown_ = 0.15f;
        return;
      }
      if (controls_edit_ == ControlsEdit::FormatPick && dy != 0) {
        picker_cursor_ = (picker_cursor_ + (dy < 0 ? -1 : 1) + 6) % 6;
        bump();
        nav_cooldown_ = 0.15f;
        return;
      }
      if (controls_edit_ == ControlsEdit::HzPick && dy != 0) {
        picker_cursor_ = (picker_cursor_ + (dy < 0 ? -1 : 1) + kHzOptionCount) % kHzOptionCount;
        bump();
        nav_cooldown_ = 0.15f;
        return;
      }
      // Rate uses up/down + A (see confirm) — bare stick Y must not change speed.
      if (controls_edit_ == ControlsEdit::None && dx != 0) {
        move_cursor(dx);
        nav_cooldown_ = 0.15f;
        return;
      }
      return;
    }
    if (dy != 0) {
      if (visible_ && (screen_ == Screen::Browser || screen_ == Screen::Format) && !controls_visible_) {
        move_cursor(dy);
        nav_cooldown_ = 0.15f;
        return;
      }
    }
    if (dx != 0) {
      if (visible_ && screen_ == Screen::Format) {
        move_cursor(dx);
        nav_cooldown_ = 0.15f;
      }
    }
  };

  if (in.nav_up || in.stick_y > 0.55f) nav(-1, 0);
  if (in.nav_down || in.stick_y < -0.55f) nav(1, 0);
  if (in.nav_left || in.stick_x < -0.55f) nav(0, -1);
  if (in.nav_right || in.stick_x > 0.55f) nav(0, 1);

  if (controls_visible_ && (screen_ == Screen::Player || !visible_)) {
    screen_ = Screen::Player;

    // Up+A / Down+A: step playback rate (do not also seek / activate transport).
    const bool stick_up = in.nav_up || in.stick_y > 0.55f;
    const bool stick_down = in.nav_down || in.stick_y < -0.55f;
    const bool rate_chord = stick_up != stick_down;

    // [<] / [>] with A held: same key-repeat seek as LB / RB (disabled while rate chord).
    if (!rate_chord && controls_edit_ == ControlsEdit::None &&
        (transport_cursor_ == kSeekBack || transport_cursor_ == kSeekForward)) {
      constexpr auto kInitial = std::chrono::milliseconds(400);
      constexpr auto kInterval = std::chrono::milliseconds(120);
      const auto now = std::chrono::steady_clock::now();
      bool fire = false;
      if (!in.confirm_held) {
        confirm_seek_held_ = false;
        confirm_seek_repeating_ = false;
      } else if (!confirm_seek_held_) {
        confirm_seek_held_ = true;
        confirm_seek_repeating_ = false;
        confirm_seek_next_ = now + kInitial;
        fire = true;
      } else if (now >= confirm_seek_next_) {
        confirm_seek_next_ = now + kInterval;
        confirm_seek_repeating_ = true;
        fire = true;
      }
      if (fire) {
        if (transport_cursor_ == kSeekBack) out.seek_back = true;
        else out.seek_forward = true;
        out.seek_scrubbing = confirm_seek_repeating_;
        bump();
      }
      out.seek_scrubbing = confirm_seek_repeating_;
    } else {
      confirm_seek_held_ = false;
      confirm_seek_repeating_ = false;
    }

    if (confirm) {
      if (controls_edit_ == ControlsEdit::Volume) {
        close_controls_edit();
        bump();
        return out;
      }
      if (controls_edit_ == ControlsEdit::FsrPick) {
        // Selection already applied on cursor move — A only closes the picker.
        close_controls_edit();
        bump();
        return out;
      }
      if (controls_edit_ == ControlsEdit::FormatPick) {
        if (picker_cursor_ < 3) {
          projection_ = static_cast<ProjectionMode>(picker_cursor_);
        } else {
          stereo_ = static_cast<StereoLayout>(picker_cursor_ - 3);
        }
        out.apply_format = true;
        out.projection = projection_;
        out.stereo = stereo_;
        close_controls_edit();
        bump();
        return out;
      }
      if (controls_edit_ == ControlsEdit::HzPick) {
        pick_hz_ = hz_option_value(picker_cursor_);
        if (pick_hz_ == preferred_hz_) {
          close_controls_edit();
          bump();
          return out;
        }
        controls_edit_ = ControlsEdit::HzConfirm;
        bump();
        return out;
      }
      if (controls_edit_ == ControlsEdit::HzConfirm) {
        preferred_hz_ = pick_hz_;
        out.refresh_hz_restart = true;
        out.preferred_hz = preferred_hz_;
        close_controls_edit();
        bump();
        return out;
      }

      if (rate_chord) {
        rate_ = step_playback_rate(rate_, stick_up ? +1 : -1);
        out.rate_changed = true;
        out.rate = rate_;
        bump();
        return out;
      }

      switch (transport_cursor_) {
        case kCuePrev: {
          if (position_sec_ <= kNearStartSec) {
            const std::string p = sibling_video(-1);
            if (!p.empty()) {
              out.open_video = true;
              out.video_path = p;
            } else {
              out.seek_start = true;
            }
          } else {
            out.seek_start = true;
          }
          break;
        }
        case kSeekBack:
        case kSeekForward:
          // Handled by confirm_held key-repeat above (same as L/R).
          break;
        case kPlayPause:
          out.play_toggle = true;
          break;
        case kStop:
          out.stop = true;
          break;
        case kNextVideo: {
          const std::string p = sibling_video(+1);
          if (!p.empty()) {
            out.open_video = true;
            out.video_path = p;
          }
          break;
        }
        case kVolume:
          controls_edit_ = ControlsEdit::Volume;
          pick_volume_ = volume_;
          break;
        case kFormat:
          controls_edit_ = ControlsEdit::FormatPick;
          pick_proj_ = projection_;
          pick_stereo_ = stereo_;
          picker_cursor_ = static_cast<int>(projection_);
          break;
        case kFsr:
          controls_edit_ = ControlsEdit::FsrPick;
          picker_cursor_ = static_cast<int>(fsr_);
          pick_fsr_ = fsr_;
          break;
        case kHz:
          controls_edit_ = ControlsEdit::HzPick;
          picker_cursor_ = hz_option_index(preferred_hz_);
          pick_hz_ = preferred_hz_;
          break;
        default:
          break;
      }
      bump();
    }
    return out;
  }

  if (!visible_) return out;

  if (confirm) {
    if (screen_ == Screen::Browser) {
      if (!entries_.empty() && cursor_ >= 0 && cursor_ < static_cast<int>(entries_.size())) {
        const Entry& e = entries_[static_cast<size_t>(cursor_)];
        if (e.is_video) {
          out.open_video = true;
          out.video_path = e.path.string();
          visible_ = false;
          bump();
        } else {
          activate();
        }
      }
    } else if (screen_ == Screen::Format) {
      activate();
      out.apply_format = true;
      out.projection = projection_;
      out.stereo = stereo_;
    }
  }

  return out;
}

VrMenu::Snapshot VrMenu::snapshot() const {
  std::lock_guard lock(mu_);
  Snapshot s;
  s.gen = gen_;
  s.visible = visible_;
  s.controls_visible = controls_visible_;
  s.controls_edit = controls_edit_;
  s.screen = screen_;
  if (controls_visible_) {
    s.title = media_name_.empty() ? "Playback" : media_name_;
    switch (controls_edit_) {
      case ControlsEdit::Volume:
        s.hint = "Up/Down: vol  A/B: OK";
        break;
      case ControlsEdit::FsrPick:
        s.hint = "Up/Down: pick  A: close  B: undo";
        break;
      case ControlsEdit::FormatPick:
        s.hint = "Up/Down: pick  A: OK  B: cancel";
        break;
      case ControlsEdit::HzPick:
        s.hint = "Up/Down: Hz  A: OK  B: cancel";
        break;
      case ControlsEdit::HzConfirm:
        s.hint = "A: restart  B: back";
        break;
      default:
        s.hint = "L/R: move  Up+A: faster  Down+A: slower  A: OK  B: close";
        break;
    }
  } else if (place_ == Place::Roots) {
    s.title = "Files";
    s.hint = "A: open  B: back";
  } else if (place_ == Place::RemovableList) {
    s.title = "USB";
    s.hint = "A: open  B: back";
  } else if (place_ == Place::RemovableTree) {
    s.title = cwd_.filename().empty() ? cwd_.string() : cwd_.filename().string();
    s.hint = "A: open  B: back";
  } else {
    s.title = (cwd_ == home_) ? "Home" : cwd_.filename().string();
    s.hint = "A: open  B: back";
  }
  s.entries = entries_;
  s.cursor = cursor_;
  s.scroll = scroll_;
  s.projection = projection_;
  s.stereo = stereo_;
  s.format_cursor = format_cursor_;
  s.transport_cursor = transport_cursor_;
  s.picker_cursor = picker_cursor_;
  s.playing = playing_;
  s.position_sec = position_sec_;
  s.duration_sec = duration_sec_;
  s.media_name = media_name_;
  s.info_video_codec = info_video_codec_;
  s.info_res = info_res_;
  s.info_fps = info_fps_;
  s.info_bitrate = info_bitrate_;
  s.info_audio_codec = info_audio_codec_;
  s.info_audio_rate = info_audio_rate_;
  s.info_audio_channels = info_audio_channels_;
  s.info_hwaccel = info_hwaccel_;
  s.info_output_note = info_output_note_;
  s.info_display_hz = info_display_hz_;
  s.volume = volume_;
  s.rate = rate_;
  s.fsr = fsr_;
  s.preferred_hz = preferred_hz_;
  s.display_hz = display_hz_;
  s.preview_w = preview_w_;
  s.preview_h = preview_h_;
  s.preview_rgba = preview_rgba_;
  return s;
}

std::string VrMenu::highlighted_video_path() const {
  if (screen_ != Screen::Browser || entries_.empty()) return {};
  if (cursor_ < 0 || cursor_ >= static_cast<int>(entries_.size())) return {};
  const Entry& e = entries_[static_cast<size_t>(cursor_)];
  if (!e.is_video) return {};
  return e.path.string();
}

}  // namespace vrp
