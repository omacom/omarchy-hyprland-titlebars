#pragma once

#include <map>

#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/render/Texture.hpp>
#include <hyprland/src/config/values/types/BoolValue.hpp>
#include <hyprland/src/config/values/types/IntValue.hpp>
#include <hyprland/src/config/values/types/StringValue.hpp>
#include <hyprland/src/config/values/types/ColorValue.hpp>
#include <hyprland/src/config/values/types/FontWeightValue.hpp>

inline HANDLE OMARCHY_FLOATING_PHANDLE = nullptr;

struct SOmarchyFloatingHyprButton {
  std::string cmd = "";
  bool userfg = false;
  CHyprColor fgcol = CHyprColor(0, 0, 0, 0);
  CHyprColor bgcol = CHyprColor(0, 0, 0, 0);
  float size = 10;
  std::string icon = "";
  // Icon textures keyed by monitor scale x100: a window can be drawn on
  // monitors of different scales in the same frame.
  std::map<int, SP<Render::ITexture>> iconTex;
};

class COmarchyFloatingHyprBar;

struct SOmarchyFloatingGlobalState {
  std::vector<SOmarchyFloatingHyprButton> buttons;
  std::vector<WP<COmarchyFloatingHyprBar>> bars;
  uint32_t nobarRuleIdx = 0;
  uint32_t barColorRuleIdx = 0;
  uint32_t titleColorRuleIdx = 0;

  struct {
    SP<Config::Values::CColorValue> barColor, textColor, inactiveBarColor, inactiveButtonColor, buttonBorderColor;
    SP<Config::Values::CIntValue> barHeight;
    SP<Config::Values::CIntValue> barTextSize;
    SP<Config::Values::CFontWeightValue> barTextWeight;
    SP<Config::Values::CIntValue> barPadding;
    SP<Config::Values::CIntValue> barButtonPadding;
    SP<Config::Values::CIntValue> buttonRounding;
    SP<Config::Values::CIntValue> buttonBorderSize;
    SP<Config::Values::CIntValue> barIconSize;
    SP<Config::Values::CBoolValue> barBlur, barTitleEnabled, barIconEnabled, barPartOfWindow, barPrecedenceOverBorder, enabled, iconOnHover;
    SP<Config::Values::CStringValue> barTextFont, barTextAlign, barButtonsAlignment, onDoubleClick, workspaceTag;
  } config;
};

inline UP<SOmarchyFloatingGlobalState> g_pOmarchyFloatingGlobalState;
