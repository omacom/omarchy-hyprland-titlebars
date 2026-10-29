// Synthetic Aquamarine device fixture. No uinput device or parent-seat input.
// Loads only in the explicitly marked disposable verifier compositor.
#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/devices/Tablet.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <aquamarine/input/Input.hpp>
#include <array>
#include <cstdlib>
#include <sstream>
#include <lua.hpp>

class COhtFixtureTablet : public Aquamarine::ITablet {
public:
  const std::string &getName() override {
    static const std::string name = "oht-fixture-tablet";
    return name;
  }
};
class COhtFixtureTool : public Aquamarine::ITabletTool {
public:
  COhtFixtureTool() { type = AQ_TABLET_TOOL_TYPE_PEN; }
  const std::string &getName() override {
    static const std::string name = "oht-fixture-pen";
    return name;
  }
};
class COhtFixtureTouch : public Aquamarine::ITouch {
public:
  const std::string &getName() override {
    static const std::string name = "oht-fixture-touch";
    return name;
  }
};
class COhtFixturePointer : public Aquamarine::IPointer {
public:
  const std::string &getName() override {
    static const std::string name = "oht-fixture-pointer";
    return name;
  }
};
static SP<COhtFixturePointer> mouse;
static SP<COhtFixtureTablet> tablet;
static SP<COhtFixtureTouch> touch;
static std::array<SP<COhtFixtureTool>, 2> tools;
static bool consumed = false;
static CHyprSignalListener tipListener, axisListener;

static SDispatchResult event(std::string args) {
  std::istringstream stream(args);
  std::string op;
  int id = 0, value = 0, expected = -1;
  double x = 0, y = 0;
  stream >> op >> id >> x >> y >> value >> expected;
  if (id < 0 || id > 1)
    return {.success = false, .error = "Invalid tool"};
  if (op == "destroy-tablet") {
    tablet->events.destroy.emit();
    tablet = makeShared<COhtFixtureTablet>();
    g_pInputManager->newTablet(tablet);
    return {};
  }
  if (op == "destroy") {
    tools[id]->events.destroy.emit();
    tools[id] = makeShared<COhtFixtureTool>();
    return {};
  }
  consumed = false;
  auto tipObserver = Event::bus()->m_events.input.tablet.tip.listen([](auto, auto &info) { consumed = info.cancelled; });
  auto axisObserver = Event::bus()->m_events.input.tablet.axis.listen([](auto, auto &info) { consumed = info.cancelled; });
  auto touchDownObserver = Event::bus()->m_events.input.touch.down.listen([](auto, auto &info) { consumed = info.cancelled; });
  auto touchMoveObserver = Event::bus()->m_events.input.touch.motion.listen([](auto, auto &info) { consumed = info.cancelled; });
  auto touchUpObserver = Event::bus()->m_events.input.touch.up.listen([](auto, auto &info) { consumed = info.cancelled; });
  auto mouseObserver = Event::bus()->m_events.input.mouse.button.listen([](auto, auto &info) { consumed = info.cancelled; });
  if (op == "mouse-down" || op == "mouse-up") {
    mouse->events.warp.emit({.absolute = {x, y}});
    mouse->events.button.emit({.button = BTN_LEFT, .pressed = op == "mouse-down"});
  } else if (op == "touch-down")
    touch->events.down.emit({.touchID = id, .pos = {x, y}});
  else if (op == "touch-motion")
    touch->events.move.emit({.touchID = id, .pos = {x, y}});
  else if (op == "touch-up")
    touch->events.up.emit({.touchID = id});
  else if (op == "tip")
    tablet->events.tip.emit({.tool = tools[id], .absolute = {x, y}, .down = value != 0});
  else if (op == "axis" || op == "axis-x" || op == "axis-y")
    tablet->events.axis.emit(
        {.tool = tools[id],
         .updatedAxes = (op != "axis-y" ? Aquamarine::AQ_TABLET_TOOL_AXIS_X : 0u) | (op != "axis-x" ? Aquamarine::AQ_TABLET_TOOL_AXIS_Y : 0u),
         .absolute = {x, y}});
  else if (op == "proximity")
    tablet->events.proximity.emit({.tool = tools[id], .absolute = {x, y}, .in = value != 0});
  else
    return {.success = false, .error = "Unknown tablet fixture operation"};
  if (expected >= 0 && consumed != (expected != 0))
    return {.success = false, .error = "Tablet event consumption mismatch: " + args};
  return {};
}

static int fixtureEvent(lua_State *L) {
  const auto result = event(luaL_checkstring(L, 1));
  if (!result.success)
    return luaL_error(L, "%s", result.error.c_str());
  return 0;
}

APICALL EXPORT std::string PLUGIN_API_VERSION() { return HYPRLAND_API_VERSION; }
APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
  if (!std::getenv("OHT_TABLET_FIXTURES"))
    throw std::runtime_error("Tablet fixtures require the isolated verifier environment");
  if (std::string{__hyprland_api_get_hash()} != __hyprland_api_get_client_hash())
    throw std::runtime_error("Fixture ABI mismatch");
  tablet = makeShared<COhtFixtureTablet>();
  for (auto &tool : tools)
    tool = makeShared<COhtFixtureTool>();
  g_pInputManager->newTablet(tablet);
  touch = makeShared<COhtFixtureTouch>();
  g_pInputManager->newTouchDevice(touch);
  mouse = makeShared<COhtFixturePointer>();
  g_pInputManager->newMouse(mouse);
  tipListener = Event::bus()->m_events.input.tablet.tip.listen([](auto, auto &info) { consumed = info.cancelled; });
  axisListener = Event::bus()->m_events.input.tablet.axis.listen([](auto, auto &info) { consumed = info.cancelled; });
  if (!HyprlandAPI::addLuaFunction(handle, "oht_tablet", "event", fixtureEvent))
    throw std::runtime_error("Cannot register tablet fixture Lua API");
  return {"oht-tablet-fixture", "Isolated tablet handler fixtures", "tests", "1"};
}
APICALL EXPORT void PLUGIN_EXIT() {
  tipListener.reset();
  axisListener.reset();
  mouse->events.destroy.emit();
  mouse.reset();
  touch->events.destroy.emit();
  touch.reset();
  tablet->events.destroy.emit();
  tablet.reset();
  for (auto &tool : tools)
    tool.reset();
}
