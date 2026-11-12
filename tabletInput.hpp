#pragma once

#include <hyprland/src/devices/Tablet.hpp>
#include <hyprland/src/desktop/DesktopTypes.hpp>
#include <hyprland/src/helpers/time/Time.hpp>
#include <hyprland/src/helpers/signal/Signal.hpp>
#include <vector>

class COmarchyFloatingHyprBar;
namespace Event {
struct SCallbackInfo;
}

// One capture for the plugin, rather than one listener/capture per decoration.
// The capture outlives its target so a consumed down always has a consumed up.
class COmarchyFloatingTabletInput {
public:
  COmarchyFloatingTabletInput();
  bool captured() const;
  bool active() const;
  void cancelTarget();
  void cancelWindow(const PHLWINDOW &window);

private:
  void tip(CTablet::STipEvent e, Event::SCallbackInfo &info);
  void axis(CTablet::SAxisEvent e, Event::SCallbackInfo &info);
  void reset();
  Vector2D position(const SP<CTablet> &tablet, Vector2D absolute, const Vector2D &delta = {});

  struct SCancelledTip {
    WP<Aquamarine::ITabletTool> tool;
    WP<CTablet> tablet;
  };
  std::vector<SCancelledTip> m_cancelledTips;
  WP<Aquamarine::ITabletTool> m_tool, m_lastTapTool;
  WP<CTablet> m_tablet;
  // Decorations are uniquely owned: observe these weak pointers directly;
  // unlike shared tablet/window objects, they cannot be promoted with lock().
  WP<COmarchyFloatingHyprBar> m_bar, m_lastTapBar;
  PHLWINDOWREF m_window;
  Time::steady_tp m_lastTap{};
  Vector2D m_press, m_position, m_grabOffset;
  double m_anchor = 0;
  double m_barOffset = 0;
  bool m_move = false;
  bool m_started = false;
  CHyprSignalListener m_tip, m_axis, m_proximity, m_toolDestroy, m_tabletDestroy, m_close;
};

inline UP<COmarchyFloatingTabletInput> g_pOmarchyFloatingTabletInput;
