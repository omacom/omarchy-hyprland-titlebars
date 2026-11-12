#pragma once

// What a Hyprland 0.57 plugin needs to give Lua for window snapping, written once
// for two hosts: the standalone snap-drag plugin (host.cpp) and Omarchy's
// titlebars plugin.
//
// Events, for every window move drag (a titlebar, SUPER + mouse, an app's own
// titlebar), each with (window, x, y), the pointer in layout coordinates:
//   <prefix>.drag_start   the first motion; a click without motion reports nothing
//   <prefix>.drag_move    each motion after
//   <prefix>.drag_end     after Hyprland's drop handling, or when the window closed
//
// Lua functions, under hl.plugin.<namespace>:
//   extents(window) -> { left, top, right, bottom }
//       what decorations (a titlebar, a groupbar) and the border add around
//       the client area
//   limits(window)  -> { min = { x, y }, max = { x, y } }
//       the smallest and largest client size it takes, logical px, each side
//       only where there is a limit
//
// Restore on drag: a window tagged `snap-restore:WxH/SWxSH` (the size to go back
// to / the size it was snapped to) gets WxH back on its first motion, with the
// pointer on the same spot of it; the tag goes. A window no longer SWxSH was
// resized since: it keeps its size and the tag goes.

#include <hyprland/src/plugins/PluginAPI.hpp>

#include <string>

namespace DragTracker {
  struct SConfig {
    HANDLE      handle = nullptr;
    std::string luaNamespace; // "snap_drag" -> hl.plugin.snap_drag.extents
    std::string eventPrefix;  // "snap"      -> snap.drag_start
  };

  void init(const SConfig& config);
  void exit();
}
