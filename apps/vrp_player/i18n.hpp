#pragma once

#include <QCoreApplication>
#include <QString>

class QApplication;

// English and Japanese JSON are embedded. Other locales load from
// share/monasphere/translations/ or MONASPHERE_TRANSLATIONS_DIR.
// Locale: MONASPHERE_LANG, then the system locale, then English.
void MonasphereInstallTranslations(QApplication& app);

/** HUD / VR menu strings (context = monaSphere). Missing keys stay English. */
inline QString VRP_TR(const char* source) {
  return QCoreApplication::translate("monaSphere", source);
}
