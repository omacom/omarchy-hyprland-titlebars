#pragma once

#include <string>

// The Name of the desktop entry whose StartupWMClass, or else whose file name,
// matches a window class. Empty when no entry claims the class.
std::string omarchyAppName(const std::string& windowClass);

// Explicit opt-in from a matching desktop entry. The app must draw its own
// caption/controls inside a frameless window, without toolkit CSD shadows.
bool omarchyAppHasClientControls(const std::string& windowClass);
