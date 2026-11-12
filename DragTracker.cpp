#include "DragTracker.hpp"

#include <hyprland/src/desktop/view/window/Window.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/layout/LayoutManager.hpp>
#include <hyprland/src/layout/supplementary/DragController.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/config/lua/bindings/LuaBindingsInternal.hpp>

#include <lua.h>

#include <algorithm>
#include <cstdio>
#include <optional>
#include <vector>

using CCustomEvent = Event::CEventBus::CCustomEvent;

namespace {
  DragTracker::SConfig             config;
  SP<CCustomEvent>                 dragStart, dragMove, dragEnd;
  std::vector<CHyprSignalListener> listeners;

  // The drag in progress, once it moved.
  WP<Layout::ITarget> dragging;
  PHLWINDOWREF        draggingWindow;

  constexpr std::string_view RESTORE_TAG = "snap-restore:";

  Vector2D                   cursor() {
    return g_pInputManager->getMouseCoordsInternal();
  }

  void emit(const SP<CCustomEvent>& event, PHLWINDOW window, const Vector2D& at) {
    if (!event || !window)
      return;
    if (auto res = event->emit({PHLWINDOWREF{window}, at.x, at.y}); !res)
      LOG(Log::ERR, "[{}] {} failed: {}", config.eventPrefix, event->m_name, res.error());
  }

  struct SRestore {
    Vector2D    size;    // to go back to
    Vector2D    snapped; // the size it was snapped to
    std::string tag;
  };

  std::optional<SRestore> restoreOf(PHLWINDOW window) {
    for (const auto& tag : window->m_ruleApplicator->m_tagKeeper.getTags()) {
      int w = 0, h = 0, sw = 0, sh = 0;
      if (tag.starts_with(RESTORE_TAG) && std::sscanf(tag.c_str() + RESTORE_TAG.size(), "%dx%d/%dx%d", &w, &h, &sw, &sh) == 4 && w > 0 && h > 0)
        return SRestore{{(double)w, (double)h}, {(double)sw, (double)sh}, tag};
    }
    return std::nullopt;
  }

  // The window gets its old size with the pointer on the same spot: the same
  // fraction across, and the same offset into a titlebar above the client area
  // (or the same fraction down inside it). The motion signal comes after
  // Hyprland moved the window, so it sits where the pointer put it now.
  void restore(const SP<Layout::ITarget>& target, PHLWINDOW window, const Vector2D& at) {
    const auto saved = restoreOf(window);
    if (!saved || !target->floating())
      return;
    const auto& [size, snapped, tag] = *saved;
    const auto box                   = target->position();
    if (std::abs(box.w - snapped.x) > 1 || std::abs(box.h - snapped.y) > 1) {
      window->m_ruleApplicator->m_tagKeeper.applyTag("-" + tag); // resized since it was snapped
      return;
    }
    const auto ax = std::clamp((at.x - box.x) / std::max(1.0, box.w), 0.0, 1.0);
    const auto oy = at.y < box.y ? at.y - box.y : std::clamp((at.y - box.y) / std::max(1.0, box.h), 0.0, 1.0) * size.y;

    window->m_ruleApplicator->m_tagKeeper.applyTag("-" + tag);
    g_layoutManager->setTargetGeom(CBox{at.x - size.x * ax, at.y - oy, size.x, size.y}, target);
    target->warpPositionSize();
    // Re-captures the drag from the new box without ending it; true means
    // Hyprland rejected the drag, which then ends (and reports `ended`).
    g_layoutManager->dragController()->updateDragWindow();
  }

  void finish(const Vector2D& at) {
    const auto window = draggingWindow.lock();
    dragging.reset();
    draggingWindow.reset();
    emit(dragEnd, window, at);
  }

  void onMotion() {
    const auto& controller = g_layoutManager->dragController();
    const auto  target     = controller->target();
    if (!target || controller->mode() != MBIND_MOVE)
      return;
    const auto window = target->window();
    if (!window)
      return;

    const auto at = cursor();
    if (!dragging.expired() && dragging.lock() == target) {
      emit(dragMove, window, at);
      return;
    }
    if (!dragging.expired())
      finish(at); // a new drag without an end for the last: never leave one open
    dragging       = target;
    draggingWindow = window;
    restore(target, window, at);
    emit(dragStart, window, at);
  }

