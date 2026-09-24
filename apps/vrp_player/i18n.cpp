#include "i18n.hpp"

#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLibraryInfo>
#include <QLocale>
#include <QStandardPaths>
#include <QTranslator>

#include <cstdio>

namespace {

const char* const kEmbeddedLocales[] = {"en", "ja"};

bool IsEmbeddedLocale(const QString& locale) {
  for (const char* embedded : kEmbeddedLocales) {
    if (locale == QLatin1String(embedded)) return true;
  }
  return false;
}

QString NormalizeLocaleTag(QString tag) {
  tag = tag.trimmed().replace(QLatin1Char('-'), QLatin1Char('_'));
  if (tag.compare(QStringLiteral("jp"), Qt::CaseInsensitive) == 0) return QStringLiteral("ja");
  if (tag.compare(QStringLiteral("zh"), Qt::CaseInsensitive) == 0 ||
      tag.startsWith(QStringLiteral("zh_hans"), Qt::CaseInsensitive) ||
      tag.startsWith(QStringLiteral("zh_cn"), Qt::CaseInsensitive)) {
    return QStringLiteral("zh_CN");
  }
  if (tag.startsWith(QStringLiteral("zh_tw"), Qt::CaseInsensitive) ||
      tag.startsWith(QStringLiteral("zh_hk"), Qt::CaseInsensitive) ||
      tag.startsWith(QStringLiteral("zh_hant"), Qt::CaseInsensitive)) {
    return QStringLiteral("zh_TW");
  }
  if (tag.compare(QStringLiteral("C"), Qt::CaseInsensitive) == 0 ||
      tag.compare(QStringLiteral("POSIX"), Qt::CaseInsensitive) == 0) {
    return QStringLiteral("en");
  }
  return tag;
}

QString ResolveUiLocaleTag() {
  const QByteArray forced = qgetenv("MONASPHERE_LANG");
  if (!forced.isEmpty()) return NormalizeLocaleTag(QString::fromUtf8(forced));

  const QLocale locale = QLocale::system();
  switch (locale.language()) {
    case QLocale::Japanese: return QStringLiteral("ja");
    case QLocale::English: return QStringLiteral("en");
    case QLocale::Korean: return QStringLiteral("ko");
    case QLocale::German: return QStringLiteral("de");
    case QLocale::French: return QStringLiteral("fr");
    case QLocale::Spanish: return QStringLiteral("es");
    case QLocale::Chinese: {
      const QString name = locale.name();
      if (name.startsWith(QStringLiteral("zh_TW"), Qt::CaseInsensitive) ||
          name.startsWith(QStringLiteral("zh_HK"), Qt::CaseInsensitive) ||
          name.startsWith(QStringLiteral("zh_Hant"), Qt::CaseInsensitive)) {
        return QStringLiteral("zh_TW");
      }
      return QStringLiteral("zh_CN");
    }
    default: break;
  }
  return NormalizeLocaleTag(locale.name());
}

QStringList LocaleCandidates(const QString& locale_tag) {
  QStringList candidates;
  const QString normalized = NormalizeLocaleTag(locale_tag);
  if (!normalized.isEmpty()) candidates << normalized;
  const int sep = normalized.indexOf(QLatin1Char('_'));
  if (sep > 0) {
    const QString language = normalized.left(sep);
    if (!candidates.contains(language)) candidates << language;
  }
  return candidates;
}

QStringList TranslationSearchPaths() {
  QStringList paths;
  const QByteArray env_dir = qgetenv("MONASPHERE_TRANSLATIONS_DIR");
  if (!env_dir.isEmpty()) paths << QDir::cleanPath(QString::fromLocal8Bit(env_dir));

  for (const QString& data_dir : QStandardPaths::standardLocations(QStandardPaths::GenericDataLocation)) {
    const QString path = data_dir + QStringLiteral("/monasphere/translations");
    if (!paths.contains(path)) paths << path;
  }

  const QString app_dir = QCoreApplication::applicationDirPath();
  for (const QString& path : {QDir(app_dir).filePath(QStringLiteral("../share/monasphere/translations")),
                              QDir(app_dir).filePath(QStringLiteral("translations"))}) {
    const QString cleaned = QDir::cleanPath(path);
    if (!paths.contains(cleaned)) paths << cleaned;
  }
  return paths;
}

class JsonTranslator final : public QTranslator {
 public:
  explicit JsonTranslator(QObject* parent = nullptr) : QTranslator(parent) {}

