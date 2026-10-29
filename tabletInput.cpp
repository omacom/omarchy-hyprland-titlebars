#include "tabletInput.hpp"
#include "barDeco.hpp"

#include <aquamarine/input/Input.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/helpers/MiscFunctions.hpp>
#include <hyprland/src/pointer/PointerManager.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/layout/LayoutManager.hpp>
#include <hyprland/src/state/MonitorState.hpp>
#include <hyprland/src/managers/fullscreen/FullscreenController.hpp>
#include <hyprland/src/config/supplementary/executor/Executor.hpp>

#include <algorithm>
#include <cmath>

COmarchyFloatingTabletInput::COmarchyFloatingTabletInput() {
  m_tip = Event::bus()->m_events.input.tablet.tip.listen([this](auto e, auto &info) { tip(e, info); });
  m_axis = Event::bus()->m_events.input.tablet.axis.listen([this](auto e, auto &info) { axis(e, info); });
  m_proximity = Event::bus()->m_events.input.tablet.proximity.listen([this](auto e, auto &) {
    if (!e.in && e.tool == m_tool.lock() && e.tablet == m_tablet.lock())
      cancelTarget();
  });
  m_close = Event::bus()->m_events.window.close.listen([this](PHLWINDOW window) {
    if (window == m_window.lock())
      cancelTarget();
  });
}

bool COmarchyFloatingTabletInput::captured() const { return !m_tool.expired(); }

bool COmarchyFloatingTabletInput::active() const { return captured() && !m_bar.expired(); }

void COmarchyFloatingTabletInput::cancelTarget() {
  // A cancelled target releases gesture ownership immediately, but the up for
  // its consumed down must still be swallowed. Weak records do not keep a
  // removed device/tool alive and cannot block another tool's next gesture.
  if (captured() && m_tablet) {
    std::erase_if(m_cancelledTips,
                  [this](const auto &tip) { return tip.tool.expired() || tip.tablet.expired() || (tip.tool == m_tool && tip.tablet == m_tablet); });
    m_cancelledTips.push_back({m_tool, m_tablet});
  }
  reset();
}

void COmarchyFloatingTabletInput::cancelWindow(const PHLWINDOW &window) {
  if (window && window == m_window.lock())
    cancelTarget();
}

void COmarchyFloatingTabletInput::reset() {
  m_move = false;
  m_started = false;
  m_bar.reset();
  m_window.reset();
  m_tool.reset();
  m_tablet.reset();
  m_toolDestroy.reset();
  m_tabletDestroy.reset();
}

Vector2D COmarchyFloatingTabletInput::position(const SP<CTablet> &tablet, Vector2D absolute, const Vector2D &delta) {
  // The cancellable tablet events precede Hyprland's cursor update. Apply the
  // same active-area transform and let its pointer manager handle output/region
  // mapping, negative layout origins and scale. No normalized/pixel arithmetic
  // belongs in decoration hit testing or window movement.
  if (tablet->m_relativeInput)
    Pointer::mgr()->move(delta);
  else {
    const auto area = tablet->m_activeArea;
    if (!area.empty()) {
      if (!std::isnan(absolute.x))
        absolute.x = (absolute.x - area.x) / (area.w - area.x);
      if (!std::isnan(absolute.y))
        absolute.y = (absolute.y - area.y) / (area.h - area.y);
    }
    Pointer::mgr()->warpAbsolute(absolute, tablet);
  }
  return g_pInputManager->getMouseCoordsInternal();
}

