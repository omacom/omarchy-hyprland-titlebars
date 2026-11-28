#pragma once

#include <hyprland/src/desktop/DesktopTypes.hpp>

// Whether a window has asked the compositor to decorate it, through
// xdg-decoration. A browser set to use the system title bar asks; one drawing
// its own frame asks for client-side decorations, or for nothing.
bool omarchyWantsServerDecorations(PHLWINDOW window);
