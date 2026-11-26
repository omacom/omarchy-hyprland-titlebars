#include "barDeco.hpp"

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/desktop/state/LayerState.hpp>
#include <hyprland/src/desktop/state/ViewHitTester.hpp>
#include <hyprland/src/desktop/view/window/Window.hpp>
#include <hyprland/src/desktop/view/window/WindowPresentation.hpp>
#include <hyprland/src/desktop/view/LayerSurface.hpp>
#include <hyprland/src/helpers/MiscFunctions.hpp>
#include <hyprland/src/managers/SeatManager.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/config/ConfigManager.hpp>
#include <hyprland/src/config/shared/animation/AnimationTree.hpp>
#include <hyprland/src/config/shared/parserUtils/ParserUtils.hpp>
#include <hyprland/src/config/supplementary/executor/Executor.hpp>
#include <hyprland/src/config/shared/actions/ConfigActions.hpp>
#include <hyprland/src/animation/AnimationManager.hpp>
#include <fstream>
#include <ctime>
#include <hyprland/src/protocols/LayerShell.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/layout/LayoutManager.hpp>
#include <hyprland/src/config/ConfigValue.hpp>
#include <hyprland/src/layout/space/Space.hpp>
#include <hyprland/src/layout/algorithm/Algorithm.hpp>
#include <hyprland/src/layout/supplementary/WorkspaceAlgoMatcher.hpp>
#include <hyprland/src/workspace/HLWorkspace.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/state/MonitorState.hpp>

#include "globals.hpp"
#include "BarPassElement.hpp"
#include <hyprland/src/managers/fullscreen/FullscreenController.hpp>
#include "appName.hpp"

#include <algorithm>
  #include <climits>
  #include <vector>

using namespace Render::GL;

static CHyprColor configColor(Config::INTEGER color) { return CHyprColor{static_cast<uint64_t>(color)}; }

static std::string windowLayoutName(const PHLWINDOW& window) {
  const auto workspace = window->m_workspace;
  if (!workspace)
    return "";
  const auto space = workspace->space();
  if (!space)
    return "";
  const auto algo = space->algorithm();
  if (!algo)
    return "";
  const auto& tiled = algo->tiledAlgo();
  if (!tiled)
    return "";
  return Layout::Supplementary::algoMatcher()->getNameForTiledAlgo(tiled.get());
}
static std::string activeLayoutName(const PHLWINDOW &win) {
	// The workspace API does not expose a reliable "floating layout" name (a
	// floating workspace still carries the default tiled algorithm), so the
	// window's own float state is the only stable signal. A floating window
	// always resolves to the "floating" layout options; anything else resolves
	// to its tiled layout.
	if (win->isFloating())
		return "floating";
	return windowLayoutName(win);
}

// Visibility limiter (plugin:hyprbars:show_mode): empty/unset means always;
// only "active"/"hover" restrict when a bar is drawn.
static eHyprBarMode parseShowMode(const std::string& s) {
  if (s == "active")
    return eHyprBarMode::ACTIVE;
  if (s == "hover")
    return eHyprBarMode::HOVER;
  return eHyprBarMode::ALWAYS;
}

// True when show_mode_cursor_proximity is explicitly configured. While it is
// left unset, "active" mode simply follows focus and needs no pointer
// tracking; once set (including an explicit 0) both modes gate on the pointer
// being over the window.
static bool proximityConfigured() {
    return CConfigValue<Config::INTEGER>("plugin:hyprbars:show_mode_cursor_proximity").setByUser();
}

// The pointer position only needs tracking when a visibility mode can hide a
// bar, or button icons appear on hover; otherwise mouse motion does no scan.
static bool pointerTrackingNeeded() {
    const auto& mode = g_pOmarchyFloatingGlobalState->config.showMode->value();
    if (mode == "hover")
        return true;
    return mode == "active" && proximityConfigured();
}
static std::string windowCommand(std::string command, const PHLWINDOW &window) {
  // Resolve the decoration owner synchronously. An asynchronously spawned
  // command must not choose its target from whatever happens to be focused.
  constexpr std::string_view token = "%WINDOW%";
  const auto address = std::format("0x{:x}", reinterpret_cast<uintptr_t>(window.get()));
  size_t offset = 0;
  while ((offset = command.find(token, offset)) != std::string::npos) {
    command.replace(offset, token.size(), address);
    offset += address.size();
  }
  return command;
}

COmarchyFloatingHyprBar::COmarchyFloatingHyprBar(PHLWINDOW pWindow) : IHyprWindowDecoration(pWindow) {
  m_pWindow = pWindow;

  const auto PMONITOR = pWindow->m_monitor.lock();
  PMONITOR->m_scheduledRecalc = true;

  // button events
  m_pMouseButtonCallback =
      Event::bus()->m_events.input.mouse.button.listen([&](IPointer::SButtonEvent e, Event::SCallbackInfo &info) { onMouseButton(info, e); });
  m_pTouchDownCallback = Event::bus()->m_events.input.touch.down.listen([&](ITouch::SDownEvent e, Event::SCallbackInfo &info) { onTouchDown(info, e); });
  m_pTouchUpCallback = Event::bus()->m_events.input.touch.up.listen([&](ITouch::SUpEvent e, Event::SCallbackInfo &info) { onTouchUp(info, e); });

  // move events
  m_pTouchMoveCallback = Event::bus()->m_events.input.touch.motion.listen([&](ITouch::SMotionEvent e, Event::SCallbackInfo &info) { onTouchMove(info, e); });
  m_pMouseMoveCallback = Event::bus()->m_events.input.mouse.move.listen([&](Vector2D c, Event::SCallbackInfo &info) { onMouseMove(c); });
  m_pFocusCallback = Event::bus()->m_events.window.active.listen([&](PHLWINDOW w, Desktop::eFocusReason) { updateFocus(w); });

  m_focused = Desktop::focusState()->window() == pWindow;

  Animation::mgr()->createAnimation(configColor(g_pOmarchyFloatingGlobalState->config.barColor->value()), m_cRealBarColor,
                                    Config::animationTree()->getAnimationPropertyConfig("border"), pWindow, AVARDAMAGE_NONE);
  m_cRealBarColor->setUpdateCallback([&](auto) { damageEntire(); });

}

void COmarchyFloatingHyprBar::restoreDragSettings() {
  if (m_bDragCenterOverridden) {
    static const auto DCENTER = CConfigValue<Config::BOOL>("binds:drag_center_window");
    if (DCENTER.good())
      *DCENTER.ptr() = m_prevDragCenter;
    m_bDragCenterOverridden = false;
  }
}
COmarchyFloatingHyprBar::~COmarchyFloatingHyprBar() {
  restoreDragSettings();
  std::erase(g_pOmarchyFloatingGlobalState->bars, m_self);
}

eHyprBarMode COmarchyFloatingHyprBar::combinedMode() const {
  const auto PWINDOW = m_pWindow.lock();
  if (!PWINDOW)
    return eHyprBarMode::NEVER;

  const auto layoutName = activeLayoutName(PWINDOW);

  // An explicitly configured per-layout option always overrides the global
  // enabled switch: true shows bars on that layout even when the plugin is
  // disabled globally, false hides them even when it is enabled.
  if (!layoutName.empty()) {
    CConfigValue<Config::BOOL> override("plugin:hyprbars:layout_" + layoutName + "_enabled");
    if (override.good() && override.setByUser()) {
      if (!*override)
        return eHyprBarMode::NEVER;
      return parseShowMode(g_pOmarchyFloatingGlobalState->config.showMode->value());
    }
  }

  // Upstream master switch: off means no bars anywhere (unless a layout
  // override above said otherwise).
  if (!g_pOmarchyFloatingGlobalState->config.enabled->value())
    return eHyprBarMode::NEVER;

  return parseShowMode(g_pOmarchyFloatingGlobalState->config.showMode->value());
}

