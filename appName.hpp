#pragma once

#include <string>

// The Name of the desktop entry whose StartupWMClass, or else whose file name,
// matches a window class. Empty when no entry claims the class.
std::string omarchyAppName(const std::string& windowClass);

// The icon file of the desktop entry that claims a window class, looked up in
// the GTK icon theme, what it inherits, and hicolor. Empty when none is found.
std::string omarchyAppIcon(const std::string& windowClass);
