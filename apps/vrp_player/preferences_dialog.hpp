#pragma once

#include "settings_catalog.hpp"

#include <QDialog>
#include <QHash>
#include <QString>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QFormLayout;
class QLineEdit;
class QListWidget;
class QSpinBox;
class QStackedWidget;
class QWidget;

/**
 * KDE System Settings–style preferences: sidebar categories + stacked pages.
 * Pages: Video / Audio / Sky / Live / Decode.
 */
class PreferencesDialog : public QDialog {
  Q_OBJECT
 public:
  explicit PreferencesDialog(const QHash<QString, QString>& values, QWidget* parent = nullptr);

  /** Current widget values as conf strings (key → value). */
  QHash<QString, QString> values() const;

 signals:
  void applyRequested(const QHash<QString, QString>& values);

 private:
  void buildUi(const QHash<QString, QString>& values);
  QWidget* buildPage(vrp::settings::PrefsPage page, const QHash<QString, QString>& values);
  void addEntryRow(QFormLayout* form, const vrp::settings::Entry& e, const QString& value);
  /** Live page: fixed slots live_app[0]…[9] (label + URL). */
  void addLiveUrlSection(QFormLayout* form, const QHash<QString, QString>& values);
  /** Load a value hash into existing editors (Reset / Defaults). */
  void setEditorsFrom(const QHash<QString, QString>& values);
  /** Catalog defaults for keys that appear in this dialog. */
  static QHash<QString, QString> prefsDefaults();
  /** When unlock is off, HRTF trim is fixed at 10 dB and the spinbox is disabled. */
  void wireHrtfTrimLock();
  /** When “Sky from timezone” is on, lat/lon editors are disabled (grayed out). */
  void wireSkyFromTimezone();

  static constexpr int kLiveAppSlots = 10;

  QListWidget* sidebar_ = nullptr;
  QStackedWidget* stack_ = nullptr;
  QHash<QString, QWidget*> editors_;
  /** Values when the dialog opened — “Reset” restores these. */
  QHash<QString, QString> initial_values_;
};