bool COmarchyFloatingHyprBar::isEffectivelyShown() const {
  if (m_hidden)
    return false;
  switch (combinedMode()) {
    case eHyprBarMode::NEVER: return false;
    case eHyprBarMode::ACTIVE:
      // Without an explicit proximity limit, active shows on the focused
      // window only, with no pointer tracking.
      if (!proximityConfigured())
        return m_focused;
      return m_focused && isPointerOver() && (isPointerOverBar() || withinProximity());
    case eHyprBarMode::ALWAYS: return true;
    case eHyprBarMode::HOVER:
      // Touch has no hover: while a touch gesture was the last input, fall
      // back to active mode.
      if (g_pOmarchyFloatingGlobalState->lastInputTouch)
        return proximityConfigured() ? (m_focused && withinProximity()) : m_focused;
			{
				// Dragging keeps the bar up wherever the window goes; pointer-anchored
				// hiding would fire while the window moves under the cursor.
				if (m_bDraggingThis || m_bDragPending)
					return true;
				const double PROX = g_pOmarchyFloatingGlobalState->config.showModeCursorProximity->value();
				if (PROX <= 0)
					return isPointerOver();
				// Show/hide transition on one quantity: d = distance from the cursor
				// to the window's top. The pointer must be over the window or bar
				// first (hovering in the empty space above must not show).
				if (!isPointerOver())
				{
					return false;
				}
				const bool inZone = isPointerOverBar() || proximityDistance() <= PROX;
				if (inZone)
				{
					return true;
				}
				// Hide immediately outside the zone.
				return false;
			}
		}
	return false;
}

double COmarchyFloatingHyprBar::proximityDistance() const
{
	const auto WIN = m_pWindow.lock();
	if (!validMapped(WIN))
		return std::numeric_limits<double>::max();
	const auto PWORKSPACE = WIN->m_workspace;
	const auto WORKSPACEOFFSET = PWORKSPACE && !(WIN->m_state & Desktop::View::WINDOW_STATE_PINNED) ? PWORKSPACE->m_renderOffset->value() : Vector2D();
	const auto reserved = WIN->getFullWindowReservedArea();
	// Floating windows do not move for their own bar; tiled windows subtract
	// their top reservation so the reference is stable across show/hide.
	const double reservedTop = WIN->isFloating() ? 0.0 : reserved.topLeft.y;
	return std::abs(g_pInputManager->getMouseCoordsInternal().y - (WIN->position(Desktop::View::IGeometric::GEOMETRIC_GOAL).y + WIN->presentation().floatingOffset().y + WORKSPACEOFFSET.y - reservedTop));
}

bool COmarchyFloatingHyprBar::withinProximity() const
{
	const auto PROX = g_pOmarchyFloatingGlobalState->config.showModeCursorProximity->value();
	// Unset (0) keeps the current behavior: no distance restriction.
	if (PROX <= 0)
		return true;
	return proximityDistance() <= static_cast<double>(PROX);
}

void COmarchyFloatingHyprBar::applyShownStateChange() {
  const auto ENABLED = isEffectivelyShown();
  if (ENABLED == m_bLastEnabledState)
    return;
	m_bLastEnabledState = ENABLED;
	g_pDecorationPositioner->repositionDeco(this);
  if (const auto PWINDOW = m_pWindow.lock(); PWINDOW)
    if (const auto monitor = PWINDOW->m_monitor.lock())
      monitor->m_scheduledRecalc = true;
  damageEntire();
}

void COmarchyFloatingHyprBar::refreshShownState() {
  // Input-kind flips (touch -> mouse or back) change what a hover-mode bar
  // draws; re-evaluate without waiting for a motion or focus event.
  applyShownStateChange();
}

void COmarchyFloatingHyprBar::updateFocus(PHLWINDOW activeWindow) {
  const auto focused = m_pWindow.lock() == activeWindow;
  if (focused == m_focused)
    return;
  m_focused = focused;
  applyShownStateChange();
}

void COmarchyFloatingHyprBar::updateHover() {
  // A drag keeps the bar up under the pointer, wherever the window goes.
  if (m_bDraggingThis || m_bDragPending)
    return;
  // Modes that gate on the pointer (active/hover) re-evaluate on motion;
  // always-visible bars never need to.
  if (combinedMode() == eHyprBarMode::ALWAYS)
    return;
  applyShownStateChange();
}

CBox COmarchyFloatingHyprBar::barBand() const {
  const auto WIN = m_pWindow.lock();
  CBox band;
  if (!validMapped(WIN))
    return band;

  const auto PWORKSPACE = WIN->m_workspace;
  const auto WORKSPACEOFFSET = PWORKSPACE && !(WIN->m_state & Desktop::View::WINDOW_STATE_PINNED) ? PWORKSPACE->m_renderOffset->value() : Vector2D();

  const auto HEIGHT = g_pOmarchyFloatingGlobalState->config.barHeight->value();
  const auto BORDER = WIN->presentation().borderSize();

  // The band above the window's content box where the positioner puts the bar.
  band = CBox(WIN->position(Desktop::View::IGeometric::GEOMETRIC_CURRENT).x + WIN->presentation().floatingOffset().x,
              WIN->position(Desktop::View::IGeometric::GEOMETRIC_CURRENT).y + WIN->presentation().floatingOffset().y - HEIGHT - BORDER,
              WIN->size(Desktop::View::IGeometric::GEOMETRIC_CURRENT).x, HEIGHT + BORDER);
  band.translate(WORKSPACEOFFSET);

  // On floating windows the real placement can differ (floating offset, border
  // precedence, rounding); include the live assigned box when there is one.
  const auto BARASSIGNED = assignedBoxGlobal();
  if (BARASSIGNED.w > 0 && BARASSIGNED.h > 0) {
    const double right = std::max(band.x + band.w, BARASSIGNED.x + BARASSIGNED.w);
    const double bottom = std::max(band.y + band.h, BARASSIGNED.y + BARASSIGNED.h);
    band.x = std::min(band.x, BARASSIGNED.x);
    band.y = std::min(band.y, BARASSIGNED.y);
    band.w = right - band.x;
    band.h = bottom - band.y;
  }
  return band;
}

CBox COmarchyFloatingHyprBar::hoverRegion() const {
  const auto WIN = m_pWindow.lock();
  CBox region;
  if (!validMapped(WIN))
    return region;

  const auto PWORKSPACE = WIN->m_workspace;
  const auto WORKSPACEOFFSET = PWORKSPACE && !(WIN->m_state & Desktop::View::WINDOW_STATE_PINNED) ? PWORKSPACE->m_renderOffset->value() : Vector2D();

  region = CBox(WIN->position(Desktop::View::IGeometric::GEOMETRIC_CURRENT).x + WIN->presentation().floatingOffset().x,
                WIN->position(Desktop::View::IGeometric::GEOMETRIC_CURRENT).y + WIN->presentation().floatingOffset().y,
                WIN->size(Desktop::View::IGeometric::GEOMETRIC_CURRENT).x, WIN->size(Desktop::View::IGeometric::GEOMETRIC_CURRENT).y);
  region.translate(WORKSPACEOFFSET);

  const auto BAND = barBand();
  const double right = std::max(region.x + region.w, BAND.x + BAND.w);
  const double bottom = std::max(region.y + region.h, BAND.y + BAND.h);
  region.x = std::min(region.x, BAND.x);
  region.y = std::min(region.y, BAND.y);
  region.w = right - region.x;
  region.h = bottom - region.y;
  return region;
}

