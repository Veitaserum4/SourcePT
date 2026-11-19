#pragma once

#include <atomic>
#include <cstdint>

namespace proxy::remix {
struct Draw;
}

// Fake light shafts drawn as glowing air (remixapi_DrawVolumeShaftsEXT, an
// OpenRemix extension): the additive, unlit cones and boxes Source fakes
// volumetric light with (HL2's models/effects/vol_light* props, point_spotlight
// beams, sprites/blueshaft1 and the like). Rasterized as sheets they glow alike
// edge-on and lengthwise and end in hard lines where they meet walls; as
// volumes they are brighter seen through more of them, cut softly by walls and
// shadowed by what stands in them. Render thread.
//
// OPENREMIX_PROXY_LIGHT_SHAFTS: 0 off (sheets as before); else extra material
// name prefixes, ';'-separated, beside the built-in ones.
// OPENREMIX_PROXY_LIGHT_SHAFT_STRENGTH (1): their brightness, 1 matching the
// sheet seen side-on at its source. OPENREMIX_PROXY_LIGHT_SHAFT_SHADOWS=0: no
// shadows.
namespace proxy::light_shafts {

// The draw is a light shaft: fitted and recorded for this frame (true: the
// caller does not draw it). False: not a shaft, one that could not be fitted,
// or a runtime without the extension; the caller draws it as before.
bool capture(const remix::Draw &d);

// Before Present: this frame's shafts to the runtime, then cleared.
void submit();

// The developer panel's settings (light_editor's panel), live: started from
// the environment variables above.
struct Settings {
  std::atomic<bool> enabled{true}, shadows{true};
  std::atomic<float> strength{1};
};
Settings &settings();
// The runtime has remixapi_DrawVolumeShaftsEXT (else the settings do nothing).
bool available();
// Shafts drawn last frame (the panel's status).
uint32_t lastCount();

} // namespace proxy::light_shafts
