#include "BarPassElement.hpp"
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include "barDeco.hpp"

using namespace Render::GL;

COmarchyFloatingBarPassElement::COmarchyFloatingBarPassElement(const COmarchyFloatingBarPassElement::SBarData &data_) : data(data_) { ; }

std::vector<UP<IPassElement>> COmarchyFloatingBarPassElement::draw(Render::CRenderContext &ctx) {
  data.deco->renderPass(ctx, ctx.m_data.pMonitor.lock(), data.a);
  return {};
}

bool COmarchyFloatingBarPassElement::needsLiveBlur(Render::CRenderContext &) {
  static auto PENABLEBLURGLOBAL = CConfigValue<Config::BOOL>("decoration:blur:enabled");

  // The color the bar draws in (focus, forced color and its animation), so
  // this agrees with renderPass on whether the bar blurs.
  CHyprColor color = data.deco->m_cRealBarColor ? data.deco->m_cRealBarColor->value()
                                                : data.deco->m_bForcedBarColor.value_or(CHyprColor{static_cast<uint64_t>(g_pOmarchyFloatingGlobalState->config.barColor->value())});
  color.a *= data.a;
  const bool SHOULDBLUR = g_pOmarchyFloatingGlobalState->config.barBlur->value() && *PENABLEBLURGLOBAL && color.a < 1.F;

  return SHOULDBLUR;
}

std::optional<CBox> COmarchyFloatingBarPassElement::boundingBox(Render::CRenderContext &ctx) {
  // Temporary fix: expand the bar bb a bit, otherwise occlusion gets too aggressive.
  return data.deco->assignedBoxGlobal().translate(-ctx.m_data.pMonitor->m_position).expand(10);
}

bool COmarchyFloatingBarPassElement::needsPrecomputeBlur(Render::CRenderContext &) { return false; }
