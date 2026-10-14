#pragma once

#include <QFont>

// Read the desktop font and color scheme (portal, then KDE, then GTK) and apply
// them to the process. Host Qt themes are not available inside the AppImage.
void applyDesktopAppearance();

// Monospace face from the desktop, including its point size when the desktop
// recorded one. Default-constructed when the desktop has no fixed font.
QFont desktopFixedFont();

// Proportional UI font from the desktop. Default-constructed when unknown.
QFont desktopUiFont();
