#include "decorationMode.hpp"

#include <hyprland/src/protocols/WaylandProtocol.hpp>
#include <hyprland/src/protocols/XDGShell.hpp>
#include <hyprland/protocols/xdg-decoration-unstable-v1.hpp>

// Hyprland keeps each toplevel's decoration request to itself, so its table of
// them is read through the same door barDeco.hpp opens for the input manager.
// Everything that header includes is in already, so only it is opened.
#define private public
#include <hyprland/src/protocols/XDGDecoration.hpp>
#undef private

bool omarchyWantsServerDecorations(PHLWINDOW window) {
  if (!window || !PROTO::xdgDecoration)
    return false;

  for (const auto& [resource, decoration] : PROTO::xdgDecoration->m_decorations) {
    const auto toplevel = CXDGToplevelResource::fromResource(resource);
    if (toplevel && toplevel->m_window == window)
      return decoration->mostRecentlyRequested == ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE;
  }

  return false;
}