bool COmarchyFloatingHyprBar::isPointerOver() const {
  const auto MOUSE = g_pInputManager->getMouseCoordsInternal();
  if (!hoverRegion().containsPoint(MOUSE))
    return false;

	// Only the window's actual rectangle (and its drawn bar) count; the
	// reserved band above the window must not, or the bar pre-shows before the
	// pointer reaches the window (which also feeds flicker).
	const auto WIN = m_pWindow.lock();
	CBox winBox;
	if (WIN)
	{
		winBox = {WIN->position(Desktop::View::IGeometric::GEOMETRIC_CURRENT).x + WIN->presentation().floatingOffset().x,
							WIN->position(Desktop::View::IGeometric::GEOMETRIC_CURRENT).y + WIN->presentation().floatingOffset().y,
							WIN->size(Desktop::View::IGeometric::GEOMETRIC_CURRENT).x, WIN->size(Desktop::View::IGeometric::GEOMETRIC_CURRENT).y};
		// Match hoverRegion(): slide with the workspace during workspace
		// transitions. Pinned windows stay fixed.
		const auto PWORKSPACE = WIN->m_workspace;
		const auto WORKSPACEOFFSET = PWORKSPACE && !(WIN->m_state & Desktop::View::WINDOW_STATE_PINNED) ? PWORKSPACE->m_renderOffset->value() : Vector2D();
		winBox.translate(WORKSPACEOFFSET);
	}
	const bool insideWindow = winBox.containsPoint(MOUSE);
	const bool onDrawnBar = assignedBoxGlobal().containsPoint(MOUSE);

	// Overlapping floating windows: only the topmost window under the pointer
	// counts; a covered window must not hover-show its bar.
	Desktop::CViewHitTester hitTester{*Desktop::viewState()};
  const auto TOP = hitTester.windowAt(MOUSE, Desktop::View::RESERVED_EXTENTS | Desktop::View::INPUT_EXTENTS | Desktop::View::ALLOW_FLOATING);
  if (TOP == m_pWindow)
		return insideWindow || onDrawnBar;
	if (TOP)
		return false;
	return insideWindow || onDrawnBar;
}

bool COmarchyFloatingHyprBar::isPointerOverBar() const
{
	const auto MOUSE = g_pInputManager->getMouseCoordsInternal();
	// Only the live drawn bar counts (the assigned box, which is what is
	// rendered). No geometric overreach: proximity remains the strict gate for
	// approaching from outside, while hovering the bar itself keeps it shown.
	const auto BAR = assignedBoxGlobal();
	return BAR.w > 0 && BAR.h > 0 && BAR.containsPoint(MOUSE);
}

SDecorationPositioningInfo COmarchyFloatingHyprBar::getPositioningInfo() {
  const auto HEIGHT = g_pOmarchyFloatingGlobalState->config.barHeight->value();
  const auto SHOWN = isEffectivelyShown();
  const auto PRECEDENCE = g_pOmarchyFloatingGlobalState->config.barPrecedenceOverBorder->value();

  SDecorationPositioningInfo info;
  info.policy = SHOWN ? DECORATION_POSITION_STICKY : DECORATION_POSITION_ABSOLUTE;
  info.edges = DECORATION_EDGE_TOP;
  info.priority = PRECEDENCE ? 10005 : 5000;
  info.reserved = true;
  info.desiredExtents = {{0, SHOWN ? HEIGHT : 0}, {0, 0}};
  return info;
}

void COmarchyFloatingHyprBar::onPositioningReply(const SDecorationPositioningReply &reply) {
  if (reply.assignedGeometry.size() != m_bAssignedBox.size())
    m_bWindowSizeChanged = true;

  m_bAssignedBox = reply.assignedGeometry;
}

std::string COmarchyFloatingHyprBar::getDisplayName() { return "Hyprbar"; }

bool COmarchyFloatingHyprBar::inputIsValid() {
  if (!isEffectivelyShown())
    return false;

  if (g_pSeatManager->m_seatGrab && !g_pSeatManager->m_seatGrab->accepts(m_pWindow->wlSurface()->resource()))
    return false;

  const auto MOUSE = g_pInputManager->getMouseCoordsInternal();
  auto PMONITOR = Desktop::focusState()->monitor();

  if (!PMONITOR)
    return false;

  Desktop::CViewHitTester hitTester{*Desktop::viewState()};

  const auto WINDOWATCURSOR = hitTester.windowAt(MOUSE, Desktop::View::RESERVED_EXTENTS | Desktop::View::INPUT_EXTENTS | Desktop::View::ALLOW_FLOATING);

  auto focusState = Desktop::focusState();
  auto window = focusState->window();

  // The focused window's bar can sit under another window, and a press there
  // belongs to that window.
  if (WINDOWATCURSOR != m_pWindow && (m_pWindow != window || WINDOWATCURSOR))
    return false;

  PHLLS foundSurface = nullptr;
  Vector2D surfaceCoords;

  // Check Top Layer
  hitTester.layerSurfaceAt(MOUSE, &PMONITOR->m_layerSurfaceLayers[ZWLR_LAYER_SHELL_V1_LAYER_TOP], &surfaceCoords, &foundSurface);
  if (foundSurface)
    return false;

  // Check Overlay Layer
  hitTester.layerSurfaceAt(MOUSE, &PMONITOR->m_layerSurfaceLayers[ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY], &surfaceCoords, &foundSurface);
  if (foundSurface)
    return false;

  return true;
}

void COmarchyFloatingHyprBar::onMouseButton(Event::SCallbackInfo &info, IPointer::SButtonEvent e) {
  if (e.button != BTN_LEFT)
    return;

  if (e.state != WL_POINTER_BUTTON_STATE_PRESSED) {
    // A titlebar drag can finish over a panel or another surface.
    if (m_bDraggingThis || m_bCancelledDown || m_bDragPending)
      handleUpEvent(info);
    return;
  }

  if (!inputIsValid())
    return;

  handleDownEvent(info, std::nullopt);
}

void COmarchyFloatingHyprBar::onTouchDown(Event::SCallbackInfo &info, ITouch::SDownEvent e) {
  // Don't do anything if you're already grabbed a window with another finger
  if (!inputIsValid() || e.touchID != 0)
    return;

  handleDownEvent(info, e);
}

void COmarchyFloatingHyprBar::onTouchUp(Event::SCallbackInfo &info, ITouch::SUpEvent e) {
  // A tap on a button, or a double tap, leaves no drag pending, and its press
  // was taken all the same: left armed, the bar would swallow the release of
  // the next mouse click in the window.
  if (!m_bTouchEv || e.touchID != m_touchId || !(m_bDragPending || m_bCancelledDown || m_bDraggingThis))
    return;

  handleUpEvent(info);
}

void COmarchyFloatingHyprBar::onMouseMove(Vector2D coords) {
  // Button-face highlights always follow the pointer; only the visibility
  // re-evaluation is gated to cursor-dependent modes.
  damageOnButtonHover();
  if (pointerTrackingNeeded())
    updateHover();

  if (!m_bDragPending || m_bTouchEv || !validMapped(m_pWindow) || m_touchId != 0)
    return;
  handleMovement();
}

void COmarchyFloatingHyprBar::onTouchMove(Event::SCallbackInfo &info, ITouch::SMotionEvent e) {
  if (!m_bDragPending || !m_bTouchEv || !validMapped(m_pWindow) || e.touchID != m_touchId)
    return;

  if (!m_bDraggingThis) {
    // Initial setup for dragging a window.
    (void)Config::Actions::floatWindow(Config::Actions::eTogglableAction::TOGGLE_ACTION_ENABLE, m_pWindow.lock());
    // Pin it so you can change workspaces while dragging a window
    (void)Config::Actions::pinWindow(Config::Actions::eTogglableAction::TOGGLE_ACTION_ENABLE, m_pWindow.lock());

    g_layoutManager->beginDragTarget(m_pWindow.lock()->layoutTarget(), MBIND_MOVE);
    m_bDraggingThis = true;
  }
}