void COmarchyFloatingTabletInput::tip(CTablet::STipEvent e, Event::SCallbackInfo &info) {
  if (!e.tool || !e.tablet)
    return;
  for (auto it = m_cancelledTips.begin(); it != m_cancelledTips.end();) {
    if (it->tool.expired() || it->tablet.expired()) {
      it = m_cancelledTips.erase(it);
      continue;
    }
    if (it->tool == e.tool && it->tablet == e.tablet) {
      m_cancelledTips.erase(it);
      if (!e.in) {
        info.cancelled = true;
        return;
      }
      break; // A new contact supersedes a missing old release.
    }
    ++it;
  }
  if (captured()) {
    if (e.tool != m_tool.lock() || e.tablet != m_tablet.lock())
      return;
    if (e.in && m_bar.expired()) {
      reset();
      tip(e, info);
      return;
    }
    info.cancelled = true;
    if (!e.in) {
      position(e.tablet, e.tip);
      reset();
    }
    return;
  }
  if (info.cancelled || !e.in || e.tool->type == Aquamarine::ITabletTool::AQ_TABLET_TOOL_TYPE_MOUSE || g_layoutManager->dragController()->target())
    return;

  if (std::ranges::any_of(g_pOmarchyFloatingGlobalState->bars, [](const auto &bar) { return bar && (bar->m_bDragPending || bar->m_bDraggingThis); }))
    return;

  const auto pos = position(e.tablet, e.tip);
  for (const auto &weak : g_pOmarchyFloatingGlobalState->bars) {
    const auto bar = weak;
    if (!bar || !validMapped(bar->m_pWindow) || (bar->m_bDragPending || bar->m_bDraggingThis) || !bar->inputIsValid(pos))
      continue;
    const auto box = bar->assignedBoxGlobal();
    const auto local = pos - box.pos();
    const auto &config = g_pOmarchyFloatingGlobalState->config;
    if (!VECINRECT(local, 0, 0, box.w, config.barHeight->value() - 1))
      continue;

    const auto window = bar->m_pWindow.lock();
    m_tool = e.tool;
    m_tablet = e.tablet;
    m_bar = bar;
    m_window = window;
    m_toolDestroy = e.tool->events.destroy.listen([this] { reset(); });
    m_tabletDestroy = e.tablet->m_events.destroy.listen([this] { reset(); });
    m_press = pos;
    m_position = pos;
    m_grabOffset = pos - window->layoutTarget()->position().pos();
    m_anchor = std::clamp(local.x / std::max(1.0, box.w), 0.0, 1.0);
    m_barOffset = local.y;
    info.cancelled = true;
    Desktop::focusState()->fullWindowFocus(window, Desktop::FOCUS_REASON_CLICK);
    if (window->m_isFloating)
      Desktop::windowState()->raise(window);

    if (!bar || !active() || !validMapped(window)) {
      cancelTarget();
      return;
    }
    if (bar->doButtonPress(config.barPadding->value(), config.barButtonPadding->value(), config.barHeight->value(), local,
                           config.barButtonsAlignment->value() != "left"))
      return;
    const auto now = Time::steadyNow();
    const auto &action = config.onDoubleClick->value();
    if (!action.empty() && m_lastTapBar == bar && m_lastTapTool.lock() == e.tool &&
        std::chrono::duration_cast<std::chrono::milliseconds>(now - m_lastTap).count() < 400) {
      Config::Supplementary::executor()->spawn(bar->windowCommand(action, window));
      m_lastTapBar.reset();
    } else {
      m_lastTap = now;
      m_lastTapBar = bar;
      m_lastTapTool = e.tool;
      m_move = true;
    }
    return;
  }
}

void COmarchyFloatingTabletInput::axis(CTablet::SAxisEvent e, Event::SCallbackInfo &info) {
  if (!captured() || e.tool != m_tool.lock() || e.tablet != m_tablet.lock())
    return;
  if (!m_bar || !validMapped(m_window) || m_bar->m_hidden || !g_pOmarchyFloatingGlobalState->config.enabled->value()) {
    cancelTarget();
    return;
  }
  info.cancelled = true;
  const bool x = e.updatedAxes & CTablet::HID_TABLET_TOOL_AXIS_X;
  const bool y = e.updatedAxes & CTablet::HID_TABLET_TOOL_AXIS_Y;
  if (!x && !y)
    return;
  // A different device may have moved the shared cursor since our last event.
  // Missing/relative axes belong to this tool, not that other device.
  if (e.tablet->m_relativeInput || !x || !y)
    Pointer::mgr()->warpTo(m_position);
  const auto pos = position(e.tablet, {x ? e.axis.x : NAN, y ? e.axis.y : NAN}, {x ? e.axisDelta.x : 0, y ? e.axisDelta.y : 0});
  m_position = pos;
  const auto bar = m_bar;
  const auto window = m_window.lock();
  if (!bar || !validMapped(window) || bar->m_hidden || !g_pOmarchyFloatingGlobalState->config.enabled->value()) {
    cancelTarget();
    return;
  }
  if (!m_move || (!m_started && pos == m_press))
    return;

  // Contact motion never enters Hyprland's mouse bind drag controller. Move the
  // captured target directly, preserving the initial grab rather than centering
  // the window on the pen or choosing the currently focused window.
  if (!m_started) {
    if (Fullscreen::controller()->isFullscreen(window, Fullscreen::FSMODE_MAXIMIZED)) {
      Fullscreen::controller()->setFullscreenMode(window, Fullscreen::FSMODE_NONE);
      m_grabOffset = {window->layoutTarget()->position().w * m_anchor, m_barOffset - window->getFullWindowReservedArea().topLeft.y};
    }
    m_started = true;
    m_lastTapBar.reset();
  }
  auto box = window->layoutTarget()->position();
  box.x = pos.x - m_grabOffset.x;
  box.y = pos.y - m_grabOffset.y;
  g_layoutManager->setTargetGeom(box, window->layoutTarget());
  // Match Hyprland's floating drag monitor selection without borrowing its
  // mouse-owned drag controller.
  const auto monitor = State::monitorState()->query().vec(box.middle()).run();
  if (monitor && monitor->m_activeWorkspace) {
    const auto workspace = monitor->m_activeSpecialWorkspace ? monitor->m_activeSpecialWorkspace : monitor->m_activeWorkspace;
    if (window->layoutTarget()->space() != workspace->m_space) {
      window->layoutTarget()->assignToSpace(workspace->m_space);
      // Space assignment may reposition a floating target to fit its new
      // output. Keep the pen grab in global logical coordinates.
      g_layoutManager->setTargetGeom(box, window->layoutTarget());
    }
  }
  window->layoutTarget()->warpPositionSize();
}
