#pragma once

#define WLR_USE_UNSTABLE

#include <hyprland/src/render/decorations/IHyprWindowDecoration.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/gl/GLTexture.hpp>
#include <hyprland/src/devices/IPointer.hpp>
#include <hyprland/src/devices/ITouch.hpp>
#include <hyprland/src/desktop/rule/windowRule/WindowRule.hpp>
#include <hyprland/src/helpers/AnimatedVariable.hpp>
#include <hyprland/src/helpers/time/Time.hpp>
#include <hyprland/src/helpers/signal/Signal.hpp>
#include "globals.hpp"

#define private public
#include <hyprland/src/managers/input/InputManager.hpp>
#undef private

namespace Event {
struct SCallbackInfo;
}

class COmarchyFloatingHyprBar : public IHyprWindowDecoration {
public:
  COmarchyFloatingHyprBar(PHLWINDOW);
  virtual ~COmarchyFloatingHyprBar();

  virtual SDecorationPositioningInfo getPositioningInfo();

  virtual void onPositioningReply(const SDecorationPositioningReply &reply);

  virtual void draw(Render::CRenderContext &ctx, PHLMONITOR monitor, const float &a, const Render::SWindowRenderPresentation &presentation);

  virtual eDecorationType getDecorationType();

  virtual void updateWindow(PHLWINDOW);

  virtual void damageEntire();

  virtual eDecorationLayer getDecorationLayer();

  virtual uint64_t getDecorationFlags();

  bool m_bButtonsDirty = true;

  virtual std::string getDisplayName();

  PHLWINDOW getOwner();

  void updateRules();
  void onConfigReloaded();

  WP<COmarchyFloatingHyprBar> m_self;

private:
  SBoxExtents m_seExtents;

  PHLWINDOWREF m_pWindow;

  CBox m_bAssignedBox;

  SP<Render::ITexture> m_pTextTex;
  // The dimmed "App ›" drawn before the title.
  SP<Render::ITexture> m_pPrefixTex;

  // The app icon drawn before the title, reloaded when its file or size changes.
  SP<Render::ITexture> m_pIconTex;
  std::string m_szIconPath;
  int m_iIconPx = 0;
  int m_iLastIconWidth = 0;

  bool m_bWindowSizeChanged = false;
  bool m_hidden = false;
  bool m_bTitleColorChanged = false;
  bool m_bLastEnabledState = false;
  bool m_bWindowHasFocus = false;
  // The client draws its own titlebar (client-side decorations), so this one stays out of the way.
  bool m_bClientDecorated = false;
  std::optional<CHyprColor> m_bForcedBarColor;
  std::optional<CHyprColor> m_bForcedTitleColor;

  Time::steady_tp m_lastMouseDown{};

  PHLANIMVAR<CHyprColor> m_cRealBarColor;

  Vector2D cursorRelativeToBar();

  void renderPass(Render::CRenderContext &ctx, PHLMONITOR, float const &a);
  void renderBarTitle(const Vector2D &bufferSize, const float scale, const int reservedWidth);
  void updateIcon(PHLWINDOW window, int pixels);
  std::string currentTitle(PHLWINDOW window);
  std::string currentPrefix(PHLWINDOW window, const std::string &title);
  void renderBarButtons(Render::CRenderContext &ctx, CBox *barBox, const float scale, const float a);
  void renderBarButtonsText(Render::CRenderContext &ctx, CBox *barBox, const float scale, const float a);
  void damageOnButtonHover();

  bool inputIsValid();
  void onMouseButton(Event::SCallbackInfo &info, IPointer::SButtonEvent e);
  void onTouchDown(Event::SCallbackInfo &info, ITouch::SDownEvent e);
  void onTouchUp(Event::SCallbackInfo &info, ITouch::SUpEvent e);
  void onMouseMove(Vector2D coords);
  void onTouchMove(Event::SCallbackInfo &info, ITouch::SMotionEvent e);

  void handleDownEvent(Event::SCallbackInfo &info, std::optional<ITouch::SDownEvent> touchEvent);
  void handleUpEvent(Event::SCallbackInfo &info);
  bool startTopEdgeResize(Event::SCallbackInfo &info, PHLWINDOW window, const Vector2D &coords);
  void handleMovement();
  bool doButtonPress(Config::INTEGER barPadding, Config::INTEGER barButtonPadding, Config::INTEGER barHeight, Vector2D COORDS, bool BUTTONSRIGHT);

  CBox assignedBoxGlobal();

  CHyprSignalListener m_pMouseButtonCallback;
  CHyprSignalListener m_pTouchDownCallback;
  CHyprSignalListener m_pTouchUpCallback;

  CHyprSignalListener m_pTouchMoveCallback;
  CHyprSignalListener m_pMouseMoveCallback;

  std::string m_szLastTitle;
  std::string m_szLastPrefix;

  bool m_bDraggingThis = false;
  bool m_bTouchEv = false;
  bool m_bDragPending = false;
  bool m_bCancelledDown = false;
  int m_touchId = 0;

  // store hover state for buttons as a bitfield
  unsigned int m_iButtonHoverState = 0;

  // for dynamic updates
  int m_iLastHeight = 0;

  size_t getVisibleButtonCount(Config::INTEGER barButtonPadding, Config::INTEGER barPadding, const Vector2D &bufferSize, const float scale);

  friend class COmarchyFloatingBarPassElement;
};