void COmarchyFloatingHyprBar::handleDownEvent(Event::SCallbackInfo &info, std::optional<ITouch::SDownEvent> touchEvent) {
  m_bTouchEv = touchEvent.has_value();
  if (m_bTouchEv)
    m_touchId = touchEvent.value().touchID;

  const auto PWINDOW = m_pWindow.lock();

  auto COORDS = cursorRelativeToBar();
  if (m_bTouchEv) {
    ITouch::SDownEvent e = touchEvent.value();
    PHLMONITOR PMONITOR = nullptr;
    for (auto &m : State::monitorState()->monitors()) {
      if (m->m_name == (!e.device->m_boundOutput.empty() ? e.device->m_boundOutput : "")) {
        PMONITOR = m;
        break;
      }
    }
    PMONITOR = PMONITOR ? PMONITOR : Desktop::focusState()->monitor();
    COORDS = Vector2D(PMONITOR->m_position.x + e.pos.x * PMONITOR->m_size.x, PMONITOR->m_position.y + e.pos.y * PMONITOR->m_size.y) - assignedBoxGlobal().pos();
  }

  const auto HEIGHT = g_pOmarchyFloatingGlobalState->config.barHeight->value();
  const auto BARBUTTONPADDING = g_pOmarchyFloatingGlobalState->config.barButtonPadding->value();
  const auto BARPADDING = g_pOmarchyFloatingGlobalState->config.barPadding->value();
  const auto ALIGNBUTTONS = g_pOmarchyFloatingGlobalState->config.barButtonsAlignment->value();
  const auto ON_DOUBLE_CLICK = g_pOmarchyFloatingGlobalState->config.onDoubleClick->value();

  const bool BUTTONSRIGHT = ALIGNBUTTONS != "left";

  // Hyprland measures its border resize band from the window's content, so for
  // a titled window the band sits under the bar and the edge above the bar
  // never resizes. A press in the band above the bar starts that resize here.
  if (!m_bTouchEv && startTopEdgeResize(info, PWINDOW, COORDS))
    return;

  if (!VECINRECT(COORDS, 0, 0, assignedBoxGlobal().w, HEIGHT - 1)) {

    if (m_bDraggingThis) {
      if (m_bTouchEv)
        (void)Config::Actions::floatWindow(Config::Actions::eTogglableAction::TOGGLE_ACTION_DISABLE);
      g_layoutManager->endDragTarget();
      LOG(Log::DEBUG, "[hyprbars] Dragging ended on {:x}", (uintptr_t)PWINDOW.get());
    }

    m_bDraggingThis = false;
    m_bDragPending = false;
    m_bTouchEv = false;
    return;
  }

  if (Desktop::focusState()->window() != PWINDOW)
    Desktop::focusState()->fullWindowFocus(PWINDOW, Desktop::FOCUS_REASON_CLICK);

  if (PWINDOW->isFloating())
    Desktop::windowState()->raise(PWINDOW);

  info.cancelled = true;
  m_bCancelledDown = true;

  if (doButtonPress(BARPADDING, BARBUTTONPADDING, HEIGHT, COORDS, BUTTONSRIGHT))
    return;

  if (!ON_DOUBLE_CLICK.empty() &&
      std::chrono::duration_cast<std::chrono::milliseconds>(Time::steadyNow() - m_lastMouseDown).count() < 400 /* Arbitrary delay I found suitable */) {
    Config::Supplementary::executor()->spawn(windowCommand(ON_DOUBLE_CLICK, PWINDOW));
    m_bDragPending = false;
  } else {
    m_lastMouseDown = Time::steadyNow();
    m_bDragPending = true;
    m_grabOffsetY = COORDS.y;
}
}
bool COmarchyFloatingHyprBar::startTopEdgeResize(Event::SCallbackInfo &info, PHLWINDOW window, const Vector2D &coords) {
  static const auto RESIZEONBORDER = CConfigValue<Config::INTEGER>("general:resize_on_border");
  static const auto EXTENDGRAB = CConfigValue<Config::INTEGER>("general:extend_border_grab_area");

  if (!*RESIZEONBORDER || !window || !window->isFloating() || Fullscreen::controller()->isFullscreen(window))
    return false;

  // The band runs from the top of the bar out over the border and the grab
  // area beyond it, across the bar's width and the grab area at its corners.
  const double GRAB = window->presentation().borderSize() + *EXTENDGRAB;
  const double WIDTH = assignedBoxGlobal().w;
  if (coords.y >= 0 || coords.y < -GRAB || coords.x < -GRAB || coords.x >= WIDTH + GRAB)
    return false;

  if (Desktop::focusState()->window() != window)
    Desktop::focusState()->fullWindowFocus(window, Desktop::FOCUS_REASON_CLICK);
  Desktop::windowState()->raise(window);

  g_layoutManager->beginDragTarget(window->layoutTarget(), MBIND_RESIZE);

  // The press never reaches Hyprland, so neither does the release: the up
  // handler ends the resize the way it ends a titlebar drag.
  info.cancelled = true;
  m_bCancelledDown = true;
  m_bDraggingThis = true;
  m_bDragPending = false;
  return true;
}

void COmarchyFloatingHyprBar::handleUpEvent(Event::SCallbackInfo &info) {
  // Only the bar that took the press gets here, and its window can have lost
  // focus by the release, as one minimized from its own button has. The
  // release is still this bar's: left armed, it would swallow the release of
  // a later click in the window, and the app would see the button held down.
  if (m_bCancelledDown)
    info.cancelled = true;

  m_bCancelledDown = false;
  if (m_bDraggingThis) {
    const bool MOVING = !m_bTouchEv && g_layoutManager->dragController()->mode() == MBIND_MOVE;
    g_layoutManager->endDragTarget();
    m_bDraggingThis = false;
    if (m_bTouchEv)
      (void)Config::Actions::floatWindow(Config::Actions::eTogglableAction::TOGGLE_ACTION_DISABLE);
    else if (MOVING)
      keepTitlebarReachable();

    LOG(Log::DEBUG, "[hyprbars] Dragging ended on {:x}", (uintptr_t)m_pWindow.lock().get());
  }
  restoreDragSettings();
  m_bDragPending = false;
  m_bTouchEv = false;
  m_touchId = 0;
}

void COmarchyFloatingHyprBar::handleMovement() {
  const auto window = m_pWindow.lock();
  if (m_bDraggingThis)
    return;
  if (!m_bTouchEv) {
    // A plain click never picks the window up: a drag only starts once the
    // press has been held long enough (clicks release far sooner).
    const auto held = std::chrono::duration_cast<std::chrono::milliseconds>(Time::steadyNow() - m_lastMouseDown).count();
    if (held < 75)
      return;
  }
  const auto cursor = g_pInputManager->getMouseCoordsInternal();
  const auto bar = assignedBoxGlobal();
  const auto anchor = std::clamp((cursor.x - bar.x) / std::max(1.0, bar.w), 0.0, 1.0);
  const bool maximized = Fullscreen::controller()->isFullscreen(window, Fullscreen::FSMODE_MAXIMIZED);
  // Maximized titlebar drags: the window was fullscreened, so the drag's
  // first job is to restore it while keeping the grab point under the cursor.
  // Two different strategies apply, and they must stay separate:
  //
  // - Floating windows keep their geometry when un-fullscreened, so we
  //   restore and place them ourselves with the grab point preserved.
  //   This is the established, working behavior: do not change it.
  // - Tiled windows are re-placed by their tile layout immediately after an
  //   un-fullscreen, so any manual placement is wiped out before the drag can
  //   use it — and converting the window to floating would permanently break
  //   its native lifecycle (float on drag, re-tile on drop). For tiled
  //   windows we therefore touch nothing but centering, and let Hyprland's
  //   own drag controller perform the un-fullscreen and hotspot remap.
  if (maximized) {
    if (!window->isFloating()) {
      // Tiled windows: NEVER float them here (floatWindow /
      // changeFloatingMode), and NEVER place them manually.
      //
      // - A manual placement cannot survive: the tile layout re-places the
      //   window immediately after the un-fullscreen, before the drag can
      //   use it. Restoring the grab point for tiled windows is left to
      //   Hyprland's own drag controller, which un-fullscreens the window
      //   and remaps the hotspot onto the restored geometry.
      // - Forcing the floating state breaks the native tiled drag lifecycle:
      //   the drag is supposed to float the window transiently and re-tile it
      //   on drop; pre-floating it makes the drop leave it floating for good.
      // All that is needed here is to disable centering for the duration of
      // this tiled drag, so the native remap keeps the grabbed point instead
      // of re-centering the window.
      if (!m_bDragCenterOverridden) {
        static const auto DCENTER = CConfigValue<Config::BOOL>("binds:drag_center_window");
        if (DCENTER.good() && *DCENTER) {
          m_prevDragCenter = true;
          *DCENTER.ptr() = false;
          m_bDragCenterOverridden = true;
        }
      }
    } else {
      // Floating windows keep their own geometry when un-fullscreened, so the
      // placement below sticks and defines the hotspot that beginDragTarget
      // captures. This is the established, verified behavior: do not disable
      // drag_center_window here (floating restores keep Hyprland's native
      // centering/remap), and do not route floating windows through the tiled
      // path above.
      Fullscreen::controller()->setFullscreenMode(window, Fullscreen::FSMODE_NONE);
      // Place the restored window so the grab point stays under the cursor.
      // Native beginDragTarget captures the hotspot from the position at begin
      // time, which can still be the pre-restore one, so place it explicitly.
      auto box = window->layoutTarget()->position();
      const auto reserved = window->getFullWindowReservedArea();
      box.x = cursor.x - box.w * anchor;
      box.y = cursor.y + reserved.topLeft.y - m_grabOffsetY;
      g_layoutManager->setTargetGeom(box, window->layoutTarget());
      window->layoutTarget()->warpPositionSize();
    }
  } else if (!window->isFloating() && !m_bDragCenterOverridden) {
    // Normal (non-maximized) tiled drag: same rule as above — do not float,
    // do not place; just disable centering so the native tiled drag keeps the
    // grabbed point under the cursor and still re-tiles on release.
    static const auto DCENTER = CConfigValue<Config::BOOL>("binds:drag_center_window");
    if (DCENTER.good() && *DCENTER) {
      m_prevDragCenter = true;
      *DCENTER.ptr() = false;
      m_bDragCenterOverridden = true;
    }
  }
  m_bDragPending = false;
  g_layoutManager->beginDragTarget(window->layoutTarget(), MBIND_MOVE);
  m_bDraggingThis = true;
  LOG(Log::DEBUG, "[hyprbars] Dragging initiated on {:x}", (uintptr_t)m_pWindow.lock().get());
  return;
}

