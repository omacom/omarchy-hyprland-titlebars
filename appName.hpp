#pragma once

#include <string>

// The Name of the desktop entry whose StartupWMClass, or else whose file name,
// matches a window class. Empty when no entry claims the class.
std::string omarchyAppName(const std::string& windowClass);

// A file holding the icon of the app a window belongs to: the Icon of the
// desktop entry that claims the window's class, else an icon named after the
// class. Empty when none is found.
std::string omarchyAppIcon(const std::string& windowClass);