  bool loadLocale(const QString& locale_tag) {
    map_.clear();
    loaded_locale_.clear();
    loaded_from_.clear();
    for (const QString& candidate : LocaleCandidates(locale_tag)) {
      if (IsEmbeddedLocale(candidate)) {
        const QString resource_path = QStringLiteral(":/i18n/monasphere_%1.json").arg(candidate);
        if (loadFromResource(resource_path)) {
          loaded_locale_ = candidate;
          loaded_from_ = resource_path;
          return true;
        }
        continue;
      }
      for (const QString& dir : TranslationSearchPaths()) {
        const QString file_path = dir + QStringLiteral("/monasphere_") + candidate + QStringLiteral(".json");
        if (loadFromFile(file_path)) {
          loaded_locale_ = candidate;
          loaded_from_ = file_path;
          return true;
        }
      }
    }
    return false;
  }

  QString translate(const char* context, const char* sourceText, const char* disambiguation,
                    int n) const override {
    Q_UNUSED(disambiguation);
    Q_UNUSED(n);
    if (!context || !sourceText) return {};
    const auto it = map_.constFind(QByteArray(context) + '\0' + sourceText);
    if (it != map_.constEnd()) return it.value();
    return {};
  }

  const QString& loadedLocale() const { return loaded_locale_; }
  const QString& loadedFrom() const { return loaded_from_; }
  int entryCount() const { return map_.size(); }

 private:
  bool loadFromResource(const QString& resource_path) {
    QFile file(resource_path);
    if (!file.open(QIODevice::ReadOnly)) return false;
    return ingestJson(file.readAll());
  }
  bool loadFromFile(const QString& file_path) {
    QFile file(file_path);
    if (!file.open(QIODevice::ReadOnly)) return false;
    return ingestJson(file.readAll());
  }
  bool ingestJson(const QByteArray& bytes) {
    const QJsonDocument doc = QJsonDocument::fromJson(bytes);
    const QJsonArray array = doc.isObject() ? doc.object().value(QStringLiteral("translations")).toArray()
                                            : doc.array();
    if (array.isEmpty()) return false;
    QHash<QByteArray, QString> next;
    for (const QJsonValue& entry : array) {
      if (!entry.isObject()) continue;
      const QJsonObject obj = entry.toObject();
      const QString context = obj.value(QStringLiteral("context")).toString();
      const QString source = obj.value(QStringLiteral("source")).toString();
      const QString translation = obj.value(QStringLiteral("translation")).toString();
      if (context.isEmpty() || source.isEmpty() || translation.isEmpty()) continue;
      next.insert(context.toUtf8() + '\0' + source.toUtf8(), translation);
    }
    if (next.isEmpty()) return false;
    map_ = std::move(next);
    return true;
  }

  QHash<QByteArray, QString> map_;
  QString loaded_locale_;
  QString loaded_from_;
};

bool InstallAppTranslator(QApplication& app, const QString& locale_tag) {
  auto* app_translator = new JsonTranslator(&app);
  if (app_translator->loadLocale(locale_tag)) {
    app.installTranslator(app_translator);
    std::fprintf(stderr, "monasphere: UI language: %s (%d strings, %s)\n",
                 app_translator->loadedLocale().toUtf8().constData(), app_translator->entryCount(),
                 app_translator->loadedFrom().toUtf8().constData());
    return true;
  }
  delete app_translator;
  return false;
}

}  // namespace

void MonasphereInstallTranslations(QApplication& app) {
  const QString requested = ResolveUiLocaleTag();
  if (requested.startsWith(QStringLiteral("en"))) {
    if (!InstallAppTranslator(app, QStringLiteral("en"))) {
      std::fprintf(stderr, "monasphere: UI language: English\n");
    }
    return;
  }

  if (!requested.startsWith(QStringLiteral("en"))) {
    auto* qt_translator = new QTranslator(&app);
    const QString path = QLibraryInfo::path(QLibraryInfo::TranslationsPath);
    bool qt_ok = false;
    for (const QString& candidate : LocaleCandidates(requested)) {
      if (qt_translator->load(QStringLiteral("qtbase_") + candidate, path)) {
        app.installTranslator(qt_translator);
        qt_ok = true;
        break;
      }
    }
    if (!qt_ok) delete qt_translator;
  }

  if (InstallAppTranslator(app, requested)) return;
  std::fprintf(stderr, "monasphere: UI translations for %s missing; using English\n",
               requested.toUtf8().constData());
  InstallAppTranslator(app, QStringLiteral("en"));
}