void COmarchyFloatingHyprBar::keepTitlebarReachable() {
  // A window dropped with its titlebar under a top bar or off the screen
  // can't be dragged back, so keep the reserved top (border and titlebar)
  // between the top and bottom of the work area.
  const auto window = m_pWindow.lock();
  if (!validMapped(window) || !window->isFloating())
    return;

  auto monitor = State::monitorState()->query().vec(g_pInputManager->getMouseCoordsInternal()).run();
  monitor = monitor ? monitor : window->m_monitor.lock();
  if (!monitor)
    return;

  const auto work = monitor->logicalBoxMinusReserved();
  const auto reserved = window->getFullWindowReservedArea();
  auto box = window->layoutTarget()->position();
  const double top = box.y - reserved.topLeft.y;
  const double reachable = std::clamp(top, work.y, std::max(work.y, work.y + work.h - reserved.topLeft.y));
  if (std::abs(reachable - top) < 0.5)
    return;

  box.y = reachable + reserved.topLeft.y;
  g_layoutManager->setTargetGeom(box, window->layoutTarget());
  LOG(Log::DEBUG, "[hyprbars] Moved {:x} to keep its titlebar reachable", (uintptr_t)window.get());
}

static bool isMinimizeCmd(const std::string &cmd) {
  return cmd.find("window-minimize") != std::string::npos || cmd.find("minimize") != std::string::npos;
}

static bool minimizeButtonVisibleFor(const PHLWINDOW &win) {
  if (!win)
    return true;
	// A "floating workspace" is implemented by workspace-modes.lua as a per-
	// window tag, not as a real Hyprland layout. Use that tag to decide the
	// layout for the minimize button: only windows on a floating workspace use
	// the floating option; all others use their tiled layout.
	static constexpr auto FLOAT_TAG = "omarchy-floating-workspace";
	const auto layoutName = win->m_ruleApplicator->m_tagKeeper.isTagged(FLOAT_TAG) ? std::string("floating") : windowLayoutName(win);
	if (const auto it = g_pOmarchyFloatingGlobalState->config.layoutMinimizeButtonEnabled.find(layoutName);
			it != g_pOmarchyFloatingGlobalState->config.layoutMinimizeButtonEnabled.end())
		return it->second.value->value();
  return false;
}

static std::vector<size_t> visibleButtonIndices(const PHLWINDOW &win) {
  std::vector<size_t> indices;
  const bool showMinimize = minimizeButtonVisibleFor(win);
  const auto &buttons = g_pOmarchyFloatingGlobalState->buttons;
  for (size_t i = 0; i < buttons.size(); ++i)
    if (!(isMinimizeCmd(buttons[i].cmd) && !showMinimize))
      indices.push_back(i);
  return indices;
}

static std::vector<size_t> fitVisibleButtonIndices(const PHLWINDOW &win, float availableWidth) {
  const auto candidates = visibleButtonIndices(win);
  std::vector<size_t> out;
  float remaining = availableWidth;
  for (const size_t i : candidates) {
    const float space = g_pOmarchyFloatingGlobalState->buttons[i].size + g_pOmarchyFloatingGlobalState->config.barButtonPadding->value();
    if (remaining < space)
      break;
    out.push_back(i);
    remaining -= space;
  }
  return out;
}
static float visibleButtonsWidth(const PHLWINDOW &win) {
  float width = g_pOmarchyFloatingGlobalState->config.barButtonPadding->value();
  for (const size_t i : visibleButtonIndices(win))
    width += g_pOmarchyFloatingGlobalState->buttons[i].size + g_pOmarchyFloatingGlobalState->config.barButtonPadding->value();
  return width;
}

bool COmarchyFloatingHyprBar::doButtonPress(Config::INTEGER barPadding, Config::INTEGER barButtonPadding, Config::INTEGER barHeight, Vector2D COORDS,
                                            const bool BUTTONSRIGHT) {
  const auto win = m_pWindow.lock();
  const auto indices = fitVisibleButtonIndices(win, assignedBoxGlobal().w - 2.f * barPadding);
  // check if on a button
  float offset = barPadding;

  for (const size_t i : indices) {
    const auto &b = g_pOmarchyFloatingGlobalState->buttons[i];
    const auto BARBUF = Vector2D{(int)assignedBoxGlobal().w, barHeight};
    Vector2D currentPos = Vector2D{(BUTTONSRIGHT ? BARBUF.x - barButtonPadding - b.size - offset : offset), (BARBUF.y - b.size) / 2.0}.floor();

    if (VECINRECT(COORDS, currentPos.x, currentPos.y, currentPos.x + b.size + barButtonPadding, currentPos.y + b.size)) {
      // hit on close
      Config::Supplementary::executor()->spawn(windowCommand(b.cmd, win));
      return true;
    }

    offset += barButtonPadding + b.size;
  }
  return false;
}

void COmarchyFloatingHyprBar::renderBarTitle(const Vector2D &bufferSize, const float scale) {
  const auto COLORVAL = g_pOmarchyFloatingGlobalState->config.textColor->value();
  const auto SIZE = g_pOmarchyFloatingGlobalState->config.barTextSize->value();
  const auto WEIGHT = g_pOmarchyFloatingGlobalState->config.barTextWeight->value();
  const auto FONT = g_pOmarchyFloatingGlobalState->config.barTextFont->value();
  const auto ALIGN = g_pOmarchyFloatingGlobalState->config.barTextAlign->value();
  const auto BARPADDING = g_pOmarchyFloatingGlobalState->config.barPadding->value();
  const auto BARBUTTONPADDING = g_pOmarchyFloatingGlobalState->config.barButtonPadding->value();

  float buttonSizes = visibleButtonsWidth(m_pWindow.lock());

  const int scaledSize = std::round(SIZE * scale);
  const auto scaledButtonsSize = buttonSizes * scale;
  const auto scaledBarPadding = BARPADDING * scale;
  const int paddingTotal = scaledBarPadding * 2 + scaledButtonsSize + (ALIGN != "left" ? scaledButtonsSize : 0);
  const int maxWidth = std::clamp(static_cast<int>(bufferSize.x - paddingTotal), 0, INT_MAX);

  if (m_szLastTitle.empty() || maxWidth < 1) {
    m_pTextTex = nullptr;
    return;
  }

  const CHyprColor COLOR = m_bForcedTitleColor.value_or(configColor(COLORVAL));
  m_pTextTex = g_pHyprRenderer->renderText(m_szLastTitle, COLOR, scaledSize, false, FONT, maxWidth, WEIGHT.m_value);
}

