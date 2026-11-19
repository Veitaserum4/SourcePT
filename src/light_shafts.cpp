#include "light_shafts.h"

#include <windows.h>

#include <remix/remix_c.h>
#include <remix/remix_native_ext.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "bsp_world.h"
#include "diag.h"
#include "engine_materials.h"
#include "game.h"
#include "remix_output.h"
#include "vertex_program.h"

namespace proxy::light_shafts {
namespace {

struct Vec {
  float x = 0, y = 0, z = 0;
};
Vec operator+(Vec a, Vec b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec operator-(Vec a, Vec b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec operator*(Vec a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float dot(Vec a, Vec b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec cross(Vec a, Vec b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
float length(Vec a) { return std::sqrt(dot(a, a)); }
remixapi_Float3D f3(Vec v) { return {v.x, v.y, v.z}; }

bool envOff(const char *name) {
  char v[8] = {};
  return GetEnvironmentVariableA(name, v, sizeof(v)) && v[0] == '0';
}

float envFloat(const char *name, float fallback) {
  char v[32] = {};
  return GetEnvironmentVariableA(name, v, sizeof(v)) ? float(std::atof(v)) : fallback;
}

// Material names (prefixes, any case) of Source's fake light shafts.
const std::vector<std::string> &prefixes() {
  static const std::vector<std::string> list = [] {
    std::vector<std::string> l = {"models/effects/vol_light", "sprites/spotlight", "sprites/blueshaft",
                                  "effects/lightshaft",       "effects/light_rays", "effects/lightrays",
                                  "effects/godray",           "effects/vol_light",  "effects/volumetric"};
    char extra[1024] = {};
    if (GetEnvironmentVariableA("OPENREMIX_PROXY_LIGHT_SHAFTS", extra, sizeof(extra)) && extra[0] != '0') {
      std::string s = extra;
      for (size_t start = 0; start < s.size();) {
        size_t end = s.find(';', start);
        if (end == std::string::npos)
          end = s.size();
        if (end > start)
          l.push_back(s.substr(start, end - start));
        start = end + 1;
      }
    }
    return l;
  }();
  return list;
}

// The material names as the engine binds them: a sprite drawn with a render
// mode is a copy named "materials/sprites/x_rendermode_5"; the "materials/" is
// dropped before matching.
const char *shaftName(const char *name) {
  if (!name)
    return "";
  return _strnicmp(name, "materials/", 10) == 0 ? name + 10 : name;
}

bool shaftMaterial(const char *name, uint32_t primitives) {
  name = shaftName(name);
  if (!*name)
    return false;
  for (const auto &p : prefixes())
    if (_strnicmp(name, p.c_str(), p.size()) == 0)
      return true;
  // point_spotlight's beam (sprites/glow_test02, the halo at the lamp being
  // sprites/light_glow03): a strip of segments; the same sprite as a lone
  // quad (2 triangles) is a glow, not a beam.
  (void)primitives;
  return _strnicmp(name, "sprites/glow_test02", 19) == 0;
}

const D3DVERTEXELEMENT9 *element(const D3DVERTEXELEMENT9 *e, BYTE usage, BYTE index) {
  for (; e && e->Stream != 0xff; ++e)
    if (e->Usage == usage && e->UsageIndex == index)
      return e;
  return nullptr;
}

PFN_remixapi_DrawVolumeShaftsEXT drawShafts() {
  static const auto fn = reinterpret_cast<PFN_remixapi_DrawVolumeShaftsEXT>(
      reinterpret_cast<void *>(GetProcAddress(game::runtimeModule(), "remixapi_DrawVolumeShaftsEXT")));
  return fn;
}

std::vector<remixapi_VolumeShaftEXT> g_frame;

struct Sample {
  Vec p;
  float rgb[3];
  float bright;
};

// Fitted along the fade (bright vertices to dark ones), else along the longest
// side of the bounds (its narrower end the near cap): the caps' centres, their
// extents across the axis, and the brightness at each.
struct Fit {
  Vec origin, axis, u, v;
  float nearU = 0, nearV = 0, farU = 0, farV = 0, nearBright = 1, farBright = 1;
  float color[3] = {1, 1, 1};
};

void setCaps(Fit &f, float nu, float nv, float fu, float fv) {
  // A flat cap (a sheet, a beam quad) is as deep as it is wide.
  auto thicken = [](float &a, float &b) {
    if (a < .1f * b)
      a = b;
    else if (b < .1f * a)
      b = a;
  };
  thicken(nu, nv);
  thicken(fu, fv);
  f.nearU = std::max({nu, fu * .01f, 1e-3f});
  f.nearV = std::max({nv, fv * .01f, 1e-3f});
  f.farU = std::max({fu, nu * .01f, 1e-3f});
  f.farV = std::max({fv, nv * .01f, 1e-3f});
}

void frameAcross(Fit &f) {
  const Vec along = f.axis * (1 / length(f.axis));
  // The world axis least along the shaft, made perpendicular to it.
  Vec seed = std::fabs(along.x) <= std::fabs(along.y) && std::fabs(along.x) <= std::fabs(along.z) ? Vec{1, 0, 0}
             : std::fabs(along.y) <= std::fabs(along.z)                                           ? Vec{0, 1, 0}
                                                                                                   : Vec{0, 0, 1};
  f.u = seed - along * dot(seed, along);
  f.u = f.u * (1 / length(f.u));
  f.v = cross(along, f.u);
}

bool fitByFade(const std::vector<Sample> &s, float threshold, Fit &f) {
  Vec bmin{1e30f, 1e30f, 1e30f}, bmax{-1e30f, -1e30f, -1e30f}, dmin = bmin, dmax = bmax;
  float bsum = 0, dsum = 0, rgb[3] = {};
  int bn = 0, dn = 0;
  for (const auto &x : s) {
    const bool bright = x.bright >= threshold;
    Vec &lo = bright ? bmin : dmin, &hi = bright ? bmax : dmax;
    lo = {std::min(lo.x, x.p.x), std::min(lo.y, x.p.y), std::min(lo.z, x.p.z)};
    hi = {std::max(hi.x, x.p.x), std::max(hi.y, x.p.y), std::max(hi.z, x.p.z)};
    (bright ? bsum : dsum) += x.bright;
    (bright ? bn : dn)++;
    if (bright)
      for (int c = 0; c < 3; ++c)
        rgb[c] += x.rgb[c];
  }
  if (!bn || !dn)
    return false;
  const Vec nearC = (bmin + bmax) * .5f, farC = (dmin + dmax) * .5f;
  f.origin = nearC;
  f.axis = farC - nearC;
  if (length(f.axis) < 1e-3f)
    return false;
  frameAcross(f);
  float nu = 0, nv = 0, fu = 0, fv = 0;
  for (const auto &x : s) {
    const bool bright = x.bright >= threshold;
    const Vec d = x.p - (bright ? nearC : farC);
    const float a = std::fabs(dot(d, f.u)), b = std::fabs(dot(d, f.v));
    if (bright)
      nu = std::max(nu, a), nv = std::max(nv, b);
    else
      fu = std::max(fu, a), fv = std::max(fv, b);
  }
  setCaps(f, nu, nv, fu, fv);
  f.nearBright = bsum / bn;
  f.farBright = dsum / dn;
  for (int c = 0; c < 3; ++c)
    f.color[c] = rgb[c] / bn;
  return true;
}

// Along the mesh's main direction (its principal axis: a diagonal shaft is
// not along a world axis), the vertices within 15% of each extreme its caps;
// the brighter end, else the narrower one, the near cap.
bool fitByBounds(const std::vector<Sample> &s, Fit &f) {
  Vec mean{};
  for (const auto &x : s)
    mean = mean + x.p;
  mean = mean * (1.f / float(s.size()));
  float c[3][3] = {};
  for (const auto &x : s) {
    const Vec d = x.p - mean;
    const float v[3] = {d.x, d.y, d.z};
    for (int i = 0; i < 3; ++i)
      for (int j = 0; j < 3; ++j)
        c[i][j] += v[i] * v[j];
  }
  // Power iteration from the vertex farthest from the centre.
  Vec axis{1, 0, 0};
  float farthest = -1;
  for (const auto &x : s)
    if (const float d = dot(x.p - mean, x.p - mean); d > farthest)
      farthest = d, axis = x.p - mean;
  if (farthest < 1e-6f)
    return false;
  for (int it = 0; it < 32; ++it) {
    const Vec next{c[0][0] * axis.x + c[0][1] * axis.y + c[0][2] * axis.z,
                   c[1][0] * axis.x + c[1][1] * axis.y + c[1][2] * axis.z,
                   c[2][0] * axis.x + c[2][1] * axis.y + c[2][2] * axis.z};
    const float n = length(next);
    if (n < 1e-12f)
      break;
    axis = next * (1 / n);
  }
  float lo = 1e30f, hi = -1e30f;
  for (const auto &x : s) {
    const float t = dot(x.p - mean, axis);
    lo = std::min(lo, t), hi = std::max(hi, t);
  }
  const float span = hi - lo;
  if (span < 1e-3f)
    return false;
  struct End {
    Vec sum{};
    float bright = 0, rgb[3] = {}, width = 0;
    int n = 0;
  } ends[2];
  auto endOf = [&](const Sample &x) {
    const float t = (dot(x.p - mean, axis) - lo) / span;
    return t <= .15f ? 0 : t >= .85f ? 1 : -1;
  };
  for (const auto &x : s) {
    const int e = endOf(x);
    if (e < 0)
      continue;
    ends[e].sum = ends[e].sum + x.p;
    ends[e].bright += x.bright;
    for (int k = 0; k < 3; ++k)
      ends[e].rgb[k] += x.rgb[k];
    ++ends[e].n;
  }
  if (!ends[0].n || !ends[1].n)
    return false;
  Vec centre[2];
  for (int e = 0; e < 2; ++e) {
    centre[e] = ends[e].sum * (1.f / float(ends[e].n));
    ends[e].bright /= float(ends[e].n);
  }
  for (const auto &x : s)
    if (const int e = endOf(x); e >= 0) {
      const Vec d = x.p - centre[e];
      const Vec across = d - axis * dot(d, axis);
      ends[e].width = std::max(ends[e].width, length(across));
    }
  const int nearEnd = std::fabs(ends[0].bright - ends[1].bright) > .05f ? (ends[0].bright > ends[1].bright ? 0 : 1)
                                                                       : (ends[0].width <= ends[1].width ? 0 : 1);
  const End &n = ends[nearEnd], &fa = ends[1 - nearEnd];
  f.origin = centre[nearEnd];
  f.axis = centre[1 - nearEnd] - f.origin;
  if (length(f.axis) < 1e-3f)
    return false;
  frameAcross(f);
  float ext[2][2] = {};
  for (const auto &x : s)
    if (const int e = endOf(x); e >= 0) {
      const Vec d = x.p - centre[e];
      ext[e][0] = std::max(ext[e][0], std::fabs(dot(d, f.u)));
      ext[e][1] = std::max(ext[e][1], std::fabs(dot(d, f.v)));
    }
  setCaps(f, ext[nearEnd][0], ext[nearEnd][1], ext[1 - nearEnd][0], ext[1 - nearEnd][1]);
  f.nearBright = n.bright;
  f.farBright = fa.bright;
  for (int k = 0; k < 3; ++k)
    f.color[k] = n.rgb[k] / float(n.n);
  return true;
}

std::atomic<uint32_t> g_lastCount{0};
// The map's spotlights sent this frame (one shaft each, however many draws
// make up its beam).
std::vector<uint32_t> g_spotlightsSent;

bool isBeam(const char *material) {
  const char *n = shaftName(material);
  return _strnicmp(n, "sprites/", 8) == 0;
}

// A point_spotlight's beam (sprites/glow_test02, sprites/spotlight): the
// engine rebuilds it every frame as a fan of camera-facing quads, only the
// sides in view, so its shape cannot be fitted from them (as the view turned
// it became a wall). The map's entity gives it instead: the spotlight whose
// cone holds the draw's vertices, its origin, direction and length; the
// width at its end from how far the vertices spread from its axis. It fades
// to nothing at its end, as the engine's beam does. False: no spotlight holds
// it (a beam of no map entity), drawn as before.
bool beamFromSpotlight(const std::vector<Sample> &samples, remixapi_VolumeShaftEXT &out, bool &duplicate) {
  duplicate = false;
  const auto &lights = bsp::spotlights();
  if (lights.empty() || samples.empty())
    return false;
  Vec centre{};
  for (const auto &x : samples)
    centre = centre + x.p;
  centre = centre * (1.f / float(samples.size()));
  int best = -1;
  float bestDistance = 1e30f;
  for (size_t i = 0; i < lights.size(); ++i) {
    const auto &l = lights[i];
    const Vec o{l.origin[0], l.origin[1], l.origin[2]}, dir{l.direction[0], l.direction[1], l.direction[2]};
    const Vec d = centre - o;
    const float t = dot(d, dir);
    if (t < -l.width || t > l.length + l.width)
      continue;
    const float across = length(d - dir * t);
    if (across > l.width + std::max(t, 0.f) && across > l.width * 2)
      continue;
    if (across < bestDistance)
      bestDistance = across, best = int(i);
  }
  if (best < 0)
    return false;
  if (std::find(g_spotlightsSent.begin(), g_spotlightsSent.end(), uint32_t(best)) != g_spotlightsSent.end()) {
    duplicate = true;
    return true;
  }
  g_spotlightsSent.push_back(uint32_t(best));
  const auto &l = lights[size_t(best)];
  const Vec o{l.origin[0], l.origin[1], l.origin[2]}, dir{l.direction[0], l.direction[1], l.direction[2]};
  // The cone's widening: the steepest spread of the vertices from the axis
  // beyond the start's half width.
  const float startRadius = std::max(l.width * .5f, 1.f);
  float slope = 0;
  for (const auto &x : samples) {
    const Vec d = x.p - o;
    const float t = dot(d, dir);
    if (t > l.length * .1f)
      slope = std::max(slope, (length(d - dir * t) - startRadius) / t);
  }
  const float endRadius = std::max(startRadius, startRadius + std::min(slope, 2.f) * l.length);
  Fit f;
  f.origin = o;
  f.axis = dir * l.length;
  frameAcross(f);
  remixapi_VolumeShaftEXT s = {};
  s.sType = REMIXAPI_STRUCT_TYPE_VOLUME_SHAFT_EXT;
  s.origin = f3(o);
  s.axis = f3(f.axis);
  s.halfU = f3(f.u * startRadius);
  s.halfV = f3(f.v * startRadius);
  s.farScaleU = s.farScaleV = endRadius / startRadius;
  constexpr float kBase = .05f;
  const float scale = kBase * settings().beamStrength / (2 * startRadius) / .75f;
  auto linear = [](float c) { return std::pow(std::max(c, 0.f), 2.2f); };
  s.radiance = {linear(l.color[0]) * scale, linear(l.color[1]) * scale, linear(l.color[2]) * scale};
  s.farFade = 0;
  s.softness = .5f;
  s.shape = REMIXAPI_VOLUME_SHAFT_ELLIPSE_EXT;
  s.flags = settings().shadows ? REMIXAPI_VOLUME_SHAFT_SHADOWED_EXT : 0;
  out = s;
  static std::set<uint32_t> logged;
  if (logged.size() < 64 && logged.insert(uint32_t(best)).second)
    diag::stage("spotlight beam %d: origin (%.0f %.0f %.0f) direction (%.2f %.2f %.2f) length %.0f, radius %.1f to %.1f, "
                "colour (%.2f %.2f %.2f)",
                best, o.x, o.y, o.z, dir.x, dir.y, dir.z, l.length, startRadius, endRadius, l.color[0], l.color[1],
                l.color[2]);
  return true;
}

} // namespace

Settings &settings() {
  static Settings s;
  static const bool initialised = [] {
    s.enabled = !envOff("OPENREMIX_PROXY_LIGHT_SHAFTS");
    s.shadows = !envOff("OPENREMIX_PROXY_LIGHT_SHAFT_SHADOWS");
    s.strength = envFloat("OPENREMIX_PROXY_LIGHT_SHAFT_STRENGTH", 1);
    s.beamStrength = envFloat("OPENREMIX_PROXY_LIGHT_BEAM_STRENGTH", 1);
    return true;
  }();
  (void)initialised;
  return s;
}

bool available() { return drawShafts() != nullptr; }

uint32_t lastCount() { return g_lastCount.load(); }

bool capture(const remix::Draw &d) {
  if (!settings().enabled || !d.blend || d.dstBlend != D3DBLEND_ONE || !d.mainTarget || !d.elements || !drawShafts())
    return false;
  if (!shaftMaterial(engine_materials::currentName(), d.primCount)) {
    // Other additive draws, each material once (shaderapi_proxy_diag.log): the
    // names to look for when a map's shafts are not picked up.
    static std::set<std::string> seen;
    const char *other = engine_materials::currentName();
    if (seen.size() < 128 && other && *other && seen.insert(other).second) {
      const auto *p = element(d.elements, D3DDECLUSAGE_POSITION, 0);
      diag::stage("additive draw '%s' (%s, %u vertices, %u primitives, position type %d, src blend %u): not a "
                  "light shaft",
                  other, engine_materials::bound().shader, d.numVertices, d.primCount, p ? int(p->Type) : -1,
                  unsigned(d.srcBlend));
    }
    return false;
  }
  const auto *pos = element(d.elements, D3DDECLUSAGE_POSITION, 0);
  if (!pos || pos->Type != D3DDECLTYPE_FLOAT3 || !d.vsConstants)
    return false;
  const auto *col = element(d.elements, D3DDECLUSAGE_COLOR, 0);
  const auto &ps = d.streams[pos->Stream];
  if (!ps.data || !ps.stride)
    return false;
  // The model matrix (c58..c60, object to world rows). A shader that does not
  // read it draws world-space positions (a Sprite's beam): the registers then
  // hold whatever was drawn before, which placed beams anywhere.
  static const float identity[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
  const float *m = d.vsConstants + 58 * 4;
  if (d.vertexProgram) {
    const auto &read = vsprog::info(*d.vertexProgram).constants;
    if (!std::binary_search(read.begin(), read.end(), uint16_t(58)))
      m = identity;
  }
  if (!std::all_of(m, m + 12, [](float x) { return std::isfinite(x); }))
    return false;
  static thread_local std::vector<Sample> samples;
  samples.clear();
  float lo = 1e30f, hi = -1e30f;
  for (uint32_t i = 0; i < d.numVertices && samples.size() < 4096; ++i) {
    const uint64_t at = uint64_t(ps.offset) + uint64_t(int64_t(d.baseVertex) + d.minIndex + i) * ps.stride;
    if (at + pos->Offset + 12 > ps.size)
      break;
    float p[3];
    std::memcpy(p, ps.data + at + pos->Offset, 12);
    Sample s;
    s.p = {m[0] * p[0] + m[1] * p[1] + m[2] * p[2] + m[3], m[4] * p[0] + m[5] * p[1] + m[6] * p[2] + m[7],
           m[8] * p[0] + m[9] * p[1] + m[10] * p[2] + m[11]};
    // Its vertex colour (times alpha): a beam's fade along its length. The
    // texture is not sampled at the vertices: a cone's vertices sit on its
    // texture's dark edges (vol_light001), which made whole shafts black.
    float rgb[3] = {1, 1, 1};
    if (col && col->Type == D3DDECLTYPE_D3DCOLOR) {
      const auto &cs = d.streams[col->Stream];
      const uint64_t c = uint64_t(cs.offset) + uint64_t(int64_t(d.baseVertex) + d.minIndex + i) * cs.stride;
      if (cs.data && c + col->Offset + 4 <= cs.size) {
        uint32_t argb;
        std::memcpy(&argb, cs.data + c + col->Offset, 4);
        const float a = (argb >> 24) / 255.f;
        rgb[0] = ((argb >> 16) & 255) / 255.f * a;
        rgb[1] = ((argb >> 8) & 255) / 255.f * a;
        rgb[2] = (argb & 255) / 255.f * a;
      }
    }
    std::copy_n(rgb, 3, s.rgb);
    s.bright = std::max({rgb[0], rgb[1], rgb[2]});
    lo = std::min(lo, s.bright);
    hi = std::max(hi, s.bright);
    samples.push_back(s);
  }
  if (samples.size() < 3)
    return false;
  if (isBeam(engine_materials::currentName())) {
    remixapi_VolumeShaftEXT beamShaft;
    bool duplicate = false;
    if (!beamFromSpotlight(samples, beamShaft, duplicate))
      return false;
    if (!duplicate)
      g_frame.push_back(beamShaft);
    return true;
  }
  Fit f;
  if (!((hi - lo > .25f && fitByFade(samples, (lo + hi) * .5f, f)) || fitByBounds(samples, f)))
    return false;
  // Its hue: the textures' mean colour (base times $texture2) in linear light,
  // normalised to its brightest channel. Not their brightness: a ray texture
  // is mostly black, and its mean (or the mean's square) put shafts anywhere
  // from blinding to invisible. The brightness is a base level times the
  // vertex colours times the panel's strength.
  float hue[3] = {1, 1, 1};
  {
    const std::string names[2] = {engine_materials::current().base, engine_materials::currentTextureName("$texture2")};
    for (const auto &name : names) {
      float rgba[4];
      if (name.empty() || !bsp::textureMeanLinear(name, rgba))
        continue;
      for (int c = 0; c < 3; ++c)
        hue[c] *= rgba[c];
    }
    const float top = std::max({hue[0], hue[1], hue[2]});
    for (int c = 0; c < 3; ++c)
      hue[c] = top > 1e-6f ? hue[c] / top : 1;
  }
  // The glow per world unit at the near cap: a base level (what a ray
  // texture's sheet adds on average, seen side-on) over the thickness seen
  // through, both faces when not culled, the sides' soft fade made up for.
  constexpr float kBase = .05f;
  // A beam (a point_spotlight's, sprites/): its vertex colours are its render
  // colour times its fade, dim (0.1) however bright the game shows it: taken
  // relative to their brightest (the fade and tint), its strength its own.
  const char *material = engine_materials::currentName();
  const bool beam = _strnicmp(shaftName(material), "sprites/", 8) == 0;
  if (beam) {
    const float top = std::max({f.color[0], f.color[1], f.color[2]});
    if (top > 1e-4f)
      for (float &c : f.color)
        c /= top;
  }
  const float strength = beam ? settings().beamStrength.load() : settings().strength.load();
  const bool shadows = settings().shadows;
  const float faces = d.cullMode == D3DCULL_NONE ? 2.f : 1.f;
  const float thickness = std::max(f.nearU + f.nearV, 1e-3f);
  const float scale = kBase * faces * strength / thickness / .75f;
  remixapi_VolumeShaftEXT s = {};
  s.sType = REMIXAPI_STRUCT_TYPE_VOLUME_SHAFT_EXT;
  s.origin = f3(f.origin);
  s.axis = f3(f.axis);
  s.halfU = f3(f.u * f.nearU);
  s.halfV = f3(f.v * f.nearV);
  s.farScaleU = f.farU / f.nearU;
  s.farScaleV = f.farV / f.nearV;
  // The vertex colours as drawn (sRGB), linearised, times the hue.
  auto linear = [](float c) { return std::pow(std::max(c, 0.f), 2.2f); };
  s.radiance = {linear(f.color[0]) * hue[0] * scale, linear(f.color[1]) * hue[1] * scale,
                linear(f.color[2]) * hue[2] * scale};
  s.farFade = f.nearBright > 0 ? f.farBright / f.nearBright : 1;
  s.softness = .5f;
  // A beam (a point_spotlight's, drawn as a camera-facing strip) is round: the
  // cone inscribed in the fitted box. Model shafts (vol_light) are boxes.
  s.shape = _strnicmp(shaftName(material), "sprites/", 8) == 0 ? REMIXAPI_VOLUME_SHAFT_ELLIPSE_EXT
                                                                : REMIXAPI_VOLUME_SHAFT_BOX_EXT;
  s.flags = shadows ? REMIXAPI_VOLUME_SHAFT_SHADOWED_EXT : 0;
  g_frame.push_back(s);
  // Each material and vertex count once (shaderapi_proxy_diag.log): what was fitted.
  static std::set<std::pair<std::string, uint32_t>> logged;
  const char *name = engine_materials::currentName();
  if (logged.size() < 64 && logged.emplace(name ? name : "", d.numVertices).second)
    diag::stage("light shaft '%s' (%s, %u vertices, cull %u): %s fit, origin (%.0f %.0f %.0f) axis (%.0f %.0f %.0f) "
                "near %.1f x %.1f far %.1f x %.1f, brightness %.2f to %.2f, colour (%.2f %.2f %.2f), hue (%.2f %.2f "
                "%.2f), radiance %.4f",
                name ? name : "?", engine_materials::bound().shader, d.numVertices, unsigned(d.cullMode),
                hi - lo > .25f ? "fade" : "bounds", f.origin.x, f.origin.y, f.origin.z, f.axis.x, f.axis.y, f.axis.z,
                f.nearU, f.nearV, f.farU, f.farV, f.nearBright, f.farBright, f.color[0], f.color[1], f.color[2], hue[0],
                hue[1], hue[2], s.radiance.x);
  return true;
}

void submit() {
  if (auto draw = drawShafts(); draw && !g_frame.empty())
    draw(g_frame.data(), uint32_t(g_frame.size()));
  g_lastCount = uint32_t(g_frame.size());
  g_spotlightsSent.clear();
  static uint32_t frames = 0;
  if (!g_frame.empty() && ++frames % 600 == 1)
    diag::stage("light shafts: %zu this frame (the runtime draws the nearest 32)", g_frame.size());
  g_frame.clear();
}

} // namespace proxy::light_shafts
