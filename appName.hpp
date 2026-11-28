#pragma once

#include <string>

// The Name of the desktop entry whose StartupWMClass, or else whose file name,
// matches a window class. Empty when no entry claims the class.
std::string omarchyAppName(const std::string& windowClass);

// The icon file of the desktop entry that claims a window class, or of an icon
// named after the class when no entry does, as the Omarchy shell's dock and
// menu find it. Empty when there is none.
std::string omarchyAppIcon(const std::string& windowClass);