void COmarchyFloatingHyprBar::renderBarButtons(Render::CRenderContext &ctx, CBox *barBox, const float scale, const float a) {
  const auto BARBUTTONPADDING = g_pOmarchyFloatingGlobalState->config.barButtonPadding->value();
  const auto BARPADDING = g_pOmarchyFloatingGlobalState->config.barPadding->value();
  const auto ALIGNBUTTONS = g_pOmarchyFloatingGlobalState->config.barButtonsAlignment->value();
  const auto INACTIVECOLOR = g_pOmarchyFloatingGlobalState->config.inactiveButtonColor->value();
  const auto BUTTONROUNDING = g_pOmarchyFloatingGlobalState->config.buttonRounding->value();
  const auto BUTTONBORDERSIZE = g_pOmarchyFloatingGlobalState->config.buttonBorderSize->value();
  const auto BUTTONBORDERCOLOR = g_pOmarchyFloatingGlobalState->config.buttonBorderColor->value();

  const bool BUTTONSRIGHT = ALIGNBUTTONS != "left";
  const bool INVALIDATEICONS = m_bButtonsDirty || m_bWindowSizeChanged;

  // Filter hidden buttons before measuring width so drawing and hit-testing
  // agree on which buttons exist.
  const auto win = m_pWindow.lock();
  const auto visibleIndices = fitVisibleButtonIndices(win, barBox->w / scale - 2.f * BARPADDING);

  int offset = BARPADDING * scale;
  for (const size_t i : visibleIndices) {
    auto &button = g_pOmarchyFloatingGlobalState->buttons[i];
    const auto scaledButtonSize = button.size * scale;
    const auto scaledButtonsPad = BARBUTTONPADDING * scale;

    auto color = button.bgcol;

    if (INACTIVECOLOR > 0) {
      color = m_bWindowHasFocus ? color : configColor(INACTIVECOLOR);
      if (INVALIDATEICONS && button.userfg)
        button.iconTex.clear();
    }

    color.a *= a;

    CBox buttonBox = {barBox->x + (BUTTONSRIGHT ? barBox->w - offset - scaledButtonSize : offset), barBox->y + (barBox->h - scaledButtonSize) / 2.0,
                      scaledButtonSize, scaledButtonSize};
    buttonBox.round();

    const int maxRounding = static_cast<int>(std::round(scaledButtonSize / 2.0));
    const int rounding = BUTTONROUNDING < 0 ? maxRounding : std::clamp(static_cast<int>(std::round(BUTTONROUNDING * scale)), 0, maxRounding);
    const int borderSize = std::clamp(static_cast<int>(std::round(BUTTONBORDERSIZE * scale)), 0, maxRounding);

    if (borderSize > 0) {
      auto borderColor = configColor(BUTTONBORDERCOLOR);
      borderColor.a *= a;
      g_pHyprOpenGL->renderRect(ctx, buttonBox, borderColor, {.round = rounding});
      auto face = buttonBox.copy().expand(-borderSize);
      g_pHyprOpenGL->renderRect(ctx, face, color, {.round = std::max(0, rounding - borderSize)});
    } else
      g_pHyprOpenGL->renderRect(ctx, buttonBox, color, {.round = rounding});

    // A hovered button lifts its face in its own icon color, which follows any theme.
    if (m_iButtonHoverState & (1u << i)) {
      auto hover = button.userfg ? button.fgcol : (button.bgcol.r + button.bgcol.g + button.bgcol.b < 1) ? CHyprColor(0xFFFFFFFF) : CHyprColor(0xFF000000);
      hover.a = 0.18 * a;
      g_pHyprOpenGL->renderRect(ctx, buttonBox, hover, {.round = rounding});
    }

    offset += scaledButtonsPad + scaledButtonSize;
  }
}

void COmarchyFloatingHyprBar::renderBarButtonsText(Render::CRenderContext &ctx, CBox *barBox, const float scale, const float a) {
  const auto HEIGHT = g_pOmarchyFloatingGlobalState->config.barHeight->value();
  const auto BARBUTTONPADDING = g_pOmarchyFloatingGlobalState->config.barButtonPadding->value();
  const auto BARPADDING = g_pOmarchyFloatingGlobalState->config.barPadding->value();
  const auto ALIGNBUTTONS = g_pOmarchyFloatingGlobalState->config.barButtonsAlignment->value();
  const auto ICONONHOVER = g_pOmarchyFloatingGlobalState->config.iconOnHover->value();

  const bool BUTTONSRIGHT = ALIGNBUTTONS != "left";
  const auto COORDS = cursorRelativeToBar();

  // Same filtered list as renderBarButtons and the press handler: filter
  // hidden buttons first, then fit by width.
  const auto win = m_pWindow.lock();
  const auto visibleIndices = fitVisibleButtonIndices(win, barBox->w / scale - 2.f * BARPADDING);

  int offset = BARPADDING * scale;
  float noScaleOffset = BARPADDING;

  for (const size_t i : visibleIndices) {
    auto &button = g_pOmarchyFloatingGlobalState->buttons[i];
    const auto scaledButtonSize = button.size * scale;
    const auto scaledButtonsPad = BARBUTTONPADDING * scale;

    // check if hovering here
    const auto BARBUF = Vector2D{(int)assignedBoxGlobal().w, HEIGHT};
    Vector2D currentPos =
        Vector2D{(BUTTONSRIGHT ? BARBUF.x - BARBUTTONPADDING - button.size - noScaleOffset : noScaleOffset), (BARBUF.y - button.size) / 2.0}.floor();
    bool hovering = VECINRECT(COORDS, currentPos.x, currentPos.y, currentPos.x + button.size + BARBUTTONPADDING, currentPos.y + button.size);
    noScaleOffset += BARBUTTONPADDING + button.size;

    auto &iconTex = button.iconTex[static_cast<int>(std::round(scale * 100))];
    if ((!iconTex || iconTex->m_texID == 0) && !button.icon.empty()) {
      // render icon
      auto fgcol = button.userfg ? button.fgcol : (button.bgcol.r + button.bgcol.g + button.bgcol.b < 1) ? CHyprColor(0xFFFFFFFF) : CHyprColor(0xFF000000);

      iconTex = g_pHyprRenderer->renderText(button.icon, fgcol, std::round(button.size * 0.62 * scale), false, "sans", scaledButtonSize);
    }

    if (!iconTex || iconTex->m_texID == 0)
      continue;

    const auto iconX =
        barBox->x + (BUTTONSRIGHT ? barBox->width - offset - scaledButtonSize / 2.0 : offset + scaledButtonSize / 2.0) - iconTex->m_size.x / 2.0;
    const auto iconY = barBox->y + barBox->height / 2.0 - iconTex->m_size.y / 2.0;
    CBox pos = {iconX, iconY, iconTex->m_size.x, iconTex->m_size.y};
    pos.round(); // snap to whole pixels, otherwise the icon is sampled at a half-pixel offset and blurs (hyprwm/hyprland-plugins#711)

    if (!ICONONHOVER || (ICONONHOVER && m_iButtonHoverState > 0))
      g_pHyprOpenGL->renderTexture(ctx, iconTex, pos, {.a = a});
    offset += scaledButtonsPad + scaledButtonSize;

    bool currentBit = (m_iButtonHoverState & (1 << i)) != 0;
    if (hovering != currentBit) {
      m_iButtonHoverState ^= (1 << i);
      // damage to get rid of some artifacts when icons are "hidden"
      damageEntire();
    }
  }
}