  void onEnded() {
    if (!draggingWindow.expired() || !dragging.expired())
      finish(cursor());
  }

  int luaExtents(lua_State* L) {
    const auto window = Config::Lua::Bindings::Internal::windowFromLuaSelectorOrObject(L, 1, "extents");
    if (!window)
      return 0;
    const auto reserved = window->getFullWindowReservedArea();
    lua_createtable(L, 0, 4);
    lua_pushnumber(L, reserved.topLeft.x), lua_setfield(L, -2, "left");
    lua_pushnumber(L, reserved.topLeft.y), lua_setfield(L, -2, "top");
    lua_pushnumber(L, reserved.bottomRight.x), lua_setfield(L, -2, "right");
    lua_pushnumber(L, reserved.bottomRight.y), lua_setfield(L, -2, "bottom");
    return 1;
  }

  // Hyprland reports the limits in logical pixels, an X11 app's own hints
  // included (its backend scales them), and no maximum as a huge size.
  int luaLimits(lua_State* L) {
    const auto window = Config::Lua::Bindings::Internal::windowFromLuaSelectorOrObject(L, 1, "limits");
    if (!window)
      return 0;
    constexpr double NONE = 1e6;
    const auto       push = [L](const char* name, const Vector2D& v, bool lower) {
      lua_createtable(L, 0, 2);
      if (lower ? v.x > 1 : v.x < NONE)
        lua_pushnumber(L, v.x), lua_setfield(L, -2, "x");
      if (lower ? v.y > 1 : v.y < NONE)
        lua_pushnumber(L, v.y), lua_setfield(L, -2, "y");
      lua_setfield(L, -2, name);
    };
    lua_createtable(L, 0, 2);
    if (const auto min = window->minSize())
      push("min", *min, true);
    if (const auto max = window->maxSize())
      push("max", *max, false);
    return 1;
  }

  // Hyprland gives Lua a new event handler on every config reload, which hooks
  // up only plugin events added after it exists: hand ours over again.
  void announceEvents() {
    for (const auto& event : {dragStart, dragMove, dragEnd})
      if (event)
        Event::bus()->m_events.pluginEventAdded.emit(event);
  }
}

void DragTracker::init(const SConfig& cfg) {
  config = cfg;

  const std::vector<CCustomEvent::eType> ARGS = {CCustomEvent::TYPE_WINDOW, CCustomEvent::TYPE_DOUBLE, CCustomEvent::TYPE_DOUBLE};
  dragStart                                   = makeShared<CCustomEvent>(config.eventPrefix + ".drag_start", ARGS);
  dragMove                                    = makeShared<CCustomEvent>(config.eventPrefix + ".drag_move", ARGS);
  dragEnd                                     = makeShared<CCustomEvent>(config.eventPrefix + ".drag_end", ARGS);
  for (const auto& event : {dragStart, dragMove, dragEnd})
    HyprlandAPI::addEvent(config.handle, event);
  HyprlandAPI::addLuaFunction(config.handle, config.luaNamespace, "extents", luaExtents);
  HyprlandAPI::addLuaFunction(config.handle, config.luaNamespace, "limits", luaLimits);

  const auto& controller = g_layoutManager->dragController();
  listeners.push_back(controller->m_events.motion.listen([] { onMotion(); }));
  listeners.push_back(controller->m_events.ended.listen([] { onEnded(); }));
  listeners.push_back(Event::bus()->m_events.window.close.listen([](PHLWINDOW window) {
    if (draggingWindow.lock() == window)
      finish(cursor());
  }));
  listeners.push_back(Event::bus()->m_events.config.reloaded.listen([] { announceEvents(); }));
}

void DragTracker::exit() {
  listeners.clear();
  dragging.reset();
  draggingWindow.reset();
  for (const auto& event : {dragStart, dragMove, dragEnd})
    if (event)
      HyprlandAPI::removeEvent(config.handle, event->m_name);
  dragStart.reset();
  dragMove.reset();
  dragEnd.reset();
}
