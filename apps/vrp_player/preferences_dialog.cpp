#include "preferences_dialog.hpp"

#include <QAbstractItemView>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFontMetrics>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QSizePolicy>
#include <QSpinBox>
#include <QStackedWidget>
#include <QVBoxLayout>

namespace {

void configureFormLayout(QFormLayout* form) {
  // Editors stay Fixed (content width); hint rows Expand and wrap to the field column.
  form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
  form->setFormAlignment(Qt::AlignLeft | Qt::AlignTop);
  form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
  form->setHorizontalSpacing(16);
  form->setVerticalSpacing(12);
  form->setRowWrapPolicy(QFormLayout::DontWrapRows);
}

/** Description under a control: wraps to available width; row height grows with lines. */
class WrappingHintLabel : public QLabel {
 public:
  using QLabel::QLabel;
  bool hasHeightForWidth() const override { return true; }
  int heightForWidth(int w) const override { return QLabel::heightForWidth(std::max(1, w)); }
  QSize sizeHint() const override {
    const int w = width() > 1 ? width() : minimumSizeHint().width();
    return QSize(std::max(w, 120), heightForWidth(std::max(w, 120)));
  }
  QSize minimumSizeHint() const override { return QSize(120, QLabel::minimumSizeHint().height()); }
 protected:
  void resizeEvent(QResizeEvent* e) override {
    QLabel::resizeEvent(e);
    // Recalc row height when the field column width changes (wrap → more lines).
    updateGeometry();
  }
};

/** Combos / spins / short edits: width follows content, not the page. */
void sizeEditorToContents(QWidget* editor, vrp::settings::ValueKind kind, const char* key) {
  if (!editor) return;
  if (auto* combo = qobject_cast<QComboBox*>(editor)) {
    combo->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    combo->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    return;
  }
  if (auto* spin = qobject_cast<QSpinBox*>(editor)) {
    spin->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    return;
  }
  if (auto* dspin = qobject_cast<QDoubleSpinBox*>(editor)) {
    dspin->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    return;
  }
  if (auto* cb = qobject_cast<QCheckBox*>(editor)) {
    cb->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    return;
  }
  if (auto* edit = qobject_cast<QLineEdit*>(editor)) {
    const QFontMetrics fm(edit->font());
    int chars = 12;
    if (kind == vrp::settings::ValueKind::SizeWxH) chars = 10;
    else if (QLatin1String(key) == QLatin1String("live_output")) chars = 24;
    else if (kind == vrp::settings::ValueKind::String) chars = 20;
    const int w = fm.horizontalAdvance(QString(chars, QLatin1Char('M'))) + 24;
    edit->setFixedWidth(w);
    edit->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
  }
}

QString enumDisplayLabel(const char* key, const QString& value) {
  if (QLatin1String(key) == QLatin1String("fsr") ||
      QLatin1String(key) == QLatin1String("live_fsr")) {
    bool ok = false;
    const int n = value.toInt(&ok);
    if (!ok) return value;
    // Video path: scale is upscale from decoded source (larger × = heavier).
    switch (n) {
      case 0:
        return QCoreApplication::translate("Preferences", "Off");
      case 1:
        return QStringLiteral("1.3×");
      case 2:
        return QStringLiteral("1.5×");
      case 3:
        return QStringLiteral("2.0×");
      default:
        return value;
    }
  }
  if (QLatin1String(key) == QLatin1String("hz")) {
    if (value == QLatin1String("0"))
      return QCoreApplication::translate("Preferences", "Auto (90 Hz)");
    if (value == QLatin1String("90")) return QStringLiteral("90 Hz");
    if (value == QLatin1String("120")) return QStringLiteral("120 Hz");
  }
  if (QLatin1String(key) == QLatin1String("sky_backdrop")) {
    if (value.compare(QLatin1String("baked"), Qt::CaseInsensitive) == 0)
      return QCoreApplication::translate("Preferences", "Baked cubemap");
    if (value.compare(QLatin1String("layers"), Qt::CaseInsensitive) == 0)
      return QCoreApplication::translate("Preferences", "Live layers");
  }
  if (QLatin1String(key) == QLatin1String("live_chrome_profile")) {
    if (value.compare(QLatin1String("default"), Qt::CaseInsensitive) == 0)
      return QCoreApplication::translate("Preferences", "Default profile");
    if (value.compare(QLatin1String("isolated"), Qt::CaseInsensitive) == 0)
      return QCoreApplication::translate("Preferences", "Isolated profile");
  }
  if (QLatin1String(key) == QLatin1String("live_browser")) {
    if (value.compare(QLatin1String("firefox"), Qt::CaseInsensitive) == 0)
      return QStringLiteral("Firefox");
    if (value.compare(QLatin1String("chromium"), Qt::CaseInsensitive) == 0)
      return QStringLiteral("Chromium");
    return QStringLiteral("Chrome");
  }
  return value;
}

}  // namespace

