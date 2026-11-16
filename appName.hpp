#pragma once

#include <string>

// The Name of the desktop entry whose StartupWMClass, or else whose file name,
// matches a window class. Empty when no entry claims the class.
std::string omarchyAppName(const std::string& windowClass);

struct SAppIcon {
  std::string path;
  // Moves when the file is redrawn in place, as Omarchy's icon sets do when the
  // theme changes.
  long long stamp = 0;
};

// The icon file of the desktop entry that claims a window class (or else an
// icon named after the class), looked up in hicolor and then
// /usr/share/pixmaps. The path is empty when none is found.
SAppIcon omarchyAppIcon(const std::string& windowClass);