void COmarchyFloatingHyprBar::draw(Render::CRenderContext &ctx, PHLMONITOR pMonitor, const float &a, const Render::SWindowRenderPresentation &presentation) {
  const auto ENABLED = isEffectivelyShown();

  if (m_bLastEnabledState != ENABLED) {
    m_bLastEnabledState = ENABLED;
    g_pDecorationPositioner->repositionDeco(this);
  }

  if (!validMapped(m_pWindow) || !ENABLED)
    return;

  const auto PWINDOW = m_pWindow.lock();

  if (!PWINDOW->m_ruleApplicator->decorate().valueOrDefault())
    return;

  auto data = COmarchyFloatingBarPassElement::SBarData{this, a};
  Render::IHyprRenderer::addPassElement(ctx, makeUnique<COmarchyFloatingBarPassElement>(data));
}

void COmarchyFloatingHyprBar::renderPass(Render::CRenderContext &ctx, PHLMONITOR pMonitor, const float &a) {
  const auto PWINDOW = m_pWindow.lock();

  static auto PENABLEBLURGLOBAL = CConfigValue<Config::BOOL>("decoration:blur:enabled");
  const auto BARCOLOR = g_pOmarchyFloatingGlobalState->config.barColor->value();
  const auto HEIGHT = g_pOmarchyFloatingGlobalState->config.barHeight->value();
  const auto PRECEDENCE = g_pOmarchyFloatingGlobalState->config.barPrecedenceOverBorder->value();
  const auto ALIGNBUTTONS = g_pOmarchyFloatingGlobalState->config.barButtonsAlignment->value();
  const auto ENABLETITLE = g_pOmarchyFloatingGlobalState->config.barTitleEnabled->value();
  const auto ENABLEBLUR = g_pOmarchyFloatingGlobalState->config.barBlur->value();
  const auto INACTIVECOLOR = g_pOmarchyFloatingGlobalState->config.inactiveButtonColor->value();

  if (INACTIVECOLOR > 0) {
    bool currentWindowFocus = PWINDOW == Desktop::focusState()->window();
    if (currentWindowFocus != m_bWindowHasFocus) {
      m_bWindowHasFocus = currentWindowFocus;
      m_bButtonsDirty = true;
    }
  }

  const auto INACTIVEBAR = g_pOmarchyFloatingGlobalState->config.inactiveBarColor->value();
  const auto baseColor = PWINDOW != Desktop::focusState()->window() && INACTIVEBAR != 0 ? INACTIVEBAR : BARCOLOR;
  const CHyprColor DEST_COLOR = m_bForcedBarColor.value_or(configColor(baseColor));
  if (DEST_COLOR != m_cRealBarColor->goal())
    *m_cRealBarColor = DEST_COLOR;

  CHyprColor color = m_cRealBarColor->value();

  color.a *= a;
  const bool BUTTONSRIGHT = ALIGNBUTTONS != "left";
  const bool SHOULDBLUR = ENABLEBLUR && *PENABLEBLURGLOBAL && color.a < 1.F;

  if (HEIGHT < 1) {
    m_iLastHeight = HEIGHT;
    return;
  }

  const auto PWORKSPACE = PWINDOW->m_workspace;
  const auto WORKSPACEOFFSET = PWORKSPACE && !(PWINDOW->m_state & Desktop::View::WINDOW_STATE_PINNED) ? PWORKSPACE->m_renderOffset->value() : Vector2D();

  const auto ROUNDING = PWINDOW->presentation().rounding() + (PRECEDENCE ? 0 : PWINDOW->presentation().borderSize());

  const auto scaledRounding = ROUNDING > 0 ? ROUNDING * pMonitor->m_scale - 2 /* idk why but otherwise it looks bad due to the gaps */ : 0;

  m_seExtents = {{0, HEIGHT}, {}};

  const auto DECOBOX = assignedBoxGlobal();

  const auto BARBUF = DECOBOX.size() * pMonitor->m_scale;

  CBox titleBarBox = {DECOBOX.x - pMonitor->m_position.x, DECOBOX.y - pMonitor->m_position.y, DECOBOX.w,
                      DECOBOX.h + ROUNDING * 3 /* to fill the bottom cuz we can't disable rounding there */};

  titleBarBox.translate(PWINDOW->presentation().floatingOffset()).scale(pMonitor->m_scale).round();

  if (titleBarBox.w < 1 || titleBarBox.h < 1)
    return;

  g_pHyprOpenGL->scissor(ctx, titleBarBox);

  if (ROUNDING) {
    // the +1 is a shit garbage temp fix until renderRect supports an alpha matte
    CBox windowBox = {PWINDOW->position(Desktop::View::IGeometric::GEOMETRIC_CURRENT).x + PWINDOW->presentation().floatingOffset().x - pMonitor->m_position.x + 1,
                      PWINDOW->position(Desktop::View::IGeometric::GEOMETRIC_CURRENT).y + PWINDOW->presentation().floatingOffset().y - pMonitor->m_position.y + 1,
                      PWINDOW->size(Desktop::View::IGeometric::GEOMETRIC_CURRENT).x - 2, PWINDOW->size(Desktop::View::IGeometric::GEOMETRIC_CURRENT).y - 2};

    if (windowBox.w < 1 || windowBox.h < 1)
      return;

    // Hyprland's stencil pass for blurred surfaces that discard by alpha leaves
    // the write mask at 0, and a masked clear clears nothing: the bar would be
    // cut by whatever stencil that pass left behind.
    glStencilMask(0xFF);
    glClearStencil(0);
    glClear(GL_STENCIL_BUFFER_BIT);

    g_pHyprOpenGL->setCapStatus(GL_STENCIL_TEST, true);

    glStencilFunc(GL_ALWAYS, 1, -1);
    glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);

    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);

    windowBox.translate(WORKSPACEOFFSET).scale(pMonitor->m_scale).round();
    g_pHyprOpenGL->renderRect(ctx, windowBox, CHyprColor(0, 0, 0, 0), {.round = scaledRounding, .roundingPower = m_pWindow->presentation().roundingPower()});
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    glStencilFunc(GL_NOTEQUAL, 1, -1);
    glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
  }

  g_pHyprOpenGL->renderRect(ctx, titleBarBox, color, {.round = static_cast<int>(scaledRounding), .roundingPower = PWINDOW->presentation().roundingPower(), .blur = SHOULDBLUR, .blurA = a});

  // render title
  // The app's name is steadier than a window title, which for a web app is often
  // just its domain or the page it is on. Windows no launcher claims keep theirs.
  std::string title = omarchyAppName(PWINDOW->metadata().appID());
  if (title.empty())
    title = PWINDOW->metadata().title();
  // The title is rendered at the monitor's scale. A window dragged between
  // monitors of different scales is drawn on each, so one texture is kept per
  // scale; otherwise the title shows at the other monitor's size.
  const float visibleButtonsW = visibleButtonsWidth(PWINDOW);
  if (m_szLastTitle != title || m_bWindowSizeChanged || m_bTitleColorChanged || m_fCachedTitleButtonsWidth != visibleButtonsW) {
    m_titleTexByScale.clear();
    m_fCachedTitleButtonsWidth = visibleButtonsW;
  }
  if (ENABLETITLE) {
    m_szLastTitle = title;
    auto &tex = m_titleTexByScale[static_cast<int>(std::round(pMonitor->m_scale * 100))];
    if (!tex || tex->m_texID == 0) {
      renderBarTitle(BARBUF, pMonitor->m_scale);
      tex = m_pTextTex;
    }
    m_pTextTex = tex;
  }

  if (ROUNDING) {
    // cleanup stencil
    glClearStencil(0);
    glClear(GL_STENCIL_BUFFER_BIT);
    g_pHyprOpenGL->setCapStatus(GL_STENCIL_TEST, false);
    glStencilMask(-1);
    glStencilFunc(GL_ALWAYS, 1, 0xFF);
  }

  CBox textBox = {titleBarBox.x, titleBarBox.y, (int)BARBUF.x, (int)BARBUF.y};
  if (ENABLETITLE && m_pTextTex) {
    const auto BARPADDING = g_pOmarchyFloatingGlobalState->config.barPadding->value();
    const auto BARBUTTONPADDING = g_pOmarchyFloatingGlobalState->config.barButtonPadding->value();
    const auto ALIGN = g_pOmarchyFloatingGlobalState->config.barTextAlign->value();

    const float buttonSizes = visibleButtonsWidth(PWINDOW);

    const auto scaledBorderSize = PWINDOW->presentation().borderSize() * pMonitor->m_scale;
    const auto scaledButtonsSize = buttonSizes * pMonitor->m_scale;
    const auto scaledBarPadding = BARPADDING * pMonitor->m_scale;
    const auto xOffset = ALIGN == "left" ? std::round(scaledBarPadding + (BUTTONSRIGHT ? 0 : scaledButtonsSize))
                                         : std::round(((BARBUF.x - scaledBorderSize) / 2.0 - m_pTextTex->m_size.x / 2.0));
    const auto yOffset = std::round((BARBUF.y - m_pTextTex->m_size.y) / 2.0);
    CBox titleBox = {textBox.x + xOffset, textBox.y + yOffset, m_pTextTex->m_size.x, m_pTextTex->m_size.y};

    g_pHyprOpenGL->renderTexture(ctx, m_pTextTex, titleBox, {.a = a});
  }

  renderBarButtons(ctx, &textBox, pMonitor->m_scale, a);
  m_bButtonsDirty = false;

  g_pHyprOpenGL->scissor(ctx, nullptr);

  renderBarButtonsText(ctx, &textBox, pMonitor->m_scale, a);

  m_bWindowSizeChanged = false;
  m_bTitleColorChanged = false;

  // dynamic updates change the extents
  if (m_iLastHeight != HEIGHT) {
    PWINDOW->layoutTarget()->recalc();
    m_iLastHeight = HEIGHT;
  }
}