PreferencesDialog::PreferencesDialog(const QHash<QString, QString>& values, QWidget* parent)
    : QDialog(parent) {
  setWindowTitle(tr("Preferences"));
  setModal(true);
  resize(720, 520);
  buildUi(values);
}

void PreferencesDialog::buildUi(const QHash<QString, QString>& values) {
  initial_values_ = values;
  auto* root = new QVBoxLayout(this);
  auto* body = new QHBoxLayout;

  sidebar_ = new QListWidget(this);
  sidebar_->setFixedWidth(160);
  sidebar_->setIconSize(QSize(32, 32));
  sidebar_->setSpacing(2);
  sidebar_->setUniformItemSizes(true);
  // KDE System Settings–like: icon+text rows, single selection.
  sidebar_->setMovement(QListWidget::Static);
  sidebar_->setSelectionMode(QAbstractItemView::SingleSelection);

  stack_ = new QStackedWidget(this);

  const vrp::settings::PrefsPage pages[] = {
      vrp::settings::PrefsPage::Video,  vrp::settings::PrefsPage::Audio,
      vrp::settings::PrefsPage::Sky,    vrp::settings::PrefsPage::Live,
      vrp::settings::PrefsPage::Decode, vrp::settings::PrefsPage::Other,
  };
  for (vrp::settings::PrefsPage page : pages) {
    bool has = false;
    for (const vrp::settings::Entry& e : vrp::settings::prefsEntries()) {
      const auto p = vrp::settings::prefsPageFor(e);
      if (p && *p == page) {
        has = true;
        break;
      }
    }
    if (!has) continue;
    auto* item = new QListWidgetItem(vrp::settings::prefsPageTitle(page), sidebar_);
    item->setSizeHint(QSize(item->sizeHint().width(), 40));
    stack_->addWidget(buildPage(page, values));
  }
  sidebar_->setCurrentRow(0);
  connect(sidebar_, &QListWidget::currentRowChanged, stack_, &QStackedWidget::setCurrentIndex);

  body->addWidget(sidebar_);
  body->addWidget(stack_, 1);
  root->addLayout(body, 1);

  auto* buttons =
      new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel | QDialogButtonBox::Apply,
                           this);
  auto* reset_btn = buttons->addButton(tr("Reset"), QDialogButtonBox::ResetRole);
  auto* defaults_btn = buttons->addButton(tr("Defaults"), QDialogButtonBox::ResetRole);
  reset_btn->setToolTip(tr("Restore values from when this dialog was opened."));
  defaults_btn->setToolTip(tr("Restore shipped standard settings for all preferences."));
  connect(reset_btn, &QPushButton::clicked, this, [this] { setEditorsFrom(initial_values_); });
  connect(defaults_btn, &QPushButton::clicked, this, [this] { setEditorsFrom(prefsDefaults()); });
  connect(buttons, &QDialogButtonBox::accepted, this, [this] {
    emit applyRequested(this->values());
    accept();
  });
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  connect(buttons->button(QDialogButtonBox::Apply), &QPushButton::clicked, this, [this] {
    emit applyRequested(this->values());
  });
  root->addWidget(buttons);
}

