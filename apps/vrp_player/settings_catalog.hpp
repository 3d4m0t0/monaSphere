#pragma once

#include <QHash>
#include <QString>
#include <QStringList>
#include <QVector>

#include <optional>

namespace vrp::settings {

enum class Category {
  Session = 0,
  Video,
  Audio,
  Sky,
  Live,
  Thumbs,
  Other,
  Hidden,
};

/** Preferences dialog sidebar pages (KDE System Settings style). */
enum class PrefsPage {
  Video = 0,  // 映像
  Audio,      // 音声
  Sky,        // 空
  Live,       // Live
  LiveUrl,    // Live URL (#0–#9)
  Decode,     // デコード
  Thumbnail,  // サムネイル（ファイルダイアログ・プレビュー）
  Other,      // その他
};

enum class ValueKind {
  Bool01,     // "0" / "1" (audio_normalize also accepts on/off)
  Int,
  Float,
  Enum,       // case-sensitive unless enum_ignore_case
  Flags,      // bitmask int; enum_values[i] = bit i label (e.g. FSR scales)
  String,     // any non-empty, or empty allowed
  SizeWxH,    // e.g. 1280x720
};

enum class Visibility {
  User,      // Preferences (phase 2)
  Advanced,  // Preferences advanced section
  Secret,    // not in Preferences (token)
  Auto,      // geometry / last_dir / modes
  List,      // live_app* / live_desktop_app*
};

struct Entry {
  const char* key = nullptr;
  Category category = Category::Hidden;
  ValueKind kind = ValueKind::String;
  Visibility visibility = Visibility::User;
  const char* default_value = "";
  /** QT_TR_NOOP source for Preferences labels. */
  const char* label = "";
  /** QT_TR_NOOP short help under the control in Preferences. */
  const char* description = "";
  float float_lo = 0.f;
  float float_hi = 0.f;
  int int_lo = 0;
  int int_hi = 0;
  /** Null-terminated list for Enum (or nullptr). */
  const char* const* enum_values = nullptr;
  bool enum_ignore_case = false;
  bool string_allow_empty = false;
  /** audio_normalize: also accept on/off. */
  bool bool_accept_on_off = false;
};

/** Stable catalog order (category then key). */
const QVector<Entry>& catalog();

QString categoryTitle(Category c);
QString categorySectionComment(Category c);

/** Preferences page for an entry, or empty if not shown in the dialog. */
std::optional<PrefsPage> prefsPageFor(const Entry& e);
QString prefsPageTitle(PrefsPage p);
/** User + Advanced entries that belong on Preferences pages, catalog order. */
QVector<Entry> prefsEntries();

/** Defaults for scalar keys that belong in monasphere.conf (not List/Auto-only). */
QHash<QString, QString> defaultHash();

/**
 * True if `code` (assignment without comment) is a known key with a valid value.
 * List keys and legacy youtube_* / span tokens handled when allow_legacy is set.
 */
bool assignmentOk(const QString& code, bool allow_legacy);

/**
 * Ensure missing keys from `defaults` exist. Appends in catalog order with
 * `# --- Category ---` section comments. Never overwrites existing values.
 */
void ensureConfKeys(const QHash<QString, QString>& defaults);

QString confPath();

}  // namespace vrp::settings
