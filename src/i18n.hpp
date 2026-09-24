#pragma once

/**
 * i18n preparation for monaSphere / vrp_core.
 *
 * Qt UI and user-facing host logs: use HostWindow::tr() or VRP_TR().
 * Developer / GPU diagnostics (stdout [vrp]): keep English VRP_LOG / VRP_DBG.
 *
 * Extract later:
 *   lupdate apps/vrp_player src -ts translations/monaSphere_ja.ts
 *   lrelease translations/monaSphere_ja.ts -qm translations/monaSphere_ja.qm
 * Load in main():
 *   QTranslator t; t.load("monaSphere_" + QLocale::system().name(), ".../translations");
 *   app.installTranslator(&t);
 */

#include <QCoreApplication>
#include <QString>

/** Translate a UI / user-log string (context = monaSphere). */
inline QString VRP_TR(const char* source) {
  return QCoreApplication::translate("monaSphere", source);
}

/** Mark a string for lupdate without translating at the call site. */
#define VRP_TR_NOOP(source) QT_TRANSLATE_NOOP("monaSphere", source)