QWidget* PreferencesDialog::buildPage(vrp::settings::PrefsPage page,
                                      const QHash<QString, QString>& values) {
  auto* scroll = new QScrollArea(this);
  scroll->setWidgetResizable(true);
  scroll->setFrameShape(QFrame::NoFrame);

  auto* page_w = new QWidget(scroll);
  auto* outer = new QVBoxLayout(page_w);

  auto* title = new QLabel(vrp::settings::prefsPageTitle(page), page_w);
  QFont tf = title->font();
  tf.setPointSizeF(tf.pointSizeF() + 2.0);
  tf.setBold(true);
  title->setFont(tf);
  outer->addWidget(title);

  // One form so User + Advanced share the same right-aligned label column.
  auto* form = new QFormLayout;
  configureFormLayout(form);

  bool any_adv = false;
  for (const vrp::settings::Entry& e : vrp::settings::prefsEntries()) {
    const auto p = vrp::settings::prefsPageFor(e);
    if (!p || *p != page) continue;
    if (e.visibility == vrp::settings::Visibility::Advanced) {
      any_adv = true;
      continue;
    }
    const QString key = QString::fromLatin1(e.key);
    QString v = values.value(key);
    if (v.isEmpty() && e.default_value) v = QString::fromUtf8(e.default_value);
    addEntryRow(form, e, v);
  }

  if (any_adv) {
    auto* adv = new QLabel(tr("Advanced"), page_w);
    QFont af = adv->font();
    af.setBold(true);
    adv->setFont(af);
    adv->setContentsMargins(0, 12, 0, 0);
    form->addRow(adv);
    for (const vrp::settings::Entry& e : vrp::settings::prefsEntries()) {
      const auto p = vrp::settings::prefsPageFor(e);
      if (!p || *p != page) continue;
      if (e.visibility != vrp::settings::Visibility::Advanced) continue;
      const QString key = QString::fromLatin1(e.key);
      QString v = values.value(key);
      if (v.isEmpty() && e.default_value) v = QString::fromUtf8(e.default_value);
      addEntryRow(form, e, v);
    }
  }

  outer->addLayout(form);
  wireHrtfTrimLock();
  wireSkyFromTimezone();
  outer->addStretch(1);
  scroll->setWidget(page_w);
  return scroll;
}