eDecorationType COmarchyFloatingHyprBar::getDecorationType() { return DECORATION_CUSTOM; }

void COmarchyFloatingHyprBar::updateWindow(PHLWINDOW pWindow) {
  updateRules();
  damageEntire();
}

void COmarchyFloatingHyprBar::onConfigReloaded() {
  m_bButtonsDirty = true;
  m_bTitleColorChanged = true;
  m_pTextTex = nullptr;
  m_titleTexByScale.clear();

  g_pDecorationPositioner->repositionDeco(this);

  // Bars start disabled, so the reload that enables them has to make room for
  // them, as a change in m_hidden does.
  const auto ENABLED = isEffectivelyShown();
  if (m_bLastEnabledState != ENABLED) {
    m_bLastEnabledState = ENABLED;
    if (const auto PWINDOW = m_pWindow.lock(); PWINDOW)
      if (const auto monitor = PWINDOW->m_monitor.lock())
        monitor->m_scheduledRecalc = true;
  }

  damageEntire();
}

void COmarchyFloatingHyprBar::damageEntire() { g_pHyprRenderer->damageBox(assignedBoxGlobal()); }

Vector2D COmarchyFloatingHyprBar::cursorRelativeToBar() { return g_pInputManager->getMouseCoordsInternal() - assignedBoxGlobal().pos(); }

eDecorationLayer COmarchyFloatingHyprBar::getDecorationLayer() { return DECORATION_LAYER_UNDER; }

uint64_t COmarchyFloatingHyprBar::getDecorationFlags() {
  return DECORATION_ALLOWS_MOUSE_INPUT | (g_pOmarchyFloatingGlobalState->config.barPartOfWindow->value() ? DECORATION_PART_OF_MAIN_WINDOW : 0);
}

CBox COmarchyFloatingHyprBar::assignedBoxGlobal() const {
  if (!validMapped(m_pWindow))
    return {};

  CBox box = m_bAssignedBox;
  box.translate(g_pDecorationPositioner->getEdgeDefinedPoint(DECORATION_EDGE_TOP, m_pWindow.lock()));

  const auto PWORKSPACE = m_pWindow->m_workspace;
  const auto WORKSPACEOFFSET = PWORKSPACE && !(m_pWindow->m_state & Desktop::View::WINDOW_STATE_PINNED) ? PWORKSPACE->m_renderOffset->value() : Vector2D();

  return box.translate(WORKSPACEOFFSET);
}

PHLWINDOW COmarchyFloatingHyprBar::getOwner() { return m_pWindow.lock(); }

void COmarchyFloatingHyprBar::updateRules() {
  const auto PWINDOW = m_pWindow.lock();
  if (!PWINDOW)
    return;
  auto prevHidden = m_hidden;
  auto prevForcedTitleColor = m_bForcedTitleColor;

  m_bForcedBarColor = std::nullopt;
  m_bForcedTitleColor = std::nullopt;
  m_hidden = false;

  if (PWINDOW->m_ruleApplicator->m_otherProps.props.contains(g_pOmarchyFloatingGlobalState->nobarRuleIdx))
    m_hidden = truthy(PWINDOW->m_ruleApplicator->m_otherProps.props.at(g_pOmarchyFloatingGlobalState->nobarRuleIdx)->effect);
  // workspace_tag is an optional restriction across every layout; empty means
  // every window in an enabled layout gets a bar.
  const auto workspaceTag = g_pOmarchyFloatingGlobalState->config.workspaceTag->value();
  if (!workspaceTag.empty())
    m_hidden = m_hidden || !PWINDOW->m_ruleApplicator->m_tagKeeper.isTagged(workspaceTag);
  if (PWINDOW->m_ruleApplicator->m_otherProps.props.contains(g_pOmarchyFloatingGlobalState->barColorRuleIdx))
    m_bForcedBarColor = CHyprColor(
        Config::ParserUtils::parseColor(PWINDOW->m_ruleApplicator->m_otherProps.props.at(g_pOmarchyFloatingGlobalState->barColorRuleIdx)->effect).value_or(0));
  if (PWINDOW->m_ruleApplicator->m_otherProps.props.contains(g_pOmarchyFloatingGlobalState->titleColorRuleIdx))
    m_bForcedTitleColor =
        CHyprColor(Config::ParserUtils::parseColor(PWINDOW->m_ruleApplicator->m_otherProps.props.at(g_pOmarchyFloatingGlobalState->titleColorRuleIdx)->effect)
                       .value_or(0));

  if (prevHidden != m_hidden) {
    g_pDecorationPositioner->repositionDeco(this);
    if (const auto monitor = PWINDOW->m_monitor.lock())
      monitor->m_scheduledRecalc = true;
  }
  if (prevForcedTitleColor != m_bForcedTitleColor)
    m_bTitleColorChanged = true;
}

void COmarchyFloatingHyprBar::damageOnButtonHover() {
  const auto BARPADDING = g_pOmarchyFloatingGlobalState->config.barPadding->value();
  const auto BARBUTTONPADDING = g_pOmarchyFloatingGlobalState->config.barButtonPadding->value();
  const auto HEIGHT = g_pOmarchyFloatingGlobalState->config.barHeight->value();
  const auto ALIGNBUTTONS = g_pOmarchyFloatingGlobalState->config.barButtonsAlignment->value();
  const bool BUTTONSRIGHT = ALIGNBUTTONS != "left";

  float offset = BARPADDING;

  const auto COORDS = cursorRelativeToBar();

  unsigned int hoverState = 0;
  for (const size_t i : fitVisibleButtonIndices(m_pWindow.lock(), assignedBoxGlobal().w - 2.f * BARPADDING)) {
    const auto &b = g_pOmarchyFloatingGlobalState->buttons[i];
    const auto BARBUF = Vector2D{(int)assignedBoxGlobal().w, HEIGHT};
    Vector2D currentPos = Vector2D{(BUTTONSRIGHT ? BARBUF.x - BARBUTTONPADDING - b.size - offset : offset), (BARBUF.y - b.size) / 2.0}.floor();

    if (VECINRECT(COORDS, currentPos.x, currentPos.y, currentPos.x + b.size + BARBUTTONPADDING, currentPos.y + b.size))
      hoverState |= 1u << i;

    offset += BARBUTTONPADDING + b.size;
  }

  if (hoverState != m_iButtonHoverState) {
    m_iButtonHoverState = hoverState;
    damageEntire();
  }
}
