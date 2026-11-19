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

bool shaftMaterial(const char *name) {
  if (!name || !*name)
    return false;
  for (const auto &p : prefixes())
    if (_strnicmp(name, p.c_str(), p.size()) == 0)
      return true;
  return false;
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

// The base texture's texel at `uv` (wrapped), linear-ish 0..1 (its CPU copy;
// false when unknown).
bool texel(const void *texture, float u, float v, float out[3]) {
  static thread_local std::vector<uint8_t> pixels;
  static thread_local const void *cached = nullptr;
  static thread_local uint32_t cachedVersion = ~0u, w = 0, h = 0;
  static thread_local bool bgra = false;
  uint32_t width = 0, height = 0, version = 0;
  bool isBgra = false;
  if (!remix::texturePixels(texture, width, height, version, isBgra, nullptr) || !width || !height)
    return false;
  if (texture != cached || version != cachedVersion) {
    if (!remix::texturePixels(texture, width, height, version, isBgra, &pixels) ||
        pixels.size() < size_t(width) * height * 4)
      return false;
    cached = texture, cachedVersion = version, w = width, h = height, bgra = isBgra;
  }
  const float fu = u - std::floor(u), fv = v - std::floor(v);
  const uint32_t x = std::min(w - 1, uint32_t(fu * w)), y = std::min(h - 1, uint32_t(fv * h));
  const uint8_t *t = pixels.data() + (size_t(y) * w + x) * 4;
  out[0] = (bgra ? t[2] : t[0]) / 255.f;
  out[1] = t[1] / 255.f;
  out[2] = (bgra ? t[0] : t[2]) / 255.f;
  return true;
}

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

} // namespace