void PreferencesDialog::addEntryRow(QFormLayout* form, const vrp::settings::Entry& e,
                                    const QString& value) {
  const QString key = QString::fromLatin1(e.key);
  const QString label = QCoreApplication::translate("Preferences", e.label);
  QWidget* editor = nullptr;

  switch (e.kind) {
    case vrp::settings::ValueKind::Bool01: {
      auto* cb = new QCheckBox(this);
      const bool on = value == QLatin1String("1") ||
                      value.compare(QLatin1String("on"), Qt::CaseInsensitive) == 0;
      cb->setChecked(on);
      editor = cb;
      break;
    }
    case vrp::settings::ValueKind::Int: {
      // Special enums stored as int (fsr / hz).
      if (QLatin1String(e.key) == QLatin1String("fsr") ||
          QLatin1String(e.key) == QLatin1String("live_fsr") ||
          QLatin1String(e.key) == QLatin1String("hz")) {
        auto* combo = new QComboBox(this);
        if (QLatin1String(e.key) == QLatin1String("hz")) {
          const int opts[] = {0, 90, 120};
          for (int o : opts) {
            const QString s = QString::number(o);
            combo->addItem(enumDisplayLabel(e.key, s), s);
          }
        } else {
          for (int o = e.int_lo; o <= e.int_hi; ++o) {
            const QString s = QString::number(o);
            combo->addItem(enumDisplayLabel(e.key, s), s);
          }
        }
        const int idx = combo->findData(value);
        combo->setCurrentIndex(idx >= 0 ? idx : 0);
        editor = combo;
      } else {
        auto* spin = new QSpinBox(this);
        spin->setRange(e.int_lo, e.int_hi);
        bool ok = false;
        const int n = value.toInt(&ok);
        spin->setValue(ok ? n : e.int_lo);
        editor = spin;
      }
      break;
    }
    case vrp::settings::ValueKind::Float: {
      auto* spin = new QDoubleSpinBox(this);
      spin->setRange(static_cast<double>(e.float_lo), static_cast<double>(e.float_hi));
      spin->setDecimals(2);
      spin->setSingleStep(0.05);
      bool ok = false;
      const double n = value.toDouble(&ok);
      spin->setValue(ok ? n : static_cast<double>(e.float_lo));
      editor = spin;
      break;
    }
    case vrp::settings::ValueKind::Enum: {
      auto* combo = new QComboBox(this);
      if (e.enum_values) {
        for (const char* const* p = e.enum_values; *p; ++p) {
          const QString s = QString::fromLatin1(*p);
          combo->addItem(enumDisplayLabel(e.key, s), s);
        }
      }
      int idx = -1;
      for (int i = 0; i < combo->count(); ++i) {
        const QString d = combo->itemData(i).toString();
        if (e.enum_ignore_case ? d.compare(value, Qt::CaseInsensitive) == 0 : d == value) {
          idx = i;
          break;
        }
      }
      combo->setCurrentIndex(idx >= 0 ? idx : 0);
      editor = combo;
      break;
    }
    case vrp::settings::ValueKind::SizeWxH: {
      auto* edit = new QLineEdit(this);
      edit->setPlaceholderText(QStringLiteral("1920x1080"));
      edit->setText(value);
      editor = edit;
      break;
    }
    case vrp::settings::ValueKind::String: {
      auto* edit = new QLineEdit(this);
      edit->setText(value);
      editor = edit;
      break;
    }
  }

  if (!editor) return;
  sizeEditorToContents(editor, e.kind, e.key);
  editors_.insert(key, editor);

  // Editor stays content-sized; stretch fills the rest of the field column.
  auto* editor_cell = new QWidget(this);
  auto* editor_row = new QHBoxLayout(editor_cell);
  editor_row->setContentsMargins(0, 0, 0, 0);
  editor_row->addWidget(editor, 0, Qt::AlignLeft | Qt::AlignVCenter);
  editor_row->addStretch(1);
  editor_cell->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  form->addRow(label, editor_cell);

  if (e.description && e.description[0]) {
    auto* hint = new WrappingHintLabel(
        QCoreApplication::translate("Preferences", e.description), this);
    hint->setWordWrap(true);
    hint->setForegroundRole(QPalette::WindowText);
    hint->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    QFont hf = hint->font();
    if (hf.pointSizeF() > 0) hf.setPointSizeF(hf.pointSizeF() - 1.0);
    hint->setFont(hf);
    hint->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    // Empty label keeps the hint in the field column so it wraps with dialog width.
    form->addRow(QString(), hint);
  }
}

void PreferencesDialog::wireHrtfTrimLock() {
  auto* unlock = qobject_cast<QCheckBox*>(editors_.value(QStringLiteral("hrtf_trim_unlock")));
  auto* trim = qobject_cast<QDoubleSpinBox*>(editors_.value(QStringLiteral("hrtf_trim_db")));
  if (!unlock || !trim) return;
  auto apply = [unlock, trim]() {
    const bool on = unlock->isChecked();
    trim->setEnabled(on);
    if (!on) trim->setValue(10.0);
  };
  connect(unlock, &QCheckBox::toggled, this, apply);
  apply();
}

void PreferencesDialog::wireSkyFromTimezone() {
  auto* from_tz = qobject_cast<QCheckBox*>(editors_.value(QStringLiteral("sky_from_timezone")));
  auto* lat = qobject_cast<QDoubleSpinBox*>(editors_.value(QStringLiteral("sky_lat")));
  auto* lon = qobject_cast<QDoubleSpinBox*>(editors_.value(QStringLiteral("sky_lon")));
  if (!from_tz || (!lat && !lon)) return;
  auto apply = [from_tz, lat, lon]() {
    const bool manual = !from_tz->isChecked();
    if (lat) lat->setEnabled(manual);
    if (lon) lon->setEnabled(manual);
  };
  connect(from_tz, &QCheckBox::toggled, this, apply);
  apply();
}

QHash<QString, QString> PreferencesDialog::prefsDefaults() {
  QHash<QString, QString> out;
  for (const vrp::settings::Entry& e : vrp::settings::prefsEntries()) {
    if (!e.default_value) continue;
    out.insert(QString::fromLatin1(e.key), QString::fromUtf8(e.default_value));
  }
  return out;
}

