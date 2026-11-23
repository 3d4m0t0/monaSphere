#include "settings_catalog.hpp"

#include "video/video_decoder.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <QSet>
#include <QTextStream>

namespace vrp::settings {
namespace {

QString confCode(QString line) {
  line = line.trimmed();
  const int hash = line.indexOf(QLatin1Char('#'));
  if (hash >= 0) line.truncate(hash);
  return line.trimmed();
}

bool confInt(const QString& v, int lo, int hi) {
  bool ok = false;
  const int n = v.toInt(&ok);
  return ok && n >= lo && n <= hi;
}

bool confFloat(const QString& v, float lo, float hi) {
  bool ok = false;
  const float n = v.toFloat(&ok);
  return ok && n >= lo && n <= hi;
}

bool confOneOf(const QString& v, const char* const* opts, bool ignore_case) {
  if (!opts) return false;
  for (const char* const* p = opts; *p; ++p) {
    if (ignore_case ? v.compare(QLatin1String(*p), Qt::CaseInsensitive) == 0
                    : v == QLatin1String(*p)) {
      return true;
    }
  }
  return false;
}

bool confKeyIs(const QString& code, const QString& key, QString* value) {
  const QString prefix = key + QLatin1Char('=');
  if (!code.startsWith(prefix)) return false;
  if (value) *value = code.mid(prefix.size()).trimmed();
  return true;
}

bool entryValueOk(const Entry& e, const QString& v) {
  switch (e.kind) {
    case ValueKind::Bool01: {
      if (v == QLatin1String("0") || v == QLatin1String("1")) return true;
      if (e.bool_accept_on_off) {
        return v.compare(QLatin1String("on"), Qt::CaseInsensitive) == 0 ||
               v.compare(QLatin1String("off"), Qt::CaseInsensitive) == 0;
      }
      return false;
    }
    case ValueKind::Int:
    case ValueKind::Flags:
      return confInt(v, e.int_lo, e.int_hi);
    case ValueKind::Float:
      return confFloat(v, e.float_lo, e.float_hi);
    case ValueKind::Enum:
      return confOneOf(v, e.enum_values, e.enum_ignore_case);
    case ValueKind::String:
      return e.string_allow_empty || !v.isEmpty();
    case ValueKind::SizeWxH: {
      const QRegularExpression re(QStringLiteral("^(\\d+)x(\\d+)$"));
      const QRegularExpressionMatch m = re.match(v.toLower());
      if (!m.hasMatch()) return false;
      const int w = m.captured(1).toInt();
      const int h = m.captured(2).toInt();
      return w >= e.int_lo && w <= e.int_hi && h >= static_cast<int>(e.float_lo) &&
             h <= static_cast<int>(e.float_hi);
    }
  }
  return false;
}

bool listAssignmentOk(const QString& code) {
  {
    const QRegularExpression re(QStringLiteral("^live_app(?:\\[(\\d+)\\])?=(.*)$"));
    const QRegularExpressionMatch m = re.match(code);
    if (m.hasMatch()) {
      const QString rest = m.captured(2).trimmed();
      if (rest.isEmpty()) return true;
      const int bar = rest.indexOf(QLatin1Char('|'));
      if (bar <= 0) return false;
      const QString name = rest.left(bar).trimmed();
      const QString url = rest.mid(bar + 1).trimmed();
      return !name.isEmpty() &&
             (url.startsWith(QLatin1String("http://")) || url.startsWith(QLatin1String("https://")));
    }
  }
  {
    const QRegularExpression re(
        QStringLiteral("^live_desktop_app\\[(\\d+)\\]\\.title\\[(\\d+)\\]=(.*)$"));
    const QRegularExpressionMatch m = re.match(code);
    if (m.hasMatch()) {
      const QString rest = m.captured(3).trimmed();
      if (rest.isEmpty()) return true;
      const QString name = rest.section(QLatin1Char('|'), 0, 0).trimmed();
      return !name.isEmpty();
    }
  }
  {
    const QRegularExpression re(QStringLiteral("^live_desktop_app(?:\\[(\\d+)\\])?=(.*)$"));
    const QRegularExpressionMatch m = re.match(code);
    if (m.hasMatch()) {
      const QString rest = m.captured(2).trimmed();
      if (rest.isEmpty()) return true;
      const QStringList parts = rest.split(QLatin1Char('|'));
      return parts.size() >= 2 && !parts[0].trimmed().isEmpty() && !parts[1].trimmed().isEmpty();
    }
  }
  return false;
}

bool modesOk(const QString& v) {
  const QStringList parts = v.split(QLatin1Char(';'), Qt::SkipEmptyParts);
  if (parts.isEmpty()) return false;
  for (const QString& part : parts) {
    const QStringList f = part.split(QLatin1Char(':'));
    if (f.size() < 2) return false;
    bool ok1 = false;
    bool ok2 = false;
    const int idx = f[0].toInt(&ok1);
    const double hz = f[1].toDouble(&ok2);
    if (!ok1 || !ok2 || idx < 0 || hz <= 0.0) return false;
    if (f.size() >= 3) {
      bool ok3 = false;
      f[2].toLongLong(&ok3);
      if (!ok3) return false;
    }
  }
  return true;
}

// Enum option tables (null-terminated).
constexpr const char* kRuntime[] = {"monado", "wivrn", nullptr};
constexpr const char* kHz[] = {"0", "90", "120", nullptr};  // validated as int set
constexpr const char* kPad[] = {"auto", "simple", "oculus_touch", "ms_motion", "vive", "index",
                                "gamepad", nullptr};
constexpr const char* kHand[] = {"left", "right", nullptr};
constexpr const char* kSkyBackdrop[] = {"layers", "baked", nullptr};
constexpr const char* kSkyBakeFace[] = {"1024", "2048", "4096", nullptr};
constexpr const char* kLiveCapture[] = {"monitor", "window", nullptr};
constexpr const char* kChromeProfile[] = {"default", "isolated", nullptr};
constexpr const char* kBrowser[] = {"chrome", "chromium", "firefox", nullptr};
constexpr const char* kFsrScales[] = {"1.2×", "1.5×", "2.0×", nullptr};

}  // namespace

const QVector<Entry>& catalog() {
  static const QVector<Entry> kEntries = [] {
    QVector<Entry> e;
    e.reserve(48);

    // --- Session ---
    e.push_back({.key = "runtime",
                 .category = Category::Session,
                 .kind = ValueKind::Enum,
                 .visibility = Visibility::User,
                 .default_value = "monado",
                 .label = QT_TR_NOOP("Runtime"),
                 .enum_values = kRuntime});
    e.push_back({.key = "hz",
                 .category = Category::Session,
                 .kind = ValueKind::Int,
                 .visibility = Visibility::User,
                 .default_value = "0",
                 .label = QT_TR_NOOP("Display Hz"),
                 .description = QT_TR_NOOP("HMD refresh preference. Auto uses 90 Hz."),
                 .int_lo = 0,
                 .int_hi = 120});  // assignmentOk restricts to 0/90/120
    e.push_back({.key = "pad",
                 .category = Category::Session,
                 .kind = ValueKind::Enum,
                 .visibility = Visibility::User,
                 .default_value = "auto",
                 .label = QT_TR_NOOP("Controller profile"),
                 .enum_values = kPad});
    e.push_back({.key = "hand",
                 .category = Category::Session,
                 .kind = ValueKind::Enum,
                 .visibility = Visibility::User,
                 .default_value = "right",
                 .label = QT_TR_NOOP("Controller hand"),
                 .enum_values = kHand});
    e.push_back({.key = "gamepad",
                 .category = Category::Session,
                 .kind = ValueKind::Bool01,
                 .visibility = Visibility::User,
                 .default_value = "1",
                 .label = QT_TR_NOOP("SDL gamepad")});

    // --- Video ---
    // Cycle list for □ / X (bit0=1.2×, bit1=1.5×, bit2=2.0×). Current mode is fsr / live_fsr.
    e.push_back({.key = "fsr_scales",
                 .category = Category::Video,
                 .kind = ValueKind::Flags,
                 .visibility = Visibility::User,
                 .default_value = "7",  // all scales
                 .label = QT_TR_NOOP("FSR (file)"),
                 .description = QT_TR_NOOP(
                     "Scales cycled with □ / X during file playback. Checked sizes rotate with Off "
                     "(切). Larger × is sharper but heavier."),
                 .int_lo = 0,
                 .int_hi = 7,
                 .enum_values = kFsrScales});
    e.push_back({.key = "live_fsr_scales",
                 .category = Category::Video,
                 .kind = ValueKind::Flags,
                 .visibility = Visibility::User,
                 .default_value = "7",
                 .label = QT_TR_NOOP("FSR (Live browser)"),
                 .description = QT_TR_NOOP(
                     "Same □ / X cycle for browser Live (not desktop apps)."),
                 .int_lo = 0,
                 .int_hi = 7,
                 .enum_values = kFsrScales});
    // Active FSR mode (0=Off … 3=2.0×). Written by □ cycle; not shown in Preferences.
    e.push_back({.key = "fsr",
                 .category = Category::Video,
                 .kind = ValueKind::Int,
                 .visibility = Visibility::Auto,
                 .default_value = "2",
                 .label = QT_TR_NOOP("FSR mode (file)"),
                 .int_lo = 0,
                 .int_hi = 3});
    e.push_back({.key = "live_fsr",
                 .category = Category::Video,
                 .kind = ValueKind::Int,
                 .visibility = Visibility::Auto,
                 .default_value = "3",
                 .label = QT_TR_NOOP("FSR mode (Live)"),
                 .int_lo = 0,
                 .int_hi = 3});
    e.push_back({.key = "flat_fov",
                 .category = Category::Video,
                 .kind = ValueKind::Float,
                 .visibility = Visibility::User,
                 .default_value = "70.00",  // Options::flat_fov_deg cinema seat
                 .label = QT_TR_NOOP("Flat FOV"),
                 .description = QT_TR_NOOP(
                     "Vertical FOV for video and browser Live (degrees; Near/Cinema/Far)."),
                 .float_lo = 20.f,
                 .float_hi = 140.f});
    e.push_back({.key = "flat_distance",
                 .category = Category::Video,
                 .kind = ValueKind::Float,
                 .visibility = Visibility::User,
                 .default_value = "12.00",  // Options::screen_distance (Cinema)
                 .label = QT_TR_NOOP("Flat distance"),
                 .description = QT_TR_NOOP(
                     "Screen distance for video and browser Live in meters (Near/Cinema/Far)."),
                 .float_lo = 2.f,
                 .float_hi = 40.f});
    e.push_back({.key = "flat_fov_app",
                 .category = Category::Live,
                 .kind = ValueKind::Float,
                 .visibility = Visibility::Auto,
                 .default_value = "70.00",
                 .label = QT_TR_NOOP("Flat FOV (App)"),
                 .description = QT_TR_NOOP("Vertical FOV for desktop [アプリ] (degrees)."),
                 .float_lo = 20.f,
                 .float_hi = 140.f});
    e.push_back({.key = "flat_distance_app",
                 .category = Category::Live,
                 .kind = ValueKind::Float,
                 .visibility = Visibility::Auto,
                 .default_value = "12.00",
                 .label = QT_TR_NOOP("Flat distance (App)"),
                 .description = QT_TR_NOOP("Screen distance for desktop [アプリ] in meters."),
                 .float_lo = 2.f,
                 .float_hi = 40.f});
    e.push_back({.key = "hwaccel",
                 .category = Category::Video,
                 .kind = ValueKind::Bool01,
                 .visibility = Visibility::Advanced,
                 .default_value = "1",
                 .label = QT_TR_NOOP("Hardware decode"),
                 .description = QT_TR_NOOP("Use GPU decode when available. Off forces software.")});
    e.push_back({.key = "vulkan_video",
                 .category = Category::Video,
                 .kind = ValueKind::Bool01,
                 .visibility = Visibility::Advanced,
                 .default_value = "0",
                 .label = QT_TR_NOOP("Vulkan Video (Experimental)"),
                 .description = QT_TR_NOOP(
                     "Experimental. Prefer FFmpeg Vulkan Video zero-copy when available. "
                     "Default off — use CUDA/NVDEC on NVIDIA (or VA-API elsewhere). "
                     "Seek/scrub may be slower than CUDA until Vulkan Video improves.")});
    e.push_back({.key = "vulkan_h264_cb_cuda",
                 .category = Category::Video,
                 .kind = ValueKind::Bool01,
                 .visibility = Visibility::Advanced,
                 .default_value = "1",
                 .label = QT_TR_NOOP("H.264 → CUDA (Experimental)"),
                 .description = QT_TR_NOOP(
                     "Experimental (Vulkan Video path). On NVIDIA, decode all H.264 with "
                     "CUDA/NVDEC instead of Vulkan Video. Ignored when Vulkan Video is off, "
                     "and on AMD.")});
    e.push_back({.key = "seek_detail_log",
                 .category = Category::Video,
                 .kind = ValueKind::Bool01,
                 .visibility = Visibility::Advanced,
                 .default_value = "0",
                 .label = QT_TR_NOOP("Detailed seek log"),
                 .description = QT_TR_NOOP(
                     "Log scrub/land/arm breadcrumbs for every seek. Off keeps only milestones "
                     "and timeouts.")});

    // --- Audio ---
    e.push_back({.key = "volume",
                 .category = Category::Audio,
                 .kind = ValueKind::Float,
                 .visibility = Visibility::User,
                 .default_value = "0.2",
                 .label = QT_TR_NOOP("Volume"),
                 .description = QT_TR_NOOP("Adjusted inside the HMD menu, not here."),
                 .float_lo = 0.f,
                 .float_hi = 1.f});
    e.push_back({.key = "surround_gain",
                 .category = Category::Audio,
                 .kind = ValueKind::Float,
                 .visibility = Visibility::User,
                 .default_value = "0.80",
                 .label = QT_TR_NOOP("Surround gain"),
                 .description = QT_TR_NOOP(
                     "Level of the soft stereo→surround upmix for 2-channel sources (file / Live / "
                     "apps). Does not apply to true multi-channel tracks."),
                 .float_lo = 0.f,
                 .float_hi = 2.f});
    e.push_back({.key = "audio_normalize",
                 .category = Category::Audio,
                 .kind = ValueKind::Bool01,
                 .visibility = Visibility::User,
                 .default_value = "1",
                 .label = QT_TR_NOOP("Normalize audio"),
                 .description = QT_TR_NOOP("Match loudness across file / Live / app playback."),
                 .bool_accept_on_off = true});
    e.push_back({.key = "hrtf_trim_unlock",
                 .category = Category::Audio,
                 .kind = ValueKind::Bool01,
                 .visibility = Visibility::Advanced,
                 .default_value = "0",
                 .label = QT_TR_NOOP("Unlock HRTF trim"),
                 .description = QT_TR_NOOP(
                     "Allow changing HRTF attenuation below. Lower trim makes audio louder — "
                     "change with care."),
                 .bool_accept_on_off = true});
    e.push_back({.key = "hrtf_trim_db",
                 .category = Category::Audio,
                 .kind = ValueKind::Float,
                 .visibility = Visibility::Advanced,
                 .default_value = "10",
                 .label = QT_TR_NOOP("HRTF trim (dB)"),
                 .description = QT_TR_NOOP(
                     "Attenuation before upmix/HRTF. Reducing it makes sound louder and can clip. "
                     "Fixed at 10 dB while locked."),
                 .float_lo = 0.f,
                 .float_hi = 24.f});

    // --- Sky ---
    // User: backdrop + timezone. Advanced: bake face, milkyway, lat/lon.
    e.push_back({.key = "sky_backdrop",
                 .category = Category::Sky,
                 .kind = ValueKind::Enum,
                 .visibility = Visibility::User,
                 .default_value = "baked",
                 .label = QT_TR_NOOP("Sky backdrop"),
                 .description = QT_TR_NOOP(
                     "Baked: cubemap + live sun/moon. Layers: per-frame stars/nebulae (heavier)."),
                 .enum_values = kSkyBackdrop,
                 .enum_ignore_case = true});
    e.push_back({.key = "sky_from_timezone",
                 .category = Category::Sky,
                 .kind = ValueKind::Bool01,
                 .visibility = Visibility::User,
                 .default_value = "1",
                 .label = QT_TR_NOOP("Sky from timezone"),
                 .description = QT_TR_NOOP(
                     "Set observer lat/lon from the host timezone. Off uses manual coordinates.")});
    e.push_back({.key = "sky_bake_face",
                 .category = Category::Sky,
                 .kind = ValueKind::Enum,
                 .visibility = Visibility::Advanced,
                 .default_value = "2048",
                 .label = QT_TR_NOOP("Sky bake face size"),
                 .description = QT_TR_NOOP("Cubemap face resolution when backdrop is baked."),
                 .enum_values = kSkyBakeFace});
    e.push_back({.key = "milkyway_gain",
                 .category = Category::Sky,
                 .kind = ValueKind::Float,
                 .visibility = Visibility::Advanced,
                 .default_value = "1.00",
                 .label = QT_TR_NOOP("Milky Way gain"),
                 .description = QT_TR_NOOP("Brightness of the Gaia sky backdrop."),
                 .float_lo = 0.2f,
                 .float_hi = 2.5f});
    e.push_back({.key = "sky_lat",
                 .category = Category::Sky,
                 .kind = ValueKind::Float,
                 .visibility = Visibility::Advanced,
                 .default_value = "35.68",
                 .label = QT_TR_NOOP("Observer latitude"),
                 .description = QT_TR_NOOP("Used when “Sky from timezone” is off."),
                 .float_lo = -90.f,
                 .float_hi = 90.f});
    e.push_back({.key = "sky_lon",
                 .category = Category::Sky,
                 .kind = ValueKind::Float,
                 .visibility = Visibility::Advanced,
                 .default_value = "139.65",
                 .label = QT_TR_NOOP("Observer longitude"),
                 .description = QT_TR_NOOP("Used when “Sky from timezone” is off."),
                 .float_lo = -180.f,
                 .float_hi = 180.f});

    // --- Live ---
    // live_capture kept in conf (default monitor). Window mode is rejected at Live start.
    e.push_back({.key = "live_capture",
                 .category = Category::Live,
                 .kind = ValueKind::Enum,
                 .visibility = Visibility::User,
                 .default_value = "monitor",
                 .label = QT_TR_NOOP("Live capture"),
                 .description = QT_TR_NOOP(
                     "Only monitor + virtual display is supported. Window capture is disabled."),
                 .enum_values = kLiveCapture,
                 .enum_ignore_case = true});
    e.push_back({.key = "live_virtual",
                 .category = Category::Live,
                 .kind = ValueKind::Bool01,
                 .visibility = Visibility::User,
                 .default_value = "1",
                 .label = QT_TR_NOOP("Virtual display"),
                 .description = QT_TR_NOOP("Create a virtual output for Live (recommended on).")});
    e.push_back({.key = "live_virtual_size",
                 .category = Category::Live,
                 .kind = ValueKind::SizeWxH,
                 .visibility = Visibility::User,
                 .default_value = "1920x1080",
                 .label = QT_TR_NOOP("Virtual size"),
                 .description = QT_TR_NOOP("Base (x1) virtual resolution as WIDTHxHEIGHT."),
                 .float_lo = 360.f,   // h min
                 .float_hi = 4320.f,  // h max
                 .int_lo = 640,       // w min
                 .int_hi = 7680});    // w max
    e.push_back({.key = "live_virtual_scale",
                 .category = Category::Live,
                 .kind = ValueKind::Float,
                 .visibility = Visibility::User,
                 .default_value = "2",
                 .label = QT_TR_NOOP("Virtual scale"),
                 .description = QT_TR_NOOP("Compositor scale for the virtual display (e.g. 2 = 200%)."),
                 .float_lo = 1.f,
                 .float_hi = 3.f});
    e.push_back({.key = "live_chrome_scale",
                 .category = Category::Live,
                 .kind = ValueKind::Float,
                 .visibility = Visibility::User,
                 .default_value = "1",
                 .label = QT_TR_NOOP("Chrome scale"),
                 .description = QT_TR_NOOP(
                     "Chrome --force-device-scale-factor. Keep 1 when Virtual scale is already >1."),
                 .float_lo = 1.f,
                 .float_hi = 3.f});
    e.push_back({.key = "live_cursor_scale",
                 .category = Category::Live,
                 .kind = ValueKind::Int,
                 .visibility = Visibility::User,
                 .default_value = "1",
                 .label = QT_TR_NOOP("Cursor scale"),
                 .description = QT_TR_NOOP("Host cursor size multiplier on the virtual display."),
                 .int_lo = 1,
                 .int_hi = 6});
    e.push_back({.key = "live_chrome_profile",
                 .category = Category::Live,
                 .kind = ValueKind::Enum,
                 .visibility = Visibility::User,
                 .default_value = "default",
                 .label = QT_TR_NOOP("Browser profile"),
                 .description = QT_TR_NOOP(
                     "Default shares logins (Chrome and Firefox). Isolated is a separate profile "
                     "(needed on some GNOME/Wayland setups)."),
                 .enum_values = kChromeProfile,
                 .enum_ignore_case = true});
    e.push_back({.key = "live_chrome_warm",
                 .category = Category::Live,
                 .kind = ValueKind::Bool01,
                 .visibility = Visibility::Advanced,
                 .default_value = "1",
                 .label = QT_TR_NOOP("Create Chrome instance"),
                 .description = QT_TR_NOOP(
                     "Pre-create a Chrome process when monaSphere starts so Live connects faster. "
                     "Effective on KDE only (ignored on GNOME).")});
    e.push_back({.key = "live_browser",
                 .category = Category::Live,
                 .kind = ValueKind::Enum,
                 .visibility = Visibility::User,
                 .default_value = "chrome",
                 .label = QT_TR_NOOP("Browser"),
                 .description = QT_TR_NOOP("Browser used for Live URLs."),
                 .enum_values = kBrowser,
                 .enum_ignore_case = true});
    // live_output retired: Virtual name is always Virtual-monaSphere (not user-configurable).
    e.push_back({.key = "live_restore_output_id",
                 .category = Category::Live,
                 .kind = ValueKind::String,
                 .visibility = Visibility::Secret,
                 .default_value = "",
                 .label = "",
                 .description = "",
                 .string_allow_empty = true});
    e.push_back({.key = "live_display_log",
                 .category = Category::Live,
                 .kind = ValueKind::Bool01,
                 .visibility = Visibility::Advanced,
                 .default_value = "0",
                 .label = QT_TR_NOOP("Live display log"),
                 .description = QT_TR_NOOP("Write Live display breadcrumbs under ~/.config/monasphere/.")});
    e.push_back({.key = "live_teardown_log",
                 .category = Category::Live,
                 .kind = ValueKind::Bool01,
                 .visibility = Visibility::Advanced,
                 .default_value = "1",
                 .label = QT_TR_NOOP("Live teardown log"),
                 .description = QT_TR_NOOP("Write Live teardown breadcrumbs for debugging.")});

    // --- Thumbs (file-dialog preview + advanced seek strip) ---
    static_assert(vrp::kSeekThumbCount == 128);
    static_assert(vrp::kSeekThumbMaxEdge == 4096);
    static_assert(vrp::kFilePreviewFrameMax == 20);
    e.push_back({.key = "file_thumb_count",
                 .category = Category::Thumbs,
                 .kind = ValueKind::Int,
                 .visibility = Visibility::User,
                 .default_value = "5",
                 .label = QT_TR_NOOP("Preview frame count"),
                 .description = QT_TR_NOOP(
                     "How many file-dialog preview frames to extract, spaced evenly through the clip."),
                 .int_lo = 1,
                 .int_hi = vrp::kFilePreviewFrameMax});
    e.push_back({.key = "file_thumb_distance",
                 .category = Category::Thumbs,
                 .kind = ValueKind::Float,
                 .visibility = Visibility::User,
                 .default_value = "0.70",
                 .label = QT_TR_NOOP("Preview distance (m)"),
                 .description = QT_TR_NOOP(
                     "How far the file-dialog preview sits in front of the viewer."),
                 .float_lo = 0.30f,
                 .float_hi = 3.00f});
    e.push_back({.key = "file_thumb_x",
                 .category = Category::Thumbs,
                 .kind = ValueKind::Float,
                 .visibility = Visibility::User,
                 .default_value = "0.00",
                 .label = QT_TR_NOOP("Preview X (m)"),
                 .description = QT_TR_NOOP("Horizontal offset (positive = right)."),
                 .float_lo = -2.00f,
                 .float_hi = 2.00f});
    e.push_back({.key = "file_thumb_y",
                 .category = Category::Thumbs,
                 .kind = ValueKind::Float,
                 .visibility = Visibility::User,
                 .default_value = "-0.50",
                 .label = QT_TR_NOOP("Preview height (m)"),
                 .description = QT_TR_NOOP("Vertical offset (negative = lower)."),
                 .float_lo = -2.00f,
                 .float_hi = 2.00f});
    e.push_back({.key = "file_thumb_pitch_deg",
                 .category = Category::Thumbs,
                 .kind = ValueKind::Float,
                 .visibility = Visibility::User,
                 .default_value = "60.00",
                 .label = QT_TR_NOOP("Preview tilt (°)"),
                 .description = QT_TR_NOOP(
                     "Recline into depth (0 = upright facing you, 90 = flat away)."),
                 .float_lo = 0.f,
                 .float_hi = 90.f});
    e.push_back({.key = "file_thumb_half_w",
                 .category = Category::Thumbs,
                 .kind = ValueKind::Float,
                 .visibility = Visibility::User,
                 .default_value = "0.30",
                 .label = QT_TR_NOOP("Preview half-width (m)"),
                 .description = QT_TR_NOOP(
                     "Half-width of the preview card in meters. 180/360 previews use a slightly "
                     "smaller size."),
                 .float_lo = 0.10f,
                 .float_hi = 1.50f});
    e.push_back({.key = "thumb_count",
                 .category = Category::Thumbs,
                 .kind = ValueKind::Int,
                 .visibility = Visibility::Advanced,
                 .default_value = "128",
                 .label = QT_TR_NOOP("Seek thumbnail count"),
                 .description = QT_TR_NOOP("How many seek-bar thumbnails to build."),
                 .int_lo = vrp::kSeekThumbCountMin,
                 .int_hi = vrp::kSeekThumbCountMax});
    e.push_back({.key = "thumb_edge",
                 .category = Category::Thumbs,
                 .kind = ValueKind::Int,
                 .visibility = Visibility::Advanced,
                 .default_value = "4096",
                 .label = QT_TR_NOOP("Seek thumbnail max edge"),
                 .description = QT_TR_NOOP("Max long-edge pixels when building seek thumbnails."),
                 .int_lo = vrp::kSeekThumbEdgeMin,
                 .int_hi = vrp::kSeekThumbEdgeMax});
    e.push_back({.key = "thumb_jpeg",
                 .category = Category::Thumbs,
                 .kind = ValueKind::Bool01,
                 .visibility = Visibility::Advanced,
                 .default_value = "1",
                 .label = QT_TR_NOOP("Seek thumbnails as JPEG"),
                 .description = QT_TR_NOOP("Store seek thumbs as JPEG in memory (saves RAM).")});

    // --- Hidden ---
    e.push_back({.key = "live_restore_token",
                 .category = Category::Hidden,
                 .kind = ValueKind::String,
                 .visibility = Visibility::Secret,
                 .default_value = "",
                 .label = QT_TR_NOOP("Live restore token"),
                 .string_allow_empty = true});
    e.push_back({.key = "last_dir",
                 .category = Category::Hidden,
                 .kind = ValueKind::String,
                 .visibility = Visibility::Auto,
                 .default_value = "",
                 .label = QT_TR_NOOP("Last directory"),
                 .string_allow_empty = false});
    e.push_back({.key = "geometry",
                 .category = Category::Hidden,
                 .kind = ValueKind::String,
                 .visibility = Visibility::Auto,
                 .default_value = "",
                 .label = QT_TR_NOOP("Window geometry"),
                 .string_allow_empty = false});
    e.push_back({.key = "modes",
                 .category = Category::Hidden,
                 .kind = ValueKind::String,
                 .visibility = Visibility::Auto,
                 .default_value = "",
                 .label = QT_TR_NOOP("Display modes"),
                 .string_allow_empty = false});

    return e;
  }();
  return kEntries;
}

QString categoryTitle(Category c) {
  switch (c) {
    case Category::Session:
      return QStringLiteral("Session");
    case Category::Video:
      return QStringLiteral("Video");
    case Category::Audio:
      return QStringLiteral("Audio");
    case Category::Sky:
      return QStringLiteral("Sky");
    case Category::Live:
      return QStringLiteral("Live");
    case Category::Thumbs:
      return QStringLiteral("Thumbnail");
    case Category::Other:
      return QStringLiteral("Other");
    case Category::Hidden:
      return QStringLiteral("Internal");
  }
  return QStringLiteral("Other");
}

QString categorySectionComment(Category c) {
  return QStringLiteral("# --- %1 ---").arg(categoryTitle(c));
}

std::optional<PrefsPage> prefsPageFor(const Entry& e) {
  if (e.visibility != Visibility::User && e.visibility != Visibility::Advanced) {
    return std::nullopt;
  }
  const QLatin1String key(e.key);
  // Volume: HMD menu only. Seat FOV/distance: HMD presets only.
  // Seek thumbs (thumb_*): unused (strip disabled). File-dialog preview uses file_thumb_*.
  // live_chrome_scale: always leave at 1 (Wayland follows output / X11 uses virtual scale).
  // live_capture=window is disabled (monitor+virtual only).
  if (key == QLatin1String("volume") || key == QLatin1String("flat_fov") ||
      key == QLatin1String("flat_distance") || key == QLatin1String("flat_fov_app") ||
      key == QLatin1String("flat_distance_app") || key == QLatin1String("live_chrome_scale") ||
      key == QLatin1String("live_capture") || key == QLatin1String("thumb_count") ||
      key == QLatin1String("thumb_edge") || key == QLatin1String("thumb_jpeg")) {
    return std::nullopt;
  }
  // Decode page: hwaccel / Vulkan / seek log (conf Category may still be Video).
  if (key == QLatin1String("hwaccel") || key == QLatin1String("vulkan_video") ||
      key == QLatin1String("vulkan_h264_cb_cuda") || key == QLatin1String("seek_detail_log")) {
    return PrefsPage::Decode;
  }
  switch (e.category) {
    case Category::Video:
      return PrefsPage::Video;
    case Category::Audio:
      return PrefsPage::Audio;
    case Category::Sky:
      return PrefsPage::Sky;
    case Category::Live:
      return PrefsPage::Live;
    case Category::Other:
      return PrefsPage::Other;
    case Category::Thumbs:
      return PrefsPage::Thumbnail;
    case Category::Session:
      // Hz belongs with display; pad/hand/gamepad/runtime stay in existing menus.
      if (key == QLatin1String("hz")) return PrefsPage::Video;
      return std::nullopt;
    case Category::Hidden:
      return std::nullopt;
  }
  return std::nullopt;
}

QString prefsPageTitle(PrefsPage p) {
  // English source; monasphere_ja.json maps to 映像/音声/空/Live/Live URL/デコード/サムネイル/その他.
  switch (p) {
    case PrefsPage::Video:
      return QCoreApplication::translate("Preferences", "Video");
    case PrefsPage::Audio:
      return QCoreApplication::translate("Preferences", "Audio");
    case PrefsPage::Sky:
      return QCoreApplication::translate("Preferences", "Sky");
    case PrefsPage::Live:
      return QCoreApplication::translate("Preferences", "Live");
    case PrefsPage::LiveUrl:
      return QCoreApplication::translate("Preferences", "Live URL");
    case PrefsPage::Decode:
      return QCoreApplication::translate("Preferences", "Decode");
    case PrefsPage::Thumbnail:
      return QCoreApplication::translate("Preferences", "Thumbnail");
    case PrefsPage::Other:
      return QCoreApplication::translate("Preferences", "Other");
  }
  return {};
}

QVector<Entry> prefsEntries() {
  QVector<Entry> out;
  for (const Entry& e : catalog()) {
    if (prefsPageFor(e)) out.push_back(e);
  }
  return out;
}

QHash<QString, QString> defaultHash() {
  QHash<QString, QString> out;
  for (const Entry& e : catalog()) {
    if (e.visibility == Visibility::List) continue;
    if (e.visibility == Visibility::Auto) continue;  // only when caller supplies
    if (!e.default_value) continue;
    out.insert(QString::fromLatin1(e.key), QString::fromUtf8(e.default_value));
  }
  return out;
}

bool assignmentOk(const QString& code, bool allow_legacy) {
  if (listAssignmentOk(code)) return true;

  if (allow_legacy) {
    QString v;
    if (confKeyIs(code, QStringLiteral("youtube_url"), &v)) return !v.isEmpty();
    const QRegularExpression re(QStringLiteral("^live_restore_token\\[x([1-3])\\]=(.*)$"));
    if (re.match(code).hasMatch()) return true;
  }

  for (const Entry& e : catalog()) {
    QString v;
    if (!confKeyIs(code, QString::fromLatin1(e.key), &v)) continue;
    if (QLatin1String(e.key) == QLatin1String("hz")) {
      bool ok = false;
      const int n = v.toInt(&ok);
      return ok && (n == 0 || n == 90 || n == 120);
    }
    if (QLatin1String(e.key) == QLatin1String("modes")) return modesOk(v);
    return entryValueOk(e, v);
  }
  return false;
}

QString confPath() {
  return QDir::homePath() + QStringLiteral("/.config/monasphere/monasphere.conf");
}

void ensureConfKeys(const QHash<QString, QString>& defaults) {
  const QString path = confPath();
  QDir().mkpath(QDir::homePath() + QStringLiteral("/.config/monasphere"));
  QStringList lines;
  QSet<QString> seen;
  QSet<QString> section_present;
  QFile in(path);
  if (in.open(QIODevice::ReadOnly | QIODevice::Text)) {
    while (!in.atEnd()) {
      QString line = QString::fromUtf8(in.readLine());
      if (line.endsWith(QLatin1Char('\n'))) line.chop(1);
      if (line.endsWith(QLatin1Char('\r'))) line.chop(1);
      const QString trimmed = line.trimmed();
      lines << trimmed;
      if (trimmed.startsWith(QLatin1String("# --- ")) && trimmed.endsWith(QLatin1String(" ---"))) {
        section_present.insert(trimmed);
      }
      const QString code = confCode(trimmed);
      const int eq = code.indexOf(QLatin1Char('='));
      if (eq > 0) seen.insert(code.left(eq));
    }
    in.close();
  }

  // Append missing keys in catalog order, grouped by category section comments.
  bool added = lines.isEmpty() && !QFile::exists(path);
  Category last_section = Category::Hidden;
  bool have_last = false;

  auto append_for_key = [&](const QString& key, const QString& value, Category cat) {
    if (seen.contains(key)) return;
    const QString hdr = categorySectionComment(cat);
    if (!have_last || last_section != cat) {
      if (!section_present.contains(hdr)) {
        if (!lines.isEmpty() && !lines.last().isEmpty()) lines << QString();
        lines << hdr;
        section_present.insert(hdr);
      }
      last_section = cat;
      have_last = true;
    }
    lines << key + QLatin1Char('=') + value;
    seen.insert(key);
    added = true;
  };

  // Catalog order first.
  for (const Entry& e : catalog()) {
    const QString key = QString::fromLatin1(e.key);
    if (!defaults.contains(key)) continue;
    append_for_key(key, defaults.value(key), e.category);
  }
  // Any extra keys in defaults not in catalog (should be rare).
  for (auto it = defaults.cbegin(); it != defaults.cend(); ++it) {
    if (seen.contains(it.key())) continue;
    append_for_key(it.key(), it.value(), Category::Hidden);
  }

  if (!added) return;
  QFile out(path);
  if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) return;
  QTextStream ts(&out);
  for (const QString& line : lines) ts << line << '\n';
}

}  // namespace vrp::settings