Settings &settings() {
  static Settings s;
  static const bool initialised = [] {
    s.enabled = !envOff("OPENREMIX_PROXY_LIGHT_SHAFTS");
    s.shadows = !envOff("OPENREMIX_PROXY_LIGHT_SHAFT_SHADOWS");
    s.strength = envFloat("OPENREMIX_PROXY_LIGHT_SHAFT_STRENGTH", 1);
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
  if (!shaftMaterial(engine_materials::currentName()))
    return false;
  const auto *pos = element(d.elements, D3DDECLUSAGE_POSITION, 0);
  if (!pos || pos->Type != D3DDECLTYPE_FLOAT3 || !d.vsConstants)
    return false;
  const auto *col = element(d.elements, D3DDECLUSAGE_COLOR, 0);
  const auto *uv = element(d.elements, D3DDECLUSAGE_TEXCOORD, 0);
  const auto &ps = d.streams[pos->Stream];
  if (!ps.data || !ps.stride)
    return false;
  // The model matrix (c58..c60, object to world rows); world-space draws
  // (beams, dynamic meshes) have identity there.
  const float *m = d.vsConstants + 58 * 4;
  if (!std::all_of(m, m + 12, [](float x) { return std::isfinite(x); }))
    return false;
  static thread_local std::vector<Sample> samples;
  samples.clear();
  float lo = 1e30f, hi = -1e30f;
  bool texelsRead = false;
  for (uint32_t i = 0; i < d.numVertices && samples.size() < 4096; ++i) {
    const uint64_t at = uint64_t(ps.offset) + uint64_t(int64_t(d.baseVertex) + d.minIndex + i) * ps.stride;
    if (at + pos->Offset + 12 > ps.size)
      break;
    float p[3];
    std::memcpy(p, ps.data + at + pos->Offset, 12);
    Sample s;
    s.p = {m[0] * p[0] + m[1] * p[1] + m[2] * p[2] + m[3], m[4] * p[0] + m[5] * p[1] + m[6] * p[2] + m[7],
           m[8] * p[0] + m[9] * p[1] + m[10] * p[2] + m[11]};
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
    // The texture at the vertex: a fade painted along the shaft (vol_light's).
    if (uv && uv->Type == D3DDECLTYPE_FLOAT2 && d.texture0) {
      const auto &us = d.streams[uv->Stream];
      const uint64_t c = uint64_t(us.offset) + uint64_t(int64_t(d.baseVertex) + d.minIndex + i) * us.stride;
      float t[2], texel3[3];
      if (us.data && c + uv->Offset + 8 <= us.size) {
        std::memcpy(t, us.data + c + uv->Offset, 8);
        if (texel(d.texture0, t[0], t[1], texel3)) {
          texelsRead = true;
          for (int k = 0; k < 3; ++k)
            rgb[k] *= texel3[k];
        }
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
  Fit f;
  if (!((hi - lo > .25f && fitByFade(samples, (lo + hi) * .5f, f)) || fitByBounds(samples, f)))
    return false;
  // Texels not read at the vertices (no CPU copy of the texture): the sheet's
  // mean colour instead, its base texture's times its second's
  // (UnlitTwoTexture: a mask times dust), and their alpha where the draw
  // blends by it. A ray texture is mostly dark: taken as white it glowed
  // several times too bright.
  float mean[3] = {1, 1, 1};
  if (!texelsRead) {
    const std::string names[2] = {engine_materials::current().base, engine_materials::currentTextureName("$texture2")};
    for (const auto &name : names) {
      float rgba[4];
      if (name.empty() || !bsp::textureMeanLinear(name, rgba))
        continue;
      const float a = d.srcBlend == D3DBLEND_SRCALPHA ? rgba[3] : 1;
      for (int c = 0; c < 3; ++c)
        mean[c] *= rgba[c] * a;
    }
    for (int c = 0; c < 3; ++c)
      f.color[c] *= mean[c];
  }
  // The glow per world unit at the near cap: what the sheet adds there seen
  // side-on (both faces without culling) over the thickness seen through.
  const float strength = settings().strength;
  const bool shadows = settings().shadows;
  const float faces = d.cullMode == D3DCULL_NONE ? 2.f : 1.f;
  const float thickness = std::max(f.nearU + f.nearV, 1e-3f);
  const float scale = faces * strength / thickness / .75f; // the sides' soft fade made up for
  remixapi_VolumeShaftEXT s = {};
  s.sType = REMIXAPI_STRUCT_TYPE_VOLUME_SHAFT_EXT;
  s.origin = f3(f.origin);
  s.axis = f3(f.axis);
  s.halfU = f3(f.u * f.nearU);
  s.halfV = f3(f.v * f.nearV);
  s.farScaleU = f.farU / f.nearU;
  s.farScaleV = f.farV / f.nearV;
  // The colour as drawn (sRGB texels and vertex colours), linearised.
  // The colour as drawn (sRGB vertex colours and texels), linearised; a texture
  // mean is linear already.
  auto linear = [](float c) { return std::pow(std::max(c, 0.f), 2.2f); };
  if (texelsRead)
    s.radiance = {linear(f.color[0]) * scale, linear(f.color[1]) * scale, linear(f.color[2]) * scale};
  else
    s.radiance = {linear(f.color[0] / std::max(mean[0], 1e-6f)) * mean[0] * scale,
                  linear(f.color[1] / std::max(mean[1], 1e-6f)) * mean[1] * scale,
                  linear(f.color[2] / std::max(mean[2], 1e-6f)) * mean[2] * scale};
  s.farFade = f.nearBright > 0 ? f.farBright / f.nearBright : 1;
  s.softness = .5f;
  s.shape = REMIXAPI_VOLUME_SHAFT_BOX_EXT;
  s.flags = shadows ? REMIXAPI_VOLUME_SHAFT_SHADOWED_EXT : 0;
  g_frame.push_back(s);
  // Each material and vertex count once (shaderapi_proxy_diag.log): what was fitted.
  static std::set<std::pair<std::string, uint32_t>> logged;
  const char *name = engine_materials::currentName();
  if (logged.size() < 64 && logged.emplace(name ? name : "", d.numVertices).second)
    diag::stage("light shaft '%s' (%s, %u vertices, cull %u): %s fit, origin (%.0f %.0f %.0f) axis (%.0f %.0f %.0f) "
                "near %.1f x %.1f far %.1f x %.1f, brightness %.2f to %.2f, colour (%.2f %.2f %.2f)%s, radiance %.4f",
                name ? name : "?", engine_materials::bound().shader, d.numVertices, unsigned(d.cullMode),
                hi - lo > .25f ? "fade" : "bounds", f.origin.x, f.origin.y, f.origin.z, f.axis.x, f.axis.y, f.axis.z,
                f.nearU, f.nearV, f.farU, f.farV, f.nearBright, f.farBright, f.color[0], f.color[1], f.color[2],
                texelsRead ? " from texels" : " from texture means", s.radiance.x);
  return true;
}

void submit() {
  if (auto draw = drawShafts(); draw && !g_frame.empty())
    draw(g_frame.data(), uint32_t(g_frame.size()));
  g_lastCount = uint32_t(g_frame.size());
  static uint32_t frames = 0;
  if (!g_frame.empty() && ++frames % 600 == 1)
    diag::stage("light shafts: %zu this frame (the runtime draws the nearest 32)", g_frame.size());
  g_frame.clear();
}

} // namespace proxy::light_shafts