void PreferencesDialog::setEditorsFrom(const QHash<QString, QString>& values) {
  const QHash<QString, QString> fallback = prefsDefaults();
  for (auto it = editors_.begin(); it != editors_.end(); ++it) {
    const QString& key = it.key();
    QWidget* w = it.value();
    QString v = values.value(key);
    if (v.isEmpty()) v = fallback.value(key);
    if (auto* cb = qobject_cast<QCheckBox*>(w)) {
      const bool on = v == QLatin1String("1") ||
                      v.compare(QLatin1String("on"), Qt::CaseInsensitive) == 0;
      cb->setChecked(on);
    } else if (auto* combo = qobject_cast<QComboBox*>(w)) {
      int idx = combo->findData(v);
      if (idx < 0) {
        for (int i = 0; i < combo->count(); ++i) {
          if (combo->itemData(i).toString().compare(v, Qt::CaseInsensitive) == 0) {
            idx = i;
            break;
          }
        }
      }
      if (idx >= 0) combo->setCurrentIndex(idx);
    } else if (auto* spin = qobject_cast<QSpinBox*>(w)) {
      bool ok = false;
      const int n = v.toInt(&ok);
      if (ok) spin->setValue(n);
    } else if (auto* dspin = qobject_cast<QDoubleSpinBox*>(w)) {
      bool ok = false;
      const double n = v.toDouble(&ok);
      if (ok) dspin->setValue(n);
    } else if (auto* edit = qobject_cast<QLineEdit*>(w)) {
      edit->setText(v);
    }
  }
  // Re-apply dependent enablement (setChecked may no-op if already matching).
  if (auto* unlock = qobject_cast<QCheckBox*>(editors_.value(QStringLiteral("hrtf_trim_unlock")))) {
    if (auto* trim = qobject_cast<QDoubleSpinBox*>(editors_.value(QStringLiteral("hrtf_trim_db")))) {
      const bool on = unlock->isChecked();
      trim->setEnabled(on);
      if (!on) trim->setValue(10.0);
    }
  }
  if (auto* from_tz = qobject_cast<QCheckBox*>(editors_.value(QStringLiteral("sky_from_timezone")))) {
    const bool manual = !from_tz->isChecked();
    if (auto* lat = qobject_cast<QDoubleSpinBox*>(editors_.value(QStringLiteral("sky_lat"))))
      lat->setEnabled(manual);
    if (auto* lon = qobject_cast<QDoubleSpinBox*>(editors_.value(QStringLiteral("sky_lon"))))
      lon->setEnabled(manual);
  }
}

QHash<QString, QString> PreferencesDialog::values() const {
  QHash<QString, QString> out;
  const auto* unlock_cb =
      qobject_cast<const QCheckBox*>(editors_.value(QStringLiteral("hrtf_trim_unlock")));
  const bool hrtf_locked = unlock_cb && !unlock_cb->isChecked();
  for (auto it = editors_.cbegin(); it != editors_.cend(); ++it) {
    const QString& key = it.key();
    QWidget* w = it.value();
    if (auto* cb = qobject_cast<QCheckBox*>(w)) {
      out.insert(key, cb->isChecked() ? QStringLiteral("1") : QStringLiteral("0"));
    } else if (auto* combo = qobject_cast<QComboBox*>(w)) {
      out.insert(key, combo->currentData().toString());
    } else if (auto* spin = qobject_cast<QSpinBox*>(w)) {
      out.insert(key, QString::number(spin->value()));
    } else if (auto* dspin = qobject_cast<QDoubleSpinBox*>(w)) {
      if (key == QLatin1String("hrtf_trim_db") && hrtf_locked) {
        out.insert(key, QStringLiteral("10.00"));
      } else {
        out.insert(key, QString::number(dspin->value(), 'f', 2));
      }
    } else if (auto* edit = qobject_cast<QLineEdit*>(w)) {
      out.insert(key, edit->text().trimmed());
    }
  }
  return out;
}
